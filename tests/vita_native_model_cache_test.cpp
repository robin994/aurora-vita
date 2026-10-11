#include "../lib/gx/native_model_recipe.hpp"
#include "../lib/gx/native_model_registry.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
using namespace aurora::gx::fifo;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
struct Entry {int material=0;NativeModelRecipeRef recipe{};};
int main(){
  // A single set deliberately forces every insertion to compete. Lookup
  // equality is semantic, not a hash equality; several variants share key 7.
  NativeModelCache<Entry> cache;cache.initialize(4,4);bool replaced=false;
  for(int i=1;i<=4;++i){auto& value=cache.allocate(7,replaced);CHECK(!replaced);value.material=i;}
  CHECK(cache.size()==4);
  CHECK(!cache.find(7,[](const Entry& e){return e.material==99;}));
  CHECK(!cache.find(8));
  CHECK(cache.find(7,[](const Entry& e){return e.material==1;}));
  auto& fifth=cache.allocate(7,replaced);CHECK(replaced);fifth.material=5;
  CHECK(cache.size()==4);
  CHECK(cache.find(7,[](const Entry& e){return e.material==1;}));
  CHECK(!cache.find(7,[](const Entry& e){return e.material==2;}));
  CHECK(cache.find(7,[](const Entry& e){return e.material==3;}));
  CHECK(cache.find(7,[](const Entry& e){return e.material==4;}));
  CHECK(cache.find(7,[](const Entry& e){return e.material==5;}));
  CHECK(cache.erase_if([](const Entry& e){return e.material==5;})==1);
  CHECK(cache.size()==3&&!cache.find(7,[](const Entry& e){return e.material==5;}));
  // Replacing metadata drops only its own reference. FIFO ownership keeps an
  // older immutable recipe alive until its outstanding command is consumed.
  NativeModelCache<Entry> ownership;ownership.initialize(1,1);
  auto mutableRecipe=std::make_shared<NativeModelRecipe>();mutableRecipe->identity=123;
  mutableRecipe->before={1,2,3};
  NativeModelRecipeRef queued=mutableRecipe;
  std::weak_ptr<const NativeModelRecipe> weak=queued;
  ownership.allocate(1,replaced).recipe=mutableRecipe;mutableRecipe.reset();
  ownership.allocate(2,replaced);CHECK(replaced&&!weak.expired());
  CHECK(queued->identity==123&&queued->before==std::vector<uint8_t>({1,2,3}));
  queued.reset();CHECK(weak.expired());
  cache.clear();CHECK(cache.size()==0&&!cache.find(7));
  CHECK(cache.storage_bytes()>0);cache.release();CHECK(cache.storage_bytes()==0);
  cache.initialize();CHECK(cache.storage_bytes()>0&&cache.size()==0);
  cache.release();ownership.release();CHECK(NativeModelRecipe::liveRecipes.load()==0);
  // Aurora owns the semantic identity, independent of a Strikers packet
  // layout. Model cache hits compare all material and stream descriptors,
  // not just the display-list pointer/hash.
  uint8_t dl[64]{};
  uint8_t positions[36]{},colors[12]{};
  NativeModelDescriptor desc{};
  desc.displayList=reinterpret_cast<uintptr_t>(dl);
  desc.displayListBytes=64;
  desc.view=11;desc.program=3;desc.vertexCount=48;
  desc.materialSet=7;desc.materialState=8;desc.rasterState=9;
  desc.textureState=0x1234567887654321ULL;desc.texConfig=3;
  desc.userStateKey=6;desc.primitive=0;desc.streamCount=2;
  desc.textures[0]=0x2000;
  desc.streams[0]={reinterpret_cast<uintptr_t>(positions),36,0,12,true};
  desc.streams[1]={reinterpret_cast<uintptr_t>(colors),12,2,4,true};
  CHECK(desc.valid());
  auto hold=std::make_shared<NativeModelRecipe>();
  hold->identity=24;hold->source=dl;hold->bytes=64;
  NativeModelRegistry registry;
  CHECK(!registry.lookup(desc));CHECK(registry.store(desc,hold));
  CHECK(registry.lookup(desc)&&*registry.lookup(desc)==hold&&registry.size()==1);
  const auto check_difference=[&](auto mutate){
    auto copy=desc;mutate(copy);
    CHECK(copy.valid()&&!registry.lookup(copy));
  };
  check_difference([](auto& d){++d.view;});
  check_difference([](auto& d){++d.program;});
  check_difference([](auto& d){++d.vertexCount;});
  check_difference([](auto& d){++d.materialSet;});
  check_difference([](auto& d){++d.materialState;});
  check_difference([](auto& d){++d.rasterState;});
  check_difference([](auto& d){++d.textureState;});
  check_difference([](auto& d){++d.sourceGeneration;});
  check_difference([](auto& d){++d.materialGeneration;});
  check_difference([](auto& d){++d.userStateKey;});
  check_difference([](auto& d){++d.texConfig;});
  check_difference([](auto& d){++d.primitive;});
  check_difference([](auto& d){++d.textures[0];});
  check_difference([](auto& d){++d.streams[0].address;});
  check_difference([](auto& d){++d.streams[0].dataBytes;});
  check_difference([](auto& d){++d.streams[0].stride;});
  check_difference([](auto& d){++d.streams[0].semantic;});
  check_difference([](auto& d){d.streams[1].bigEndian=false;});
  check_difference([](auto& d){++d.displayListBytes;});
  check_difference([](auto& d){++d.displayList;});
  auto invalid=desc;invalid.streamCount=9;CHECK(!invalid.valid()&&!registry.store(invalid,hold));
  invalid=desc;invalid.streams[0].dataBytes=0;
  CHECK(!invalid.valid()&&!registry.lookup(invalid));
  CHECK(registry.metadata_bytes()>0);
  // The queued recipe remains owned even after both producer cache and
  // caller release their handles; no GPU buffer lifetime is tied to it.
  NativeModelRecipeRef registeredQueued=*registry.lookup(desc);
  std::weak_ptr<const NativeModelRecipe> retained=registeredQueued;
  hold.reset();
  CHECK(registry.invalidate_source(0)==0);
  CHECK(registry.invalidate_source(desc.displayList)==1);
  CHECK(registry.invalidate_source(desc.displayList)==0);
  CHECK(!registry.lookup(desc)&&registry.size()==0);
  CHECK(!retained.expired()&&registeredQueued->identity==24);
  registeredQueued.reset();CHECK(retained.expired());
  registry.clear();CHECK(registry.size()==0);
  registry.release();CHECK(registry.size()==0);
  CHECK(NativeModelRecipe::liveRecipes.load()==0);

  ModelAdmissionFacts staticFacts{};
  staticFacts.packet=true;staticFacts.compiled=true;staticFacts.flags=0x800;
  staticFacts.view=staticFacts.previousView=11;staticFacts.hasStreams=true;
  staticFacts.displayList=true;staticFacts.allowedProgram=staticFacts.allowedView=true;
  staticFacts.vertices=48;staticFacts.streams=2;staticFacts.listBytes=64;
  staticFacts.ownedMatrix=true;staticFacts.position=true;
  CHECK(native_model_static_admissible(staticFacts));
  staticFacts.textureMask=1;CHECK(!native_model_static_admissible(staticFacts));
  staticFacts.textureMask=0;staticFacts.userData=true;
  CHECK(!native_model_static_admissible(staticFacts));
  NativeModelRuntimeFacts dynamic{};
  dynamic.packet=true;dynamic.compiled=true;dynamic.displayList=true;
  dynamic.supportedProgram=dynamic.supportedView=true;
  dynamic.vertices=48;dynamic.primitive=3;dynamic.streams=3;dynamic.listBytes=64;
  CHECK(native_model_runtime_admissible(dynamic));
  dynamic.primitive=4;CHECK(!native_model_runtime_admissible(dynamic));
  dynamic.primitive=3;dynamic.supportedProgram=false;
  CHECK(!native_model_runtime_admissible(dynamic));
  puts("native model cache: exact identity on collisions, selective LRU replacement, bounded occupancy, in-flight ownership, reset");
}
