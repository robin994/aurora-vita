#pragma once
#include <cstdint>

namespace aurora::gx {
enum class StateDomain : uint8_t {
  Vertex = 1, Fragment = 2, Textures = 4, Raster = 8, Clear = 16, Layout = 32, All = 63,
};
constexpr StateDomain operator|(StateDomain a, StateDomain b) noexcept {
  return static_cast<StateDomain>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
// Only decoded writes publish revisions. Unknown/legacy writes use All; no
// cache infers content identity from a pointer to mutable guest memory.
struct StateRevisions {
  uint64_t serial = 1, vertex = 1, fragment = 1, textures = 1, raster = 1;
  void mark(StateDomain domains) noexcept {
    ++serial;
    const auto bits = static_cast<unsigned>(domains);
    if (bits & static_cast<unsigned>(StateDomain::Vertex)) vertex = serial;
    if (bits & static_cast<unsigned>(StateDomain::Fragment)) fragment = serial;
    if (bits & static_cast<unsigned>(StateDomain::Textures)) textures = serial;
    if (bits & static_cast<unsigned>(StateDomain::Raster)) raster = serial;
  }
};
} // namespace aurora::gx
