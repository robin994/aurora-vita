#include "../gx/gx.hpp"
#include "../dolphin/vi/vi_internal.hpp"
#include "../../platforms/vita/aurora_vita_backend.hpp"
#include "../../platforms/vita/gx/aurora_vita_draw_sink.hpp"

#include <algorithm>
#include <cmath>

namespace aurora::gx {

GXState g_gxState{};

void initialize() noexcept {
  g_gxState = GXState{};
  g_gxState.dstAlpha = UINT32_MAX;
}

void shutdown() noexcept {
  clear_copy_texture_cache();
  clear_display_copy_cache();
}

void clear_copy_texture_cache() noexcept {
  g_gxState.copyTextures.clear();
  g_gxState.copyTextureCache.clear();
  aurora::vita::draw_sink().clear_copy_textures();
}

void clear_display_copy_cache() noexcept {
  g_gxState.displayCopyTexture.reset();
  g_gxState.displayCopyWidth = 0;
  g_gxState.displayCopyHeight = 0;
}

void set_display_copy_present_source() noexcept {}

void evict_copy_texture(const void* dest) noexcept {
  if (!dest) return;
  g_gxState.copyTextures.erase(dest);
  for (auto it = g_gxState.copyTextureCache.begin(); it != g_gxState.copyTextureCache.end();) {
    if (it->first.dest == dest) {
      it = g_gxState.copyTextureCache.erase(it);
    } else {
      ++it;
    }
  }
  aurora::vita::draw_sink().evict_copy_tex(dest);
}

void evict_texture_object(u32 texObjId) noexcept {
  for (auto& obj : g_gxState.loadedTextures) {
    if (obj.texObjId == texObjId) obj.set_no_cache(true);
  }
}

void evict_tlut_object(u32 tlutObjId) noexcept {
  for (auto& obj : g_gxState.loadedTluts) {
    if (obj.tlutObjId == tlutObjId) obj.set_no_cache(true);
  }
}

void invalidate_static_texture_cache() noexcept {
  for (auto& texture : g_gxState.textures) texture.reset();
  for (auto& texture : g_gxState.loadedTextures) texture.set_no_cache(true);
  g_gxState.stateDirty = true;
}

Vec2<uint32_t> logical_fb_size() noexcept {
  return vi::configured_fb_size();
}

gfx::Viewport map_logical_viewport(const gfx::Viewport& logicalViewport) noexcept {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) return logicalViewport;
  const auto [logicalWidth, logicalHeight] = logical_fb_size();
  const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
  if (!logicalWidth || !logicalHeight || !targetWidth || !targetHeight) return logicalViewport;
  const float sx = static_cast<float>(targetWidth) / static_cast<float>(logicalWidth);
  const float sy = static_cast<float>(targetHeight) / static_cast<float>(logicalHeight);
  return {
      logicalViewport.left * sx,
      logicalViewport.top * sy,
      logicalViewport.width * sx,
      logicalViewport.height * sy,
      logicalViewport.znear,
      logicalViewport.zfar,
  };
}

gfx::ClipRect map_logical_scissor(const gfx::ClipRect& logicalScissor) noexcept {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) return logicalScissor;
  const auto [logicalWidth, logicalHeight] = logical_fb_size();
  const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
  if (!logicalWidth || !logicalHeight || !targetWidth || !targetHeight) return logicalScissor;

  const float sx = static_cast<float>(targetWidth) / static_cast<float>(logicalWidth);
  const float sy = static_cast<float>(targetHeight) / static_cast<float>(logicalHeight);
  const int32_t left = std::clamp(static_cast<int32_t>(std::floor(logicalScissor.x * sx)), 0,
                                  static_cast<int32_t>(targetWidth));
  const int32_t top = std::clamp(static_cast<int32_t>(std::floor(logicalScissor.y * sy)), 0,
                                 static_cast<int32_t>(targetHeight));
  const int32_t right = std::clamp(static_cast<int32_t>(std::ceil((logicalScissor.x + logicalScissor.width) * sx)),
                                   left, static_cast<int32_t>(targetWidth));
  const int32_t bottom = std::clamp(static_cast<int32_t>(std::ceil((logicalScissor.y + logicalScissor.height) * sy)),
                                    top, static_cast<int32_t>(targetHeight));
  return {left, top, right - left, bottom - top};
}

