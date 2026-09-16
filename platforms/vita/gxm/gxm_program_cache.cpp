#include "gxm_program_cache.hpp"
#include <cstdio>
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

void ProgramBinaryCache::configure(const char* path) noexcept {
  root_.clear();
  hits_ = misses_ = 0;
  if (!path || !*path) return;
  root_ = std::string(path) + "/gxm-cg-gxp-v1";
  const size_t colon = root_.find(':');
  for (size_t i = colon == std::string::npos ? 0 : colon + 1; i < root_.size(); ++i)
    if (root_[i] == '/') sceIoMkdir(root_.substr(0, i).c_str(), 0777);
  sceIoMkdir(root_.c_str(), 0777);
  std::fprintf(stderr, "[aurora-gxm] program_cache abi=gxm-cg-gxp-v1\n");
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
    std::fprintf(stderr, "[aurora-gxm] program_cache hits=%u misses=%u\n", hits_, misses_);
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
} // namespace aurora::vita::gxm
