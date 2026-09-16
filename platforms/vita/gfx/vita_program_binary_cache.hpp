#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace aurora::vita::gfx {
struct ProgramCacheHeader {
  uint32_t magic=0x41564750u;
  uint32_t version=1;
  uint64_t sourceHash=0;
  uint64_t binaryHash=0;
  uint32_t length=0;
  uint32_t format=0;
};
static_assert(sizeof(ProgramCacheHeader)==32);
inline constexpr size_t MaxProgramCacheBytes=4u*1024u*1024u;
inline uint64_t program_cache_hash(const void* bytes,size_t count,uint64_t hash=14695981039346656037ull) noexcept {
  const auto* p=static_cast<const uint8_t*>(bytes);
  for(size_t i=0;i<count;++i)hash=(hash^p[i])*1099511628211ull;
  return hash;
}
inline uint64_t program_source_hash(const char* vertex,const char* fragment) noexcept {
  // Include the binding contract and a terminator between stages, not only the
  // fragment text: Vita's GLSL pair compiler assigns shared varying semantics.
  static constexpr char contract[]="aurora-vitagl-binary-v1-attrs-pos0-color1,2-tex3:10-basis11:13";
  auto h=program_cache_hash(contract,sizeof(contract));
  h=program_cache_hash(vertex,std::strlen(vertex)+1,h);
  return program_cache_hash(fragment,std::strlen(fragment)+1,h);
}
inline bool valid_program_cache(const ProgramCacheHeader& header,const uint8_t* payload,size_t bytes,
                                uint64_t expectedSource,size_t attributeBytes,uint32_t maxAttributes=16) noexcept {
  if(!payload||header.magic!=0x41564750u||header.version!=1||header.sourceHash!=expectedSource||
     bytes!=header.length||bytes>MaxProgramCacheBytes||bytes<attributeBytes+12||attributeBytes<4)return false;
  if(program_cache_hash(payload,bytes)!=header.binaryHash)return false;
  uint32_t attributes=0,vertexBytes=0,fragmentBytes=0;
  std::memcpy(&attributes,payload,4);
  if(attributes>maxAttributes)return false;
  std::memcpy(&vertexBytes,payload+attributeBytes,4);
  if(vertexBytes<4||vertexBytes>bytes-attributeBytes-8)return false;
  const size_t fragmentOffset=attributeBytes+4+vertexBytes;
  std::memcpy(&fragmentBytes,payload+fragmentOffset,4);
  if(fragmentBytes<4||fragmentBytes!=bytes-fragmentOffset-4)return false;
  // Matrix-index metadata is inside each serialized shader. Check it before
  // entering vitaGL's deserializer, which assumes its input is well-formed.
  for(unsigned i=0;i<2;++i){
    const size_t offset=i?fragmentOffset+4:attributeBytes+4;
    const size_t size=i?fragmentBytes:vertexBytes;
    uint32_t matrices=0;std::memcpy(&matrices,payload+offset,4);
    if(matrices>(size-4)/4)return false;
  }
  return true;
}
} // namespace aurora::vita::gfx
