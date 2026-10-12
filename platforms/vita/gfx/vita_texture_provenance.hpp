#pragma once
#include <cstddef>
#include <cstdint>

namespace aurora::vita::gfx {

// Encoded GX source hashes are diagnostic identities, never cache keys.
// Match scripts/audit_mkw_brres_materials.py (FNV-1a 64, at most 1 MiB).
inline constexpr size_t TextureFingerprintLimit = 1024u * 1024u;
inline constexpr size_t bounded_texture_fingerprint_bytes(size_t bytes) noexcept {
  return bytes < TextureFingerprintLimit ? bytes : TextureFingerprintLimit;
}
inline uint64_t fingerprint_texture_source(const void* source,size_t bytes) noexcept {
  if(!source || !bytes)return 0;
  const auto* data=static_cast<const uint8_t*>(source);
  uint64_t result=UINT64_C(14695981039346656037);
  for(size_t i=0;i<bytes;++i)
    result=(result^data[i])*UINT64_C(1099511628211);
  return result;
}

} // namespace aurora::vita::gfx
