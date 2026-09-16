#pragma once
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <cstddef>
#include <cstdint>

namespace aurora::vita::gxm {
enum class MemoryKind { CpuGpu, ColorSurface, VertexUsse, FragmentUsse };

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
private:
  SceUID uid_ = -1;
  void* data_ = nullptr;
  size_t bytes_ = 0;
  unsigned usseOffset_ = 0;
  MemoryKind kind_ = MemoryKind::CpuGpu;
  bool mapped_ = false;
};
} // namespace aurora::vita::gxm
