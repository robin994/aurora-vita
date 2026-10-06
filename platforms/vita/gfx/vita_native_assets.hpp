#pragma once
#include "vita_draw_adapter.hpp"
#include "gxm/gxm_texture_layout.hpp"
#include <string>
namespace aurora::vita::gfx {
using NativeAssetReader=bool (*)(const char* path,std::vector<uint8_t>& bytes,void* context);
struct NativeAssetStats {uint64_t textureHits=0,geometryHits=0,misses=0,rejected=0;};
// Configure before starting the GX consumer, clear only after its shutdown.
void configure_native_assets(NativeAssetReader reader,void* context) noexcept;
NativeAssetStats native_asset_stats() noexcept;
std::vector<uint8_t> native_texture_request(const TextureDesc& desc);
std::vector<uint8_t> native_geometry_request(const uint8_t* raw,size_t bytes,uint32_t count,
                                            SourcePrimitive primitive,const VertexDecodeLayout& layout);
std::string native_asset_path(const std::vector<uint8_t>& request);
// Shared compiler/consumer contract. Source bytes are verified exactly in the
// record; the content hash is a lookup only. Original GX data remains fallback.
bool compile_native_asset(const std::vector<uint8_t>& request,std::vector<uint8_t>& record);
bool load_native_texture(const TextureDesc& desc,gxm::CompactTextureData& out,bool allowBc1) noexcept;
bool load_native_geometry(const uint8_t* raw,size_t bytes,uint32_t count,SourcePrimitive primitive,
                          const VertexDecodeLayout& layout,PreparedDraw& out) noexcept;
} // namespace aurora::vita::gfx
