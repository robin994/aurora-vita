#include "gxm_program_cache.hpp"
#include "gfx/vita_native_assets.hpp"
#include "../vita_log.hpp"
#include <cstdlib>
#include <cstdio>
#include <dirent.h>
#include <new>
#include <psp2/io/stat.h>

namespace aurora::vita::gxm {
namespace {
std::string cache_file(const std::string& root, uint64_t hash, ProgramStage stage) {
  if (root.empty()) return {};
  char file[40];
  std::snprintf(file, sizeof(file), "/%c-%016llx.gxp",
                stage == ProgramStage::Vertex ? 'v' : 'f',
                static_cast<unsigned long long>(hash));
  return root + file;
}
}

void ProgramBinaryCache::configure(const char* path,bool deferWrites) noexcept {
  pending_.clear();deferWrites_=deferWrites;ramHits_=0;
  root_.clear();
  hits_ = misses_ = archiveHits_ = 0;
  if (!path || !*path) return;
  root_ = std::string(path) + "/gxm-cg-gxp-v1";
  const size_t colon = root_.find(':');
  for (size_t i = colon == std::string::npos ? 0 : colon + 1; i < root_.size(); ++i)
    if (root_[i] == '/') sceIoMkdir(root_.substr(0, i).c_str(), 0777);
  sceIoMkdir(root_.c_str(), 0777);
  AURORA_VITA_LOG_INFO("[aurora-gxm] program_cache abi=gxm-cg-gxp-v1\n");
}

bool ProgramBinaryCache::load(uint64_t sourceHash, ProgramStage stage,
                              std::vector<uint32_t>& words) noexcept {
  if(deferWrites_)if(const auto* cached=pending_.find(sourceHash,static_cast<uint32_t>(stage))) {
    try {
      if(decode_gxm_program_cache(*cached,sourceHash,stage,words)) {++hits_;++ramHits_;return true;}
    } catch(const std::bad_alloc&) {++misses_;return false;}
  }
  const auto path = cache_file(root_, sourceHash, stage);
  std::vector<uint8_t> record;
  FILE* file = path.empty()?nullptr:std::fopen(path.c_str(), "rb");
  if(file) {
    GxmProgramCacheHeader header{};
    if(std::fread(&header,sizeof header,1,file)==1&&header.length&&header.length<=gfx::MaxProgramCacheBytes) {
      record.resize(sizeof header+header.length);std::memcpy(record.data(),&header,sizeof header);
      if(std::fread(record.data()+sizeof header,1,header.length,file)!=header.length||std::fgetc(file)!=EOF)record.clear();
    }
    std::fclose(file);
  }
  if(!decode_gxm_program_cache(record,sourceHash,stage,words)) {
    const auto nativePath=cache_file("native/v1/shaders/gxm-cg-gxp-v1",sourceHash,stage);
    if(!gfx::read_native_blob(nativePath.c_str(),record,sizeof(GxmProgramCacheHeader)+gfx::MaxProgramCacheBytes)||
        !decode_gxm_program_cache(record,sourceHash,stage,words)){++misses_;return false;}
    ++archiveHits_;
    if(archiveHits_<=4||(archiveHits_&(archiveHits_-1))==0)
      AURORA_VITA_LOG_INFO("[aurora-gxm] native_shader hits=%u source=%016llx stage=%u\n",
          archiveHits_,static_cast<unsigned long long>(sourceHash),unsigned(stage));
  }
  ++hits_;
  if (hits_ <= 4 || (hits_ & (hits_ - 1)) == 0)
    AURORA_VITA_LOG_DEBUG("[aurora-gxm] program_cache hits=%u misses=%u\n",hits_,misses_);
  return true;
}

bool ProgramBinaryCache::write_record(uint64_t sourceHash,ProgramStage stage,
                                      const std::vector<uint8_t>& record) const {
  const auto path=cache_file(root_,sourceHash,stage);
  if(path.empty())return false;
  const std::string temporary=path+".tmp";
  FILE* file=std::fopen(temporary.c_str(),"wb");
  if(!file)return false;
  const bool wrote=std::fwrite(record.data(),1,record.size(),file)==record.size();
  const bool closed=std::fclose(file)==0;
  // Atomic replacement preserves the previous cache file on any failure.
  if(wrote&&closed&&std::rename(temporary.c_str(),path.c_str())==0)return true;
  std::remove(temporary.c_str());return false;
}

void ProgramBinaryCache::save(uint64_t sourceHash,ProgramStage stage,
                              const std::vector<uint32_t>& words,size_t byteLength) noexcept {
  if(root_.empty()||words.empty()||!byteLength||byteLength>words.size()*sizeof(uint32_t)||
     byteLength>gfx::MaxProgramCacheBytes)return;
  try {
    GxmProgramCacheHeader header{};
    header.sourceHash=sourceHash;header.length=static_cast<uint32_t>(byteLength);
    header.stage=static_cast<uint32_t>(stage);
    header.binaryHash=gfx::program_cache_hash(words.data(),byteLength);
    if(!valid_gxm_program_cache(header,words.data(),byteLength,sourceHash,stage))return;
    std::vector<uint8_t> record(sizeof header+byteLength);
    std::memcpy(record.data(),&header,sizeof header);
    std::memcpy(record.data()+sizeof header,words.data(),byteLength);
    if(deferWrites_)pending_.stage(sourceHash,header.stage,record);
    else write_record(sourceHash,stage,record);
  } catch(const std::bad_alloc&) {
    // The compiled shader remains valid even when persistence cannot allocate.
  }
}

void ProgramBinaryCache::flush(const char* phase) noexcept {
  if(!deferWrites_)return;
  size_t saved=0;
  try {
    saved=pending_.flush([this](uint64_t hash,uint32_t stage,const auto& bytes) {
      return write_record(hash,static_cast<ProgramStage>(stage),bytes);
    });
  } catch(const std::bad_alloc&) {}
  AURORA_VITA_LOG_INFO("[aurora-gxm] deferred_shader_cache flush=%s saved=%u written_bytes=%llu remaining=%u queued_bytes=%u dropped=%llu errors=%llu ram_hits=%llu\n",
      phase?phase:"unknown",unsigned(saved),static_cast<unsigned long long>(pending_.written_bytes()),
      unsigned(pending_.size()),unsigned(pending_.bytes()),static_cast<unsigned long long>(pending_.dropped()),
      static_cast<unsigned long long>(pending_.errors()),static_cast<unsigned long long>(ramHits_));
}

size_t ProgramBinaryCache::preload(std::vector<PreloadedProgram>& programs, size_t maxPrograms) noexcept {
  programs.clear();
  if (root_.empty() || !maxPrograms) return 0;
  DIR* dir = opendir(root_.c_str());
  if (!dir) return 0;
  while (programs.size() < maxPrograms) {
    const dirent* entry = readdir(dir);
    if (!entry) break;
    const char* name = entry->d_name;
    if (!name || (name[0] != 'v' && name[0] != 'f') || name[1] != '-') continue;
    char* end = nullptr;
    const uint64_t hash = std::strtoull(name + 2, &end, 16);
    if (!hash || !end || std::strcmp(end, ".gxp") != 0) continue;
    const ProgramStage stage = name[0] == 'v' ? ProgramStage::Vertex : ProgramStage::Fragment;
    PreloadedProgram program{};
    program.sourceHash = hash;
    program.stage = stage;
    if (!load(hash, stage, program.words) || program.words.empty()) continue;
    programs.push_back(std::move(program));
  }
  closedir(dir);
  return programs.size();
}
} // namespace aurora::vita::gxm
