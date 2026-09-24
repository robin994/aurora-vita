#pragma once
#include "vita_vertex_decode.hpp"
#include <cstdint>
#include <cstring>

namespace aurora::vita::gfx {
namespace detail {
template<unsigned Words>
inline void copy_vertex_words(uint8_t* destination, const void* source) noexcept {
#if defined(__GNUC__)
  // Streaming layouts are word aligned. may_alias preserves float/color bits
  // without strict-aliasing violations; unusual layouts retain bounded memcpy.
  if (((reinterpret_cast<uintptr_t>(destination) | reinterpret_cast<uintptr_t>(source)) & 3u) == 0) {
    using AliasWord = uint32_t __attribute__((__may_alias__));
    auto* out = reinterpret_cast<AliasWord*>(destination);
    const auto* in = reinterpret_cast<const AliasWord*>(source);
    out[0] = in[0];
    if constexpr (Words >= 3) { out[1] = in[1]; out[2] = in[2]; }
    if constexpr (Words == 4) out[3] = in[3];
    return;
  }
#endif
  std::memcpy(destination, source, Words * sizeof(uint32_t));
}
}

// Header-visible to the game adapter as well as Aurora's own streaming path.
// Only already prepared canonical attributes are copied; no FP arithmetic.
inline void pack_gpu_vertex_inline(uint8_t* dst, const CanonicalVertex& src,
                                   const VertexLayout& layout) noexcept {
  for (unsigned i = 0; i < layout.count; ++i) {
    const auto& a = layout.attributes[i];
    if (a.location == 0) detail::copy_vertex_words<4>(dst+a.offset, src.position);
    else if (a.location == 1) detail::copy_vertex_words<1>(dst+a.offset, src.color0);
    else if (a.location == 2) detail::copy_vertex_words<1>(dst+a.offset, src.color1);
    else if (a.location >= 3 && a.location < 3+MaxTextures)
      detail::copy_vertex_words<3>(dst+a.offset, src.texcoord[a.location-3]);
    else if (a.location == 11) detail::copy_vertex_words<3>(dst+a.offset, src.normal);
    else if (a.location == 12) detail::copy_vertex_words<3>(dst+a.offset, src.binormal);
    else if (a.location == 13) detail::copy_vertex_words<3>(dst+a.offset, src.tangent);
    else if (a.location == 14) {
      if(a.scalar==VertexScalar::F32&&a.components==3) {
        // Three exact 24-bit integers carry PN plus eight raw GX texture-matrix
        // selectors without consuming eight additional GXM attributes.
        const uint8_t pn=src.pnMatrixIndex;
        const auto pack3=[](uint8_t a,uint8_t b,uint8_t c) noexcept {
          return static_cast<float>(uint32_t(a)|(uint32_t(b)<<8)|(uint32_t(c)<<16));
        };
        const float packed[3]{
          pack3(pn,src.texMatrixIndex[0],src.texMatrixIndex[1]),
          pack3(src.texMatrixIndex[2],src.texMatrixIndex[3],src.texMatrixIndex[4]),
          pack3(src.texMatrixIndex[5],src.texMatrixIndex[6],src.texMatrixIndex[7])
        };
        detail::copy_vertex_words<3>(dst+a.offset,packed);
      } else dst[a.offset]=src.pnMatrixIndex;
    }
  }
}
}
