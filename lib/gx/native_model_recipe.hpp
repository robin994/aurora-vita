#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include "native_model_cache.hpp"

namespace aurora::gx::fifo {
// CPU-owned material commands only. Geometry is pinned independently on each
// submission; no frame uniform pointer or GPU handle crosses the FIFO boundary.
struct NativeModelRecipe {
  static constexpr size_t MaxStateBytes = 8192;
  static uint32_t max_live_recipes() noexcept {return native_model_live_limit();}
  inline static std::atomic<uint32_t> liveRecipes{0};
  NativeModelRecipe(){const auto live=liveRecipes.fetch_add(1,std::memory_order_relaxed)+1;
    native_model_cache_peak(ModelCachePeak::LiveRecipes,live);}
  ~NativeModelRecipe(){liveRecipes.fetch_sub(1,std::memory_order_relaxed);}
  NativeModelRecipe(const NativeModelRecipe&)=delete;
  NativeModelRecipe& operator=(const NativeModelRecipe&)=delete;
  uint64_t identity = 0;
  const uint8_t* source = nullptr;
  uint32_t bytes = 0;
  uint32_t materialBytes = 0;
  std::vector<uint8_t> before, after;
};
using NativeModelRecipeRef = std::shared_ptr<const NativeModelRecipe>;
struct NativeModelStats { uint64_t attempted=0, dispatched=0, fallback=0, compiled=0; };

bool begin_native_model_recording(const void* source,uint32_t bytes) noexcept;
NativeModelRecipeRef finish_native_model_recording(const float* position) noexcept;
void native_model_record_draw_boundary() noexcept;
bool write_native_model_recipe(const NativeModelRecipeRef& recipe,const float* position=nullptr,bool fullState=false) noexcept;
uint64_t active_native_model_identity() noexcept;
bool active_native_model_single_draw(uint8_t primitive,uint8_t fmt,uint16_t stride,const uint8_t* source) noexcept;
NativeModelStats native_model_stats() noexcept;

// A failed capture is replayed before the original writer continues. Bounds
// never truncate a GX command, and compound DLs retain the ordinary FIFO path.
namespace detail {
extern bool sNativeModelRecording;
bool record_native_model_bytes(const void* data,uint32_t bytes) noexcept;
}
} // namespace aurora::gx::fifo
