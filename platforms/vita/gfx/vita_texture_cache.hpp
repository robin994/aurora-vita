#pragma once
#include "vita_gfx_types.hpp"
#include "vita_native_fwd.hpp"
#include "vita_hash_map.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>
namespace aurora::vita::gfx {
class TextureCache {
public:
  explicit TextureCache(size_t budget=24*1024*1024):budget_(budget){}~TextureCache();
  Handle get_or_upload(const TextureDesc& desc,uint64_t frame,FrameStats* stats=nullptr) noexcept;
  void bind(Handle h,unsigned unit,const SamplerDesc& sampler) noexcept;
  void erase(Handle h) noexcept;void clear() noexcept;void trim(uint64_t frame) noexcept;
  size_t invalidate_source_range(uint64_t start,size_t bytes) noexcept;
  size_t bytes()const noexcept{return bytes_;}size_t entries()const noexcept{return byKey_.size();}
  size_t budget() const noexcept{return budget_;}
  size_t high_water_bytes() const noexcept{return highWaterBytes_;}
  uint64_t evictions() const noexcept{return evictions_;}
  // GPU-memory/OOM hardening telemetry.
  uint64_t alloc_fail_total() const noexcept{return allocFailTotal_;}
  uint64_t pre_evictions() const noexcept{return preEvictions_;}
  uint64_t pre_evicted_bytes() const noexcept{return preEvictedBytes_;}
  uint64_t last_requested_bytes() const noexcept{return lastRequestedBytes_;}
private:
  friend class Renderer;
#if defined(AURORA_VITA_RENDERER_GXM)
  gxm::Renderer* native_=nullptr;
#endif
  struct Entry{Handle handle=InvalidHandle;unsigned gl=0;uint64_t key=0,lastUse=0;size_t bytes=0;bool hasMipmaps=false,cacheable=true;uint64_t sourceId=0,paletteSourceId=0;size_t sourceBytes=0,paletteBytes=0;SamplerDesc sampler{};bool samplerValid=false;uint8_t explicitMipCount=0,appliedMipCount=0;};
  // Evict LRU entries (never the entry keyed protectKey) until bytes_+requiredBytes fits
  // under budget_ with headroom. Runs BEFORE any vitaGL allocation.
  void pre_evict(size_t requiredBytes,uint64_t frame,uint64_t protectKey) noexcept;
  NodeHashMap<uint64_t,Entry> byKey_;
  // Node-map references are stable across rehash, so handles resolve
  // directly to Entry with one lookup instead of handle->key->entry. Keep only
  // live handles here: non-cacheable texture handles are monotonic and a vector
  // indexed by them would grow for the lifetime of a long-running game.
  FlatHashMap<Handle,Entry*> byHandle_{};
  Handle next_=1;size_t budget_=0,bytes_=0,highWaterBytes_=0;uint64_t evictions_=0;
  uint64_t allocFailTotal_=0,preEvictions_=0,preEvictedBytes_=0,lastRequestedBytes_=0,retrySuppressTotal_=0;
  // A failed GPU allocation used to be retried by every draw that referenced the
  // same GX texture. Under pressure this can turn one OOM into hundreds of decode /
  // glTexImage attempts in a single frame. Remember only this frame's failures so
  // the texture can be retried normally after older cache entries become evictable.
  uint64_t failedFrame_=~uint64_t{0};
  FlatHashSet<uint64_t> failedKeys_{};
  // Reused by the CMPR -> DXT1 fast path so streaming new textures does not
  // allocate and free a temporary buffer for every cache miss.
  std::vector<uint8_t> nativeCompressedScratch_{};
  // Reused tiled-GX -> linear native-format staging. I/I+A/RGB565 stay at
  // 1/2 bytes per texel instead of expanding to a 4-byte RGBA upload.
  std::vector<uint8_t> nativeLinearScratch_{};
  // Same principle for all decoded GX formats. Keeping the largest RGBA upload
  // buffer alive removes allocator churn when games stream many texture mips.
  std::vector<uint8_t> rgbaDecodeScratch_{};
};
} // namespace aurora::vita::gfx
