#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "native_model_census.hpp"
#include "native_model_recipe.hpp"
#include "native_model_sources.hpp"

namespace aurora::gx::fifo {

// App-independent GX display-list/material identity. The app only supplies
// immutable metadata; matrices, skin palettes, lighting, scissor and live
// texture bindings are reconstructed from the current GX state on the owner
// thread. A matching identity is NOT permission to reuse GPU buffers.
struct NativeModelStreamId {
  uintptr_t address = 0;
  uint32_t dataBytes = 0;
  uint8_t semantic = 0;
  uint8_t stride = 0;
  bool bigEndian = false;

  bool operator==(const NativeModelStreamId& rhs) const noexcept {
    return address == rhs.address && dataBytes == rhs.dataBytes &&
           semantic == rhs.semantic && stride == rhs.stride &&
           bigEndian == rhs.bigEndian;
  }
};

struct NativeModelDescriptor {
  uintptr_t displayList = 0;
  uint32_t displayListBytes = 0;
  uint32_t view = 0, program = 0, vertexCount = 0;
  uint32_t materialSet = 0, materialState = 0, rasterState = 0;
  uint64_t textureState = 0;
  // Optional loader-provided generations. Until supplied, the FIFO pin and
  // GXM resident-geometry verifier still validate actual source revisions.
  uint64_t sourceGeneration = 0, materialGeneration = 0;
  uint32_t userStateKey = 0;
  uint8_t primitive = 0, streamCount = 0, texConfig = 0;
  std::array<uintptr_t, 6> textures{};
  std::array<NativeModelStreamId, 8> streams{};

  bool valid() const noexcept {
    if (!displayList || displayListBytes < 3 ||
        displayListBytes > 256u * 1024u || !streamCount ||
        streamCount > streams.size()) return false;
    for (unsigned i = 0; i < streamCount; ++i) {
      if (!streams[i].address || !streams[i].stride ||
          !streams[i].dataBytes) return false;
    }
    return true;
  }

  bool operator==(const NativeModelDescriptor& rhs) const noexcept {
    if (displayList != rhs.displayList ||
        displayListBytes != rhs.displayListBytes || view != rhs.view ||
        program != rhs.program || vertexCount != rhs.vertexCount ||
        materialSet != rhs.materialSet ||
        materialState != rhs.materialState ||
        rasterState != rhs.rasterState ||
        textureState != rhs.textureState ||
        sourceGeneration != rhs.sourceGeneration ||
        materialGeneration != rhs.materialGeneration ||
        userStateKey != rhs.userStateKey ||
        primitive != rhs.primitive || streamCount != rhs.streamCount ||
        texConfig != rhs.texConfig || textures != rhs.textures)
      return false;
    for (unsigned i = 0; i < streamCount; ++i)
      if (!(streams[i] == rhs.streams[i])) return false;
    return true;
  }

  uint64_t cache_key() const noexcept {
    // Deliberately allow same-source variants to collide. The lookup
    // verifies *every* descriptor field, not just the hash or address.
    return (uint64_t(displayList) << 6) ^ uint64_t(view);
  }
};

// The static gate is now part of Aurora, rather than an extra Strikers
// predicate which can silently diverge from the census classifier.
inline bool native_model_static_admissible(const ModelAdmissionFacts& facts)
    noexcept {
  return (model_admission_rejections(facts) & ModelPacketRejectMask) == 0;
}

struct NativeModelRuntimeFacts {
  bool packet = false, compiled = false, displayList = false;
  bool supportedView = false, supportedProgram = false;
  uint32_t vertices = 0, primitive = 0, streams = 0, listBytes = 0;
};

// Expanded indexed/lit material mode is still experimental. Admission only
// lets the owner thread *attempt* its independently guarded fast path.
inline bool native_model_runtime_admissible(
    const NativeModelRuntimeFacts& facts) noexcept {
  return facts.packet && facts.compiled && facts.displayList &&
         facts.supportedView && facts.supportedProgram &&
         facts.vertices >= 48 && facts.primitive <= 3 &&
         facts.streams >= 1 && facts.streams <= 8 &&
         facts.listBytes >= 3 && facts.listBytes <= 256u * 1024u;
}

// R3-A early producer bypass is deliberately narrower than the owner-thread
// native material guard. It may omit GLX only when the callback would not
// switch any state, and a permanent source has already been captured.
struct NativeModelEarlyFacts {
  uint32_t callbackFlags = 0;
  bool compiledDraw = false;
  bool stableView = false;
  bool staticUnlitProgram = false;
  bool identityMatrix = false;
  bool matchingLiveProgram = false;
  bool matchingLiveStreams = false;
  bool noUserData = false;
  bool producerClean = false;
  bool noDrawModifiers = false;
};

inline bool native_model_early_admissible(const NativeModelEarlyFacts& facts) noexcept {
  return facts.callbackFlags == 0x800u && facts.compiledDraw &&
         facts.stableView && facts.staticUnlitProgram && facts.identityMatrix &&
         facts.matchingLiveProgram && facts.matchingLiveStreams &&
         facts.noUserData && facts.producerClean && facts.noDrawModifiers;
}

// Producer-owned only. The FIFO owns an independent shared_ptr on dispatch;
// cache replacement never retires an in-flight recipe or a consumer GPU
// resource. Existing mode 0 continues to avoid lookup and allocations.
class NativeModelRegistry {
  struct Entry {
    NativeModelDescriptor descriptor{};
    NativeModelRecipeRef recipe{};
  };

