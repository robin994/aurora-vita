#include "../platforms/vita/gxm/gxm_program_cache.hpp"
#include <cstdlib>
#include <cstring>

using namespace aurora::vita::gxm;
static void check(bool ok) {if(!ok)std::abort();}
static std::vector<uint8_t> encoded(uint64_t hash,ProgramStage stage,uint8_t value,size_t size=32) {
  std::vector<uint8_t> bytes(sizeof(GxmProgramCacheHeader)+size,value);
  GxmProgramCacheHeader h{};h.sourceHash=hash;h.stage=static_cast<uint32_t>(stage);h.length=size;
  h.binaryHash=aurora::vita::gfx::program_cache_hash(bytes.data()+sizeof h,size);
  std::memcpy(bytes.data(),&h,sizeof h);return bytes;
}
int main() {
  DeferredProgramWrites queue;
  const auto first=encoded(10,ProgramStage::Vertex,42);
  const auto fragment=encoded(10,ProgramStage::Fragment,43);
  check(queue.stage(10,1,first)&&queue.stage(10,2,fragment));
  check(queue.stage(10,1,first)&&queue.size()==2);
  std::vector<uint32_t> words;
  check(decode_gxm_program_cache(*queue.find(10,1),10,ProgramStage::Vertex,words));
  check(!decode_gxm_program_cache(*queue.find(10,1),10,ProgramStage::Fragment,words));
  auto corrupt=first;corrupt.back()^=1;
  check(!decode_gxm_program_cache(corrupt,10,ProgramStage::Vertex,words));
  const auto replacement=encoded(10,ProgramStage::Vertex,44,64);
  check(queue.stage(10,1,replacement)&&*queue.find(10,1)==replacement);
  check(queue.bytes()==replacement.size()+fragment.size());
  check(queue.flush([](auto,auto,const auto&) {return false;})==0);
  check(queue.size()==2&&queue.errors()==2&&queue.written_bytes()==0);
  check(queue.flush([](auto,uint32_t stage,const auto&) {return stage==1;})==1);
  check(!queue.find(10,1)&&queue.find(10,2)&&queue.written_bytes()==replacement.size());
  check(queue.flush([](auto,auto,const auto&) {return true;})==1&&queue.bytes()==0);
  queue.clear();
  std::vector<uint8_t> large(DeferredProgramWrites::MaxRecordBytes,7);
  for(unsigned i=0;i<8;++i)check(queue.stage(i,1,large));
  check(queue.bytes()==DeferredProgramWrites::MaxBytes&&!queue.stage(99,1,first));
  check(queue.stage(0,1,first)&&queue.bytes()==7*large.size()+first.size());
  check(queue.stage(99,1,first));
  queue.clear();
  for(unsigned i=0;i<DeferredProgramWrites::MaxRecords;++i)check(queue.stage(i,1,first));
  check(!queue.stage(1000,1,first)&&queue.size()==DeferredProgramWrites::MaxRecords);
  check(!queue.stage(1001,3,first));
  large.push_back(0);check(!queue.stage(0,1,large));
  check(*queue.find(0,1)==first);
}
