#include "../lib/gx/prepared_display_list.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
using namespace aurora::gx::fifo;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main() {
  std::array<uint8_t,64> bytes{};bytes[0]=0x90;bytes[2]=3;
  auto d=prepare_display_list(bytes.data(),64,12);
  CHECK(d.valid&&d.count==3&&d.vertexBytes==36&&d.command==0x90);
  // A CP format change must validate again with its new stride.
  CHECK(!prepare_display_list(bytes.data(),64,24).valid);
  bytes[63]=0x61;CHECK(!prepare_display_list(bytes.data(),64,12).valid);bytes[63]=0;
  CHECK(!prepare_display_list(bytes.data(),38,12).valid);
  CHECK(!prepare_display_list(bytes.data(),2,12).valid);
  CHECK(!prepare_display_list(nullptr,64,12).valid);
  CHECK(!prepare_display_list(bytes.data(),64,0).valid);
  bytes[2]=0;CHECK(!prepare_display_list(bytes.data(),64,12).valid);bytes[2]=3;
  bytes[0]=0x61;CHECK(!prepare_display_list(bytes.data(),64,12).valid);
  for(unsigned cmd=0x80;cmd<=0xbf;++cmd) {bytes[0]=cmd;CHECK(prepare_display_list(bytes.data(),64,12).valid);}
  bytes[0]=0xc0;CHECK(!prepare_display_list(bytes.data(),64,12).valid);
  std::puts("prepared display list bounds, state stride and opcode fallback passed");
}
