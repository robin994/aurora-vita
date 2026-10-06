#pragma once
#include <cstdint>
#include <cstddef>
namespace aurora::gx::fifo {
struct PreparedDisplayList {
  uint32_t vertexBytes=0, stride=0;
  uint16_t count=0;
  uint8_t command=0;
  bool valid=false;
};
inline PreparedDisplayList prepare_display_list(const uint8_t* data,size_t bytes,uint32_t stride) noexcept {
  PreparedDisplayList out{};
  if(!data||bytes<3||!stride)return out;
  const uint8_t opcode=data[0]&0xf8u;
  if(opcode<0x80u||opcode>0xb8u)return out;
  const uint16_t count=uint16_t(data[1])<<8|data[2];
  const uint64_t payload=uint64_t(count)*stride;
  if(!count||payload>UINT32_MAX||payload>bytes-3)return out;
  for(size_t i=3+size_t(payload);i<bytes;++i)if(data[i])return out;
  out.vertexBytes=uint32_t(payload);out.stride=stride;out.count=count;out.command=data[0];out.valid=true;
  return out;
}
} // namespace aurora::gx::fifo
