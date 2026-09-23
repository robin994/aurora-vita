#include "gfx/vita_memory_revision.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
unsigned checks=0;
void check(bool result) {
  ++checks;
  if(!result) {
    std::fprintf(stderr,"memory revision failed at check %u\n",checks);
    std::exit(1);
  }
}
}

int main() {
  using namespace aurora::vita::gfx;

  alignas(65536) static std::array<unsigned char,131072> memory{};

  auto stamp=memory_range_stamp(memory.data()+1024,128);
  const auto initialPageRevision=stamp.pageRevision;

  // Same coarse 64 KiB page, but outside the tracked geometry range.
  note_memory_write(memory.data()+4096,64);
  check(memory_range_revision(memory.data()+1024,128)!=initialPageRevision);
  check(!memory_range_changed(memory.data()+1024,128,stamp));

  // An adjacent write must not overlap either.
  note_memory_write(memory.data()+896,128);
  check(!memory_range_changed(memory.data()+1024,128,stamp));

  // Touching even one byte of the tracked interval invalidates it.
  note_memory_write(memory.data()+1100,1);
  check(memory_range_changed(memory.data()+1024,128,stamp));

  // A fresh stamp survives writes in another 64 KiB page.
  auto second=memory_range_stamp(memory.data()+2048,256);
  note_memory_write(memory.data()+65536+2048,256);
  check(!memory_range_changed(memory.data()+2048,256,second));

  // A write spanning into the range is detected.
  note_memory_write(memory.data()+2000,64);
  check(memory_range_changed(memory.data()+2048,256,second));

  std::printf("PASS memory revision: %u exact-overlap checks\n",checks);
  return 0;
}
