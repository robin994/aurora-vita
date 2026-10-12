#pragma once

#include <aurora_vita_view_draw_capture.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

namespace aurora::vita::gfx {

// One GX/GXM consumer owns mark/draw/complete; start/stop/read is performed only
// after the game thread has joined that consumer at an end_frame boundary.
// No file I/O, locking, or heap allocation takes place in the draw path.
class ViewDrawCapture {
public:
    bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }
    bool payload_enabled() const noexcept { return capturePayloads_.load(std::memory_order_relaxed) && enabled(); }
    bool texture_probe_enabled() const noexcept { return textureProbe_.load(std::memory_order_relaxed); }
    void set_texture_probe(bool enable) noexcept { textureProbe_.store(enable,std::memory_order_relaxed); }

    bool start(size_t limit, bool capturePayloads = false) noexcept {
        return start_filtered(limit,capturePayloads,0,0,0);
    }

    bool start_filtered(size_t limit,bool capturePayloads,uint32_t first,uint32_t last,
                        uint64_t fragmentHash) noexcept {
        if (enabled() || !limit || limit > 65536) return false;
        if (((first==0)!=(last==0)) || first>last || last>16384 ||
            (fragmentHash && first)) return false;
        std::unique_ptr<AuroraViewDrawRecord[]> next(new (std::nothrow) AuroraViewDrawRecord[limit]{});
        if (!next) return false;
        std::unique_ptr<AuroraViewTextureRecord[]> textures(
            new (std::nothrow) AuroraViewTextureRecord[4096]{});
        if(!textures) return false;
        records_ = std::move(next);
        textures_=std::move(textures);
        textureCount_=0;
        capacity_ = limit;
        count_ = 0;
        lost_.store(0, std::memory_order_relaxed);
        frame_ = 0;
        consumer_frame_ = 0;
        view_ = 255;
        draw_ = 0;
        suppressFirst_=first;
        suppressLast_=last;
        suppressHash_=fragmentHash;
        capturePayloads_.store(capturePayloads, std::memory_order_relaxed);
        enabled_.store(true, std::memory_order_release);
        return true;
    }

    void stop() noexcept { enabled_.store(false, std::memory_order_release); }

    void mark(uint64_t producerFrame, uint32_t view) noexcept {
        if (!enabled()) return;
        if (!producerFrame || view > 255 || (view >= 34 && view != 255) ||
            (frame_ && producerFrame < frame_)) { ++lost_; return; }
        if (producerFrame != frame_) draw_ = 0;
        frame_ = producerFrame;
        view_ = static_cast<uint8_t>(view);
        AuroraViewDrawRecord marker{};
        marker.type = AURORA_VIEW_DRAW_MARKER;
        marker.producer_frame = frame_;
        marker.view = view_;
        append(marker);
    }

    void draw(const AuroraViewDrawRecord& drawRecord) noexcept {
        if (!enabled()) return;
        if (!frame_) { ++lost_; return; }
        AuroraViewDrawRecord row = drawRecord;
        row.type = AURORA_VIEW_DRAW_DRAW;
        row.view = view_;
        row.producer_frame = frame_;
        row.logical_draw = ++draw_;
        append(row);
    }

    void suppressed_draw() noexcept { if (enabled()) ++lost_; }
    bool should_suppress_next(uint64_t fragmentHash) const noexcept {
        if(!enabled())return false;
        const uint32_t next=draw_+1;
        return (suppressHash_ && fragmentHash==suppressHash_) ||
               (suppressFirst_ && next>=suppressFirst_ && next<=suppressLast_);
    }
    bool texture(const AuroraViewTextureRecord& record) noexcept {
        if(!enabled() || !textures_ || !record.handle)return false;
        for(size_t i=0;i<textureCount_;++i)
            if(textures_[i].handle==record.handle){
                textures_[i]=record;return true;
            }
        if(textureCount_>=4096){++lost_;return false;}
        textures_[textureCount_++]=record;
        return true;
    }
    size_t texture_count() const noexcept {return textureCount_;}
    size_t read_texture(size_t start,AuroraViewTextureRecord* out,size_t maximum) const noexcept {
        if(enabled()||!out||!maximum||start>=textureCount_)return 0;
        const size_t n=std::min(textureCount_-start,maximum);
        std::memcpy(out,textures_.get()+start,n*sizeof(*out));
        return n;
    }

    void complete(uint64_t consumerFrame) noexcept {
        if (!enabled()) return;
        if (!frame_ || !consumerFrame || consumerFrame < consumer_frame_) {
            ++lost_;
            return;
        }
        consumer_frame_ = consumerFrame;
        AuroraViewDrawRecord row{};
        row.type = AURORA_VIEW_DRAW_FRAME_COMPLETE;
        row.producer_frame = frame_;
        row.consumer_frame = consumerFrame;
        append(row);
        // The next draw must have a new ordered marker, never inherit a view.
        frame_ = 0;
        view_ = 255;
    }

    size_t count() const noexcept { return count_; }
    size_t capacity() const noexcept { return capacity_; }
    uint64_t lost() const noexcept { return lost_.load(std::memory_order_relaxed); }

    size_t read(size_t start, AuroraViewDrawRecord* out, size_t maximum) const noexcept {
        if (enabled() || !out || !maximum || start >= count_) return 0;
        const size_t total = std::min(count_ - start, maximum);
        std::memcpy(out, records_.get() + start, total * sizeof(*out));
        return total;
    }

private:
    void append(AuroraViewDrawRecord& record) noexcept {
        if (count_ >= capacity_) { ++lost_; return; }
        record.sequence = count_ + 1;
        records_[count_++] = record;
    }

    std::atomic<bool> enabled_{false};
    std::atomic<bool> capturePayloads_{false};
    std::atomic<bool> textureProbe_{false};
    std::unique_ptr<AuroraViewDrawRecord[]> records_;
    std::unique_ptr<AuroraViewTextureRecord[]> textures_;
    size_t capacity_ = 0;
    size_t count_ = 0;
    size_t textureCount_ = 0;
    uint32_t suppressFirst_=0,suppressLast_=0;
    uint64_t suppressHash_=0;
    // The on-device queue is bounded to 65536 rows; 32-bit atomics avoid an
    // ARMv7 libatomic dependency on cross-thread overflow/error increments.
    std::atomic<uint32_t> lost_{0};
    uint64_t frame_ = 0;
    uint64_t consumer_frame_ = 0;
    uint32_t draw_ = 0;
    uint8_t view_ = 255;
};

inline ViewDrawCapture& view_draw_capture() noexcept {
    static ViewDrawCapture instance;
    return instance;
}

} // namespace aurora::vita::gfx
