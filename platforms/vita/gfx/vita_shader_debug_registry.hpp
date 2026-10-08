#pragma once

#include <aurora_vita_shader_debug.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace aurora::vita::gfx {

// Only the GX owner registers programs or submits draws. The game thread may
// snapshot/toggle entries concurrently; the draw path never takes a mutex after
// the program has been registered. Slots never move while a renderer is alive.
class ShaderDebugRegistry {
public:
    static constexpr size_t MaxFragments = 1024;

    struct Slot {
        uint64_t hash = 0;
        uint8_t stages = 0;
        bool native = false;
        std::atomic<bool> enabled{true};
        // Native 32-bit atomics avoid libatomic/64-bit CAS in the Vita draw loop.
        std::atomic<uint32_t> draws{0};
        std::atomic<uint32_t> skipped{0};
    };

    bool recording() const noexcept { return recording_.load(std::memory_order_relaxed); }
    bool bypassed() const noexcept { return bypass_.load(std::memory_order_relaxed); }
    void set_recording(bool active) noexcept {
        recording_.store(active, std::memory_order_relaxed);
        if (!active) restore_all();
    }
    void set_bypass(bool active) noexcept { bypass_.store(active, std::memory_order_relaxed); }

    // Call only after the GX owner has been shut down; pointers are invalidated.
    void clear_catalog() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < count_; ++i) {
            slots_[i].hash = 0;
            slots_[i].stages = 0;
            slots_[i].native = false;
            slots_[i].enabled.store(true, std::memory_order_relaxed);
            slots_[i].draws.store(0, std::memory_order_relaxed);
            slots_[i].skipped.store(0, std::memory_order_relaxed);
        }
        count_ = 0;
        bypass_.store(false, std::memory_order_relaxed);
    }

    Slot* register_fragment(uint64_t hash, uint8_t stages, bool native) noexcept {
        if (!recording() || !hash) return nullptr;
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < count_; ++i)
            if (slots_[i].hash == hash) return &slots_[i];
        if (count_ == MaxFragments) return nullptr;
        Slot& slot = slots_[count_++];
        slot.hash = hash;
        slot.stages = stages;
        slot.native = native;
        slot.enabled.store(true, std::memory_order_relaxed);
        slot.draws.store(0, std::memory_order_relaxed);
        slot.skipped.store(0, std::memory_order_relaxed);
        return &slot;
    }

    bool skip_draw(Slot* slot) noexcept {
        if (!slot || !recording() || bypassed()) return false;
        slot->draws.fetch_add(1, std::memory_order_relaxed);
        if (slot->enabled.load(std::memory_order_relaxed)) return false;
        slot->skipped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    size_t snapshot(AuroraVitaDebugFragment* out, size_t capacity) const noexcept {
        if (!out || !capacity) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        size_t written = 0;
        for (size_t i = 0; i < count_ && written < capacity; ++i) {
            const Slot& slot = slots_[i];
            const uint32_t draws = slot.draws.load(std::memory_order_relaxed);
            if (!draws) continue; // Exclude prewarmed or otherwise unused stages.
            out[written++] = {slot.hash, draws, slot.skipped.load(std::memory_order_relaxed),
                              slot.stages, static_cast<uint8_t>(slot.native),
                              static_cast<uint8_t>(slot.enabled.load(std::memory_order_relaxed)), 0};
        }
        return written;
    }

    bool set_enabled(uint64_t hash, bool enabled) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < count_; ++i)
            if (slots_[i].hash == hash) {
                slots_[i].enabled.store(enabled, std::memory_order_relaxed);
                return true;
            }
        return false;
    }

    void restore_all() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < count_; ++i)
            slots_[i].enabled.store(true, std::memory_order_relaxed);
    }

private:
    mutable std::mutex mutex_;
    std::array<Slot, MaxFragments> slots_{};
    size_t count_ = 0;
    std::atomic<bool> recording_{false};
    std::atomic<bool> bypass_{false};
};

inline ShaderDebugRegistry& shader_debug_registry() noexcept {
    static ShaderDebugRegistry registry;
    return registry;
}

} // namespace aurora::vita::gfx
