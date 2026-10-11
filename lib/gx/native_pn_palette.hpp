#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace aurora::gx::fifo {
// Native frame data, not a GX command stream. Position and normal rows retain
// their exact float bits. The normal translation column is never overwritten.
struct NativePnMatrix {
  uint32_t slot = 0;
  std::array<float, 12> position{};
  std::array<float, 9> normal{};
};
inline constexpr size_t NativePnPaletteCapacity = 10;
static_assert(sizeof(NativePnMatrix) == 88);
static_assert(std::is_trivially_copyable_v<NativePnMatrix>);

struct NativePnPaletteStats {
  uint64_t queued = 0, applied = 0, matrices = 0;
};

// Configure before starting the worker. Quiet runs do not update counters.
void set_native_pn_palette_enabled(bool enabled, bool diagnostics) noexcept;
bool can_write_native_pn_palette() noexcept;
// Copies the complete palette into the existing ordered batch. A rejection
// leaves every matrix untouched; the producer can use its ordinary GX loads.
bool write_native_pn_palette(const NativePnMatrix* matrices, size_t count) noexcept;
NativePnPaletteStats native_pn_palette_stats() noexcept;
} // namespace aurora::gx::fifo
