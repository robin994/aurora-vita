#include "gxm/gxm_renderer.hpp"
#include "gfx/vita_vertex_decode.hpp"
#include "gfx/vita_vertex_pipeline.hpp"
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using namespace aurora::vita;
using Mat4 = std::array<float, 16>;
Mat4 multiply(const Mat4& a, const Mat4& b) {
  Mat4 result{};
  for (unsigned c = 0; c < 4; ++c)
    for (unsigned r = 0; r < 4; ++r)
      for (unsigned k = 0; k < 4; ++k) result[c*4+r] += a[k*4+r] * b[c*4+k];
  return result;
}
Mat4 cube_projection(float time) {
  const float c = std::cos(time), s = std::sin(time);
  const float cx = std::cos(time*.7f), sx = std::sin(time*.7f);
  Mat4 ry{{c,0,-s,0, 0,1,0,0, s,0,c,0, 0,0,-4,1}};
  Mat4 rx{{1,0,0,0, 0,cx,sx,0, 0,-sx,cx,0, 0,0,0,1}};
  const float near = .1f, far = 100.f, f = 1.8f;
  // GX depth: near -> -w, far -> 0; the native shader maps it to its viewport.
  Mat4 projection{{f/(960.f/544.f),0,0,0, 0,f,0,0, 0,0,near/(near-far),-1, 0,0,far*near/(near-far),0}};
  return multiply(projection, multiply(ry, rx));
}
int run() {
  sceIoMkdir("ux0:data/aurora-vita", 0777);
  if (std::freopen("ux0:data/aurora-vita/gxm_probe.log", "w", stderr)) std::setvbuf(stderr, nullptr, _IONBF, 0);
  std::fprintf(stderr, "[gxm-probe] build=0.1 renderer=GXM title=AURVGXM01 no_vitagl=1\n");
  gxm::Renderer renderer;
  if (!renderer.initialize()) return 1;
  gfx::PipelineDesc desc;
  desc.reversedZ = false;
  desc.cull = gfx::CullMode::None;
  desc.layout = gfx::gpu_vertex_layout();
  desc.texgenCount = 1;
  desc.colorChannels[0].materialSource = gfx::ColorSource::Vertex;
  desc.colorChannels[2].materialSource = gfx::ColorSource::Vertex;
  auto& tev = desc.tev.stages[0];
  tev.texture = 0; tev.texCoord = 0;
  tev.color = {gfx::TevColorArg::Zero, gfx::TevColorArg::TexColor, gfx::TevColorArg::RasColor, gfx::TevColorArg::Zero};
  tev.alpha = {gfx::TevAlphaArg::Zero, gfx::TevAlphaArg::TexAlpha, gfx::TevAlphaArg::RasAlpha, gfx::TevAlphaArg::Zero};
  const uint64_t pipeline = renderer.create_pipeline(desc);
  if (!pipeline) return 2;
  const float positions[8][3]{{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                             {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
  const unsigned faces[6][4]{{0,1,2,3},{5,4,7,6},{4,0,3,7},{1,5,6,2},{3,2,6,7},{4,5,1,0}};
  const uint8_t colors[6][4]{{255,90,90,255},{90,255,120,255},{100,150,255,255},
                            {255,220,90,255},{200,130,255,255},{90,240,240,255}};
  const float uv[4][2]{{0,0},{1,0},{1,1},{0,1}};
  std::vector<gfx::CanonicalVertex> canonical(24);
  std::array<uint16_t, 36> indices{};
  for (unsigned face = 0; face < 6; ++face) {
    for (unsigned v = 0; v < 4; ++v) {
      auto& dst = canonical[face*4+v];
      std::memcpy(dst.position, positions[faces[face][v]], 3*sizeof(float));
      std::memcpy(dst.color0, colors[face], 4);
      std::memcpy(dst.texcoord[0], uv[v], 2*sizeof(float));
      dst.texcoord[0][2] = 1.f;
    }
    const uint16_t order[]{0,1,2,0,2,3};
    for (unsigned i = 0; i < 6; ++i) indices[face*6+i] = face*4+order[i];
  }
  gfx::VertexTransformState identity;
  if (!gfx::run_vertex_pipeline(canonical, desc, identity)) return 3;
  std::array<gfx::GpuVertex,24> vertices{};
  for (unsigned i = 0; i < vertices.size(); ++i) {
    std::memcpy(vertices[i].position, canonical[i].position, sizeof(vertices[i].position));
    std::memcpy(vertices[i].color0, canonical[i].color0, sizeof(vertices[i].color0));
    std::memcpy(vertices[i].color1, canonical[i].color1, sizeof(vertices[i].color1));
    std::memcpy(vertices[i].texcoord, canonical[i].texcoord, sizeof(vertices[i].texcoord));
  }
  // A real tiled GX RGB565 source exercises the shared texture decoder.
  std::array<uint8_t, 32> encoded{};
  for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x) {
    const uint16_t pixel = (x+y)%2 ? 0xffff : 0x4208;
    encoded[(y*4+x)*2] = uint8_t(pixel >> 8); encoded[(y*4+x)*2+1] = uint8_t(pixel);
  }
  gfx::TextureDesc texture{};
  texture.width = texture.height = 4; texture.format = gfx::TextureFormat::RGB565;
  texture.data = encoded.data(); texture.dataSize = encoded.size(); texture.sourceId = 1;
  const auto tex = renderer.create_texture(texture);
  const auto vb = renderer.create_buffer(vertices.data(), sizeof(vertices));
  const auto ib = renderer.create_buffer(indices.data(), sizeof(indices));
  if (!tex || !vb || !ib) return 4;
  gfx::DrawPacket packet;
  packet.pipelineKey = pipeline; packet.vertexCount = vertices.size(); packet.indexCount = indices.size();
  packet.vertices = {vb,0,sizeof(vertices)}; packet.indices = {ib,0,sizeof(indices)};
  packet.textures[0].texture = tex;
  packet.textures[0].sampler.minFilter = packet.textures[0].sampler.magFilter = gfx::Filter::Nearest;
  std::fprintf(stderr, "[gxm-probe] pipeline_and_resources_ready vertices=24 triangles=12 gx_rgb565=1\n");
  const auto start = sceKernelGetProcessTimeWide();
  auto interval = start;
  unsigned frames = 0;
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
  for (; frames < 600; ++frames) {
    SceCtrlData pad{}; sceCtrlPeekBufferPositive(0, &pad, 1);
    if (pad.buttons & SCE_CTRL_START) break;
    const float elapsed = float(sceKernelGetProcessTimeWide() - start) / 1000000.f;
    packet.uniforms.mvp = cube_projection(elapsed);
    // Cross toggles exact pixel scissoring without changing any GX/game logic.
    packet.scissor = (pad.buttons & SCE_CTRL_CROSS) ? gfx::Scissor{301,137,359,251} : gfx::Scissor{0,0,960,544};
    if (!renderer.begin_frame({.025f,.04f,.07f,1.f}) || !renderer.draw(packet) || !renderer.end_frame()) {
      std::fprintf(stderr, "[gxm-probe] FAILED frame=%u %s\n", frames, renderer.last_error()); return 5;
    }
    if ((frames+1)%120 == 0) {
      const auto now = sceKernelGetProcessTimeWide();
      const double fps = 120000000.0 / double(now-interval);
      std::fprintf(stderr, "[gxm-probe] frame=%u fps=%.2f draws=%u triangles=%u submit_us=%llu\n",
          frames+1, fps, renderer.stats().drawCalls, renderer.stats().triangles,
          static_cast<unsigned long long>(renderer.stats().cpuFrameUs));
      interval = now;
    }
  }
  // Keep slow diagnostic readback and filesystem I/O outside the FPS windows.
  if (frames) {
    std::vector<uint8_t> pixels;
    if (!renderer.readback_rgba8(pixels)) return 6;
    std::vector<uint8_t> rgb(pixels.size() / 4 * 3);
    for (size_t p = 0; p < pixels.size()/4; ++p) std::memcpy(rgb.data()+p*3, pixels.data()+p*4, 3);
    FILE* image = std::fopen("ux0:data/aurora-vita/gxm_probe.ppm", "wb");
    if (!image) return 7;
    const bool headerWritten = std::fprintf(image, "P6\n960 544\n255\n") > 0;
    const bool pixelsWritten = std::fwrite(rgb.data(), 1, rgb.size(), image) == rgb.size();
    const bool closed = std::fclose(image) == 0;
    if (!headerWritten || !pixelsWritten || !closed) return 7;
    std::fprintf(stderr, "[gxm-probe] framebuffer_capture=960x544 frame=%u\n", frames);

    // Check pixel-exact scissor edges, not just tile-level clipping.
    const gfx::Scissor clip{301,137,359,251};
    packet.scissor = clip;
    if (!renderer.begin_frame({.025f,.04f,.07f,1.f}) || !renderer.draw(packet) || !renderer.end_frame() ||
        !renderer.readback_rgba8(pixels)) return 8;
    unsigned outsideChanges = 0, insideChanges = 0;
    for (unsigned y = 0; y < 544; ++y) for (unsigned x = 0; x < 960; ++x) {
      const bool changed = std::memcmp(pixels.data(), pixels.data() + (size_t(y)*960+x)*4, 3) != 0;
      const bool inside = x >= unsigned(clip.x) && x < unsigned(clip.x+clip.width) &&
                          y >= unsigned(clip.y) && y < unsigned(clip.y+clip.height);
      if (changed) { if (inside) ++insideChanges; else ++outsideChanges; }
    }
    std::fprintf(stderr, "[gxm-probe] scissor_outside_changes=%u scissor_inside_changes=%u\n", outsideChanges, insideChanges);
    if (outsideChanges || !insideChanges) return 9;
  }
  renderer.shutdown();
  std::fprintf(stderr, "[gxm-probe] completed frames=%u clean_shutdown=1\n", frames);
  return 0;
}
} // namespace
int main() {
  const int result = run();
  std::fprintf(stderr, "[gxm-probe] exit=%d\n", result);
  sceKernelExitProcess(result);
  return result;
}
