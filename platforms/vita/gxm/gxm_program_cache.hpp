#pragma once
#include "gfx/vita_program_binary_cache.hpp"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace aurora::vita::gxm {
enum class ProgramStage : uint32_t { Vertex = 1, Fragment = 2 };

struct GxmProgramCacheHeader {
  uint32_t magic = 0x41564758u; // "AVGX"
  uint32_t version = 1;
  uint64_t sourceHash = 0;
  uint64_t binaryHash = 0;
  uint32_t length = 0;
  uint32_t stage = 0;
};
static_assert(sizeof(GxmProgramCacheHeader) == 32);

inline uint64_t gxm_program_source_hash(const char* source, ProgramStage stage) noexcept {
  static constexpr char contract[] = "aurora-gxm-cg-gxp-v1";
  auto hash = gfx::program_cache_hash(contract, sizeof(contract));
  const auto rawStage = static_cast<uint32_t>(stage);
  hash = gfx::program_cache_hash(&rawStage, sizeof(rawStage), hash);
  return source ? gfx::program_cache_hash(source, std::strlen(source) + 1, hash) : hash;
}

inline bool valid_gxm_program_cache(const GxmProgramCacheHeader& header, const void* payload,
                                    size_t bytes, uint64_t expectedSource, ProgramStage stage) noexcept {
  return payload && header.magic == 0x41564758u && header.version == 1 &&
      header.sourceHash == expectedSource && header.stage == static_cast<uint32_t>(stage) &&
      header.length == bytes && bytes >= 16 && bytes <= gfx::MaxProgramCacheBytes &&
      gfx::program_cache_hash(payload, bytes) == header.binaryHash;
}

class ProgramBinaryCache {
public:
  void configure(const char* path) noexcept;
  bool load(uint64_t sourceHash, ProgramStage stage, std::vector<uint32_t>& words) noexcept;
  void save(uint64_t sourceHash, ProgramStage stage, const std::vector<uint32_t>& words,
            size_t byteLength) noexcept;
  uint32_t hits() const noexcept { return hits_; }
  uint32_t misses() const noexcept { return misses_; }
private:
  std::string root_;
  uint32_t hits_ = 0;
  uint32_t misses_ = 0;
};
} // namespace aurora::vita::gxm
