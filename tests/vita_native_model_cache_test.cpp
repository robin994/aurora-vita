#include "../lib/gx/native_model_recipe.hpp"
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
  puts("native model cache: exact identity on collisions, selective LRU replacement, bounded occupancy, in-flight ownership, reset");
}
