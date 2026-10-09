#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace aurora::gx::fifo {
inline bool native_model_cache_enabled() noexcept {
  static const bool enabled=[] {const char* p=std::getenv("STRIKERS_GXM_NATIVE_MODEL_CACHE");
    return p&&std::strcmp(p,"1")==0;}();
  return enabled;
}
inline constexpr size_t NativeModelCacheCapacity=256;
inline constexpr size_t NativeModelCacheWays=4;
inline uint32_t native_model_live_limit() noexcept {return native_model_cache_enabled()?384u:96u;}

// CPU metadata only. A key chooses a set; the caller still checks the entire
// semantic identity. Multiple material variants may deliberately share a key.
// Entries own neither GPU buffers nor pointers into queued uniform snapshots.
template<class T> class NativeModelCache {
  struct Slot {uint64_t key=0,age=0;T value{};};
  std::vector<Slot> slots_;
  size_t ways_=0,size_=0;
  uint64_t clock_=0;
  size_t start(uint64_t key) const noexcept {
    key^=key>>33;key*=0xff51afd7ed558ccdULL;key^=key>>33;
    return size_t(key%(slots_.size()/ways_))*ways_;
  }
public:
  void initialize(size_t capacity=NativeModelCacheCapacity,size_t ways=NativeModelCacheWays) {
    if(!slots_.empty())return;
    if(!capacity||!ways||capacity%ways)return;
    slots_.resize(capacity);ways_=ways;
  }
  template<class Match> T* find(uint64_t key,Match match) noexcept {
    if(slots_.empty())return nullptr;
    const size_t first=start(key);
    for(size_t i=first;i<first+ways_;++i){auto& s=slots_[i];
      if(s.age&&s.key==key&&match(s.value)){s.age=++clock_;return &s.value;}}
    return nullptr;
  }
  T* find(uint64_t key) noexcept {return find(key,[](const T&){return true;});}
  T& allocate(uint64_t key,bool& replaced) noexcept {
    const size_t first=start(key);size_t victim=first;
    for(size_t i=first;i<first+ways_;++i){
      if(!slots_[i].age){victim=i;break;}
      if(slots_[i].age<slots_[victim].age)victim=i;
    }
    auto& s=slots_[victim];replaced=s.age!=0;
    if(!replaced)++size_;
    s.value=T{};s.key=key;s.age=++clock_;return s.value;
  }
  void clear() noexcept {for(auto& s:slots_)s=Slot{};size_=0;clock_=0;}
  void release() noexcept {clear();std::vector<Slot>{}.swap(slots_);ways_=0;}
  size_t size() const noexcept {return size_;}
  size_t storage_bytes() const noexcept {return slots_.capacity()*sizeof(Slot);}
};

enum class ModelCacheEvent : uint8_t {
  ProducerLookup,ProducerHit,ProducerMiss,ProducerInsert,ProducerReplace,
  CaptureAttempt,CaptureContextReject,CaptureSourceReject,CaptureBusyReject,CaptureLimitReject,
  CaptureAborted,TransportAttempt,TransportContractReject,TransportPinReject,TransportQueueReject,
  ConsumerAttempt,ConsumerHit,ConsumerDisabled,ConsumerMissing,ConsumerPin,ConsumerRuntime,
  ConsumerPipeline,ConsumerGuard,ConsumerVertexProgram,ConsumerGeometry,ConsumerLayout,ConsumerBuffers,
  ConsumerTextures,ConsumerTextureModes,ConsumerInsert,ConsumerReplace,ConsumerClear,Count
};
inline constexpr std::array<const char*,size_t(ModelCacheEvent::Count)> ModelCacheEventNames{
  "producer_lookup","producer_hit","producer_miss","producer_insert","producer_replace",
  "capture_attempt","capture_context_reject","capture_source_reject","capture_busy_reject","capture_limit_reject",
  "capture_aborted","transport_attempt","transport_contract_reject","transport_pin_reject","transport_queue_reject",
  "consumer_attempt","consumer_hit","consumer_disabled","consumer_missing","consumer_pin","consumer_runtime",
  "consumer_pipeline","consumer_guard","consumer_vertex_program","consumer_geometry","consumer_layout","consumer_buffers",
  "consumer_textures","consumer_texture_modes","consumer_insert","consumer_replace","consumer_clear"};
enum class ModelCachePeak : uint8_t {
  ProducerSlots,ConsumerSlots,ProducerMetadataBytes,ConsumerMetadataBytes,LiveRecipes,
  RecipePayloadBytes,PendingSegments,PendingPinnedBytes,QueueBatches,Count
};
inline constexpr std::array<const char*,size_t(ModelCachePeak::Count)> ModelCachePeakNames{
  "producer_slots","consumer_slots","producer_metadata_bytes","consumer_metadata_bytes","live_recipes",
  "recipe_payload_bytes","pending_segments","pending_pinned_bytes","queue_batches"};
struct ModelCacheSnapshot {
  std::array<uint64_t,size_t(ModelCacheEvent::Count)> events{};
  std::array<uint64_t,size_t(ModelCachePeak::Count)> peaks{};
};
inline bool native_model_cache_diagnostics() noexcept {
  static const bool enabled=[] {const char* p=std::getenv("STRIKERS_GXM_NATIVE_MODEL_CENSUS");
    return p&&std::strcmp(p,"1")==0;}();
  return enabled;
}
class ModelCacheDiagnostics {
  std::mutex mutex_;
  ModelCacheSnapshot state_{};
public:
  void event(ModelCacheEvent event) noexcept {std::lock_guard<std::mutex> lock(mutex_);++state_.events[size_t(event)];}
  void peak(ModelCachePeak peak,uint64_t value) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);auto& p=state_.peaks[size_t(peak)];p=std::max(p,value);
  }
  ModelCacheSnapshot snapshot() noexcept {std::lock_guard<std::mutex> lock(mutex_);return state_;}
};
inline ModelCacheDiagnostics& native_model_cache_diagnostic_state(){static ModelCacheDiagnostics state;return state;}
inline void native_model_cache_event(ModelCacheEvent event) noexcept {
  if(native_model_cache_diagnostics())native_model_cache_diagnostic_state().event(event);
}
inline void native_model_cache_peak(ModelCachePeak peak,uint64_t value) noexcept {
  if(native_model_cache_diagnostics())native_model_cache_diagnostic_state().peak(peak,value);
}
inline ModelCacheSnapshot native_model_cache_snapshot() noexcept {
  return native_model_cache_diagnostics()?native_model_cache_diagnostic_state().snapshot():ModelCacheSnapshot{};
}
} // namespace aurora::gx::fifo
