#pragma once

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(__vita__)
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace aurora::vita::io {

constexpr size_t DefaultBufferBytes = 16u * 1024u;

#if defined(__vita__)
using Handle = SceUID;
constexpr Handle InvalidHandle = -1;

inline Handle open_read(const char* path) noexcept {
  return path && *path ? sceIoOpen(path, SCE_O_RDONLY, 0) : InvalidHandle;
}
inline Handle open_write(const char* path, bool append) noexcept {
  if(!path || !*path) return InvalidHandle;
  const int flags = SCE_O_WRONLY | SCE_O_CREAT | (append ? SCE_O_APPEND : SCE_O_TRUNC);
  return sceIoOpen(path, flags, 0666);
}
inline int64_t read_raw(Handle fd, void* data, size_t bytes) noexcept {
  return sceIoRead(fd, data, bytes);
}
inline int64_t write_raw(Handle fd, const void* data, size_t bytes) noexcept {
  return sceIoWrite(fd, data, bytes);
}
inline int close_raw(Handle fd) noexcept { return sceIoClose(fd); }
inline int remove_path(const char* path) noexcept { return sceIoRemove(path); }
inline int rename_path(const char* from, const char* to) noexcept { return sceIoRename(from, to); }
inline int mkdir_path(const char* path, int mode=0777) noexcept { return sceIoMkdir(path, mode); }
#else
using Handle = int;
constexpr Handle InvalidHandle = -1;

inline Handle open_read(const char* path) noexcept {
  return path && *path ? ::open(path, O_RDONLY) : InvalidHandle;
}
inline Handle open_write(const char* path, bool append) noexcept {
  if(!path || !*path) return InvalidHandle;
  const int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
  return ::open(path, flags, 0666);
}
inline int64_t read_raw(Handle fd, void* data, size_t bytes) noexcept {
  return ::read(fd, data, bytes);
}
inline int64_t write_raw(Handle fd, const void* data, size_t bytes) noexcept {
  return ::write(fd, data, bytes);
}
inline int close_raw(Handle fd) noexcept { return ::close(fd); }
inline int remove_path(const char* path) noexcept { return ::unlink(path); }
inline int rename_path(const char* from, const char* to) noexcept { return ::rename(from, to); }
inline int mkdir_path(const char* path, int mode=0777) noexcept { return ::mkdir(path, static_cast<mode_t>(mode)); }
#endif

class BufferedReader {
public:
  BufferedReader() = default;
  explicit BufferedReader(const char* path) noexcept { open(path); }
  ~BufferedReader() { close(); }
  BufferedReader(const BufferedReader&) = delete;
  BufferedReader& operator=(const BufferedReader&) = delete;

  bool open(const char* path) noexcept {
    close();
    fd_ = open_read(path);
    begin_ = end_ = 0;
    eof_ = false;
    return fd_ >= 0;
  }
  void close() noexcept {
    if(fd_ >= 0) close_raw(fd_);
    fd_ = InvalidHandle;
    begin_ = end_ = 0;
    eof_ = false;
  }
  bool is_open() const noexcept { return fd_ >= 0; }

  size_t read(void* dst, size_t bytes) noexcept {
    if(!dst || !bytes || fd_ < 0) return 0;
    auto* out = static_cast<uint8_t*>(dst);
    size_t total = 0;
    while(total < bytes) {
      const size_t buffered = end_ - begin_;
      if(buffered) {
        const size_t take = std::min(buffered, bytes - total);
        std::memcpy(out + total, buffer_.data() + begin_, take);
        begin_ += take;
        total += take;
        continue;
      }
      begin_ = end_ = 0;
      const size_t remaining = bytes - total;
      if(remaining >= buffer_.size()) {
        const int64_t got = read_raw(fd_, out + total, remaining);
        if(got <= 0) { eof_ = true; break; }
        total += static_cast<size_t>(got);
        continue;
      }
      const int64_t got = read_raw(fd_, buffer_.data(), buffer_.size());
      if(got <= 0) { eof_ = true; break; }
      end_ = static_cast<size_t>(got);
    }
    return total;
  }

  bool read_exact(void* dst, size_t bytes) noexcept { return read(dst, bytes) == bytes; }

  int get_byte() noexcept {
    uint8_t value = 0;
    return read(&value, 1) == 1 ? static_cast<int>(value) : -1;
  }

  bool eof() noexcept {
    if(begin_ < end_) return false;
    if(eof_) return true;
    const int64_t got = read_raw(fd_, buffer_.data(), buffer_.size());
    if(got <= 0) { eof_ = true; begin_ = end_ = 0; return true; }
    begin_ = 0;
    end_ = static_cast<size_t>(got);
    return false;
  }

private:
  Handle fd_ = InvalidHandle;
  std::array<uint8_t, DefaultBufferBytes> buffer_{};
  size_t begin_ = 0;
  size_t end_ = 0;
  bool eof_ = false;
};

class BufferedWriter {
public:
  BufferedWriter() = default;
  BufferedWriter(const char* path, bool append=false) noexcept { open(path, append); }
  ~BufferedWriter() { close(); }
  BufferedWriter(const BufferedWriter&) = delete;
  BufferedWriter& operator=(const BufferedWriter&) = delete;

  bool open(const char* path, bool append=false) noexcept {
    close();
    fd_ = open_write(path, append);
    used_ = 0;
    failed_ = fd_ < 0;
    return fd_ >= 0;
  }
  bool is_open() const noexcept { return fd_ >= 0 && !failed_; }

  bool write(const void* src, size_t bytes) noexcept {
    if((bytes && !src) || fd_ < 0 || failed_) return false;
    const auto* data = static_cast<const uint8_t*>(src);
    size_t offset = 0;
    while(offset < bytes) {
      if(used_ == 0 && bytes - offset >= buffer_.size()) {
        if(!write_direct(data + offset, bytes - offset)) return false;
        return true;
      }
      const size_t room = buffer_.size() - used_;
      const size_t take = std::min(room, bytes - offset);
      std::memcpy(buffer_.data() + used_, data + offset, take);
      used_ += take;
      offset += take;
      if(used_ == buffer_.size() && !flush()) return false;
    }
    return true;
  }

  bool write_format(const char* fmt, ...) noexcept {
    if(!fmt) return false;
    va_list args;
    va_start(args, fmt);
    const bool ok = write_vformat(fmt, args);
    va_end(args);
    return ok;
  }

  bool write_vformat(const char* fmt, va_list args) noexcept {
    if(!fmt) return false;
    std::array<char, 1024> local{};
    va_list copy;
    va_copy(copy, args);
    const int required = std::vsnprintf(local.data(), local.size(), fmt, copy);
    va_end(copy);
    if(required < 0) return false;
    if(static_cast<size_t>(required) < local.size()) {
      return write(local.data(), static_cast<size_t>(required));
    }
    std::vector<char> dynamic(static_cast<size_t>(required) + 1u);
    va_list second;
    va_copy(second, args);
    const int written = std::vsnprintf(dynamic.data(), dynamic.size(), fmt, second);
    va_end(second);
    return written == required && write(dynamic.data(), static_cast<size_t>(written));
  }

  bool flush() noexcept {
    if(fd_ < 0 || failed_) return false;
    if(!used_) return true;
    const bool ok = write_direct(buffer_.data(), used_);
    if(ok) used_ = 0;
    return ok;
  }

  bool close() noexcept {
    if(fd_ < 0) return !failed_;
    const bool flushed = flush();
    const int rc = close_raw(fd_);
    fd_ = InvalidHandle;
    const bool ok = flushed && rc >= 0 && !failed_;
    used_ = 0;
    return ok;
  }

private:
  bool write_direct(const uint8_t* data, size_t bytes) noexcept {
    size_t total = 0;
    while(total < bytes) {
      const int64_t wrote = write_raw(fd_, data + total, bytes - total);
      if(wrote <= 0) { failed_ = true; return false; }
      total += static_cast<size_t>(wrote);
    }
    return true;
  }

  Handle fd_ = InvalidHandle;
  std::array<uint8_t, DefaultBufferBytes> buffer_{};
  size_t used_ = 0;
  bool failed_ = false;
};

inline bool write_file(const char* path, const void* data, size_t bytes) noexcept {
  BufferedWriter writer(path, false);
  return writer.is_open() && writer.write(data, bytes) && writer.close();
}

inline bool append_file(const char* path, const void* data, size_t bytes) noexcept {
  BufferedWriter writer(path, true);
  return writer.is_open() && writer.write(data, bytes) && writer.close();
}

inline bool replace_file(const char* temporary, const char* finalPath) noexcept {
  if(!temporary || !finalPath) return false;
  (void)remove_path(finalPath);
  if(rename_path(temporary, finalPath) >= 0) return true;
  (void)remove_path(temporary);
  return false;
}

} // namespace aurora::vita::io