MappedRenderState map_logical_render_state() noexcept {
  return {map_logical_viewport(g_gxState.logicalViewport), map_logical_scissor(g_gxState.logicalScissor)};
}

void set_logical_viewport(const gfx::Viewport& viewport) noexcept {
  if (viewport != g_gxState.logicalViewport) g_gxState.stateDirty = true;
  g_gxState.logicalViewport = viewport;
  set_render_viewport(map_logical_viewport(viewport));
}

void set_render_viewport(const gfx::Viewport& viewport) noexcept {
  if (viewport != g_gxState.renderViewport) g_gxState.stateDirty = true;
  g_gxState.renderViewport = viewport;
}

void set_logical_scissor(const gfx::ClipRect& scissor) noexcept {
  if (scissor != g_gxState.logicalScissor) g_gxState.stateDirty = true;
  g_gxState.logicalScissor = scissor;
  set_render_scissor(map_logical_scissor(scissor));
}

void set_render_scissor(const gfx::ClipRect& scissor) noexcept {
  if (scissor != g_gxState.renderScissor) g_gxState.stateDirty = true;
  g_gxState.renderScissor = scissor;
}

const gfx::TextureBind& get_texture(GXTexMapID id) noexcept {
  return g_gxState.textures[static_cast<size_t>(id)];
}

void resolve_sampled_textures(const ShaderInfo&) noexcept {}

void notify_copy_texture_created() noexcept {
  g_gxState.stateDirty = true;
}

u8 comp_type_size(GXAttr attr, GXCompType type) noexcept {
  switch (attr) {
  case GX_VA_PNMTXIDX:
  case GX_VA_TEX0MTXIDX:
  case GX_VA_TEX1MTXIDX:
  case GX_VA_TEX2MTXIDX:
  case GX_VA_TEX3MTXIDX:
  case GX_VA_TEX4MTXIDX:
  case GX_VA_TEX5MTXIDX:
  case GX_VA_TEX6MTXIDX:
  case GX_VA_TEX7MTXIDX:
    return 1;
  case GX_VA_CLR0:
  case GX_VA_CLR1:
    switch (type) {
    case GX_RGB565:
    case GX_RGBA4: return 2;
    case GX_RGB8:
    case GX_RGBA6: return 3;
    case GX_RGBX8:
    case GX_RGBA8: return 4;
    default: return 0;
    }
  default:
    switch (type) {
    case GX_U8:
    case GX_S8: return 1;
    case GX_U16:
    case GX_S16: return 2;
    case GX_F32: return 4;
    default: return 0;
    }
  }
}

u8 comp_cnt_count(GXAttr attr, GXCompCnt cnt) noexcept {
  switch (attr) {
  case GX_VA_PNMTXIDX:
  case GX_VA_TEX0MTXIDX:
  case GX_VA_TEX1MTXIDX:
  case GX_VA_TEX2MTXIDX:
  case GX_VA_TEX3MTXIDX:
  case GX_VA_TEX4MTXIDX:
  case GX_VA_TEX5MTXIDX:
  case GX_VA_TEX6MTXIDX:
  case GX_VA_TEX7MTXIDX:
    return 1;
  case GX_VA_POS:
    return cnt == GX_POS_XY ? 2 : cnt == GX_POS_XYZ ? 3 : 0;
  case GX_VA_NRM:
    return cnt == GX_NRM_XYZ ? 3 : (cnt == GX_NRM_NBT || cnt == GX_NRM_NBT3) ? 9 : 0;
  case GX_VA_CLR0:
  case GX_VA_CLR1:
    return 1;
  case GX_VA_TEX0:
  case GX_VA_TEX1:
  case GX_VA_TEX2:
  case GX_VA_TEX3:
  case GX_VA_TEX4:
  case GX_VA_TEX5:
  case GX_VA_TEX6:
  case GX_VA_TEX7:
    return cnt == GX_TEX_S ? 1 : cnt == GX_TEX_ST ? 2 : 0;
  default:
    return 0;
  }
}

} // namespace aurora::gx
