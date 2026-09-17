#include "gxm_memory.hpp"
#include <psp2/kernel/clib.h>
#include <algorithm>
#include <limits>
#include <utility>

namespace aurora::vita::gxm {
namespace {
struct Candidate {
  SceKernelMemBlockType type;
  size_t alignment;
  MemoryPool pool;
};

MemoryStats g_stats{};

struct CdramPool {
  SceUID uid = -1;
  void* base = nullptr;
  size_t bytes = 0;
  SceClibMspace mspace = nullptr;
  bool mapped = false;
} g_cdramPool;

size_t* current_for(MemoryPool pool) noexcept {
  switch (pool) {
  case MemoryPool::Cdram: return &g_stats.cdramCurrent;
  case MemoryPool::User: return &g_stats.userCurrent;
  case MemoryPool::Phycont: return &g_stats.phycontCurrent;
  case MemoryPool::Cdialog: return &g_stats.cdialogCurrent;
  default: return nullptr;
  }
}

size_t* peak_for(MemoryPool pool) noexcept {
  switch (pool) {
  case MemoryPool::Cdram: return &g_stats.cdramPeak;
  case MemoryPool::User: return &g_stats.userPeak;
  case MemoryPool::Phycont: return &g_stats.phycontPeak;
  case MemoryPool::Cdialog: return &g_stats.cdialogPeak;
  default: return nullptr;
  }
}

uint32_t* allocations_for(MemoryPool pool) noexcept {
  switch (pool) {
  case MemoryPool::Cdram: return &g_stats.cdramAllocations;
  case MemoryPool::User: return &g_stats.userAllocations;
  case MemoryPool::Phycont: return &g_stats.phycontAllocations;
  case MemoryPool::Cdialog: return &g_stats.cdialogAllocations;
  default: return nullptr;
  }
}

void account_add(MemoryPool pool, size_t bytes, bool fallback) noexcept {
  auto* current = current_for(pool);
  auto* peak = peak_for(pool);
  auto* allocations = allocations_for(pool);
  if (!current || !peak || !allocations) return;
  *current += bytes;
  *peak = std::max(*peak, *current);
  ++*allocations;
  if (fallback) ++g_stats.fallbackAllocations;
}

void account_remove(MemoryPool pool, size_t bytes) noexcept {
  auto* current = current_for(pool);
  if (!current) return;
  *current = bytes <= *current ? *current - bytes : 0;
}

void account_pool_add(size_t bytes) noexcept {
  g_stats.cdramPoolUsed += bytes;
  g_stats.cdramPoolPeak = std::max(g_stats.cdramPoolPeak, g_stats.cdramPoolUsed);
}

void account_pool_remove(size_t bytes) noexcept {
  g_stats.cdramPoolUsed = bytes <= g_stats.cdramPoolUsed ?
      g_stats.cdramPoolUsed - bytes : 0;
}

constexpr Candidate kGpuPreferred[] = {
    {SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256u * 1024u, MemoryPool::Cdram},
    {SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4u * 1024u, MemoryPool::User},
    {SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, 1024u * 1024u, MemoryPool::Phycont},
    {SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_NC_RW, 4u * 1024u, MemoryPool::Cdialog},
};

constexpr Candidate kCpuPreferred[] = {
    {SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4u * 1024u, MemoryPool::User},
    {SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, 1024u * 1024u, MemoryPool::Phycont},
    {SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_NC_RW, 4u * 1024u, MemoryPool::Cdialog},
    {SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256u * 1024u, MemoryPool::Cdram},
};

constexpr Candidate kGpuFallback[] = {
    {SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4u * 1024u, MemoryPool::User},
    {SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, 1024u * 1024u, MemoryPool::Phycont},
    {SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_NC_RW, 4u * 1024u, MemoryPool::Cdialog},
};
} // namespace

int initialize_cdram_pool(size_t preferredBytes, size_t reserveBytes) noexcept {
  if (g_cdramPool.mspace) return 0;
  SceKernelFreeMemorySizeInfo freeMemory{};
  freeMemory.size = sizeof(freeMemory);
  int error = sceKernelGetFreeMemorySize(&freeMemory);
  if (error < 0) return error;
  const size_t freeBytes = freeMemory.size_cdram > 0 ? size_t(freeMemory.size_cdram) : 0;
  if (freeBytes <= reserveBytes + 256u * 1024u) return SCE_GXM_ERROR_OUT_OF_MEMORY;
  size_t bytes = std::min(preferredBytes, freeBytes - reserveBytes);
  bytes &= ~size_t(256u * 1024u - 1u);
  if (bytes < 4u * 1024u * 1024u) return SCE_GXM_ERROR_OUT_OF_MEMORY;
  SceUID uid = sceKernelAllocMemBlock("aurora-gxm-cdram-pool",
      SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, bytes, nullptr);
  if (uid < 0) return uid;
  void* base = nullptr;
  error = sceKernelGetMemBlockBase(uid, &base);
  if (error < 0) { sceKernelFreeMemBlock(uid); return error; }
  error = sceGxmMapMemory(base, bytes, SCE_GXM_MEMORY_ATTRIB_RW);
  if (error < 0) { sceKernelFreeMemBlock(uid); return error; }
  SceClibMspace mspace = sceClibMspaceCreate(base, bytes);
  if (!mspace) {
    sceGxmUnmapMemory(base);
    sceKernelFreeMemBlock(uid);
    return SCE_GXM_ERROR_OUT_OF_MEMORY;
  }
  g_cdramPool.uid = uid;
  g_cdramPool.base = base;
  g_cdramPool.bytes = bytes;
  g_cdramPool.mspace = mspace;
  g_cdramPool.mapped = true;
  g_stats.cdramPoolBytes = bytes;
  account_add(MemoryPool::Cdram, bytes, false);
  return 0;
}

void shutdown_cdram_pool() noexcept {
  if (!g_cdramPool.mspace) return;
  sceClibMspaceDestroy(g_cdramPool.mspace);
  if (g_cdramPool.mapped) sceGxmUnmapMemory(g_cdramPool.base);
  if (g_cdramPool.uid >= 0) sceKernelFreeMemBlock(g_cdramPool.uid);
  account_remove(MemoryPool::Cdram, g_cdramPool.bytes);
  g_stats.cdramPoolBytes = 0;
  g_stats.cdramPoolUsed = 0;
  g_cdramPool = {};
}

MemoryStats memory_stats() noexcept { return g_stats; }

const char* memory_pool_name(MemoryPool pool) noexcept {
  switch (pool) {
  case MemoryPool::Cdram: return "cdram";
  case MemoryPool::User: return "user";
  case MemoryPool::Phycont: return "phycont";
  case MemoryPool::Cdialog: return "cdialog";
  default: return "none";
  }
}

MemoryBlock::MemoryBlock(MemoryBlock&& other) noexcept { *this = std::move(other); }
MemoryBlock& MemoryBlock::operator=(MemoryBlock&& other) noexcept {
  if (this == &other) return *this;
  reset();
  uid_ = std::exchange(other.uid_, -1);
  data_ = std::exchange(other.data_, nullptr);
  bytes_ = std::exchange(other.bytes_, 0);
  usseOffset_ = std::exchange(other.usseOffset_, 0);
  kind_ = other.kind_;
  pool_ = std::exchange(other.pool_, MemoryPool::None);
  pooled_ = std::exchange(other.pooled_, false);
  mapped_ = std::exchange(other.mapped_, false);
  return *this;
}
int MemoryBlock::allocate(size_t bytes, MemoryKind kind) noexcept {
  if (data_ || !bytes) return SCE_GXM_ERROR_INVALID_VALUE;
  kind_ = kind;
  const bool usse = kind == MemoryKind::VertexUsse || kind == MemoryKind::FragmentUsse;
  if (kind == MemoryKind::GpuResource && g_cdramPool.mspace) {
    constexpr size_t alignment = 4096u;
    if (bytes <= std::numeric_limits<uint32_t>::max() - (alignment - 1)) {
      const size_t rounded = (bytes + alignment - 1) & ~(alignment - 1);
      if (void* base = sceClibMspaceMemalign(g_cdramPool.mspace, alignment, rounded)) {
        data_ = base;
        bytes_ = rounded;
        pool_ = MemoryPool::Cdram;
        pooled_ = true;
        account_pool_add(bytes_);
        return 0;
      }
    }
  }
  const Candidate usseCandidate{SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4096u, MemoryPool::User};
  const bool pooledGpu = kind == MemoryKind::GpuResource && g_cdramPool.mspace;
  const Candidate* candidates = usse ? &usseCandidate :
      (pooledGpu ? kGpuFallback :
       ((kind == MemoryKind::GpuResource || kind == MemoryKind::ColorSurface) ?
          kGpuPreferred : kCpuPreferred));
  const size_t candidateCount = usse ? 1u : (pooledGpu ? 3u : 4u);
  int lastError = SCE_GXM_ERROR_OUT_OF_MEMORY;
  for (size_t i = 0; i < candidateCount; ++i) {
    const Candidate& candidate = candidates[i];
    if (bytes > std::numeric_limits<uint32_t>::max() - (candidate.alignment - 1))
      return SCE_GXM_ERROR_INVALID_VALUE;
    const size_t rounded = (bytes + candidate.alignment - 1) & ~(candidate.alignment - 1);
    SceUID uid = sceKernelAllocMemBlock("aurora-gxm", candidate.type, rounded, nullptr);
    if (uid < 0) { lastError = uid; continue; }
    void* base = nullptr;
    int error = sceKernelGetMemBlockBase(uid, &base);
    if (error < 0) {
      sceKernelFreeMemBlock(uid);
      lastError = error;
      continue;
    }
    unsigned usseOffset = 0;
    if (kind == MemoryKind::VertexUsse)
      error = sceGxmMapVertexUsseMemory(base, rounded, &usseOffset);
    else if (kind == MemoryKind::FragmentUsse)
      error = sceGxmMapFragmentUsseMemory(base, rounded, &usseOffset);
    else
      error = sceGxmMapMemory(base, rounded, SCE_GXM_MEMORY_ATTRIB_RW);
    if (error < 0) {
      sceKernelFreeMemBlock(uid);
      lastError = error;
      continue;
    }
    uid_ = uid;
    data_ = base;
    bytes_ = rounded;
    usseOffset_ = usseOffset;
    pool_ = candidate.pool;
    mapped_ = true;
    account_add(pool_, bytes_, pooledGpu || i != 0);
    return 0;
  }
  return lastError;
}
void MemoryBlock::reset() noexcept {
  if (pooled_) {
    if (data_ && g_cdramPool.mspace) sceClibMspaceFree(g_cdramPool.mspace, data_);
    if (bytes_) account_pool_remove(bytes_);
    uid_ = -1;
    data_ = nullptr;
    bytes_ = 0;
    usseOffset_ = 0;
    pool_ = MemoryPool::None;
    pooled_ = false;
    mapped_ = false;
    return;
  }
  if (mapped_) {
    if (kind_ == MemoryKind::VertexUsse) sceGxmUnmapVertexUsseMemory(data_);
    else if (kind_ == MemoryKind::FragmentUsse) sceGxmUnmapFragmentUsseMemory(data_);
    else sceGxmUnmapMemory(data_);
  }
  if (bytes_ && pool_ != MemoryPool::None) account_remove(pool_, bytes_);
  if (uid_ >= 0) sceKernelFreeMemBlock(uid_);
  uid_ = -1;
  data_ = nullptr;
  bytes_ = 0;
  usseOffset_ = 0;
  pool_ = MemoryPool::None;
  pooled_ = false;
  mapped_ = false;
}
} // namespace aurora::vita::gxm
