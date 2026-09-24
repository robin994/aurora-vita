#include "gxm_program_cache.hpp"
#include <cstdlib>
#include <cstdio>
#include "../vita_diag.hpp"
#include <dirent.h>
#include <psp2/io/stat.h>

namespace aurora::vita::gxm {
namespace {
char stage_directory(ProgramStage stage) noexcept {
  return stage==ProgramStage::Vertex?'v':'f';
}

char shard_directory(uint64_t hash) noexcept {
  static constexpr char Hex[]="0123456789abcdef";
  return Hex[(hash>>60u)&0x0fu];
}

std::string cache_file(const std::string& root, uint64_t hash, ProgramStage stage) {
  if (root.empty()) return {};
  char file[48];
  std::snprintf(file, sizeof(file), "/%c/%c/%016llx.gxp",
                stage_directory(stage),shard_directory(hash),
                static_cast<unsigned long long>(hash));
  return root + file;
}

std::string shard_path(const std::string& root, ProgramStage stage, unsigned shard) {
  static constexpr char Hex[]="0123456789abcdef";
  if(root.empty()||shard>=16)return {};
  char suffix[8];
  std::snprintf(suffix,sizeof(suffix),"/%c/%c",stage_directory(stage),Hex[shard]);
  return root+suffix;
}

bool parse_cache_name(const char* name,uint64_t& hash) noexcept {
  if(!name||!*name)return false;
  char* end=nullptr;
  hash=std::strtoull(name,&end,16);
  return hash&&end&&std::strcmp(end,".gxp")==0;
}
}

void ProgramBinaryCache::configure(const char* path) noexcept {
  root_.clear();
  hits_ = misses_ = 0;
  if (!path || !*path) return;
  root_ = std::string(path) + "/gxm-cg-gxp-v1";
  const size_t colon = root_.find(':');
  for (size_t i = colon == std::string::npos ? 0 : colon + 1; i < root_.size(); ++i)
    if (root_[i] == '/') sceIoMkdir(root_.substr(0, i).c_str(), 0777);
  sceIoMkdir(root_.c_str(), 0777);
  constexpr ProgramStage Stages[]{ProgramStage::Vertex,ProgramStage::Fragment};
  for(const auto stage:Stages) {
    const std::string stageRoot=root_+"/"+stage_directory(stage);
    sceIoMkdir(stageRoot.c_str(),0777);
    for(unsigned shard=0;shard<16;++shard) {
      const auto dir=shard_path(root_,stage,shard);
      sceIoMkdir(dir.c_str(),0777);
    }
  }
  AURORA_VITA_DIAGF("[aurora-gxm] program_cache abi=gxm-cg-gxp-v1 shards=32 root=%s\n",root_.c_str());
}

bool ProgramBinaryCache::load(uint64_t sourceHash, ProgramStage stage,
                              std::vector<uint32_t>& words) noexcept {
  const auto path = cache_file(root_, sourceHash, stage);
  if (path.empty()) return false;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) { ++misses_; return false; }
  GxmProgramCacheHeader header{};
  if (std::fread(&header, sizeof(header), 1, file) != 1 || !header.length ||
      header.length > gfx::MaxProgramCacheBytes) {
    std::fclose(file); ++misses_; return false;
  }
  std::vector<uint8_t> bytes(header.length);
  const bool readOk = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size() &&
      std::fgetc(file) == EOF;
  std::fclose(file);
  if (!readOk || !valid_gxm_program_cache(header, bytes.data(), bytes.size(), sourceHash, stage)) {
    ++misses_;
    return false;
  }
  words.assign((bytes.size() + 3u) / 4u, 0);
  std::memcpy(words.data(), bytes.data(), bytes.size());
  ++hits_;
  if (hits_ <= 4 || (hits_ & (hits_ - 1)) == 0)
    AURORA_VITA_DIAGF( "[aurora-gxm] program_cache hits=%u misses=%u\n", hits_, misses_);
  return true;
}

void ProgramBinaryCache::save(uint64_t sourceHash, ProgramStage stage,
                              const std::vector<uint32_t>& words, size_t byteLength) noexcept {
  const auto path = cache_file(root_, sourceHash, stage);
  if (path.empty() || words.empty() || !byteLength || byteLength > words.size() * sizeof(uint32_t) ||
      byteLength > gfx::MaxProgramCacheBytes) return;
  GxmProgramCacheHeader header{};
  header.sourceHash = sourceHash;
  header.length = static_cast<uint32_t>(byteLength);
  header.stage = static_cast<uint32_t>(stage);
  header.binaryHash = gfx::program_cache_hash(words.data(), byteLength);
  if (!valid_gxm_program_cache(header, words.data(), byteLength, sourceHash, stage)) return;
  const std::string temporary = path + ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "wb");
  if (!file) return;
  const bool wrote = std::fwrite(&header, sizeof(header), 1, file) == 1 &&
      std::fwrite(words.data(), 1, byteLength, file) == byteLength;
  const bool closed = std::fclose(file) == 0;
  if (wrote && closed) {
    std::remove(path.c_str());
    if (std::rename(temporary.c_str(), path.c_str()) == 0) return;
  }
  std::remove(temporary.c_str());
}

size_t ProgramBinaryCache::preload(std::vector<PreloadedProgram>& programs, size_t maxPrograms) noexcept {
  programs.clear();
  if (root_.empty() || !maxPrograms) return 0;
  constexpr ProgramStage Stages[]{ProgramStage::Vertex,ProgramStage::Fragment};
  for(const auto stage:Stages) {
    for(unsigned shard=0;shard<16&&programs.size()<maxPrograms;++shard) {
      const auto dirPath=shard_path(root_,stage,shard);
      DIR* dir=opendir(dirPath.c_str());
      if(!dir)continue;
      while(programs.size()<maxPrograms) {
        const dirent* entry=readdir(dir);
        if(!entry)break;
        uint64_t hash=0;
        if(!parse_cache_name(entry->d_name,hash)||shard_directory(hash)!=dirPath.back())continue;
        PreloadedProgram program{};
        program.sourceHash=hash;
        program.stage=stage;
        if(!load(hash,stage,program.words)||program.words.empty())continue;
        programs.push_back(std::move(program));
      }
      closedir(dir);
    }
  }
  return programs.size();
}
} // namespace aurora::vita::gxm
