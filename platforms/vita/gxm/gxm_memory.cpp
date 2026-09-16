#include "gxm_memory.hpp"
#include <limits>
#include <utility>

namespace aurora::vita::gxm {
MemoryBlock::MemoryBlock(MemoryBlock&& other) noexcept { *this = std::move(other); }
MemoryBlock& MemoryBlock::operator=(MemoryBlock&& other) noexcept {
  if (this == &other) return *this;
  reset();
  uid_ = std::exchange(other.uid_, -1);
  data_ = std::exchange(other.data_, nullptr);
  bytes_ = std::exchange(other.bytes_, 0);
  usseOffset_ = std::exchange(other.usseOffset_, 0);
  kind_ = other.kind_;
  mapped_ = std::exchange(other.mapped_, false);
  return *this;
}
int MemoryBlock::allocate(size_t bytes, MemoryKind kind) noexcept {
  if (data_ || !bytes) return SCE_GXM_ERROR_INVALID_VALUE;
  const size_t alignment = kind == MemoryKind::ColorSurface ? 256u * 1024u : 4096u;
  if (bytes > std::numeric_limits<uint32_t>::max() - (alignment - 1))
    return SCE_GXM_ERROR_INVALID_VALUE;
  bytes_ = (bytes + alignment - 1) & ~(alignment - 1);
  kind_ = kind;
  const auto type = kind == MemoryKind::ColorSurface
      ? SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW : SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;
  uid_ = sceKernelAllocMemBlock("aurora-gxm", type, bytes_, nullptr);
  if (uid_ < 0) { const int error = uid_; reset(); return error; }
  int error = sceKernelGetMemBlockBase(uid_, &data_);
  if (error < 0) { reset(); return error; }
  if (kind == MemoryKind::VertexUsse)
    error = sceGxmMapVertexUsseMemory(data_, bytes_, &usseOffset_);
  else if (kind == MemoryKind::FragmentUsse)
    error = sceGxmMapFragmentUsseMemory(data_, bytes_, &usseOffset_);
  else
    error = sceGxmMapMemory(data_, bytes_, SCE_GXM_MEMORY_ATTRIB_RW);
  if (error < 0) { reset(); return error; }
  mapped_ = true;
  return 0;
}
void MemoryBlock::reset() noexcept {
  if (mapped_) {
    if (kind_ == MemoryKind::VertexUsse) sceGxmUnmapVertexUsseMemory(data_);
    else if (kind_ == MemoryKind::FragmentUsse) sceGxmUnmapFragmentUsseMemory(data_);
    else sceGxmUnmapMemory(data_);
  }
  if (uid_ >= 0) sceKernelFreeMemBlock(uid_);
  uid_ = -1;
  data_ = nullptr;
  bytes_ = 0;
  usseOffset_ = 0;
  mapped_ = false;
}
} // namespace aurora::vita::gxm
