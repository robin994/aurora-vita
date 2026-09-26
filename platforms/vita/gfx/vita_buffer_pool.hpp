#pragma once
#include "vita_gfx_types.hpp"
#include "vita_hash_map.hpp"
#include "vita_native_fwd.hpp"
#include <cstddef>
#include <cstdint>
namespace aurora::vita::gfx {
class BufferPool {
public:
  BufferPool()=default;~BufferPool();
  BufferPool(const BufferPool&)=delete;BufferPool&operator=(const BufferPool&)=delete;
  Handle create_vertex(const void* data,size_t bytes,bool dynamic=false) noexcept;
  Handle create_index(const void* data,size_t bytes,bool dynamic=false) noexcept;
  bool update(Handle h,const void* data,size_t bytes,size_t offset=0) noexcept;
  // Native GXM streaming pages are CpuGpu-visible. This exposes their backing
  // storage only for buffers created as dynamic; other backends return null.
  void* writable(Handle h,size_t bytes,size_t offset=0) noexcept;
  void destroy(Handle h) noexcept;void clear() noexcept;
  // Destroy storage whose last GPU use is known to be retired (the caller
  // waited more frames than the display queue can hold). Never synchronizes.
  void destroy_retired(Handle h) noexcept;
  // Complete submitted reads before the streaming allocator reuses storage.
  void wait_idle() noexcept;
#if defined(__vita__) && !defined(AURORA_VITA_RENDERER_GXM)
  unsigned gl_id(Handle h) const noexcept;
  unsigned gl_target(Handle h) const noexcept;
#endif
private:
  friend class Renderer;
#if defined(AURORA_VITA_RENDERER_GXM)
  gxm::Renderer* native_=nullptr;
#endif
  struct Entry{unsigned id=0;unsigned target=0;size_t bytes=0;bool dynamic=false;};
  FlatHashMap<Handle,Entry> map_;Handle next_=1;
};
} // namespace aurora::vita::gfx