  std::array<Entry, 64> ring_{};
  NativeModelCache<Entry> associative_{};
  size_t ringNext_ = 0, ringSize_ = 0;
  NativeModelSources<> sources_{};

 public:
  uint64_t register_source(uintptr_t displayList, const void* data,
                           uint32_t bytes, uint32_t ownerLevel) noexcept {
    // A reused allocation is a new asset even if its bytes happen to match.
    invalidate_source(displayList);
    const auto generation = sources_.register_source(displayList, data, bytes, ownerLevel);
    if (generation) native_model_cache_peak(ModelCachePeak::ProducerMetadataBytes, metadata_bytes());
    return generation;
  }

  uint64_t source_generation(uintptr_t displayList) const noexcept {
    const auto* source = sources_.find(displayList);
    return source ? source->generation : 0;
  }

  template<class Retire>
  size_t release_sources_after(uint32_t ownerLevel, Retire&& retire) noexcept {
    return sources_.release_after(ownerLevel, [&](const NativeModelSourceSpan& source) {
      invalidate_source(source.displayList);
      retire(source);
    });
  }

  // Borrowed reference valid only until the next mutation on the producer
  // thread. The FIFO copies its shared_ptr into an owned queued segment.
  const NativeModelRecipeRef* lookup(const NativeModelDescriptor& descriptor)
      noexcept {
    if (!descriptor.valid()) return nullptr;
    native_model_cache_event(ModelCacheEvent::ProducerLookup);
    const auto same = [&](const Entry& entry) {
      return entry.recipe && entry.descriptor == descriptor &&
             entry.recipe->bytes == descriptor.displayListBytes;
    };
    const Entry* matched = nullptr;
    if (native_model_cache_enabled()) {
      associative_.initialize();
      matched = associative_.find(descriptor.cache_key(), same);
    } else {
      for (const auto& entry : ring_)
        if (same(entry)) { matched = &entry; break; }
    }
    native_model_cache_event(matched ? ModelCacheEvent::ProducerHit
                                      : ModelCacheEvent::ProducerMiss);
    return matched ? &matched->recipe : nullptr;
  }

  // A hit may skip the producer's redundant GLX callback. The flush callback
  // emits any pending GX state *before* queueing the native segment. A miss
  // never touches GX; the caller must run the unchanged reference callback.
  template<class Flush>
  bool dispatch_prepared(const NativeModelEarlyFacts& facts,
                         const NativeModelDescriptor& descriptor,
                         Flush&& flush) noexcept {
    native_model_cache_event(ModelCacheEvent::ProducerEarlyAttempt);
    if(!native_model_early_admissible(facts) || !descriptor.valid() ||
       !descriptor.sourceGeneration) {
      native_model_cache_event(ModelCacheEvent::ProducerEarlyRejected);
      return false;
    }
    const auto* existing = lookup(descriptor);
    if(!existing) {
      native_model_cache_event(ModelCacheEvent::ProducerEarlyRejected);
      return false;
    }
    flush();
    if(!write_native_model_recipe(*existing)) {
      native_model_cache_event(ModelCacheEvent::ProducerEarlyQueueReject);
      return false;
    }
    native_model_cache_event(ModelCacheEvent::ProducerEarlyAdmitted);
    return true;
  }

