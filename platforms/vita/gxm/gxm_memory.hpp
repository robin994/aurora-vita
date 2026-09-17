#pragma once
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <cstddef>
#include <cstdint>

namespace aurora::vita::gxm {
enum class MemoryKind {
  // CPU-written resources: prefer normal uncached RAM and only spill into
  // GPU-local pools when the ordinary GPU-mapped heap is exhausted.
  CpuGpu,
  // GPU-heavy resources: textures, depth and offscreen render targets.
  // Match vitaGL's GPU-oriented policy and consume CDRAM first.
  GpuResource,
  // Display surfaces are GPU-heavy too, but retain a distinct semantic tag so
  // diagnostics can tell front/back buffers apart from ordinary resources.
  ColorSurface,
  VertexUsse,
  FragmentUsse
};

enum class MemoryPool : uint8_t { None, Cdram, User, Phycont, Cdialog };

struct MemoryStats {
  size_t cdramCurrent = 0, cdramPeak = 0;
  size_t cdramPoolBytes = 0, cdramPoolUsed = 0, cdramPoolPeak = 0;
  size_t userCurrent = 0, userPeak = 0;
  size_t phycontCurrent = 0, phycontPeak = 0;
  size_t cdialogCurrent = 0, cdialogPeak = 0;
  uint32_t cdramAllocations = 0, userAllocations = 0;
  uint32_t phycontAllocations = 0, cdialogAllocations = 0;
  uint32_t fallbackAllocations = 0;
};

// Reserve one GPU-local heap after display surfaces have been created. Small
// textures can then share a CDRAM memblock instead of paying the 256 KiB
// memblock granularity independently.
int initialize_cdram_pool(size_t preferredBytes, size_t reserveBytes) noexcept;
void shutdown_cdram_pool() noexcept;
MemoryStats memory_stats() noexcept;
const char* memory_pool_name(MemoryPool pool) noexcept;

// Move-only ownership. Call reset before terminating GXM, after GPU completion.
class MemoryBlock {
public:
  MemoryBlock() = default;
  ~MemoryBlock() { reset(); }
  MemoryBlock(const MemoryBlock&) = delete;
  MemoryBlock& operator=(const MemoryBlock&) = delete;
  MemoryBlock(MemoryBlock&& other) noexcept;
  MemoryBlock& operator=(MemoryBlock&& other) noexcept;
  int allocate(size_t bytes, MemoryKind kind) noexcept;
  void reset() noexcept;
  void* data() const noexcept { return data_; }
  size_t size() const noexcept { return bytes_; }
  unsigned usse_offset() const noexcept { return usseOffset_; }
  MemoryPool pool() const noexcept { return pool_; }
private:
  SceUID uid_ = -1;
  void* data_ = nullptr;
  size_t bytes_ = 0;
  unsigned usseOffset_ = 0;
  MemoryKind kind_ = MemoryKind::CpuGpu;
  MemoryPool pool_ = MemoryPool::None;
  bool pooled_ = false;
  bool mapped_ = false;
};
} // namespace aurora::vita::gxm
