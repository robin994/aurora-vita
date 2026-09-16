#pragma once
#include "vita_gfx_types.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aurora::vita::gfx {
struct DecodeResult {
  std::vector<uint8_t> rgba;
  uint32_t width = 0;
  uint32_t height = 0;
  bool ok = false;
};
enum class NativeTextureFormat : uint8_t {
  None,
  Intensity8,
  LuminanceAlpha8,
  Rgb565,
};
size_t encoded_texture_size(uint32_t width, uint32_t height, TextureFormat format) noexcept;
size_t encoded_mip_chain_size(uint32_t width, uint32_t height, TextureFormat format, uint8_t mipCount) noexcept;
size_t dxt1_texture_size(uint32_t width, uint32_t height) noexcept;
uint8_t native_texture_bytes_per_pixel(TextureFormat format) noexcept;
bool transcode_texture_native(const TextureDesc& desc, NativeTextureFormat& format, std::vector<uint8_t>& out) noexcept;
bool transcode_cmpr_to_dxt1(const TextureDesc& desc, std::vector<uint8_t>& out) noexcept;
// Cold-upload diagnostics only; disabled during ordinary rendering.
void set_texture_decode_diagnostics(bool enabled) noexcept;
bool decode_texture_rgba8(const TextureDesc& desc, std::vector<uint8_t>& out) noexcept;
DecodeResult decode_texture_rgba8(const TextureDesc& desc) noexcept;
} // namespace aurora::vita::gfx