  bool store(const NativeModelDescriptor& descriptor,
             NativeModelRecipeRef recipe) noexcept {
    if (!descriptor.valid() || !recipe || !recipe->identity ||
        recipe->bytes != descriptor.displayListBytes ||
        !recipe->source) return false;
    bool replaced = false;
    size_t slots = 0;
    Entry* destination = nullptr;
    if (native_model_cache_enabled()) {
      associative_.initialize();
      destination = &associative_.allocate(descriptor.cache_key(), replaced);
      slots = associative_.size();
    } else {
      destination = &ring_[ringNext_++ % ring_.size()];
      replaced = bool(destination->recipe);
      if (!replaced) ++ringSize_;
      slots = ringSize_;
    }
    destination->descriptor = descriptor;
    destination->recipe = std::move(recipe);
    native_model_cache_event(ModelCacheEvent::ProducerInsert);
    if (replaced) native_model_cache_event(ModelCacheEvent::ProducerReplace);
    native_model_cache_peak(ModelCachePeak::ProducerSlots, slots);
    native_model_cache_peak(ModelCachePeak::ProducerMetadataBytes, metadata_bytes());
    return true;
  }

  // Model unload/reload invalidates only that source. Outstanding FIFO jobs
  // still own their immutable recipe, and the consumer checks GPU residency.
  size_t invalidate_source(uintptr_t displayList) noexcept {
    if (!displayList) return 0;
    if (native_model_cache_enabled())
      return associative_.erase_if([&](const Entry& entry) {
        return entry.descriptor.displayList == displayList;
      });
    size_t removed = 0;
    for (auto& entry : ring_) if (entry.recipe &&
        entry.descriptor.displayList == displayList) {
      entry = {};
      --ringSize_;
      ++removed;
    }
    return removed;
  }

  // The producer still flushes its live GX state before reaching this slot.
  // A miss records the *original* GX list; a hit is a typed FIFO recipe and
  // a rejected consumer hit falls back to the same pinned GX bytes in order.
  // Returning false means the caller must issue its original GX draw.
  template<class EmitOriginal>
  bool dispatch_or_capture(const NativeModelDescriptor& descriptor,
                           const void* displayListData, uint32_t bytes,
                           EmitOriginal&& emitOriginal,
                           bool recordStages = false) noexcept {
    if (!descriptor.valid() ||
        descriptor.displayListBytes != bytes || !displayListData)
      return false;
    const auto* existing = lookup(descriptor);
    if (existing) {
      const bool queued = write_native_model_recipe(*existing);
      if (recordStages) record_native_model_stage(
          queued ? ModelStage::QueueAccepted : ModelStage::QueueRejected);
      return queued;
    }
    if (recordStages) record_native_model_stage(ModelStage::BeginAttempt);
    if (!begin_native_model_recording(displayListData, bytes)) {
      if (recordStages) record_native_model_stage(ModelStage::BeginRejected);
      return false;
    }
    emitOriginal(displayListData, bytes);
    auto recipe = finish_native_model_recording(nullptr);
    if (recordStages) record_native_model_stage(
        recipe ? ModelStage::CaptureSealed : ModelStage::CaptureAborted);
    if (recipe && recipe->before.empty() && recipe->after.empty())
      store(descriptor, std::move(recipe));
    // A failed capture already restores the original GX bytes in FIFO.
    return true;
  }

  void clear() noexcept {
    ring_ = {};
    ringNext_ = 0;
    ringSize_ = 0;
    associative_.clear();
    sources_.clear();
  }

  void release() noexcept {
    clear();
    associative_.release();
  }

  size_t size() const noexcept {
    if (native_model_cache_enabled()) return associative_.size();
    return ringSize_;
  }
  size_t metadata_bytes() const noexcept {
    return sizeof(ring_) + associative_.storage_bytes() + sizeof(sources_);
  }
};

inline NativeModelRegistry& native_model_registry() noexcept {
  static NativeModelRegistry registry;
  return registry;
}

} // namespace aurora::gx::fifo
