#include "gxm_renderer.hpp"
#include "gxm_memory.hpp"
#include "gxm_shader_gen.hpp"
#include "gfx/vita_pipeline_key.hpp"
#include "gfx/vita_texture_decode.hpp"
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <vitashark.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aurora::vita::gxm {
namespace {
using namespace gfx;
struct DisplayRequest {
  void* address;
  uint32_t width, height, stride;
  bool wait;
  std::atomic<int>* error;
};
void display_callback(const void* opaque) {
  const auto& request = *static_cast<const DisplayRequest*>(opaque);
  SceDisplayFrameBuf frame{};
  frame.size = sizeof(frame); frame.base = request.address;
  frame.width = request.width; frame.height = request.height; frame.pitch = request.stride;
  frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
  int error = sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
  if (error >= 0 && request.wait) error = sceDisplayWaitVblankStart();
  if (error < 0) request.error->store(error, std::memory_order_relaxed);
}
void* patch_alloc(void*, SceSize bytes) { return std::malloc(bytes); }
void patch_free(void*, void* p) { std::free(p); }
void shader_log(const char* message, shark_log_level level, int line) {
  std::fprintf(stderr, "[aurora-gxm][shader] level=%d line=%d %s\n", int(level), line, message ? message : "");
}
SceGxmAttributeFormat attribute_format(VertexScalar s, bool normalized) {
  switch (s) {
  case VertexScalar::F32: return SCE_GXM_ATTRIBUTE_FORMAT_F32;
  case VertexScalar::S8: return normalized ? SCE_GXM_ATTRIBUTE_FORMAT_S8N : SCE_GXM_ATTRIBUTE_FORMAT_S8;
  case VertexScalar::U8: return normalized ? SCE_GXM_ATTRIBUTE_FORMAT_U8N : SCE_GXM_ATTRIBUTE_FORMAT_U8;
  case VertexScalar::S16: return normalized ? SCE_GXM_ATTRIBUTE_FORMAT_S16N : SCE_GXM_ATTRIBUTE_FORMAT_S16;
  case VertexScalar::U16: return normalized ? SCE_GXM_ATTRIBUTE_FORMAT_U16N : SCE_GXM_ATTRIBUTE_FORMAT_U16;
  }
  return SCE_GXM_ATTRIBUTE_FORMAT_F32;
}
SceGxmBlendFactor blend_factor(BlendFactor factor) {
  static constexpr SceGxmBlendFactor values[]{SCE_GXM_BLEND_FACTOR_ZERO, SCE_GXM_BLEND_FACTOR_ONE,
      SCE_GXM_BLEND_FACTOR_SRC_COLOR, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
      SCE_GXM_BLEND_FACTOR_DST_COLOR, SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
      SCE_GXM_BLEND_FACTOR_SRC_ALPHA, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      SCE_GXM_BLEND_FACTOR_DST_ALPHA, SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA};
  return values[unsigned(factor)];
}
SceGxmDepthFunc depth_func(Compare compare) {
  static constexpr SceGxmDepthFunc values[]{SCE_GXM_DEPTH_FUNC_NEVER, SCE_GXM_DEPTH_FUNC_LESS,
      SCE_GXM_DEPTH_FUNC_EQUAL, SCE_GXM_DEPTH_FUNC_LESS_EQUAL, SCE_GXM_DEPTH_FUNC_GREATER,
      SCE_GXM_DEPTH_FUNC_NOT_EQUAL, SCE_GXM_DEPTH_FUNC_GREATER_EQUAL, SCE_GXM_DEPTH_FUNC_ALWAYS};
  return values[unsigned(compare)];
}
SceGxmTextureAddrMode address_mode(WrapMode mode) {
  return mode == WrapMode::Clamp ? SCE_GXM_TEXTURE_ADDR_CLAMP :
      mode == WrapMode::Mirror ? SCE_GXM_TEXTURE_ADDR_MIRROR : SCE_GXM_TEXTURE_ADDR_REPEAT;
}
SceGxmBlendInfo blend_info(const PipelineDesc& d) {
  SceGxmBlendInfo b{};
  b.colorMask = (d.colorWrite ? SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G | SCE_GXM_COLOR_MASK_B : 0) |
      (d.alphaWrite ? SCE_GXM_COLOR_MASK_A : 0);
  b.colorFunc = b.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
  b.colorSrc = b.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
  b.colorDst = b.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
  if (d.blendMode == BlendMode::Blend) {
    b.colorSrc = b.alphaSrc = blend_factor(d.srcFactor);
    b.colorDst = b.alphaDst = blend_factor(d.dstFactor);
  } else if (d.blendMode == BlendMode::Subtract) {
    b.colorFunc = b.alphaFunc = SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT;
    b.colorDst = b.alphaDst = SCE_GXM_BLEND_FACTOR_ONE;
  }
  if (d.dstAlpha >= 0 && d.alphaWrite) {
    b.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
    b.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE; b.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
  }
  return b;
}
bool is_slice_valid(const BufferSlice& s, size_t capacity) {
  return s.offset <= capacity && s.size <= capacity - s.offset;
}
} // namespace

struct Renderer::Impl {
  struct Surface { MemoryBlock memory; SceGxmColorSurface color{}; SceGxmSyncObject* sync = nullptr; };
  struct Buffer { MemoryBlock memory; size_t bytes = 0; };
  struct Texture { MemoryBlock memory; SceGxmTexture descriptor{}; };
  struct Pipeline {
    PipelineDesc desc{};
    std::vector<uint32_t> vertexCode, fragmentCode;
    SceGxmShaderPatcherId vertexId = nullptr, fragmentId = nullptr;
    SceGxmVertexProgram* vertex = nullptr;
    SceGxmFragmentProgram* fragment = nullptr;
    const SceGxmProgramParameter *mvp = nullptr, *kcolor = nullptr, *tevreg = nullptr, *clip = nullptr;
    uint8_t textureMask = 0;
  };
  Config config{};
  SceGxmContext* context = nullptr;
  SceGxmRenderTarget* target = nullptr;
  SceGxmShaderPatcher* patcher = nullptr;
  void* hostMemory = nullptr;
  MemoryBlock vdm, vertexRing, fragmentRing, fragmentUsseRing, patchBuffer, vertexUsse, fragmentUsse, depth;
  SceGxmDepthStencilSurface depthSurface{};
  std::array<Surface, 3> surfaces;
  std::unordered_map<uint64_t, std::unique_ptr<Pipeline>> pipelines;
  std::unordered_map<Handle, Buffer> buffers;
  std::unordered_map<Handle, Texture> textures;
  std::unordered_map<uint64_t, Handle> textureCache;
  FrameStats stats{};
  std::string error;
  std::atomic<int> displayError{0};
  size_t resourceBytes = 0;
  Handle nextHandle = 1;
  Handle clearVertices = 0, clearIndices = 0;
  uint64_t clearPipeline = 0, frameStarted = 0;
  uint32_t stride = 0, front = 0, back = 1;
  bool initialized = false, ownsGxm = false, ownsCompiler = false, inScene = false, displayed = false;

  bool fail(const char* operation, int code = 0) {
    char text[384];
    std::snprintf(text, sizeof(text), "%s (0x%08x)", operation, unsigned(code));
    error = text;
    std::fprintf(stderr, "[aurora-gxm] %s\n", text);
    return false;
  }
  bool check(int code, const char* operation) { return code >= 0 || fail(operation, code); }
  bool alloc(MemoryBlock& memory, size_t bytes, MemoryKind kind) {
    return check(memory.allocate(bytes, kind), "allocate/map GPU memory");
  }
  bool has_budget(size_t bytes) {
    const size_t rounded = (bytes + 4095u) & ~size_t(4095u);
    return rounded >= bytes && resourceBytes <= config.resourceBudgetBytes &&
        rounded <= config.resourceBudgetBytes - resourceBytes;
  }
  void destroy_pipeline(Pipeline& p) noexcept {
    if (p.fragment) sceGxmShaderPatcherReleaseFragmentProgram(patcher, p.fragment);
    if (p.vertex) sceGxmShaderPatcherReleaseVertexProgram(patcher, p.vertex);
    if (p.fragmentId) sceGxmShaderPatcherUnregisterProgram(patcher, p.fragmentId);
    if (p.vertexId) sceGxmShaderPatcherUnregisterProgram(patcher, p.vertexId);
    p.fragment = nullptr; p.vertex = nullptr; p.fragmentId = nullptr; p.vertexId = nullptr;
  }
  bool compile(const std::string& source, shark_type type, std::vector<uint32_t>& storage) {
    if (source.size() > UINT32_MAX) return fail("shader source too large");
    uint32_t size = uint32_t(source.size());
    const SceGxmProgram* program = shark_compile_shader(source.c_str(), &size, type);
    if (!program || !size) { shark_clear_output(); return fail("Cg compilation failed; see shader diagnostics"); }
    // Compiler output belongs to vitaShaRK. Registered headers must outlive the
    // patched programs, so take an aligned owned copy before clearing output.
    storage.resize((size + 3u) / 4u);
    std::memcpy(storage.data(), program, size);
    shark_clear_output();
    return check(sceGxmProgramCheck(reinterpret_cast<const SceGxmProgram*>(storage.data())), "validate GXP");
  }
};

Renderer::Renderer() : impl_(new Impl) {}
Renderer::~Renderer() { shutdown(); }
const char* Renderer::last_error() const noexcept { return impl_->error.c_str(); }
const FrameStats& Renderer::stats() const noexcept { return impl_->stats; }

bool Renderer::initialize(const Config& config) {
  auto& d = *impl_;
  if (d.initialized) return true;
  if (!config.width || config.width > 960 || !config.height || config.height > 544 ||
      config.displayBuffers < 2 || config.displayBuffers > 3 || !config.maxPipelines ||
      config.parameterBufferBytes < 0x40000 || config.parameterBufferBytes > UINT32_MAX)
    return d.fail("invalid GXM config");
  d.config = config; d.error.clear(); d.displayError = 0;
  d.stride = (config.width + 63u) & ~63u;
  const auto abort = [&]() { shutdown(); return false; };
  SceGxmInitializeParams init{};
  init.displayQueueMaxPendingCount = config.displayBuffers - 1;
  init.displayQueueCallback = display_callback;
  init.displayQueueCallbackDataSize = sizeof(DisplayRequest);
  init.parameterBufferSize = config.parameterBufferBytes;
  if (!d.check(sceGxmInitialize(&init), "initialize GXM")) return false;
  d.ownsGxm = true;
  if (!d.alloc(d.vdm, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, MemoryKind::CpuGpu) ||
      !d.alloc(d.vertexRing, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, MemoryKind::CpuGpu) ||
      !d.alloc(d.fragmentRing, SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, MemoryKind::CpuGpu) ||
      !d.alloc(d.fragmentUsseRing, SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, MemoryKind::FragmentUsse)) return abort();
  d.hostMemory = std::calloc(1, SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
  if (!d.hostMemory) { d.fail("allocate GXM context"); return abort(); }
  SceGxmContextParams ctx{};
  ctx.hostMem = d.hostMemory; ctx.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
  ctx.vdmRingBufferMem = d.vdm.data(); ctx.vdmRingBufferMemSize = d.vdm.size();
  ctx.vertexRingBufferMem = d.vertexRing.data(); ctx.vertexRingBufferMemSize = d.vertexRing.size();
  ctx.fragmentRingBufferMem = d.fragmentRing.data(); ctx.fragmentRingBufferMemSize = d.fragmentRing.size();
  ctx.fragmentUsseRingBufferMem = d.fragmentUsseRing.data(); ctx.fragmentUsseRingBufferMemSize = d.fragmentUsseRing.size();
  ctx.fragmentUsseRingBufferOffset = d.fragmentUsseRing.usse_offset();
  if (!d.check(sceGxmCreateContext(&ctx, &d.context), "create context")) return abort();
  SceGxmRenderTargetParams rt{};
  rt.width = config.width; rt.height = config.height; rt.scenesPerFrame = 1;
  rt.multisampleMode = SCE_GXM_MULTISAMPLE_NONE; rt.driverMemBlock = -1;
  if (!d.check(sceGxmCreateRenderTarget(&rt, &d.target), "create render target")) return abort();
  for (unsigned i = 0; i < config.displayBuffers; ++i) {
    auto& s = d.surfaces[i];
    if (!d.alloc(s.memory, size_t(d.stride) * config.height * 4, MemoryKind::ColorSurface)) return abort();
    std::memset(s.memory.data(), 0, s.memory.size());
    if (!d.check(sceGxmColorSurfaceInit(&s.color, SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR,
        SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
        config.width, config.height, d.stride, s.memory.data()), "initialize display surface") ||
        !d.check(sceGxmSyncObjectCreate(&s.sync), "create display sync")) return abort();
  }
  const uint32_t tw = (config.width + 31u) & ~31u, th = (config.height + 31u) & ~31u;
  if (!d.alloc(d.depth, size_t(tw) * th * 4, MemoryKind::CpuGpu)) return abort();
  if (!d.check(sceGxmDepthStencilSurfaceInit(&d.depthSurface, SCE_GXM_DEPTH_STENCIL_FORMAT_DF32,
      SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, tw, d.depth.data(), nullptr), "initialize depth surface")) return abort();
  sceGxmDepthStencilSurfaceSetBackgroundDepth(&d.depthSurface, 1.f);
  if (!d.alloc(d.patchBuffer, 1024 * 1024, MemoryKind::CpuGpu) ||
      !d.alloc(d.vertexUsse, 1024 * 1024, MemoryKind::VertexUsse) ||
      !d.alloc(d.fragmentUsse, 2 * 1024 * 1024, MemoryKind::FragmentUsse)) return abort();
  SceGxmShaderPatcherParams patch{};
  patch.hostAllocCallback = patch_alloc; patch.hostFreeCallback = patch_free;
  patch.bufferMem = d.patchBuffer.data(); patch.bufferMemSize = d.patchBuffer.size();
  patch.vertexUsseMem = d.vertexUsse.data(); patch.vertexUsseMemSize = d.vertexUsse.size();
  patch.vertexUsseOffset = d.vertexUsse.usse_offset();
  patch.fragmentUsseMem = d.fragmentUsse.data(); patch.fragmentUsseMemSize = d.fragmentUsse.size();
  patch.fragmentUsseOffset = d.fragmentUsse.usse_offset();
  if (!d.check(sceGxmShaderPatcherCreate(&patch, &d.patcher), "create shader patcher")) return abort();
  if (!d.check(shark_init(config.shaderCompilerPath), "initialize native Cg compiler")) return abort();
  d.ownsCompiler = true;
  shark_install_log_cb(shader_log);
  shark_set_warnings_level(SHARK_WARN_HIGH);
  d.initialized = true;
  PipelineDesc clear{};
  clear.reversedZ = false; clear.cull = CullMode::None; clear.depthFunc = Compare::Always;
  clear.layout.count = 1; clear.layout.attributes[0] = {0,4,VertexScalar::F32,false,16,0};
  clear.tev.stages[0].color.d = TevColorArg::Konst;
  clear.tev.stages[0].alpha.d = TevAlphaArg::Konst;
  clear.tev.stages[0].konstColor = KonstColorSel::K0;
  clear.tev.stages[0].konstAlpha = KonstAlphaSel::K0A;
  d.clearPipeline = create_pipeline(clear);
  const float vertices[]{-1,-1,0,1, 3,-1,0,1, -1,3,0,1};
  const uint16_t indices[]{0,1,2};
  d.clearVertices = create_buffer(vertices, sizeof(vertices));
  d.clearIndices = create_buffer(indices, sizeof(indices));
  if (!d.clearPipeline || !d.clearVertices || !d.clearIndices) return abort();
  std::fprintf(stderr, "[aurora-gxm] initialized %ux%u buffers=%u renderer=SceGxm native_cg=1\n",
      config.width, config.height, config.displayBuffers);
  return true;
}

uint64_t Renderer::create_pipeline(const PipelineDesc& desc) {
  auto& d = *impl_;
  if (!d.initialized || d.inScene) { d.fail("create pipelines between scenes"); return 0; }
  const uint64_t key = pipeline_key(desc);
  if (d.pipelines.find(key) != d.pipelines.end()) { ++d.stats.pipelineHits; return key; }
  if (d.pipelines.size() >= d.config.maxPipelines) { d.fail("native pipeline budget exhausted"); return 0; }
  const auto source = build_tev_cg(desc);
  if (!source.ok()) { d.fail(source.error.c_str()); return 0; }
  auto pipeline = std::make_unique<Impl::Pipeline>();
  auto& p = *pipeline; p.desc = desc; p.textureMask = source.textureMask;
  if (!d.compile(source.vertex, SHARK_VERTEX_SHADER, p.vertexCode) ||
      !d.compile(source.fragment, SHARK_FRAGMENT_SHADER, p.fragmentCode)) return 0;
  const auto* vp = reinterpret_cast<const SceGxmProgram*>(p.vertexCode.data());
  const auto* fp = reinterpret_cast<const SceGxmProgram*>(p.fragmentCode.data());
  const auto abort = [&]() { d.destroy_pipeline(p); return uint64_t{0}; };
  if (!d.check(sceGxmShaderPatcherRegisterProgram(d.patcher, vp, &p.vertexId), "register vertex GXP") ||
      !d.check(sceGxmShaderPatcherRegisterProgram(d.patcher, fp, &p.fragmentId), "register fragment GXP")) return abort();
  std::vector<SceGxmVertexAttribute> attributes;
  for (unsigned i = 0; i < desc.layout.count; ++i) {
    const auto& a = desc.layout.attributes[i];
    char name[24];
    if (!a.location) std::snprintf(name, sizeof(name), "a_position");
    else if (a.location < 3) std::snprintf(name, sizeof(name), "a_color%u", unsigned(a.location - 1));
    else std::snprintf(name, sizeof(name), "a_tex%u", unsigned(a.location - 3));
    const auto* parameter = sceGxmProgramFindParameterByName(vp, name);
    if (!parameter) continue; // Optimized out, no hardware stream binding needed.
    if (sceGxmProgramParameterGetCategory(parameter) != SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE) {
      d.fail("vertex reflection category mismatch"); return abort();
    }
    SceGxmVertexAttribute out{};
    out.offset = a.offset; out.componentCount = a.components;
    out.format = attribute_format(a.scalar, a.normalized);
    out.regIndex = sceGxmProgramParameterGetResourceIndex(parameter);
    attributes.push_back(out);
  }
  SceGxmVertexStream stream{};
  stream.stride = desc.layout.attributes[0].stride;
  stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
  if (!d.check(sceGxmShaderPatcherCreateVertexProgram(d.patcher, p.vertexId, attributes.data(),
      attributes.size(), &stream, 1, &p.vertex), "patch vertex program")) return abort();
  const auto blend = blend_info(desc);
  if (!d.check(sceGxmShaderPatcherCreateFragmentProgram(d.patcher, p.fragmentId, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
      SCE_GXM_MULTISAMPLE_NONE, &blend, vp, &p.fragment), "patch fragment program")) return abort();
  p.mvp = sceGxmProgramFindParameterByName(vp, "u_mvp");
  p.kcolor = sceGxmProgramFindParameterByName(fp, "u_kcolor");
  p.tevreg = sceGxmProgramFindParameterByName(fp, "u_tevreg");
  p.clip = sceGxmProgramFindParameterByName(fp, "u_clip_rect");
  d.pipelines.emplace(key, std::move(pipeline));
  ++d.stats.pipelineMisses;
  return key;
}

Handle Renderer::create_buffer(const void* data, size_t bytes) {
  auto& d = *impl_;
  if (!d.initialized || d.inScene || !data || !bytes || bytes > UINT32_MAX || !d.nextHandle || !d.has_budget(bytes)) {
    d.fail("invalid buffer upload or resource budget exhausted"); return 0;
  }
  Impl::Buffer buffer;
  if (!d.alloc(buffer.memory, bytes, MemoryKind::CpuGpu)) return 0;
  buffer.bytes = bytes;
  std::memcpy(buffer.memory.data(), data, bytes);
  d.resourceBytes += buffer.memory.size();
  const Handle handle = d.nextHandle++;
  d.buffers.emplace(handle, std::move(buffer));
  return handle;
}

Handle Renderer::create_texture(const TextureDesc& desc) {
  auto& d = *impl_;
  if (!d.initialized || d.inScene || !desc.data || !desc.width || desc.width > 4096 || !desc.height || desc.height > 4096 ||
      desc.mipCount != 1 || desc.generateMipmaps || !d.nextHandle) {
    d.fail("texture upload requires a bounded single-mip image between scenes"); return 0;
  }
  const size_t encoded = encoded_texture_size(desc.width, desc.height, desc.format);
  if (!encoded || desc.dataSize < encoded) { d.fail("truncated encoded texture"); return 0; }
  const uint64_t key = texture_key(desc);
  if (desc.cacheable) {
    const auto it = d.textureCache.find(key);
    if (it != d.textureCache.end()) { ++d.stats.textureHits; return it->second; }
  }
  const uint32_t rowBytes = ((desc.width + 7u) & ~7u) * 4u;
  const size_t bytes = size_t(rowBytes) * desc.height;
  if (!d.has_budget(bytes)) { d.fail("native texture budget exhausted"); return 0; }
  auto pixels = decode_texture_rgba8(desc);
  if (!pixels.ok) { d.fail("common GX texture decode failed"); return 0; }
  Impl::Texture texture;
  if (!d.alloc(texture.memory, bytes, MemoryKind::CpuGpu)) return 0;
  std::memset(texture.memory.data(), 0, bytes);
  for (uint32_t y = 0; y < desc.height; ++y)
    std::memcpy(static_cast<uint8_t*>(texture.memory.data()) + size_t(y) * rowBytes,
        pixels.rgba.data() + size_t(y) * desc.width * 4, desc.width * 4);
  // Ordinary linear textures permit sampler filter changes. LINEAR_STRIDED
  // rejects them with SCE_GXM_ERROR_UNSUPPORTED on hardware. The native linear
  // RGBA8 layout has the same eight-texel row padding used above.
  if (!d.check(sceGxmTextureInitLinear(&texture.descriptor, texture.memory.data(),
      SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, desc.width, desc.height, 0), "initialize native texture")) return 0;
  d.resourceBytes += texture.memory.size();
  const Handle handle = d.nextHandle++;
  d.textures.emplace(handle, std::move(texture));
  if (desc.cacheable) d.textureCache.emplace(key, handle);
  ++d.stats.textureMisses; ++d.stats.textureUploads;
  return handle;
}

bool Renderer::begin_frame(const Color& color, float clearDepth) {
  auto& d = *impl_;
  if (!d.initialized || d.inScene || !std::isfinite(clearDepth) || clearDepth < 0.f || clearDepth > 1.f)
    return d.fail("invalid begin_frame");
  const int displayError = d.displayError.load(std::memory_order_relaxed);
  if (displayError < 0) return d.fail("display callback failed", displayError);
  d.stats = {}; d.frameStarted = sceKernelGetProcessTimeWide();
  auto& surface = d.surfaces[d.back];
  if (!d.check(sceGxmBeginScene(d.context, 0, d.target, nullptr, nullptr, surface.sync,
      &surface.color, &d.depthSurface), "begin scene")) return false;
  d.inScene = true;
  DrawPacket clear{};
  clear.pipelineKey = d.clearPipeline;
  clear.vertices = {d.clearVertices, 0, 48}; clear.indices = {d.clearIndices, 0, 6};
  clear.vertexCount = 3; clear.indexCount = 3;
  clear.viewport.width = float(d.config.width); clear.viewport.height = float(d.config.height);
  clear.scissor.width = d.config.width; clear.scissor.height = d.config.height;
  clear.uniforms.kcolor[0] = {color.r, color.g, color.b, color.a};
  clear.uniforms.mvp[14] = clearDepth - 1.f;
  if (!draw(clear)) { sceGxmEndScene(d.context, nullptr, nullptr); d.inScene = false; return false; }
  return true;
}

bool Renderer::draw(const DrawPacket& packet) {
  auto& d = *impl_;
  if (!d.inScene) return d.fail("draw outside a scene");
  const auto pi = d.pipelines.find(packet.pipelineKey);
  const auto vi = d.buffers.find(packet.vertices.buffer), ii = d.buffers.find(packet.indices.buffer);
  if (pi == d.pipelines.end() || vi == d.buffers.end() || ii == d.buffers.end()) return d.fail("unknown pipeline or buffer handle");
  const auto& p = *pi->second;
  const auto& pipeline = p.desc;
  if (packet.instanceCount != 1 || packet.fixedVertexUniforms || !packet.indexCount || packet.firstVertex ||
      !is_slice_valid(packet.vertices, vi->second.bytes) || !is_slice_valid(packet.indices, ii->second.bytes) ||
      packet.vertices.offset % 4 || packet.indices.offset % 2 || packet.indexCount > packet.indices.size / 2)
    return d.fail("invalid native indexed draw");
  if ((pipeline.primitive == Primitive::Triangles && packet.indexCount % 3) || packet.indexCount < 3)
    return d.fail("invalid triangle index count");
  const auto& vp = packet.viewport;
  if (!std::isfinite(vp.x) || !std::isfinite(vp.y) || !std::isfinite(vp.width) || !std::isfinite(vp.height) ||
      vp.width <= 0 || vp.height <= 0 || !std::isfinite(vp.znear) || !std::isfinite(vp.zfar) ||
      vp.znear < 0 || vp.znear > 1 || vp.zfar < 0 || vp.zfar > 1) return d.fail("invalid native viewport");
  const auto* indices = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(ii->second.memory.data()) + packet.indices.offset);
  const uint32_t stride = pipeline.layout.attributes[0].stride;
  const size_t base = packet.absoluteVertexIndices ? 0 : packet.vertices.offset;
  const size_t end = size_t(packet.vertices.offset) + packet.vertices.size;
  for (uint32_t i = 0; i < packet.indexCount; ++i) {
    const size_t offset = base + size_t(indices[i]) * stride;
    // GXM's INDEX_16BIT stream has a documented limit below 64000.
    if (indices[i] >= 64000 || offset < packet.vertices.offset || offset > end || stride > end - offset)
      return d.fail("vertex index outside the supplied slice or GXM range");
  }
  if (pipeline.cull == CullMode::All || packet.scissor.width <= 0 || packet.scissor.height <= 0) return true;
  // Bind a descriptor copy per unit: sampling one image with two samplers must
  // never mutate another unit's state or queued resource metadata.
  std::array<SceGxmTexture, MaxTextures> boundTextures{};
  for (unsigned i = 0; i < MaxTextures; ++i) if (p.textureMask & (1u << i)) {
    const auto& binding = packet.textures[i];
    const auto ti = d.textures.find(binding.texture);
    const auto& s = binding.sampler;
    if (binding.source != TextureSource::Cache || ti == d.textures.end() ||
        s.minFilter > Filter::Linear || s.magFilter > Filter::Linear || s.wrapS > WrapMode::Mirror || s.wrapT > WrapMode::Mirror ||
        !std::isfinite(s.lodBias) || !std::isfinite(s.minLod) || !std::isfinite(s.maxLod) || s.minLod > 0 || s.maxLod < 0)
      return d.fail("missing texture or unsupported sampler/EFB texture");
    auto& native = boundTextures[i]; native = ti->second.descriptor;
    if (!d.check(sceGxmTextureSetMinFilter(&native, s.minFilter == Filter::Nearest ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR), "set min filter") ||
        !d.check(sceGxmTextureSetMagFilter(&native, s.magFilter == Filter::Nearest ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR), "set mag filter") ||
        !d.check(sceGxmTextureSetUAddrMode(&native, address_mode(s.wrapS)), "set U wrap") ||
        !d.check(sceGxmTextureSetVAddrMode(&native, address_mode(s.wrapT)), "set V wrap")) return false;
  }
  sceGxmSetVertexProgram(d.context, p.vertex);
  sceGxmSetFragmentProgram(d.context, p.fragment);
  const auto compare = pipeline.depthTest ? depth_func(pipeline.depthFunc) : SCE_GXM_DEPTH_FUNC_ALWAYS;
  const auto write = pipeline.depthTest && pipeline.depthWrite ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;
  sceGxmSetFrontDepthFunc(d.context, compare); sceGxmSetBackDepthFunc(d.context, compare);
  sceGxmSetFrontDepthWriteEnable(d.context, write); sceGxmSetBackDepthWriteEnable(d.context, write);
  sceGxmSetCullMode(d.context, pipeline.cull == CullMode::None ? SCE_GXM_CULL_NONE :
      pipeline.cull == CullMode::Back ? SCE_GXM_CULL_CCW : SCE_GXM_CULL_CW);
  const float low = std::min(vp.znear, vp.zfar), high = std::max(vp.znear, vp.zfar);
  sceGxmSetViewport(d.context, vp.x + vp.width * .5f, vp.width * .5f,
      vp.y + vp.height * .5f, -vp.height * .5f, (low + high) * .5f, (high - low) * .5f);
  // Pixel-exact scissor is emitted into Cg; GXM region clipping alone is tile based.
  const float clip[]{float(packet.scissor.x), float(packet.scissor.y),
      float(int64_t(packet.scissor.x) + packet.scissor.width), float(int64_t(packet.scissor.y) + packet.scissor.height)};
  void* vertexUniforms = nullptr;
  if (p.mvp) {
    if (!d.check(sceGxmReserveVertexDefaultUniformBuffer(d.context, &vertexUniforms), "reserve vertex uniforms") ||
        !d.check(sceGxmSetUniformDataF(vertexUniforms, p.mvp, 0, 16, packet.uniforms.mvp.data()), "upload projection")) return false;
  }
  void* fragmentUniforms = nullptr;
  if (!d.check(sceGxmReserveFragmentDefaultUniformBuffer(d.context, &fragmentUniforms), "reserve fragment uniforms")) return false;
  const auto upload = [&](const SceGxmProgramParameter* parameter, unsigned count, const float* source) {
    // Reflection can shrink unused tails of uniform arrays.
    if (!parameter) return true;
    count = std::min(count, sceGxmProgramParameterGetComponentCount(parameter) * sceGxmProgramParameterGetArraySize(parameter));
    return d.check(sceGxmSetUniformDataF(fragmentUniforms, parameter, 0, count, source), "upload fragment uniform");
  };
  if (!upload(p.kcolor, 16, packet.uniforms.kcolor[0].data()) ||
      !upload(p.tevreg, 16, packet.uniforms.tevreg[0].data()) || !upload(p.clip, 4, clip)) return false;
  for (unsigned i = 0; i < MaxTextures; ++i) if (p.textureMask & (1u << i))
    if (!d.check(sceGxmSetFragmentTexture(d.context, i, &boundTextures[i]), "bind native texture")) return false;
  if (!d.check(sceGxmSetVertexStream(d.context, 0, static_cast<uint8_t*>(vi->second.memory.data()) + base), "bind native vertex stream")) return false;
  const auto primitive = pipeline.primitive == Primitive::Triangles ? SCE_GXM_PRIMITIVE_TRIANGLES :
      pipeline.primitive == Primitive::TriangleFan ? SCE_GXM_PRIMITIVE_TRIANGLE_FAN : SCE_GXM_PRIMITIVE_TRIANGLE_STRIP;
  if (!d.check(sceGxmDraw(d.context, primitive, SCE_GXM_INDEX_FORMAT_U16, indices, packet.indexCount), "draw indexed")) return false;
  ++d.stats.drawCalls;
  d.stats.triangles += pipeline.primitive == Primitive::Triangles ? packet.indexCount / 3 : packet.indexCount - 2;
  return true;
}

bool Renderer::end_frame(bool present) {
  auto& d = *impl_;
  if (!d.inScene) return d.fail("end_frame outside a scene");
  const int error = sceGxmEndScene(d.context, nullptr, nullptr);
  d.inScene = false;
  if (!d.check(error, "end scene")) return false;
  auto& surface = d.surfaces[d.back];
  if (present) {
    if (!d.check(sceGxmPadHeartbeat(&surface.color, surface.sync), "display heartbeat")) return false;
    const DisplayRequest request{surface.memory.data(), d.config.width, d.config.height, d.stride, d.config.waitVblank, &d.displayError};
    if (!d.check(sceGxmDisplayQueueAddEntry(d.surfaces[d.front].sync, surface.sync, &request), "queue present")) return false;
    d.displayed = true; d.front = d.back; d.back = (d.back + 1) % d.config.displayBuffers;
  } else {
    // Discarded presents retain the current display; complete writes before the
    // same offscreen surface can become the next scene's target.
    sceGxmFinish(d.context);
  }
  d.stats.cpuFrameUs = sceKernelGetProcessTimeWide() - d.frameStarted;
  return true;
}

bool Renderer::readback_rgba8(std::vector<uint8_t>& pixels) {
  auto& d = *impl_;
  if (!d.initialized || d.inScene || !d.displayed) return d.fail("readback requires a completed presented frame");
  sceGxmFinish(d.context);
  pixels.resize(size_t(d.config.width) * d.config.height * 4);
  const auto* source = static_cast<const uint8_t*>(d.surfaces[d.front].memory.data());
  for (uint32_t y = 0; y < d.config.height; ++y)
    std::memcpy(pixels.data() + size_t(y) * d.config.width * 4,
        source + size_t(y) * d.stride * 4, size_t(d.config.width) * 4);
  return true;
}

void Renderer::shutdown() noexcept {
  auto& d = *impl_;
  if (d.inScene && d.context) { sceGxmEndScene(d.context, nullptr, nullptr); d.inScene = false; }
  if (d.context) sceGxmFinish(d.context);
  if (d.ownsGxm) sceGxmDisplayQueueFinish();
  // Stop scanout before releasing display backing storage.
  if (d.displayed) { sceDisplaySetFrameBuf(nullptr, SCE_DISPLAY_SETBUF_NEXTFRAME); sceDisplayWaitVblankStart(); }
  for (auto& entry : d.pipelines) d.destroy_pipeline(*entry.second);
  d.pipelines.clear(); d.textures.clear(); d.textureCache.clear(); d.buffers.clear();
  if (d.patcher) { sceGxmShaderPatcherDestroy(d.patcher); d.patcher = nullptr; }
  if (d.ownsCompiler) { shark_clear_output(); shark_install_log_cb(nullptr); shark_end(); d.ownsCompiler = false; }
  if (d.target) { sceGxmDestroyRenderTarget(d.target); d.target = nullptr; }
  if (d.context) { sceGxmDestroyContext(d.context); d.context = nullptr; }
  for (auto& s : d.surfaces) { if (s.sync) sceGxmSyncObjectDestroy(s.sync); s.sync = nullptr; s.memory.reset(); }
  d.depth.reset(); d.patchBuffer.reset(); d.vertexUsse.reset(); d.fragmentUsse.reset();
  d.vdm.reset(); d.vertexRing.reset(); d.fragmentRing.reset(); d.fragmentUsseRing.reset();
  std::free(d.hostMemory); d.hostMemory = nullptr;
  if (d.ownsGxm) { sceGxmTerminate(); d.ownsGxm = false; }
  d.initialized = false; d.displayed = false; d.resourceBytes = 0;
  d.clearVertices = d.clearIndices = 0; d.clearPipeline = 0;
  d.front = 0; d.back = 1;
}
} // namespace aurora::vita::gxm
