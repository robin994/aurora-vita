#pragma once

#include "../internal.hpp"

#include <cstring>

namespace aurora::gx::fifo {

namespace detail {
extern uint8_t* sBufferData;
extern uint32_t sBufferSize;
extern uint32_t sBufferCapacity;
extern bool sInDisplayList;
extern uint8_t* sDlBuffer;
extern uint32_t sDlSize;
extern uint32_t sDlWritePos;
extern uint32_t sStableSourceOffset;
extern uint32_t sStableSourceBytes;
extern const uint8_t* sStableSource;
} // namespace detail

void init();

#if defined(MKW_TARGET_VITA) || defined(TARGET_VITA)
using VitaWorkerTask = void (*)(void*);

// The Vita port runs GX decode/translation and renderer submission on a
// dedicated consumer thread. The game thread remains the sole FIFO producer.
bool start_worker();
void shutdown_worker();
bool worker_running();

// `drain()` only seals/enqueues the producer buffer on Vita. Use these barriers
// for APIs that need the decoded GX state or renderer side effects immediately.
void drain_sync();
void wait_idle();
void run_sync(VitaWorkerTask task, void* context);
void process_sync(const uint8_t* data, uint32_t size, bool bigEndian);
#endif

// Out-of-line slow path: grows internal buffer then appends data
void write_data_grow(const void* data, uint32_t length);

inline void write_data(const void* data, const uint32_t length) {
  if (!detail::sInDisplayList)
    LIKELY {
      if (detail::sBufferSize + length <= detail::sBufferCapacity)
        LIKELY {
          std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
          detail::sBufferSize += length;
          return;
        }
      write_data_grow(data, length);
    }
  else if (detail::sDlWritePos + length <= detail::sDlSize) {
    std::memcpy(detail::sDlBuffer + detail::sDlWritePos, data, length);
    detail::sDlWritePos += length;
  }
}

// Append one permanent display-list span while remembering its original guest
// address. The command processor can then propagate a stable geometry identity
// even though the bytes themselves are copied into the fast FIFO buffer.
inline void write_stable_data(const void* data,const uint32_t length) {
  if(detail::sInDisplayList) { write_data(data,length); return; }
  detail::sStableSourceOffset=detail::sBufferSize;
  detail::sStableSourceBytes=length;
  detail::sStableSource=static_cast<const uint8_t*>(data);
  write_data(data,length);
}

inline void write_u8(const uint8_t val) {
  if (!detail::sInDisplayList)
    LIKELY {
      if (detail::sBufferSize < detail::sBufferCapacity)
        LIKELY {
          detail::sBufferData[detail::sBufferSize++] = val;
          return;
        }
      write_data_grow(&val, 1);
    }
  else if (detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = val;
  }
}

inline void write_u16(const uint16_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_u32(const uint32_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_u64(const uint64_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_f32(const float val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

// Display list recording
void begin_display_list(uint8_t* buf, uint32_t size);
uint32_t end_display_list();
bool in_display_list();

// Drain the internal FIFO buffer through the command processor
void drain();

// Internal buffer inspection
const uint8_t* get_buffer_data();
uint32_t get_buffer_size();
const uint8_t* stable_source_for(const uint8_t* data,size_t bytes);
void clear_buffer();

} // namespace aurora::gx::fifo
