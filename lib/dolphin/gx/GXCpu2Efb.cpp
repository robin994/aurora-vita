#include "gx.hpp"

#if defined(MKW_TARGET_VITA)
#include "../../vita/gfx_frontend.hpp"
#include "../../gx/fifo.hpp"
#else
#include "../../gfx/depth_peek.hpp"
#endif

#include <dolphin/gx/GXCpu2Efb.h>

#if defined(MKW_TARGET_VITA)
namespace {
GXBool s_colorUpdate = GX_TRUE;
GXBool s_alphaUpdate = GX_TRUE;
GXBool s_depthUpdate = GX_TRUE;
GXCompare s_depthFunc = GX_LEQUAL;
}

void GXPokeAlphaMode(GXCompare, u8) {}
void GXPokeAlphaRead(GXAlphaReadMode) {}
void GXPokeAlphaUpdate(GXBool update_enable) { s_alphaUpdate = update_enable; }
void GXPokeBlendMode(GXBlendMode, GXBlendFactor, GXBlendFactor, GXLogicOp) {}
void GXPokeColorUpdate(GXBool update_enable) { s_colorUpdate = update_enable; }
void GXPokeDstAlpha(GXBool, u8) {}
void GXPokeDither(GXBool) {}
void GXPokeZMode(GXBool, GXCompare func, GXBool update_enable) {
  s_depthFunc = func;
  s_depthUpdate = update_enable;
}

void GXPeekARGB(u16, u16, u32* color) {
  if (color != nullptr) *color = 0xff000000u;
}

void GXPokeARGB(u16, u16, u32) {
  (void)s_colorUpdate;
  (void)s_alphaUpdate;
}

void GXPeekZ(u16, u16, u32* z) {
  aurora::gx::fifo::drain_sync();
  if (z != nullptr) *z = g_gxState.clearDepth & 0x00ffffffu;
}

void GXPokeZ(u16, u16, u32) {
  (void)s_depthUpdate;
  (void)s_depthFunc;
}

u32 GXCompressZ16(u32 z24, GXZFmt16) { return (z24 >> 8) & 0xffffu; }
u32 GXDecompressZ16(u32 z16, GXZFmt16) { return (z16 & 0xffffu) << 8; }
#else
void GXPeekZ(u16 x, u16 y, u32* z) {
  aurora::gfx::depth_peek::poll();

  if (z != nullptr) {
    u32 value = 0;
    if (aurora::gfx::depth_peek::read_latest(x, y, value)) {
      *z = value;
    } else {
      *z = g_gxState.clearDepth & 0x00ffffffu;
    }
  }

  aurora::gfx::depth_peek::request_snapshot();
}
#endif
