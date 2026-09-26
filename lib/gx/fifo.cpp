#include "fifo.hpp"
#include "fifo.hpp"
#include "command_processor.hpp"
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

namespace detail {
uint8_t* sBufferData = nullptr;
uint32_t sBufferSize = 0;
uint32_t sBufferCapacity = 0;
bool sInDisplayList = false;
uint8_t* sDlBuffer = nullptr;
uint32_t sDlSize = 0;
uint32_t sDlWritePos = 0;
std::array<StableSourceSpan, StableSourceSpanCapacity> sStableSourceSpans{};
uint32_t sStableSourceSpanCount = 0;
#if defined(MKW_TARGET_VITA)
const uint8_t* sActiveProcessData = nullptr;
uint32_t sActiveProcessSize = 0;
const StableSourceSpan* sActiveStableSourceSpans = nullptr;
uint32_t sActiveStableSourceSpanCount = 0;
#endif
} // namespace detail

#if defined(MKW_TARGET_VITA)
namespace {
// Keep the producer close to the consumer. Four batches are enough to overlap
// game simulation with GX decode without letting indexed-array pointers remain
// outstanding for a large fraction of a frame.
constexpr uint32_t VitaQueueDepth = 4;
constexpr size_t VitaWorkerStackBytes = 1024u * 1024u;

enum class VitaJobType : uint8_t { Fifo, Callback, Stop };

struct VitaJob {
  VitaJobType type = VitaJobType::Fifo;
  std::vector<uint8_t> bytes;
  bool bigEndian = true;
  std::array<StableSourceSpan, StableSourceSpanCapacity> stableSourceSpans{};
  uint32_t stableSourceSpanCount = 0;
  VitaWorkerTask task = nullptr;
  void* context = nullptr;
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
  uint32_t producer = 0;
  uint32_t consumer = 0;
  std::atomic<bool> running{false};
};

VitaWorkerState sVitaWorker{};

bool on_worker_thread() noexcept {
  return sVitaWorker.thread >= 0 && sceKernelGetThreadId() == sVitaWorker.thread;
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
  // Binary wakeup: a stale token is harmless because waiters always re-check
  // the monotonically increasing completion serial.
  sceKernelSignalSema(sVitaWorker.completedSignal, 1);
}

int vita_worker_main(SceSize, void*) {
  for (;;) {
    if (sceKernelWaitSema(sVitaWorker.ready, 1, nullptr) < 0) {
      sceKernelDelayThread(100);
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
      detail::sActiveProcessData = job.bytes.data();
      detail::sActiveProcessSize = static_cast<uint32_t>(job.bytes.size());
      detail::sActiveStableSourceSpans = job.stableSourceSpans.data();
      detail::sActiveStableSourceSpanCount = job.stableSourceSpanCount;
      if (!job.bytes.empty()) process(job.bytes.data(), static_cast<uint32_t>(job.bytes.size()), job.bigEndian);
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
    signal_completion(serial);
    sceKernelSignalSema(sVitaWorker.space, 1);
    ++sVitaWorker.consumer;
  }
  return 0;
}

uint64_t enqueue_job(VitaJobType type, VitaWorkerTask task, void* context,
                     const uint8_t* bytes, uint32_t byteCount,
                     const StableSourceSpan* stableSpans, uint32_t stableSpanCount,
                     bool bigEndian = true) {
  if (!sVitaWorker.running.load(std::memory_order_acquire)) return 0;
  while (sceKernelWaitSema(sVitaWorker.space, 1, nullptr) < 0) sceKernelDelayThread(100);

  VitaJob& job = sVitaWorker.jobs[sVitaWorker.producer % VitaQueueDepth];
  job.type = type;
  job.bigEndian = bigEndian;
  job.task = task;
  job.context = context;
  job.stableSourceSpanCount = std::min(stableSpanCount, StableSourceSpanCapacity);
  if (job.stableSourceSpanCount != 0 && stableSpans != nullptr) {
    std::copy_n(stableSpans, job.stableSourceSpanCount, job.stableSourceSpans.begin());
  }
  if (bytes != nullptr && byteCount != 0) job.bytes.assign(bytes, bytes + byteCount);
  else job.bytes.clear();
  job.serial = sVitaWorker.submitted.fetch_add(1, std::memory_order_acq_rel) + 1;
  const uint64_t serial = job.serial;
  std::atomic_thread_fence(std::memory_order_release);
  ++sVitaWorker.producer;
  sceKernelSignalSema(sVitaWorker.ready, 1);
  return serial;
}

void wait_serial(uint64_t serial) noexcept {
  if (serial == 0) return;
  while (sVitaWorker.completed.load(std::memory_order_acquire) < serial) {
    if (sceKernelWaitSema(sVitaWorker.completedSignal, 1, nullptr) < 0) sceKernelDelayThread(100);
  }
}

uint64_t enqueue_current_fifo() {
  if (detail::sBufferSize == 0) return sVitaWorker.submitted.load(std::memory_order_acquire);
  const uint64_t serial = enqueue_job(
      VitaJobType::Fifo, nullptr, nullptr, detail::sBufferData, detail::sBufferSize,
      detail::sStableSourceSpans.data(), detail::sStableSourceSpanCount, true);
  if (serial != 0) {
    detail::sBufferSize = 0;
    detail::sStableSourceSpanCount = 0;
  }
  return serial;
}
} // namespace

bool start_worker() {
  if (sVitaWorker.running.load(std::memory_order_acquire)) return true;
  sVitaWorker.producer = sVitaWorker.consumer = 0;
  sVitaWorker.submitted.store(0, std::memory_order_release);
  sVitaWorker.completed.store(0, std::memory_order_release);
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
  if (on_worker_thread()) {
    if (task) task(context);
    return;
  }
  if (!worker_running()) {
    drain_sync();
    if (task) task(context);
    return;
  }
  enqueue_current_fifo();
  wait_serial(enqueue_job(VitaJobType::Callback, task, context, nullptr, 0, nullptr, 0));
}

void process_sync(const uint8_t* data, uint32_t size, bool bigEndian) {
  if (data == nullptr || size == 0) return;
  if (on_worker_thread()) {
    process(data, size, bigEndian);
    return;
  }
  if (!worker_running()) {
    drain_sync();
    process(data, size, bigEndian);
    return;
  }
  enqueue_current_fifo();
  wait_serial(enqueue_job(VitaJobType::Fifo, nullptr, nullptr, data, size, nullptr, 0, bigEndian));
}

void shutdown_worker() {
  if (!worker_running()) return;
  drain_sync();
  const uint64_t serial = enqueue_job(VitaJobType::Stop, nullptr, nullptr, nullptr, 0, nullptr, 0);
  wait_serial(serial);
  sceKernelWaitThreadEnd(sVitaWorker.thread, nullptr, nullptr);
  sVitaWorker.running.store(false, std::memory_order_release);
  destroy_worker_primitives();
}
#endif

void init() {
  constexpr uint32_t initialCapacity = 64 * 1024;
#if defined(MKW_TARGET_VITA)
  wait_idle();
#endif
  reset_cp_register_cache();
  free(detail::sBufferData);
  detail::sBufferData = static_cast<uint8_t*>(malloc(initialCapacity));
  detail::sBufferSize = 0;
  detail::sBufferCapacity = initialCapacity;
  detail::sInDisplayList = false;
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
  detail::sStableSourceSpanCount = 0;
}

void write_stable_data_from(const void* bytes, const void* stableSource, uint32_t length) {
  if (detail::sInDisplayList) {
    write_data(bytes, length);
    return;
  }
  if (bytes == nullptr || stableSource == nullptr || length == 0) return;

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

void write_stable_data(const void* data, uint32_t length) {
  write_stable_data_from(data, data, length);
}

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
#if !defined(MKW_TARGET_VITA)
  // SEALED, not DONE.
  const auto waited = aurora::wait_for_frame_worker_sealed();
  if (waited.count() > 0) UNLIKELY {
    note_drain_wait(static_cast<uint64_t>(waited.count()));
  }
#endif
  if (detail::sBufferSize == 0) return;
#if defined(MKW_TARGET_VITA)
  if (worker_running()) {
    enqueue_current_fifo();
    return;
  }
#endif
  process(detail::sBufferData, detail::sBufferSize, true);
  clear_buffer();
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
  detail::sBufferSize = 0;
  detail::sStableSourceSpanCount = 0;
}

} // namespace aurora::gx::fifo
