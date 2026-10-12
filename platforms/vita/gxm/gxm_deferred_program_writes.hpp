#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace aurora::vita::gxm {
// Only validated GXP records enter this persistence queue. Live GPU programs
// have independent ownership: rejecting a record never rejects a shader.
class DeferredProgramWrites {
public:
  static constexpr size_t MaxBytes=2*1024*1024, MaxRecords=256;
  static constexpr size_t MaxRecordBytes=256*1024;
  struct Entry { uint64_t hash; uint32_t stage; std::vector<uint8_t> bytes; };
  bool stage(uint64_t hash,uint32_t stage,std::span<const uint8_t> bytes) {
    if((stage!=1&&stage!=2)||bytes.empty()||bytes.size()>MaxRecordBytes) {
      ++dropped_;return false;
    }
    for(auto& entry:entries_)if(entry.hash==hash&&entry.stage==stage) {
      if(entry.bytes.size()==bytes.size()&&std::equal(bytes.begin(),bytes.end(),entry.bytes.begin()))return true;
      if(bytes.size()>MaxBytes-(bytes_-entry.bytes.size())) {++dropped_;return false;}
      std::vector<uint8_t> replacement(bytes.begin(),bytes.end());
      bytes_=bytes_-entry.bytes.size()+replacement.size();
      entry.bytes=std::move(replacement);return true;
    }
    if(entries_.size()>=MaxRecords||bytes.size()>MaxBytes-bytes_) {++dropped_;return false;}
    Entry entry{hash,stage,{bytes.begin(),bytes.end()}};
    entries_.push_back(std::move(entry));bytes_+=bytes.size();return true;
  }
  const std::vector<uint8_t>* find(uint64_t hash,uint32_t stage) const noexcept {
    for(const auto& entry:entries_)if(entry.hash==hash&&entry.stage==stage)return &entry.bytes;
    return nullptr;
  }
  template<class Writer> size_t flush(Writer&& writer) {
    size_t saved=0;
    for(size_t i=0;i<entries_.size();) {
      const auto& entry=entries_[i];
      if(!writer(entry.hash,entry.stage,entry.bytes)) {++errors_;++i;continue;}
      writtenBytes_+=entry.bytes.size();bytes_-=entry.bytes.size();
      entries_.erase(entries_.begin()+i);++saved;
    }
    return saved;
  }
  void clear() noexcept {entries_.clear();bytes_=0;dropped_=errors_=writtenBytes_=0;}
  size_t size() const noexcept {return entries_.size();}
  size_t bytes() const noexcept {return bytes_;}
  uint64_t dropped() const noexcept {return dropped_;}
  uint64_t errors() const noexcept {return errors_;}
  uint64_t written_bytes() const noexcept {return writtenBytes_;}
private:
  std::vector<Entry> entries_;
  size_t bytes_=0;
  uint64_t dropped_=0,errors_=0,writtenBytes_=0;
};
} // namespace aurora::vita::gxm
