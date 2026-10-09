#include "fifo.hpp"
#include "native_model_census.hpp"
#include "fifo.hpp"
#include "command_processor.hpp"
#include "bp_write_cache.hpp"
#include "prepared_display_list.hpp"
#include "gx.hpp"
#if defined(MKW_TARGET_VITA)
#include "../../platforms/vita/gfx/vita_telemetry.hpp"
#include "../../platforms/vita/gfx/vita_memory_revision.hpp"
#include "../../platforms/vita/vita_thread_utils.hpp"
#include "display_list_shadow.hpp"
#include "../../platforms/vita/gfx/vita_view_draw_capture.hpp"
#include "../../platforms/vita/aurora_vita_backend.hpp"
#include "../../platforms/vita/gx/aurora_vita_draw_sink.hpp"
#endif
#include "../internal.hpp"

#include <chrono>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(MKW_TARGET_VITA)
#include <psp2/kernel/threadmgr.h>
#define ZoneScopedN(name) ((void)0)
#define TracyPlot(name, value) ((void)0)
#else
#include "tracy/Tracy.hpp"
#endif

namespace aurora::gx::fifo {
static Module Log("aurora::gx::fifo");

namespace {
BpWriteCache sBpWriteCache;
} // namespace

void set_bp_write_cache_enabled(bool enabled) {
  sBpWriteCache.invalidate();
  detail::sBpWriteCacheEnabled = enabled;
}

void invalidate_bp_write_cache() {
  sBpWriteCache.invalidate();
}

bool bp_write_unchanged(uint32_t value) {
#if defined(MKW_TARGET_VITA)
  // Keep the cache solely on the producer with synchronous GX consumption.
  // The asynchronous worker retains the existing complete command stream.
  if (worker_running()) return false;
#endif
  return !sBpWriteCache.should_emit(value);
}

namespace detail {
uint8_t* sBufferData = nullptr;
uint32_t sBufferSize = 0;
uint32_t sBufferCapacity = 0;
bool sInDisplayList = false;
bool sBpWriteCacheEnabled = false;
uint8_t* sDlBuffer = nullptr;
uint32_t sDlSize = 0;
uint32_t sDlWritePos = 0;
std::array<StableSourceSpan, StableSourceSpanCapacity> sStableSourceSpans{};
uint32_t sStableSourceSpanCount = 0;
std::atomic<bool> sNativeReplayTracking{false};
bool sNativeModelRecording=false;
uint64_t sProducerWriteEpoch=0;
#if defined(MKW_TARGET_VITA)
const uint8_t* sActiveProcessData = nullptr;
uint32_t sActiveProcessSize = 0;
const StableSourceSpan* sActiveStableSourceSpans = nullptr;
uint32_t sActiveStableSourceSpanCount = 0;
uint64_t sActivePinnedIdentity=0;
uint32_t sActivePinnedBytes=0;
#endif
} // namespace detail

#if defined(MKW_TARGET_VITA)
namespace {
// Keep the producer close to the consumer. Four batches are enough to overlap
// game simulation with GX decode without letting indexed-array pointers remain
// outstanding for a large fraction of a frame.
// Pinned display-list segments keep the large payloads out of the job vectors,
// so a slightly deeper queue now costs little memory and lets the game thread
// overlap short producer bursts with the GX consumer instead of stalling on
// `melee_gx_space`. end_frame() is still the hard lifetime boundary.
constexpr uint32_t VitaQueueDepth = 8;
constexpr size_t VitaWorkerStackBytes = 1024u * 1024u;
constexpr uint32_t VitaJobSegmentCapacity = 128;

enum class VitaJobType : uint8_t { Fifo, Callback, Stop };
enum class VitaJobSegmentType : uint8_t { Inline, PinnedDisplayList, ViewMarker, NativeDrawReplay, NativeModel };

struct VitaJobSegment {
  VitaJobSegmentType type = VitaJobSegmentType::Inline;
  uint32_t offset = 0;
  uint32_t bytes = 0;
  const uint8_t* stableSource = nullptr;
  uint64_t pinnedIdentity=0;
  DisplayListShadowCache::PinnedBytes pinned{};
  NativeModelRecipeRef model{};
  std::array<float,12> position{};
  bool modelFullState=false,modelPatchPosition=false;
};

struct VitaJob {
  VitaJobType type = VitaJobType::Fifo;
  std::vector<uint8_t> bytes;
  bool bigEndian = true;
  std::array<StableSourceSpan, StableSourceSpanCapacity> stableSourceSpans{};
  uint32_t stableSourceSpanCount = 0;
  std::array<VitaJobSegment, VitaJobSegmentCapacity> segments{};
  uint32_t segmentCount = 0;
  VitaWorkerTask task = nullptr;
  void* context = nullptr;
  // Owned copy of an asynchronous task's arguments (run_async).
  alignas(8) std::array<uint8_t, 128> inlineContext{};
  uint64_t serial = 0;
};

struct VitaWorkerState {
  SceUID thread = -1;
  SceUID ready = -1;
  SceUID space = -1;
  SceUID completedSignal = -1;
  std::array<VitaJob, VitaQueueDepth> jobs{};
  std::atomic<uint64_t> submitted{0};
  std::atomic<uint64_t> completed{0};
  std::atomic<bool> completionWakePending{false};
  uint32_t producer = 0;
  uint32_t consumer = 0;
  std::atomic<bool> running{false};
};

VitaWorkerState sVitaWorker{};
std::array<VitaJobSegment, VitaJobSegmentCapacity> sPendingSegments{};
uint32_t sPendingSegmentCount = 0;
uint32_t sPendingInlineOffset = 0;
uint32_t sPendingPinnedBytes = 0;
// Game-thread time blocked on the worker: waiting for a free queue slot and
// waiting for a serial/draw-done token. Read and cleared by take_wait_stats().
std::atomic<uint64_t> sProducerWaitUs{0}, sConsumerWaitUs{0};
std::atomic<uint64_t> sNativeReplayAttempts{0},sNativeReplayHits{0},sNativeReplayFallbacks{0};
std::atomic<uint64_t> sNativeModelAttempts{0},sNativeModelHits{0},sNativeModelFallbacks{0},sNativeModelCompiled{0};
ModelAdmissionCensus sNativeModelCensus;
std::atomic<bool> sNativeModelCensusObserved{false};
uint64_t sActiveNativeModelIdentity=0,sNextNativeModelIdentity=0;
const uint8_t* sActiveNativeModelSource=nullptr;
const uint8_t* sActiveNativeModelData=nullptr;
std::shared_ptr<NativeModelRecipe> sNativeModelCapture;
bool sNativeModelSawDraw=false;
void abort_native_model_recording() noexcept;

bool on_worker_thread() noexcept {
  return sVitaWorker.thread >= 0 && sceKernelGetThreadId() == sVitaWorker.thread;
}

void reset_pending_segments() noexcept {
  for (uint32_t i = 0; i < sPendingSegmentCount; ++i) {
    sPendingSegments[i].pinned.reset();sPendingSegments[i].model.reset();
  }
  sPendingSegmentCount = 0;
  sPendingInlineOffset = 0;
  sPendingPinnedBytes = 0;
}

bool seal_inline_segment() noexcept {
  if (detail::sBufferSize <= sPendingInlineOffset) return true;
  if (sPendingSegmentCount == VitaJobSegmentCapacity) return false;
  auto& segment = sPendingSegments[sPendingSegmentCount++];
  segment = {};
  segment.type = VitaJobSegmentType::Inline;
  segment.offset = sPendingInlineOffset;
  segment.bytes = detail::sBufferSize - sPendingInlineOffset;
  sPendingInlineOffset = detail::sBufferSize;
  return true;
}

void destroy_worker_primitives() noexcept {
  if (sVitaWorker.thread >= 0) sceKernelDeleteThread(sVitaWorker.thread);
  if (sVitaWorker.ready >= 0) sceKernelDeleteSema(sVitaWorker.ready);
  if (sVitaWorker.space >= 0) sceKernelDeleteSema(sVitaWorker.space);
  if (sVitaWorker.completedSignal >= 0) sceKernelDeleteSema(sVitaWorker.completedSignal);
  sVitaWorker.thread = sVitaWorker.ready = sVitaWorker.space = sVitaWorker.completedSignal = -1;
}

void signal_completion(uint64_t serial) noexcept {
  sVitaWorker.completed.store(serial, std::memory_order_release);
  // completedSignal is binary. Only publish a new token when none is already
  // pending; otherwise sceKernelSignalSema would return SCE_KERNEL_ERROR_SEMA_OVF
  // as soon as two jobs complete before a waiter consumes the first token.
  bool expected = false;
  if (sVitaWorker.completionWakePending.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
    if (sceKernelSignalSema(sVitaWorker.completedSignal, 1) < 0)
      sVitaWorker.completionWakePending.store(false, std::memory_order_release);
  }
}

int vita_worker_main(SceSize, void*) {
  for (;;) {
    if (sceKernelWaitSema(sVitaWorker.ready, 1, nullptr) < 0) {
      aurora::vita::thread::delay_us(100);
      continue;
    }

    VitaJob& job = sVitaWorker.jobs[sVitaWorker.consumer % VitaQueueDepth];
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t serial = job.serial;

    if (job.type == VitaJobType::Stop) {
      signal_completion(serial);
      sceKernelSignalSema(sVitaWorker.space, 1);
      ++sVitaWorker.consumer;
      break;
    }

    if (job.type == VitaJobType::Fifo) {
      if (job.segmentCount != 0) {
        for (uint32_t i = 0; i < job.segmentCount; ++i) {
          const auto& segment = job.segments[i];
          if (segment.type == VitaJobSegmentType::ViewMarker) {
            // A marker reuses otherwise irrelevant segment fields so the
            // default FIFO job/segment footprint is unchanged.
            aurora::vita::view_draw_mark_consumer(segment.pinnedIdentity, segment.offset);
            continue;
          }
          if (segment.bytes == 0) continue;
          if (segment.type == VitaJobSegmentType::Inline) {
            if (segment.offset > job.bytes.size() || segment.bytes > job.bytes.size() - segment.offset) continue;
            detail::sActiveProcessData = job.bytes.data();
            detail::sActiveProcessSize = static_cast<uint32_t>(job.bytes.size());
            detail::sActiveStableSourceSpans = job.stableSourceSpans.data();
            detail::sActiveStableSourceSpanCount = job.stableSourceSpanCount;
            process(job.bytes.data() + segment.offset, segment.bytes, job.bigEndian);
          } else if (segment.pinned && segment.bytes <= segment.pinned->size()) {
            StableSourceSpan stable{};
            stable.offset = 0;
            stable.bytes = segment.bytes;
            stable.source = segment.stableSource;
            detail::sActiveProcessData = segment.pinned->data();
            detail::sActiveProcessSize = segment.bytes;
            detail::sActiveStableSourceSpans = &stable;
            detail::sActiveStableSourceSpanCount = segment.stableSource ? 1u : 0u;
            detail::sActivePinnedIdentity=segment.pinnedIdentity;
            detail::sActivePinnedBytes=segment.bytes;
            bool replayed=false;
            if(segment.type==VitaJobSegmentType::NativeModel&&segment.model){
              const auto& model=*segment.model;
              sNativeModelAttempts.fetch_add(1,std::memory_order_relaxed);
              // Replay the compiled material's state effects before the draw,
              // including CP array bindings. Keep the GX mirror coherent for
              // unrelated GX callers and for exact-slot reference fallback.
              const auto offset=segment.modelFullState?0u:model.materialBytes;
              if(offset<model.before.size())process(model.before.data()+offset,uint32_t(model.before.size()-offset),true);
              if(segment.modelPatchPosition){
              std::array<uint8_t,53> xf{};xf[0]=0x10;xf[2]=0x0b;
              for(unsigned n=0;n<12;++n){uint32_t bits;std::memcpy(&bits,&segment.position[n],4);
                for(unsigned b=0;b<4;++b)xf[5+n*4+b]=uint8_t(bits>>(24-b*8));}
              process(xf.data(),uint32_t(xf.size()),true);
              }
              sActiveNativeModelIdentity=model.identity;
              sActiveNativeModelSource=segment.stableSource;sActiveNativeModelData=segment.pinned->data();
              replayed=aurora::vita::draw_sink().submit_native_model_recipe(model.identity,segment.pinnedIdentity);
              if(replayed)sNativeModelHits.fetch_add(1,std::memory_order_relaxed);
              else sNativeModelFallbacks.fetch_add(1,std::memory_order_relaxed);
            }
            if(segment.type==VitaJobSegmentType::NativeDrawReplay){
              sNativeReplayAttempts.fetch_add(1,std::memory_order_relaxed);
              // A6 must not turn a compound/side-effecting GX DL into one
              // replayed draw. In that case run its original bytes in order.
              const auto recipe=prepare_display_list(segment.pinned->data(),segment.bytes,
                                                      g_gxState.lastVtxSize);
              replayed=job.bigEndian&&segment.stableSource&&recipe.valid&&
                  (recipe.command&0xF8u)==0x90u&&
                  (recipe.command&7u)==uint8_t(g_gxState.lastVtxFmt)&&
                  aurora::vita::draw_sink().replay_last_native_draw(segment.stableSource,segment.pinnedIdentity);
              if(replayed)sNativeReplayHits.fetch_add(1,std::memory_order_relaxed);
              else sNativeReplayFallbacks.fetch_add(1,std::memory_order_relaxed);
            }
            if(!replayed && (!job.bigEndian || !submit_prepared_display_list(
                      segment.pinned,segment.bytes,segment.stableSource,segment.pinnedIdentity)))
              process(segment.pinned->data(), segment.bytes, job.bigEndian);
            if(segment.type==VitaJobSegmentType::NativeModel&&segment.model){
              sActiveNativeModelIdentity=0;
              sActiveNativeModelSource=sActiveNativeModelData=nullptr;
              const auto& after=segment.model->after;
              if(!after.empty())process(after.data(),uint32_t(after.size()),true);
            }
            detail::sActivePinnedIdentity=0;
            detail::sActivePinnedBytes=0;
          }
        }
      } else {
        detail::sActiveProcessData = job.bytes.data();
        detail::sActiveProcessSize = static_cast<uint32_t>(job.bytes.size());
        detail::sActiveStableSourceSpans = job.stableSourceSpans.data();
        detail::sActiveStableSourceSpanCount = job.stableSourceSpanCount;
        if (!job.bytes.empty()) process(job.bytes.data(), static_cast<uint32_t>(job.bytes.size()), job.bigEndian);
      }
      detail::sActiveProcessData = nullptr;
      detail::sActiveProcessSize = 0;
      detail::sActiveStableSourceSpans = nullptr;
      detail::sActiveStableSourceSpanCount = 0;
    } else if (job.type == VitaJobType::Callback && job.task != nullptr) {
      job.task(job.context);
    }

    job.task = nullptr;
    job.context = nullptr;
    job.stableSourceSpanCount = 0;
    for (uint32_t i = 0; i < job.segmentCount; ++i) {
      job.segments[i].pinned.reset();job.segments[i].model.reset();
    }
    job.segmentCount = 0;
    signal_completion(serial);
    sceKernelSignalSema(sVitaWorker.space, 1);
    ++sVitaWorker.consumer;
  }
  return 0;
}

uint64_t enqueue_job(VitaJobType type, VitaWorkerTask task, void* context,
                     const uint8_t* bytes, uint32_t byteCount,
                     const StableSourceSpan* stableSpans, uint32_t stableSpanCount,
                     bool bigEndian = true, const void* ownedContext = nullptr,
                     size_t ownedContextBytes = 0,
                     const VitaJobSegment* segments = nullptr, uint32_t segmentCount = 0) {
  if (!sVitaWorker.running.load(std::memory_order_acquire)) return 0;
  {
    // Producer back-pressure: time the game thread waits for a free slot.
    const uint64_t t0 = aurora::vita::gfx::telemetry_now_us();
    while (sceKernelWaitSema(sVitaWorker.space, 1, nullptr) < 0)
      aurora::vita::thread::delay_us(100);
    if(aurora::vita::runtime_diagnostics_enabled())
      sProducerWaitUs.fetch_add(aurora::vita::gfx::telemetry_now_us() - t0, std::memory_order_relaxed);
  }

  VitaJob& job = sVitaWorker.jobs[sVitaWorker.producer % VitaQueueDepth];
  job.type = type;
  job.bigEndian = bigEndian;
  job.task = task;
  job.context = context;
  if (ownedContext != nullptr && ownedContextBytes != 0) {
    std::memcpy(job.inlineContext.data(), ownedContext, ownedContextBytes);
    job.context = job.inlineContext.data();
  }
  job.stableSourceSpanCount = std::min(stableSpanCount, StableSourceSpanCapacity);
  if (job.stableSourceSpanCount != 0 && stableSpans != nullptr) {
    std::copy_n(stableSpans, job.stableSourceSpanCount, job.stableSourceSpans.begin());
  }
  if (bytes != nullptr && byteCount != 0) job.bytes.assign(bytes, bytes + byteCount);
  else job.bytes.clear();
  job.segmentCount = std::min(segmentCount, VitaJobSegmentCapacity);
  for (uint32_t i = 0; i < job.segmentCount; ++i) job.segments[i] = segments[i];
  job.serial = sVitaWorker.submitted.fetch_add(1, std::memory_order_acq_rel) + 1;
  const uint64_t serial = job.serial;
  native_model_cache_peak(ModelCachePeak::QueueBatches,serial-sVitaWorker.completed.load(std::memory_order_acquire));
  std::atomic_thread_fence(std::memory_order_release);
  ++sVitaWorker.producer;
  sceKernelSignalSema(sVitaWorker.ready, 1);
  return serial;
}

void wait_serial(uint64_t serial) noexcept {
  if (serial == 0) return;
  if (sVitaWorker.completed.load(std::memory_order_acquire) >= serial) return;
  const uint64_t t0 = aurora::vita::gfx::telemetry_now_us();
  struct Account { uint64_t t0; ~Account() {
    if(aurora::vita::runtime_diagnostics_enabled())
      sConsumerWaitUs.fetch_add(aurora::vita::gfx::telemetry_now_us() - t0, std::memory_order_relaxed); } } account{t0};
  while (sVitaWorker.completed.load(std::memory_order_acquire) < serial) {
    // completedSignal is intentionally binary. More than one producer-side
    // waiter can observe the same outstanding serial, so one waiter may consume
    // the sole wake token. Use a bounded kernel wait and always re-check the
    // monotonic completion serial instead of sleeping forever on a lost wake.
    unsigned int timeoutUs = 1000;
    const int waitResult = sceKernelWaitSema(sVitaWorker.completedSignal, 1, &timeoutUs);
    if (waitResult >= 0)
      sVitaWorker.completionWakePending.store(false, std::memory_order_release);
  }
}

uint64_t enqueue_current_fifo() {
  if (detail::sBufferSize == 0 && sPendingSegmentCount == 0)
    return sVitaWorker.submitted.load(std::memory_order_acquire);
  if (!seal_inline_segment()) return 0;
  const uint64_t serial = enqueue_job(
      VitaJobType::Fifo, nullptr, nullptr, detail::sBufferData, detail::sBufferSize,
      detail::sStableSourceSpans.data(), detail::sStableSourceSpanCount, true, nullptr, 0,
      sPendingSegments.data(), sPendingSegmentCount);
  if (serial != 0) {
    detail::sBufferSize = 0;
    detail::sStableSourceSpanCount = 0;
    reset_pending_segments();
  }
  return serial;
}
} // namespace

bool start_worker() {
  if (sVitaWorker.running.load(std::memory_order_acquire)) return true;
  invalidate_bp_write_cache();
  sVitaWorker.producer = sVitaWorker.consumer = 0;
  sVitaWorker.submitted.store(0, std::memory_order_release);
  sVitaWorker.completed.store(0, std::memory_order_release);
  sVitaWorker.completionWakePending.store(false, std::memory_order_release);
  sVitaWorker.ready = sceKernelCreateSema("melee_gx_ready", 0, 0, VitaQueueDepth, nullptr);
  sVitaWorker.space = sceKernelCreateSema("melee_gx_space", 0, VitaQueueDepth, VitaQueueDepth, nullptr);
  sVitaWorker.completedSignal = sceKernelCreateSema("melee_gx_done", 0, 0, 1, nullptr);
  if (sVitaWorker.ready < 0 || sVitaWorker.space < 0 || sVitaWorker.completedSignal < 0) {
    destroy_worker_primitives();
    return false;
  }
  sVitaWorker.thread = sceKernelCreateThread("melee_gx_frontend", vita_worker_main, 0x10000100,
                                             VitaWorkerStackBytes, 0,
                                             SCE_KERNEL_CPU_MASK_USER_2, nullptr);
  if (sVitaWorker.thread < 0) {
    destroy_worker_primitives();
    return false;
  }
  sVitaWorker.running.store(true, std::memory_order_release);
  if (sceKernelStartThread(sVitaWorker.thread, 0, nullptr) < 0) {
    sVitaWorker.running.store(false, std::memory_order_release);
    destroy_worker_primitives();
    return false;
  }
  return true;
}

bool worker_running() { return sVitaWorker.running.load(std::memory_order_acquire); }

void write_view_marker(uint32_t view, uint64_t producerFrame) {
  auto& trace=aurora::vita::gfx::view_draw_capture();
  if(!trace.enabled()||in_display_list())return;
  if(!worker_running()||on_worker_thread()) {
    // Synchronous fallback: decode all earlier writes first, then advance the
    // consumer-owned view. No scene finish or GPU synchronization is added.
    if(!on_worker_thread())drain();
    aurora::vita::view_draw_mark_consumer(producerFrame,view);
    return;
  }
  if(sPendingSegmentCount>=VitaJobSegmentCapacity-2u)drain();
  if(!seal_inline_segment()||sPendingSegmentCount==VitaJobSegmentCapacity) {
    trace.suppressed_draw(); // records lost: parser must refuse the capture.
    return;
  }
  auto& segment=sPendingSegments[sPendingSegmentCount++];
  segment={};
  segment.type=VitaJobSegmentType::ViewMarker;
  segment.pinnedIdentity=producerFrame;
  segment.offset=view;
}

void take_wait_stats(uint64_t& producerWaitUs, uint64_t& consumerWaitUs) {
  producerWaitUs = sProducerWaitUs.exchange(0, std::memory_order_relaxed);
  consumerWaitUs = sConsumerWaitUs.exchange(0, std::memory_order_relaxed);
}

void wait_idle() {
  if (!worker_running() || on_worker_thread()) return;
  const uint64_t serial = sVitaWorker.submitted.load(std::memory_order_acquire);
  wait_serial(serial);
}

void drain_sync() {
  if (on_worker_thread()) return;
  if (!worker_running()) {
    if (detail::sBufferSize != 0) {
      process(detail::sBufferData, detail::sBufferSize, true);
      clear_buffer();
    }
    return;
  }
  wait_serial(enqueue_current_fifo());
}

void run_sync(VitaWorkerTask task, void* context) {
  if(!on_worker_thread()&&detail::sNativeModelRecording)abort_native_model_recording();
  if (on_worker_thread()) {
    if (task) task(context);
    return;
  }
  // The callback can mutate DrawSink/GX state without writing FIFO bytes.
  // Never admit a producer-side replay across that ordered side effect.
  if (task && detail::sNativeReplayTracking.load(std::memory_order_relaxed))
    ++detail::sProducerWriteEpoch;
  if (!worker_running()) {
    drain_sync();
    if (task) task(context);
    return;
  }
  enqueue_current_fifo();
  wait_serial(enqueue_job(VitaJobType::Callback, task, context, nullptr, 0, nullptr, 0));
}

uint64_t run_async(VitaWorkerTask task, const void* context, size_t contextBytes) {
  if(!on_worker_thread()&&detail::sNativeModelRecording)abort_native_model_recording();
  if (on_worker_thread()) {
    if (task) task(const_cast<void*>(context));
    return 0;
  }
  if (task && detail::sNativeReplayTracking.load(std::memory_order_relaxed))
    ++detail::sProducerWriteEpoch;
  if (!worker_running() || contextBytes > 128) {
    drain_sync();
    if (task) task(const_cast<void*>(context));
    return 0;
  }
  enqueue_current_fifo();
  return enqueue_job(VitaJobType::Callback, task, nullptr, nullptr, 0, nullptr, 0, true,
                     context, contextBytes);
}

uint64_t submit_marker() {
  if (on_worker_thread()) return 0;
  if (!worker_running()) {
    drain_sync();
    return 0;
  }
  return enqueue_current_fifo();
}

void wait_marker(uint64_t serial) {
  if (on_worker_thread() || !worker_running()) return;
  wait_serial(serial);
}

void process_sync(const uint8_t* data, uint32_t size, bool bigEndian) {
  if(!on_worker_thread()&&detail::sNativeModelRecording)abort_native_model_recording();
  if (data == nullptr || size == 0) return;
  if (on_worker_thread()) {
    process(data, size, bigEndian);
    return;
  }
  if (detail::sNativeReplayTracking.load(std::memory_order_relaxed))
    ++detail::sProducerWriteEpoch;
  invalidate_bp_write_cache();
  if (!worker_running()) {
    drain_sync();
    process(data, size, bigEndian);
    return;
  }
  enqueue_current_fifo();
  wait_serial(enqueue_job(VitaJobType::Fifo, nullptr, nullptr, data, size, nullptr, 0, bigEndian));
}

void shutdown_worker() {
  if(!on_worker_thread()&&detail::sNativeModelRecording)abort_native_model_recording();
  if (!worker_running()) return;
  drain_sync();
  const uint64_t serial = enqueue_job(VitaJobType::Stop, nullptr, nullptr, nullptr, 0, nullptr, 0);
  wait_serial(serial);
  sceKernelWaitThreadEnd(sVitaWorker.thread, nullptr, nullptr);
  sVitaWorker.running.store(false, std::memory_order_release);
  invalidate_bp_write_cache();
  destroy_worker_primitives();
}
#endif

void init() {
  constexpr uint32_t initialCapacity = 64 * 1024;
#if defined(MKW_TARGET_VITA)
  wait_idle();
#endif
  reset_cp_register_cache();
  invalidate_bp_write_cache();
  free(detail::sBufferData);
  detail::sBufferData = static_cast<uint8_t*>(malloc(initialCapacity));
  detail::sBufferSize = 0;
  detail::sBufferCapacity = initialCapacity;
  detail::sInDisplayList = false;
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
  detail::sStableSourceSpanCount = 0;
#if defined(MKW_TARGET_VITA)
  reset_pending_segments();
#endif
}

void write_stable_data_from(const void* bytes, const void* stableSource, uint32_t length) {
  if (detail::sInDisplayList) {
    write_data(bytes, length);
    return;
  }
  if (bytes == nullptr || stableSource == nullptr || length == 0) return;
  invalidate_bp_write_cache();
#if defined(MKW_TARGET_VITA)
  const bool profile = aurora::vita::gfx::g_fifoProfileEnabled;
  const uint64_t profileStart = profile ? aurora::vita::gfx::telemetry_now_us() : 0;
  struct ProfileScope {
    bool on; uint64_t start; const void* src; uint32_t len;
    ~ProfileScope() {
      if (!on) return;
      auto& p = aurora::vita::gfx::fifo_profile_accumulator();
      p.dlBytes += len; ++p.dlCalls;
      p.dlCopyUs += aurora::vita::gfx::telemetry_now_us() - start;
      // CDRAM user mappings live in the 0x6xxxxxxx range on Vita.
      const uintptr_t a = reinterpret_cast<uintptr_t>(src);
      if (a >= 0x60000000u && a < 0x70000000u) p.dlCdramBytes += len;
    }
  } profileScope{profile, profileStart, bytes, length};
#endif

  // Keep batches bounded so producer/consumer overlap stays responsive while
  // avoiding the previous one-job-per-display-list serialization.
  constexpr uint32_t StableBatchByteLimit = 256u * 1024u;
  const bool batchTooLarge = detail::sBufferSize != 0 &&
      (length > StableBatchByteLimit || detail::sBufferSize > StableBatchByteLimit - length);
  if (detail::sStableSourceSpanCount == StableSourceSpanCapacity || batchTooLarge) drain();

  auto& span = detail::sStableSourceSpans[detail::sStableSourceSpanCount++];
  span.offset = detail::sBufferSize;
  span.bytes = length;
  span.source = static_cast<const uint8_t*>(stableSource);
  write_data(bytes, length);
}

#if defined(MKW_TARGET_VITA)
namespace {
// Strikers keeps its permanent display lists in CDRAM, which the CPU reads
// uncached: re-reading ~800 KiB of them every frame dominated the GX frontend.
// Validate every reuse against the live bytes until hardware establishes why
// revision-only reuse glitched. This diagnostic guard retains the original
// guest identity and publishes any untracked write to downstream caches.
DisplayListShadowCache sDisplayListShadows(8u * 1024u * 1024u, false);
bool sDisplayListShadowEnabled = true;

DisplayListShadowCache::PinnedBytes display_list_shadow_pin(const void* data, uint32_t length,uint64_t* identity,
                                                           bool immutableRecipe=false) {
  // With the async GX consumer the immutable shadow is the transport itself,
  // not merely an optional cache: keeping the large display-list payload out
  // of the FIFO is what removes producer memcpy/back-pressure. The runtime
  // switch still controls the synchronous shadow optimization.
  if (!sDisplayListShadowEnabled && !worker_running()) return nullptr;
#if defined(__vita__)
  const uintptr_t address = reinterpret_cast<uintptr_t>(data);
  // Ordinary cached-RAM lists keep their existing direct copy path. A native
  // recipe requires an immutable pin regardless of the source memory type.
  if (!display_list_shadow_source_allowed(address,immutableRecipe)) return nullptr;
#endif
  // Recipes skip the ordinary DL decode, so validate live bytes even if the
  // caller missed a write notification (notably mutable PN selector bytes).
  return sDisplayListShadows.pin(data,length,identity,immutableRecipe);
}

bool append_pinned_display_list(const void* guestSource, uint32_t length,
                                DisplayListShadowCache::PinnedBytes pinned,uint64_t identity,
                                VitaJobSegmentType type=VitaJobSegmentType::PinnedDisplayList) {
  if (!worker_running() || !pinned || pinned->size() < length || length == 0) return false;
  constexpr uint32_t StableBatchByteLimit = 256u * 1024u;
  if (sPendingSegmentCount >= VitaJobSegmentCapacity - 2u ||
      (sPendingPinnedBytes != 0 &&
       (length > StableBatchByteLimit || sPendingPinnedBytes > StableBatchByteLimit - length))) {
    drain();
  }
  if (!seal_inline_segment() || sPendingSegmentCount == VitaJobSegmentCapacity) return false;
  auto& segment = sPendingSegments[sPendingSegmentCount++];
  segment = {};
  segment.type = type;
  segment.bytes = length;
  segment.stableSource = static_cast<const uint8_t*>(guestSource);
  segment.pinnedIdentity=identity;
  segment.pinned = std::move(pinned);
  sPendingPinnedBytes += length;
  native_model_cache_peak(ModelCachePeak::PendingSegments,sPendingSegmentCount);
  native_model_cache_peak(ModelCachePeak::PendingPinnedBytes,sPendingPinnedBytes);
  if (aurora::vita::gfx::g_fifoProfileEnabled) {
    auto& p = aurora::vita::gfx::fifo_profile_accumulator();
    ++p.dlPinnedCalls;
    p.dlPinnedBytes += length;
  }
  return true;
}
} // namespace
#endif

#if defined(MKW_TARGET_VITA)
void set_display_list_shadow_enabled(bool enabled) {
  sDisplayListShadowEnabled = enabled;
  if (!enabled) {
    sDisplayListShadows.clear();
  }
}
#endif

void write_stable_data(const void* data, uint32_t length) {
#if defined(MKW_TARGET_VITA)
  if(detail::sNativeModelRecording){
    if(!sNativeModelSawDraw&&data==sNativeModelCapture->source&&length==sNativeModelCapture->bytes){
      sNativeModelSawDraw=true;return;
    }
    // Reject compound/unexpected lists without losing any recorded bytes.
    detail::record_native_model_bytes(nullptr,NativeModelRecipe::MaxStateBytes+1);
  }
#endif
  // Pinned DLs are separate FIFO segments and never reach write_data().
  // Without this guard an unrelated GXCallDisplayList between two packet
  // callbacks could change the consumer's pipeline yet pass the replay gate.
  if(detail::sNativeReplayTracking.load(std::memory_order_relaxed) &&
     !detail::sInDisplayList && data && length)
    ++detail::sProducerWriteEpoch;
#if defined(MKW_TARGET_VITA)
  // The span keeps the guest address as its stable source identity.
  if (!detail::sInDisplayList && data != nullptr && length != 0) {
    uint64_t identity=0;
    if (auto shadow = display_list_shadow_pin(data, length,&identity)) {
      if (append_pinned_display_list(data, length, shadow,identity)) return;
      write_stable_data_from(shadow->data(), data, length);
      return;
    }
  }
#endif
  write_stable_data_from(data, data, length);
}

#if defined(MKW_TARGET_VITA)
namespace {
void abort_native_model_recording() noexcept {
  auto recording=std::move(sNativeModelCapture);
  detail::sNativeModelRecording=false;
  if(!recording)return;
  native_model_cache_event(ModelCacheEvent::CaptureAborted);
  if(!recording->before.empty())write_data(recording->before.data(),uint32_t(recording->before.size()));
  if(sNativeModelSawDraw)write_stable_data(recording->source,recording->bytes);
  if(!recording->after.empty())write_data(recording->after.data(),uint32_t(recording->after.size()));
  sNativeModelSawDraw=false;
}
}
bool detail::record_native_model_bytes(const void* data,uint32_t bytes) noexcept {
  auto& output=sNativeModelSawDraw?sNativeModelCapture->after:sNativeModelCapture->before;
  if((bytes&&!data)||bytes>NativeModelRecipe::MaxStateBytes-output.size()){
    abort_native_model_recording();return false;
  }
  if(bytes){
    const size_t needed=output.size()+bytes;
    if(needed>output.capacity())output.reserve(std::min(NativeModelRecipe::MaxStateBytes,
        std::max(needed,std::max(size_t(64),output.capacity()*2))));
    const auto* p=static_cast<const uint8_t*>(data);output.insert(output.end(),p,p+bytes);
  }
  return true;
}
bool begin_native_model_recording(const void* source,uint32_t bytes) noexcept {
  native_model_cache_event(ModelCacheEvent::CaptureAttempt);
  const auto reject=[](ModelCacheEvent event){native_model_cache_event(event);return false;};
  if(!worker_running()||on_worker_thread()||in_display_list())return reject(ModelCacheEvent::CaptureContextReject);
  if(!source||bytes<3)return reject(ModelCacheEvent::CaptureSourceReject);
  if(detail::sNativeModelRecording)return reject(ModelCacheEvent::CaptureBusyReject);
  if(NativeModelRecipe::liveRecipes.load(std::memory_order_relaxed)>=NativeModelRecipe::max_live_recipes())
    return reject(ModelCacheEvent::CaptureLimitReject);
  sNativeModelCapture=std::make_shared<NativeModelRecipe>();
  sNativeModelCapture->identity=++sNextNativeModelIdentity;
  sNativeModelCapture->source=static_cast<const uint8_t*>(source);
  sNativeModelCapture->bytes=bytes;
  sNativeModelSawDraw=false;
  invalidate_bp_write_cache();
  detail::sNativeModelRecording=true;
  return true;
}
NativeModelRecipeRef finish_native_model_recording(const float* position) noexcept {
  if(!detail::sNativeModelRecording)return {};
  if(!sNativeModelSawDraw){abort_native_model_recording();return {};}
  NativeModelRecipeRef recipe=sNativeModelCapture;
  native_model_cache_peak(ModelCachePeak::RecipePayloadBytes,recipe->before.capacity()+recipe->after.capacity());
  // No captured byte reaches the queue until the complete recipe is sealed.
  detail::sNativeModelRecording=false;
  if(!write_native_model_recipe(recipe,position,true)){
    abort_native_model_recording();return {};
  }
  sNativeModelCapture.reset();sNativeModelSawDraw=false;
  sNativeModelCompiled.fetch_add(1,std::memory_order_relaxed);
  return recipe;
}
void native_model_record_draw_boundary() noexcept {
  if(detail::sNativeModelRecording&&!sNativeModelSawDraw)
    sNativeModelCapture->materialBytes=uint32_t(sNativeModelCapture->before.size());
}
bool write_native_model_recipe(const NativeModelRecipeRef& recipe,const float* position,bool fullState) noexcept {
  native_model_cache_event(ModelCacheEvent::TransportAttempt);
  const auto reject=[](ModelCacheEvent event){native_model_cache_event(event);return false;};
  if(!recipe||!recipe->identity||recipe->materialBytes>recipe->before.size()||!worker_running()||on_worker_thread()||in_display_list())return reject(ModelCacheEvent::TransportContractReject);
  uint64_t identity=0;
  auto pinned=display_list_shadow_pin(recipe->source,recipe->bytes,&identity,true);
  if(!pinned||!identity)return reject(ModelCacheEvent::TransportPinReject);
  if(!append_pinned_display_list(recipe->source,recipe->bytes,std::move(pinned),identity,
                                  VitaJobSegmentType::NativeModel))return reject(ModelCacheEvent::TransportQueueReject);
  auto& segment=sPendingSegments[sPendingSegmentCount-1];
  segment.model=recipe;segment.modelFullState=fullState;segment.modelPatchPosition=position!=nullptr;
  if(position)std::copy_n(position,12,segment.position.begin());
  invalidate_bp_write_cache();
  if(detail::sNativeReplayTracking.load(std::memory_order_relaxed))++detail::sProducerWriteEpoch;
  return true;
}
uint64_t active_native_model_identity() noexcept{return sActiveNativeModelIdentity;}
uint32_t native_model_transport_rejections() noexcept {
  uint32_t mask=0;
  if(!worker_running())mask|=model_reject_bit(ModelReject::WorkerInactive);
  if(on_worker_thread())return mask|model_reject_bit(ModelReject::WorkerContext);
  if(in_display_list())mask|=model_reject_bit(ModelReject::InsideDisplayList);
  if(detail::sNativeModelRecording)mask|=model_reject_bit(ModelReject::RecordingBusy);
  if(NativeModelRecipe::liveRecipes.load(std::memory_order_relaxed)>=NativeModelRecipe::max_live_recipes())
    mask|=model_reject_bit(ModelReject::RecipeLimit);
  return mask;
}
void record_native_model_admission(const ModelAdmissionFacts& facts,bool eligible) noexcept {
  sNativeModelCensus.observe(facts,eligible);
  sNativeModelCensusObserved.store(true,std::memory_order_release);
}
void record_native_model_stage(ModelStage stage) noexcept {sNativeModelCensus.event(stage);}
ModelCensusSnapshot native_model_census_snapshot() noexcept {
  if(!sNativeModelCensusObserved.load(std::memory_order_acquire))return {};
  return sNativeModelCensus.snapshot();
}
ModelCensusEvidence native_model_census_evidence() noexcept {return sNativeModelCensus.evidence();}
bool active_native_model_single_draw(uint8_t primitive,uint8_t fmt,uint16_t stride,const uint8_t* source) noexcept {
  if(!sActiveNativeModelIdentity||!sActiveNativeModelSource||!sActiveNativeModelData||
     source!=sActiveNativeModelSource+3)return false;
  const auto draw=prepare_display_list(sActiveNativeModelData,detail::sActivePinnedBytes,stride);
  return draw.valid&&(draw.command&0xf8u)==primitive&&(draw.command&7u)==fmt;
}
NativeModelStats native_model_stats() noexcept {
  return {sNativeModelAttempts.load(std::memory_order_relaxed),sNativeModelHits.load(std::memory_order_relaxed),
          sNativeModelFallbacks.load(std::memory_order_relaxed),sNativeModelCompiled.load(std::memory_order_relaxed)};
}
void set_native_draw_replay_tracking(bool enabled) noexcept {
  detail::sNativeReplayTracking.store(enabled,std::memory_order_relaxed);
  detail::sProducerWriteEpoch=0;
  sNativeReplayAttempts.store(0,std::memory_order_relaxed);
  sNativeReplayHits.store(0,std::memory_order_relaxed);
  sNativeReplayFallbacks.store(0,std::memory_order_relaxed);
}
uint64_t producer_write_epoch() noexcept {return detail::sProducerWriteEpoch;}
bool native_draw_replay_tracking_enabled() noexcept {
  return detail::sNativeReplayTracking.load(std::memory_order_relaxed);
}
uint64_t active_pinned_identity() noexcept {return detail::sActivePinnedIdentity;}
uint32_t active_pinned_bytes() noexcept {return detail::sActivePinnedBytes;}
NativeReplayStats native_replay_stats() noexcept {
  return {sNativeReplayAttempts.load(std::memory_order_relaxed),
          sNativeReplayHits.load(std::memory_order_relaxed),
          sNativeReplayFallbacks.load(std::memory_order_relaxed)};
}
bool write_native_draw_replay(const void* data,uint32_t length) noexcept {
  if(!native_draw_replay_tracking_enabled()||!worker_running()||on_worker_thread()||
     in_display_list()||!data||length<3)return false;
  uint64_t identity=0;
  auto pinned=display_list_shadow_pin(data,length,&identity);
  if(!identity||!pinned)return false;
  return append_pinned_display_list(data,length,std::move(pinned),identity,
                                    VitaJobSegmentType::NativeDrawReplay);
}
#endif

void write_data_grow(const void* data, uint32_t length) {
  uint32_t needed = detail::sBufferSize + length;
  uint32_t newCap = std::max(detail::sBufferCapacity * 2, needed);
  detail::sBufferData = static_cast<uint8_t*>(realloc(detail::sBufferData, newCap));
  std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
  detail::sBufferSize = needed;
  detail::sBufferCapacity = newCap;
}

void begin_display_list(uint8_t* buf, uint32_t size) {
  detail::sInDisplayList = true;
  detail::sDlBuffer = buf;
  detail::sDlSize = size;
  detail::sDlWritePos = 0;
}

uint32_t end_display_list() {
  detail::sInDisplayList = false;
  uint32_t bytesWritten = detail::sDlWritePos;
  uint32_t padded = (bytesWritten + 31) & ~31u;
  while (detail::sDlWritePos < padded && detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = 0;
  }
  if(detail::sNativeReplayTracking.load(std::memory_order_relaxed) && detail::sDlWritePos)
    ++detail::sProducerWriteEpoch;
#if defined(MKW_TARGET_VITA)
  // Recorded lists are written here, not through DCFlush: publish the write so
  // cached shadows (and stable-geometry revisions) of this buffer are dropped.
  if (detail::sDlBuffer != nullptr && detail::sDlWritePos != 0)
    aurora::vita::gfx::note_memory_write(detail::sDlBuffer, detail::sDlWritePos);
#endif
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
  return padded;
}

bool in_display_list() { return detail::sInDisplayList; }

// How much of the producer's frame is spent blocked before it may decode the next batch of GX commands.
static void note_drain_wait(uint64_t nanos) noexcept {
  ZoneScopedN("FIFO drain wait");
  TracyPlot("aurora: fifoDrainWaitUs", static_cast<int64_t>(nanos / 1000));
}

void drain() {
#if defined(MKW_TARGET_VITA)
  if(!on_worker_thread()&&detail::sNativeModelRecording)abort_native_model_recording();
#endif
#if !defined(MKW_TARGET_VITA)
  // SEALED, not DONE.
  const auto waited = aurora::wait_for_frame_worker_sealed();
  if (waited.count() > 0) UNLIKELY {
    note_drain_wait(static_cast<uint64_t>(waited.count()));
  }
#endif
#if defined(MKW_TARGET_VITA)
  if (detail::sBufferSize == 0 && sPendingSegmentCount == 0) return;
#else
  if (detail::sBufferSize == 0) return;
#endif
#if defined(MKW_TARGET_VITA)
  if (worker_running()) {
    enqueue_current_fifo();
    return;
  }
#endif
  process(detail::sBufferData, detail::sBufferSize, true);
  // These bytes were consumed, so the producer cache still describes live BP
  // state. Public clear_buffer() instead discards bytes and invalidates it.
  detail::sBufferSize = 0;
  detail::sStableSourceSpanCount = 0;
}

const uint8_t* get_buffer_data() { return detail::sBufferData; }
uint32_t get_buffer_size() { return detail::sBufferSize; }
const uint8_t* stable_source_for(const uint8_t* data,size_t bytes) {
  const StableSourceSpan* spans=detail::sStableSourceSpans.data();
  uint32_t count=detail::sStableSourceSpanCount;
  const uint8_t* processData=detail::sBufferData;
  size_t processBytes=detail::sBufferSize;
#if defined(MKW_TARGET_VITA)
  if(detail::sActiveProcessData){
    spans=detail::sActiveStableSourceSpans;
    count=detail::sActiveStableSourceSpanCount;
    processData=detail::sActiveProcessData;
    processBytes=detail::sActiveProcessSize;
  }
#endif
  if(!data||!processData||!spans||count==0)return nullptr;
  const uintptr_t base=reinterpret_cast<uintptr_t>(processData);
  const uintptr_t address=reinterpret_cast<uintptr_t>(data);
  if(address<base||address-base>processBytes)return nullptr;
  const size_t offset=static_cast<size_t>(address-base);
  if(bytes>processBytes-offset)return nullptr;
  uint32_t lo=0,hi=count;
  while(lo<hi){
    const uint32_t mid=lo+(hi-lo)/2;
    if(spans[mid].offset<=offset)lo=mid+1;
    else hi=mid;
  }
  if(lo==0)return nullptr;
  const auto& span=spans[lo-1];
  const size_t relative=offset-span.offset;
  if(relative>span.bytes||bytes>static_cast<size_t>(span.bytes-relative))return nullptr;
  return span.source+relative;
}
void clear_buffer() {
  invalidate_bp_write_cache();
  if(detail::sNativeReplayTracking.load(std::memory_order_relaxed))
    ++detail::sProducerWriteEpoch;
  detail::sBufferSize = 0;
  detail::sStableSourceSpanCount = 0;
#if defined(MKW_TARGET_VITA)
  reset_pending_segments();
#endif
}

} // namespace aurora::gx::fifo
