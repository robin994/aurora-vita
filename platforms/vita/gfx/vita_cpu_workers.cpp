#include "vita_cpu_workers.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

#if defined(__vita__)
#include <pthread.h>
#endif

namespace aurora::vita::gfx {

#if defined(__vita__)
namespace {
constexpr uint32_t MaxWorkers = 2;
constexpr size_t WorkerStackBytes = 128u * 1024u;

struct CpuWorkerState {
  pthread_mutex_t mutex{};
  pthread_cond_t wake{};
  pthread_cond_t done{};
  bool syncReady = false;
  bool initialized = false;
  bool stop = false;
  uint64_t generation = 0;
  CpuRangeTask task = nullptr;
  void* context = nullptr;
  std::array<pthread_t, MaxWorkers> threads{};
  std::array<uint32_t, MaxWorkers> indices{{0, 1}};
  std::array<size_t, MaxWorkers> begin{};
  std::array<size_t, MaxWorkers> end{};
  std::array<bool, MaxWorkers> active{};
  std::array<bool, MaxWorkers> result{{true, true}};
  uint32_t workerCount = 0;
  uint32_t activeWorkers = 0;
  uint32_t completedWorkers = 0;
  size_t minItems = 512;
};

CpuWorkerState g_workers{};

void* cpu_worker_main(void* opaque) {
  const uint32_t index = *static_cast<const uint32_t*>(opaque);
  uint64_t seenGeneration = 0;

  pthread_mutex_lock(&g_workers.mutex);
  for (;;) {
    while (!g_workers.stop && g_workers.generation == seenGeneration) {
      pthread_cond_wait(&g_workers.wake, &g_workers.mutex);
    }
    if (g_workers.stop) {
      pthread_mutex_unlock(&g_workers.mutex);
      return nullptr;
    }

    const uint64_t generation = g_workers.generation;
    const CpuRangeTask task = g_workers.task;
    void* const context = g_workers.context;
    const size_t begin = g_workers.begin[index];
    const size_t end = g_workers.end[index];
    const bool active = g_workers.active[index];
    pthread_mutex_unlock(&g_workers.mutex);

    const bool ok = !active || !task || task(context, begin, end, index + 1);

    pthread_mutex_lock(&g_workers.mutex);
    seenGeneration = generation;
    if (active) {
      g_workers.result[index] = ok;
      ++g_workers.completedWorkers;
      if (g_workers.completedWorkers >= g_workers.activeWorkers) {
        pthread_cond_signal(&g_workers.done);
      }
    }
  }
}

void destroy_sync_primitives() noexcept {
  if (!g_workers.syncReady) return;
  pthread_cond_destroy(&g_workers.done);
  pthread_cond_destroy(&g_workers.wake);
  pthread_mutex_destroy(&g_workers.mutex);
  g_workers.syncReady = false;
}
} // namespace

bool initialize_cpu_workers(uint32_t workerThreads, size_t minItems) noexcept {
  if (g_workers.initialized) shutdown_cpu_workers();

  g_workers.stop = false;
  g_workers.generation = 0;
  g_workers.task = nullptr;
  g_workers.context = nullptr;
  g_workers.workerCount = 0;
  g_workers.activeWorkers = 0;
  g_workers.completedWorkers = 0;
  g_workers.minItems = std::max<size_t>(1, minItems);

  if (pthread_mutex_init(&g_workers.mutex, nullptr) != 0) return false;
  if (pthread_cond_init(&g_workers.wake, nullptr) != 0) {
    pthread_mutex_destroy(&g_workers.mutex);
    return false;
  }
  if (pthread_cond_init(&g_workers.done, nullptr) != 0) {
    pthread_cond_destroy(&g_workers.wake);
    pthread_mutex_destroy(&g_workers.mutex);
    return false;
  }
  g_workers.syncReady = true;

  const uint32_t requested = std::min(workerThreads, MaxWorkers);
  pthread_attr_t attr{};
  const bool attrReady = pthread_attr_init(&attr) == 0;
  if (attrReady) (void)pthread_attr_setstacksize(&attr, WorkerStackBytes);

  for (uint32_t i = 0; i < requested; ++i) {
    const int rc = pthread_create(&g_workers.threads[i], attrReady ? &attr : nullptr,
                                  cpu_worker_main, &g_workers.indices[i]);
    if (rc != 0) break;
    ++g_workers.workerCount;
  }
  if (attrReady) pthread_attr_destroy(&attr);

  g_workers.initialized = true;
  std::fprintf(stderr,
               "[aurora-vita] cpu workers=%u lanes=%u parallel_min_vertices=%llu\n",
               g_workers.workerCount, g_workers.workerCount + 1,
               static_cast<unsigned long long>(g_workers.minItems));
  return true;
}

void shutdown_cpu_workers() noexcept {
  if (!g_workers.initialized && !g_workers.syncReady) return;

  if (g_workers.syncReady) {
    pthread_mutex_lock(&g_workers.mutex);
    g_workers.stop = true;
    ++g_workers.generation;
    pthread_cond_broadcast(&g_workers.wake);
    pthread_mutex_unlock(&g_workers.mutex);
  }

  for (uint32_t i = 0; i < g_workers.workerCount; ++i) {
    pthread_join(g_workers.threads[i], nullptr);
  }
  destroy_sync_primitives();

  g_workers.initialized = false;
  g_workers.stop = false;
  g_workers.workerCount = 0;
  g_workers.activeWorkers = 0;
  g_workers.completedWorkers = 0;
  g_workers.task = nullptr;
  g_workers.context = nullptr;
}

bool cpu_parallel_for(size_t count, CpuRangeTask task, void* context) noexcept {
  if (!task) return false;
  if (count == 0) return true;
  if (!g_workers.initialized || g_workers.workerCount == 0) {
    return task(context, 0, count, 0);
  }

  // `minItems` is the minimum useful amount of work per execution lane, not
  // merely the threshold for waking every worker.  Waking two Vita pthreads for
  // a ~150-vertex draw used to split it into ~50-vertex chunks, where condition
  // variable traffic cost more than the decode/transform work itself.  Scale the
  // active lane count with the draw instead and keep small draws on the caller.
  const uint32_t maxLanes = g_workers.workerCount + 1;
  const uint32_t usefulLanes = static_cast<uint32_t>(std::min<size_t>(
      maxLanes, std::max<size_t>(1, count / g_workers.minItems)));
  if (usefulLanes <= 1) return task(context, 0, count, 0);

  const uint32_t lanes = usefulLanes;
  const size_t chunk = (count + lanes - 1) / lanes;
  const size_t mainEnd = std::min(count, chunk);

  pthread_mutex_lock(&g_workers.mutex);
  g_workers.task = task;
  g_workers.context = context;
  g_workers.activeWorkers = 0;
  g_workers.completedWorkers = 0;
  for (uint32_t i = 0; i < g_workers.workerCount; ++i) {
    const size_t begin = std::min(count, chunk * static_cast<size_t>(i + 1));
    const size_t end = std::min(count, begin + chunk);
    const bool laneEnabled = i + 1 < lanes;
    g_workers.begin[i] = begin;
    g_workers.end[i] = end;
    g_workers.active[i] = laneEnabled && begin < end;
    g_workers.result[i] = true;
    if (g_workers.active[i]) ++g_workers.activeWorkers;
  }
  ++g_workers.generation;
  pthread_cond_broadcast(&g_workers.wake);
  pthread_mutex_unlock(&g_workers.mutex);

  const bool mainResult = task(context, 0, mainEnd, 0);

  pthread_mutex_lock(&g_workers.mutex);
  while (g_workers.completedWorkers < g_workers.activeWorkers) {
    pthread_cond_wait(&g_workers.done, &g_workers.mutex);
  }
  bool workersResult = true;
  for (uint32_t i = 0; i < g_workers.workerCount; ++i) {
    if (g_workers.active[i]) workersResult = workersResult && g_workers.result[i];
  }
  g_workers.task = nullptr;
  g_workers.context = nullptr;
  pthread_mutex_unlock(&g_workers.mutex);
  return mainResult && workersResult;
}

uint32_t cpu_worker_threads() noexcept { return g_workers.workerCount; }
uint32_t cpu_execution_lanes() noexcept { return g_workers.workerCount + 1; }
size_t cpu_parallel_min_items() noexcept { return g_workers.minItems; }

#else
namespace {
size_t g_minItems = 512;
}

bool initialize_cpu_workers(uint32_t, size_t minItems) noexcept {
  g_minItems = std::max<size_t>(1, minItems);
  return true;
}
void shutdown_cpu_workers() noexcept {}
bool cpu_parallel_for(size_t count, CpuRangeTask task, void* context) noexcept {
  return task ? task(context, 0, count, 0) : false;
}
uint32_t cpu_worker_threads() noexcept { return 0; }
uint32_t cpu_execution_lanes() noexcept { return 1; }
size_t cpu_parallel_min_items() noexcept { return g_minItems; }
#endif

} // namespace aurora::vita::gfx
