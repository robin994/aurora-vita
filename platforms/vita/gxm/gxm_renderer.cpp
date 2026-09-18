#include "gxm_renderer.hpp"
#include "gxm_memory.hpp"
#include "gxm_program_cache.hpp"
#include "gxm_shader_gen.hpp"
#include "gxm_texture_layout.hpp"
#include "gfx/vita_pipeline_key.hpp"
#include "gfx/vita_texture_decode.hpp"
#include "gfx/vita_sampler_units.hpp"
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

void log_memory_state(const char* phase) {
  const auto stats = memory_stats();
  SceKernelFreeMemorySizeInfo freeMemory{};
  freeMemory.size = sizeof(freeMemory);
  const int freeResult = sceKernelGetFreeMemorySize(&freeMemory);
  std::fprintf(stderr,
      "[aurora-gxm] memory phase=%s "
      "cdram=%llu peak=%llu allocs=%u pool=%llu pool_used=%llu pool_peak=%llu "
      "user=%llu peak=%llu allocs=%u "
      "phycont=%llu peak=%llu allocs=%u "
      "cdialog=%llu peak=%llu allocs=%u fallbacks=%u "
      "free_cdram=%d free_user=%d free_phycont=%d query=0x%08x\n",
      phase,
      static_cast<unsigned long long>(stats.cdramCurrent),
      static_cast<unsigned long long>(stats.cdramPeak), stats.cdramAllocations,
      static_cast<unsigned long long>(stats.cdramPoolBytes),
      static_cast<unsigned long long>(stats.cdramPoolUsed),
      static_cast<unsigned long long>(stats.cdramPoolPeak),
      static_cast<unsigned long long>(stats.userCurrent),
      static_cast<unsigned long long>(stats.userPeak), stats.userAllocations,
      static_cast<unsigned long long>(stats.phycontCurrent),
      static_cast<unsigned long long>(stats.phycontPeak), stats.phycontAllocations,
      static_cast<unsigned long long>(stats.cdialogCurrent),
      static_cast<unsigned long long>(stats.cdialogPeak), stats.cdialogAllocations,
      stats.fallbackAllocations,
      freeResult >= 0 ? freeMemory.size_cdram : -1,
      freeResult >= 0 ? freeMemory.size_user : -1,
      freeResult >= 0 ? freeMemory.size_phycont : -1,
      unsigned(freeResult));

  // Keep a lightweight persistent copy as well. The game redirects its own
  // diagnostics to runtime.log, while the native renderer writes to stderr;
  // on retail hardware that stream is not always captured by VitaCompanion.
  if (FILE* file = std::fopen("ux0:data/SmashMeleeVita/gxm_memory.log", "a")) {
    std::fprintf(file,
        "phase=%s cdram=%llu peak=%llu allocs=%u pool=%llu pool_used=%llu pool_peak=%llu "
        "user=%llu peak=%llu allocs=%u phycont=%llu peak=%llu allocs=%u "
        "cdialog=%llu peak=%llu allocs=%u fallbacks=%u free_cdram=%d free_user=%d "
        "free_phycont=%d query=0x%08x\n",
        phase,
        static_cast<unsigned long long>(stats.cdramCurrent),
        static_cast<unsigned long long>(stats.cdramPeak), stats.cdramAllocations,
        static_cast<unsigned long long>(stats.cdramPoolBytes),
        static_cast<unsigned long long>(stats.cdramPoolUsed),
        static_cast<unsigned long long>(stats.cdramPoolPeak),
        static_cast<unsigned long long>(stats.userCurrent),
        static_cast<unsigned long long>(stats.userPeak), stats.userAllocations,
        static_cast<unsigned long long>(stats.phycontCurrent),
        static_cast<unsigned long long>(stats.phycontPeak), stats.phycontAllocations,
        static_cast<unsigned long long>(stats.cdialogCurrent),
        static_cast<unsigned long long>(stats.cdialogPeak), stats.cdialogAllocations,
        stats.fallbackAllocations,
        freeResult >= 0 ? freeMemory.size_cdram : -1,
        freeResult >= 0 ? freeMemory.size_user : -1,
        freeResult >= 0 ? freeMemory.size_phycont : -1,
        unsigned(freeResult));
    std::fclose(file);
  }
}

void display_callback(const void* opaque) {
  const auto& request = *static_cast<const DisplayRequest*>(opaque);
  SceDisplayFrameBuf frame{};
  frame.size = sizeof(frame); frame.base = request.address;
  frame.width = request.width; frame.height = request.height; frame.pitch = request.stride;
  frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
  // Match vitaGL's proven swap contract: schedule the buffer for NEXTFRAME and,
  // when vsync is requested, keep the display-queue callback alive through that
  // retrace. This paces queue retirement instead of letting pending swaps build
  // up until DisplayQueueAddEntry has to absorb multiple refresh intervals.
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
  } else if(d.blendMode==BlendMode::Logic) {
    if(d.logicOp==LogicOp::Clear) b.colorSrc=b.alphaSrc=SCE_GXM_BLEND_FACTOR_ZERO;
    if(d.logicOp==LogicOp::Noop) {
      b.colorSrc=b.alphaSrc=SCE_GXM_BLEND_FACTOR_ZERO;
      b.colorDst=b.alphaDst=SCE_GXM_BLEND_FACTOR_ONE;
    }
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
bool same_sampler(const SamplerDesc& a,const SamplerDesc& b) noexcept {
  return a.wrapS==b.wrapS&&a.wrapT==b.wrapT&&a.minFilter==b.minFilter&&a.magFilter==b.magFilter&&
         a.lodBias==b.lodBias&&a.minLod==b.minLod&&a.maxLod==b.maxLod;
}
bool same_viewport(const Viewport& a,const Viewport& b) noexcept {
  return a.x==b.x&&a.y==b.y&&a.width==b.width&&a.height==b.height&&a.znear==b.znear&&a.zfar==b.zfar;
}
} // namespace

struct Renderer::Impl {
  struct Surface { MemoryBlock memory; SceGxmColorSurface color{}; SceGxmSyncObject* sync = nullptr; };
  struct Buffer { MemoryBlock memory; size_t bytes = 0; bool inFlight = false; };
  struct Texture {
    MemoryBlock memory, depth;
    SceGxmTexture descriptor{};
    SceGxmColorSurface color{};
    SceGxmDepthStencilSurface depthSurface{};
    SceGxmRenderTarget* target = nullptr;
    SceGxmSyncObject* sync = nullptr;
    uint32_t width = 0, height = 0, stride = 0;
    uint32_t mipCount = 1;
    bool swizzled = false;
    SamplerDesc descriptorSampler{};
    bool descriptorSamplerValid = false;
    bool inFlight = false, depthValid = false;
    ~Texture() {
      if (sync) sceGxmSyncObjectDestroy(sync);
      if (target) sceGxmDestroyRenderTarget(target);
    }
  };
  struct CompiledStage {
    std::vector<uint32_t> code;
  };
  struct Pipeline {
    PipelineDesc desc{};
    std::shared_ptr<CompiledStage> vertexCode, fragmentCode;
    SceGxmShaderPatcherId vertexId = nullptr, fragmentId = nullptr;
    SceGxmVertexProgram* vertex = nullptr;
    SceGxmFragmentProgram* fragment = nullptr;
    const SceGxmProgramParameter *mvp = nullptr, *kcolor = nullptr, *tevreg = nullptr, *clip = nullptr;
    const SceGxmProgramParameter *fogColor=nullptr,*fogParams=nullptr,*fogRange=nullptr,*viewportWidth=nullptr;
    const SceGxmProgramParameter *indirectMatrices=nullptr,*texcoordScale=nullptr,*textureSizeBias=nullptr,*textureTransform=nullptr,*textureWrap=nullptr,*textureForceOpaque=nullptr,*textureCopyMode=nullptr;
    const SceGxmProgramParameter *gxPosition=nullptr,*gxNormal=nullptr,*gxMaterial=nullptr,*gxAmbient=nullptr,*gxLight=nullptr;
    const SceGxmProgramParameter *gxPositionPalette=nullptr,*gxNormalPalette=nullptr;
    std::array<const SceGxmProgramParameter*,MaxTextures> gxTexture{};
    std::array<const SceGxmProgramParameter*,MaxTextures> gxPost{};
    uint8_t textureMask = 0;
    bool inFlight = false;
  };
  Config config{};
  uint64_t profileFrame = 0;
  bool profileDraws = false;
  SceGxmContext* context = nullptr;
  SceGxmRenderTarget* target = nullptr;
  SceGxmShaderPatcher* patcher = nullptr;
  void* hostMemory = nullptr;
  MemoryBlock vdm, vertexRing, fragmentRing, fragmentUsseRing, patchBuffer, vertexUsse, fragmentUsse, depth;
  SceGxmDepthStencilSurface depthSurface{};
  std::array<Surface, 3> surfaces;
  std::unordered_map<uint64_t, std::unique_ptr<Pipeline>> pipelines;
  std::unordered_map<uint64_t, std::shared_ptr<CompiledStage>> stageCache;
  std::unordered_map<Handle, Buffer> buffers;
  std::unordered_map<Handle, std::unique_ptr<Texture>> textures;
  std::unordered_map<uint64_t, Handle> textureCache;
  FrameStats stats{};
  std::string error;
  std::atomic<int> displayError{0};
  size_t resourceBytes = 0;
  Handle nextHandle = 1;
  Handle clearVertices = 0, clearIndices = 0;
  Handle boundTarget = 0, blitVertices = 0, displaySource = 0, copyVertices = 0;
  uint64_t clearPipeline = 0, frameStarted = 0;
  uint64_t boundPipelineKey = 0;
  uint32_t stride = 0, front = 0, back = 1;
  bool initialized = false, ownsGxm = false, ownsCompiler = false, inScene = false, displayed = false;
  bool frameActive = false, depthValid = false;
  bool pipelineStateValid = false, viewportValid = false;
  bool vertexStreamValid = false;
  Viewport cachedViewport{};
  Handle boundVertexBuffer = 0;
  size_t boundVertexBase = 0;
  uint8_t textureBindingValidMask = 0;
  std::array<Handle,MaxTextures> boundTextureHandles{};
  std::array<SamplerDesc,MaxTextures> boundTextureSamplers{};
  SceGxmSyncObject* pendingVertexDependency = nullptr;
  bool pendingFragmentTransferSync = false;
  Scissor displayCopySource{};
  bool displayCopySourceValid = false;
  Scissor efbCopySource{};
  uint32_t efbCopySourceWidth = 0, efbCopySourceHeight = 0;
  bool efbCopyFlipX = false, efbCopyFlipY = false, efbCopyGeometryValid = false;
  ProgramBinaryCache programCache;
  uint32_t stageMemoryHits = 0, stageCompiles = 0;

  uint32_t width() const { return boundTarget ? textures.at(boundTarget)->width : config.width; }
  uint32_t height() const { return boundTarget ? textures.at(boundTarget)->height : config.height; }
  bool end_scene() {
    if (!inScene) return true;
    const int result = sceGxmEndScene(context, nullptr, nullptr);
    inScene = false;
    if (boundTarget) textures.at(boundTarget)->depthValid = true;
    else depthValid = true;
    return check(result, "end native scene");
  }
  bool ensure_scene() {
    if (inScene) return true;
    if (!frameActive) return fail("draw outside a frame");
    auto* ds = &depthSurface;
    auto* cs = &surfaces[back].color;
    auto* rt = target;
    auto* sync = surfaces[back].sync;
    bool loadDepth = depthValid;
    if (boundTarget) {
      auto& t = *textures.at(boundTarget);
      ds = t.depth.data() ? &t.depthSurface : nullptr;
      cs = &t.color; rt = t.target; sync = t.sync;
      loadDepth = t.depthValid;
      t.inFlight = true;
    }
    if (ds) {
      // GXCopyTex and target switches can split one logical EFB frame across
      // multiple GXM scenes. Preserve Z across those boundaries for both the
      // display EFB and offscreen targets.
      sceGxmDepthStencilSurfaceSetForceLoadMode(ds, loadDepth ?
          SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
      sceGxmDepthStencilSurfaceSetForceStoreMode(ds, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
    }
    unsigned flags = sync ? SCE_GXM_SCENE_FRAGMENT_SET_DEPENDENCY : 0u;
    SceGxmSyncObject* vertexDependency = pendingVertexDependency;
    if (vertexDependency) flags |= SCE_GXM_SCENE_VERTEX_WAIT_FOR_DEPENDENCY;
    if (pendingFragmentTransferSync) flags |= SCE_GXM_SCENE_FRAGMENT_TRANSFER_SYNC;
    if (!check(sceGxmBeginScene(context, flags, rt, nullptr, vertexDependency, sync, cs, ds), "begin native scene")) return false;
    pendingVertexDependency = nullptr;
    pendingFragmentTransferSync = false;
    pipelineStateValid=false;viewportValid=false;vertexStreamValid=false;
    textureBindingValidMask=0;boundPipelineKey=0;boundVertexBuffer=0;boundVertexBase=0;
    inScene = true;
    return true;
  }

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
  std::shared_ptr<CompiledStage> compile_stage(const std::string& source, shark_type type,
                                               ProgramStage stage) {
    const uint64_t sourceHash = gxm_program_source_hash(source.c_str(), stage);
    if (const auto it = stageCache.find(sourceHash); it != stageCache.end()) {
      ++stageMemoryHits;
      return it->second;
    }
    auto compiled = std::make_shared<CompiledStage>();
    if (programCache.load(sourceHash, stage, compiled->code)) {
      if (check(sceGxmProgramCheck(reinterpret_cast<const SceGxmProgram*>(compiled->code.data())),
                "validate cached GXP")) {
        stageCache.emplace(sourceHash, compiled);
        return compiled;
      }
      error.clear();
      compiled->code.clear();
    }
    if (source.size() > UINT32_MAX) { fail("shader source too large"); return {}; }
    const uint64_t started = sceKernelGetProcessTimeWide();
    uint32_t size = uint32_t(source.size());
    const SceGxmProgram* program = shark_compile_shader(source.c_str(), &size, type);
    if (!program || !size) { shark_clear_output(); fail("Cg compilation failed; see shader diagnostics"); return {}; }
    // Compiler output belongs to vitaShaRK. Registered headers must outlive the
    // patched programs, so take an aligned owned copy before clearing output.
    compiled->code.resize((size + 3u) / 4u);
    std::memcpy(compiled->code.data(), program, size);
    shark_clear_output();
    if (!check(sceGxmProgramCheck(reinterpret_cast<const SceGxmProgram*>(compiled->code.data())), "validate GXP")) return {};
    programCache.save(sourceHash, stage, compiled->code, size);
    stageCache.emplace(sourceHash, compiled);
    ++stageCompiles;
    const uint64_t elapsed = sceKernelGetProcessTimeWide() - started;
    if (stageCompiles <= 8 || (stageCompiles & (stageCompiles - 1)) == 0)
      std::fprintf(stderr,
          "[aurora-gxm] stage_compile stage=%c hash=%016llx us=%llu compiled=%u mem_hits=%u disk_hits=%u disk_misses=%u\n",
          stage == ProgramStage::Vertex ? 'v' : 'f', static_cast<unsigned long long>(sourceHash),
          static_cast<unsigned long long>(elapsed), stageCompiles, stageMemoryHits,
          programCache.hits(), programCache.misses());
    return compiled;
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
  log_memory_state("before-init");
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
  const int poolResult = initialize_cdram_pool(config.cdramPoolBytes, config.cdramReserveBytes);
  if (poolResult < 0)
    std::fprintf(stderr,
        "[aurora-gxm] cdram_pool unavailable requested=%zu reserve=%zu error=0x%08x; using mapped fallbacks\n",
        config.cdramPoolBytes, config.cdramReserveBytes, unsigned(poolResult));
  const uint32_t tw = (config.width + 31u) & ~31u, th = (config.height + 31u) & ~31u;
  if (!d.alloc(d.depth, size_t(tw) * th * 4, MemoryKind::GpuResource)) return abort();
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
  d.programCache.configure(config.programCachePath);
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
  log_memory_state("after-init");
  return true;
}

uint64_t Renderer::create_pipeline(const PipelineDesc& desc) {
  auto& d = *impl_;
  if (!d.initialized) { d.fail("create pipeline before initialization"); return 0; }
  const uint64_t key = pipeline_key(desc);
  if (d.pipelines.find(key) != d.pipelines.end()) { ++d.stats.pipelineHits; return key; }
  if (d.pipelines.size() >= d.config.maxPipelines) { d.fail("native pipeline budget exhausted"); return 0; }
  const auto source = build_tev_cg(desc);
  if (!source.ok()) { d.fail(source.error.c_str()); return 0; }
  auto pipeline = std::make_unique<Impl::Pipeline>();
  auto& p = *pipeline; p.desc = desc; p.textureMask = source.textureMask;
  p.vertexCode = d.compile_stage(source.vertex, SHARK_VERTEX_SHADER, ProgramStage::Vertex);
  p.fragmentCode = d.compile_stage(source.fragment, SHARK_FRAGMENT_SHADER, ProgramStage::Fragment);
  if (!p.vertexCode || !p.fragmentCode) return 0;
  const auto* vp = reinterpret_cast<const SceGxmProgram*>(p.vertexCode->code.data());
  const auto* fp = reinterpret_cast<const SceGxmProgram*>(p.fragmentCode->code.data());
  const auto abort = [&]() { d.destroy_pipeline(p); return uint64_t{0}; };
  if (!d.check(sceGxmShaderPatcherRegisterProgram(d.patcher, vp, &p.vertexId), "register vertex GXP") ||
      !d.check(sceGxmShaderPatcherRegisterProgram(d.patcher, fp, &p.fragmentId), "register fragment GXP")) return abort();
  std::vector<SceGxmVertexAttribute> attributes;
  for (unsigned i = 0; i < desc.layout.count; ++i) {
    const auto& a = desc.layout.attributes[i];
    char name[24];
    if (!a.location) std::snprintf(name, sizeof(name), "a_position");
    else if (a.location < 3) std::snprintf(name, sizeof(name), "a_color%u", unsigned(a.location - 1));
    else if (a.location <= 10) std::snprintf(name, sizeof(name), "a_tex%u", unsigned(a.location - 3));
    else if (a.location == 11) std::snprintf(name, sizeof(name), "a_normal");
    else if (a.location == 12) std::snprintf(name, sizeof(name), "a_binormal");
    else if (a.location == 13) std::snprintf(name, sizeof(name), "a_tangent");
    else std::snprintf(name, sizeof(name), "a_pn_mtx");
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
  p.fogColor=sceGxmProgramFindParameterByName(fp,"u_fog_color");
  p.fogParams=sceGxmProgramFindParameterByName(fp,"u_fog_params");
  p.fogRange=sceGxmProgramFindParameterByName(fp,"u_fog_range_k");
  p.viewportWidth=sceGxmProgramFindParameterByName(fp,"u_render_viewport_width");
  p.indirectMatrices=sceGxmProgramFindParameterByName(fp,"u_ind_mtx");
  p.texcoordScale=sceGxmProgramFindParameterByName(fp,"u_texcoord_scale");
  p.textureSizeBias=sceGxmProgramFindParameterByName(fp,"u_texture_size_bias");
  p.textureTransform=sceGxmProgramFindParameterByName(fp,"u_tex_transform");
  p.textureWrap=sceGxmProgramFindParameterByName(fp,"u_tex_wrap");
  p.textureForceOpaque=sceGxmProgramFindParameterByName(fp,"u_tex_force_opaque");
  p.textureCopyMode=sceGxmProgramFindParameterByName(fp,"u_tex_copy_mode");
  p.gxPosition=sceGxmProgramFindParameterByName(vp,"u_gx_position");
  p.gxNormal=sceGxmProgramFindParameterByName(vp,"u_gx_normal");
  p.gxPositionPalette=sceGxmProgramFindParameterByName(vp,"u_gx_position_palette");
  p.gxNormalPalette=sceGxmProgramFindParameterByName(vp,"u_gx_normal_palette");
  p.gxMaterial=sceGxmProgramFindParameterByName(vp,"u_gx_material");
  p.gxAmbient=sceGxmProgramFindParameterByName(vp,"u_gx_ambient");
  p.gxLight=sceGxmProgramFindParameterByName(vp,"u_gx_light");
  for(unsigned i=0;i<MaxTextures;++i) {
    char name[32];
    std::snprintf(name,sizeof(name),"u_gx_texture%u",i);
    p.gxTexture[i]=sceGxmProgramFindParameterByName(vp,name);
    std::snprintf(name,sizeof(name),"u_gx_post%u",i);
    p.gxPost[i]=sceGxmProgramFindParameterByName(vp,name);
  }
  d.pipelines.emplace(key, std::move(pipeline));
  ++d.stats.pipelineMisses;
  return key;
}

Handle Renderer::create_buffer(const void* data, size_t bytes) {
  auto& d = *impl_;
  if (!d.initialized || !bytes || bytes > UINT32_MAX || !d.nextHandle || !d.has_budget(bytes)) {
    d.fail("invalid buffer upload or resource budget exhausted"); return 0;
  }
  Impl::Buffer buffer;
  if (!d.alloc(buffer.memory, bytes, MemoryKind::CpuGpu)) return 0;
  buffer.bytes = bytes;
  if (data) std::memcpy(buffer.memory.data(), data, bytes);
  else std::memset(buffer.memory.data(), 0, bytes);
  d.resourceBytes += buffer.memory.size();
  const Handle handle = d.nextHandle++;
  d.buffers.emplace(handle, std::move(buffer));
  return handle;
}

Handle Renderer::create_texture(const TextureDesc& desc) {
  auto& d = *impl_;
  if (!d.initialized || !desc.data || !desc.width || desc.width > 4096 || !desc.height || desc.height > 4096 || !d.nextHandle) {
    d.fail("texture upload requires a bounded image"); return 0;
  }
  const size_t encoded = encoded_texture_size(desc.width, desc.height, desc.format);
  if (!encoded || desc.dataSize < encoded) { d.fail("truncated encoded texture"); return 0; }
  const uint64_t key = texture_key(desc);
  if (desc.cacheable) {
    const auto it = d.textureCache.find(key);
    if (it != d.textureCache.end()) { ++d.stats.textureHits; return it->second; }
  }
  const bool swizzled=(desc.width&(desc.width-1u))==0 &&
      (desc.height&(desc.height-1u))==0 && desc.mipCount<=1 && !desc.generateMipmaps;
  auto pixels=swizzled?prepare_swizzled_texture(desc):prepare_linear_texture(desc);
  if(!pixels.ok()) {d.fail(pixels.error.c_str());return 0;}
  const uint32_t rowBytes = ((desc.width + 7u) & ~7u) * 4u;
  const size_t bytes=pixels.pixels.size();
  if (!d.has_budget(bytes)) { d.fail("native texture budget exhausted"); return 0; }
  auto owned = std::make_unique<Impl::Texture>();
  auto& texture = *owned;
  texture.width=desc.width; texture.height=desc.height; texture.stride=rowBytes/4;
  texture.mipCount=pixels.mipCount;
  texture.swizzled=swizzled;
  if (!d.alloc(texture.memory, bytes, MemoryKind::GpuResource)) return 0;
  std::memcpy(texture.memory.data(),pixels.pixels.data(),bytes);
  // Ordinary linear textures permit sampler filter changes. LINEAR_STRIDED
  // rejects them with SCE_GXM_ERROR_UNSUPPORTED on hardware. The native linear
  // RGBA8 layout has the same eight-texel row padding used above.
  const int textureResult=swizzled?
      sceGxmTextureInitSwizzled(&texture.descriptor, texture.memory.data(),
          SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, desc.width, desc.height, texture.mipCount):
      sceGxmTextureInitLinear(&texture.descriptor, texture.memory.data(),
          SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, desc.width, desc.height, texture.mipCount);
  if (!d.check(textureResult, "initialize native texture")) return 0;
  d.resourceBytes += texture.memory.size();
  const Handle handle = d.nextHandle++;
  d.textures.emplace(handle, std::move(owned));
  if (desc.cacheable) d.textureCache.emplace(key, handle);
  ++d.stats.textureMisses; ++d.stats.textureUploads;
  return handle;
}

bool Renderer::begin_frame() {
  auto& d = *impl_;
  if (!d.initialized || d.frameActive)
    return d.fail("invalid begin_frame");
  const int displayError = d.displayError.load(std::memory_order_relaxed);
  if (displayError < 0) return d.fail("display callback failed", displayError);
  d.stats = {}; d.frameStarted = sceKernelGetProcessTimeWide();
  d.profileDraws = (++d.profileFrame % 120u) == 60u;
  d.frameActive = true; d.boundTarget = 0;
  return true;
}

bool Renderer::begin_frame(const Color& color, float depth) {
  return begin_frame() && clear(color, depth);
}

bool Renderer::clear(const Color& color, float depth, bool rgb, bool alpha, bool writeDepth) {
  auto& d = *impl_;
  if (!d.initialized || !d.frameActive) return d.fail("clear outside a frame");
  if (!std::isfinite(depth) || depth<0.f || depth>1.f) return d.fail("invalid clear depth");
  if (!rgb && !alpha && !writeDepth) return true;
  auto desc=d.pipelines.at(d.clearPipeline)->desc;
  desc.fragmentScissor=false; // The clear always covers the complete target.
  desc.colorWrite=rgb; desc.alphaWrite=alpha; desc.depthWrite=writeDepth;
  const auto key=create_pipeline(desc);
  if (!key) return false;
  DrawPacket packet{};
  packet.pipelineKey=key;
  packet.vertices={d.clearVertices,0,48}; packet.indices={d.clearIndices,0,6};
  packet.vertexCount=3; packet.indexCount=3;
  packet.viewport.width=float(d.width()); packet.viewport.height=float(d.height());
  packet.scissor.width=d.width(); packet.scissor.height=d.height();
  packet.uniforms.kcolor[0]={color.r,color.g,color.b,color.a};
  packet.uniforms.mvp[14]=depth-1.f;
  return draw(packet);
}

bool Renderer::bind_texture(Handle handle,unsigned unit,const SamplerDesc& s) {
  auto& d=*impl_;
  const auto it=d.textures.find(handle);
  if(unit>=MaxTextures || it==d.textures.end() || handle==d.boundTarget ||
      s.minFilter>Filter::LinearMipmapLinear || s.magFilter>Filter::Linear ||
      s.wrapS>WrapMode::Mirror || s.wrapT>WrapMode::Mirror ||
      !std::isfinite(s.lodBias) || !std::isfinite(s.minLod) || !std::isfinite(s.maxLod) ||
      s.minLod<0 || s.maxLod<s.minLod) return d.fail("invalid native texture binding");
  if(!d.ensure_scene()) return false;
  if((d.textureBindingValidMask&(1u<<unit))&&d.boundTextureHandles[unit]==handle&&
     same_sampler(d.boundTextureSamplers[unit],s)) {
    it->second->inFlight=true;
    return true;
  }
  auto& textureObject=*it->second;
  auto& texture=textureObject.descriptor;
  const bool point=s.minFilter==Filter::Nearest || s.minFilter==Filter::NearestMipmapNearest || s.minFilter==Filter::NearestMipmapLinear;
  const bool useMips=s.minFilter>Filter::Linear && it->second->mipCount>1;
  const bool trilinear=s.minFilter==Filter::NearestMipmapLinear || s.minFilter==Filter::LinearMipmapLinear;
  const unsigned mips=useMips?std::min(it->second->mipCount,unsigned(std::min(std::floor(s.maxLod),12.f))+1u):1u;
  const unsigned bias=native_lod_bias(s.lodBias);
  const auto address=[&](WrapMode mode) {
    if (!it->second->swizzled || mode==WrapMode::Clamp) return SCE_GXM_TEXTURE_ADDR_CLAMP;
    return mode==WrapMode::Repeat?SCE_GXM_TEXTURE_ADDR_REPEAT:SCE_GXM_TEXTURE_ADDR_MIRROR;
  };
  if(!textureObject.descriptorSamplerValid||!same_sampler(textureObject.descriptorSampler,s)) {
    if(!d.check(sceGxmTextureSetMinFilter(&texture,point?SCE_GXM_TEXTURE_FILTER_POINT:SCE_GXM_TEXTURE_FILTER_LINEAR),"texture min filter") ||
       !d.check(sceGxmTextureSetMagFilter(&texture,s.magFilter==Filter::Nearest?SCE_GXM_TEXTURE_FILTER_POINT:SCE_GXM_TEXTURE_FILTER_LINEAR),"texture mag filter") ||
       !d.check(sceGxmTextureSetUAddrMode(&texture,address(s.wrapS)),"texture U wrap") ||
       !d.check(sceGxmTextureSetVAddrMode(&texture,address(s.wrapT)),"texture V wrap") ||
       !d.check(sceGxmTextureSetMipmapCount(&texture,mips),"texture mip count") ||
       !d.check(sceGxmTextureSetMipFilter(&texture,useMips&&trilinear?SCE_GXM_TEXTURE_MIP_FILTER_ENABLED:SCE_GXM_TEXTURE_MIP_FILTER_DISABLED),"texture mip filter") ||
       !d.check(sceGxmTextureSetLodBias(&texture,bias),"texture LOD bias")) return false;
    textureObject.descriptorSampler=s;
    textureObject.descriptorSamplerValid=true;
  }
  if(!d.check(sceGxmSetFragmentTexture(d.context,unit,&texture),"bind fragment texture")) return false;
  d.textureBindingValidMask|=static_cast<uint8_t>(1u<<unit);
  d.boundTextureHandles[unit]=handle;d.boundTextureSamplers[unit]=s;
  it->second->inFlight=true;
  return true;
}

bool Renderer::bind_pipeline(uint64_t key,const GpuDrawUniforms& u,const Scissor& scissor,
                             const FixedVertexUniforms* fixedVertex,
                             const std::array<TextureBinding,MaxTextures>* textures) {
  auto& d=*impl_;
  const auto it=d.pipelines.find(key);
  if(it==d.pipelines.end()) return d.fail("unknown native pipeline");
  if(!d.ensure_scene()) return false;
  auto& p=*it->second;
  const auto& pipeline=p.desc;
  unsigned usedTextureCount=0;
  for(unsigned i=0;i<MaxTextures;++i)
    if(p.textureMask&(1u<<i)) usedTextureCount=i+1;
  if(!d.pipelineStateValid||d.boundPipelineKey!=key) {
    sceGxmSetVertexProgram(d.context,p.vertex);
    sceGxmSetFragmentProgram(d.context,p.fragment);
    const auto compare=pipeline.depthTest?depth_func(pipeline.depthFunc):SCE_GXM_DEPTH_FUNC_ALWAYS;
    const auto write=pipeline.depthTest&&pipeline.depthWrite?SCE_GXM_DEPTH_WRITE_ENABLED:SCE_GXM_DEPTH_WRITE_DISABLED;
    sceGxmSetFrontDepthFunc(d.context,compare);sceGxmSetBackDepthFunc(d.context,compare);
    sceGxmSetFrontDepthWriteEnable(d.context,write);sceGxmSetBackDepthWriteEnable(d.context,write);
    sceGxmSetCullMode(d.context,pipeline.cull==CullMode::None?SCE_GXM_CULL_NONE:
        pipeline.cull==CullMode::Back?SCE_GXM_CULL_CCW:SCE_GXM_CULL_CW);
    d.pipelineStateValid=true;d.boundPipelineKey=key;
  }
  const float clip[]{float(scissor.x),float(scissor.y),float(int64_t(scissor.x)+scissor.width),float(int64_t(scissor.y)+scissor.height)};
  void* vertex=nullptr;
  const bool needsVertexUniforms=p.mvp || (pipeline.fixedVertexOnGpu && fixedVertex);
  if(needsVertexUniforms && !d.check(sceGxmReserveVertexDefaultUniformBuffer(d.context,&vertex),"reserve vertex uniforms")) return false;
  const auto uploadVertex=[&](const SceGxmProgramParameter* param,unsigned count,const float* data) {
    if(!param) return true;
    count=std::min(count,sceGxmProgramParameterGetComponentCount(param)*sceGxmProgramParameterGetArraySize(param));
    return d.check(sceGxmSetUniformDataF(vertex,param,0,count,data),"upload vertex uniform");
  };
  if(p.mvp && !uploadVertex(p.mvp,16,u.mvp.data())) return false;
  if(pipeline.fixedVertexOnGpu) {
    if(!fixedVertex) return d.fail("missing fixed GX vertex uniforms");
    if(pipeline.fixedVertexIndexedPn) {
      if(!uploadVertex(p.gxPositionPalette,120,fixedVertex->positionPalette[0].data()) ||
         !uploadVertex(p.gxNormalPalette,120,fixedVertex->normalPalette[0].data())) return false;
    } else if(!uploadVertex(p.gxPosition,12,fixedVertex->position.data()) ||
              !uploadVertex(p.gxNormal,12,fixedVertex->normal.data())) return false;
    if(!uploadVertex(p.gxMaterial,16,fixedVertex->material[0].data()) ||
       !uploadVertex(p.gxAmbient,16,fixedVertex->ambient[0].data())) return false;
    unsigned lightTop=0;
    for(const auto& channel:pipeline.colorChannels)if(channel.lightingEnabled)
      for(unsigned i=0;i<MaxLights;++i)if(channel.lightMask&(1u<<i))lightTop=std::max(lightTop,i+1u);
    if(lightTop&&!uploadVertex(p.gxLight,lightTop*20u,fixedVertex->light[0].data())) return false;
    for(unsigned i=0;i<MaxTextures;++i) {
      if(!uploadVertex(p.gxTexture[i],12,fixedVertex->texture[i].data()) ||
         !uploadVertex(p.gxPost[i],12,fixedVertex->post[i].data())) return false;
    }
  }
  void* fragment=nullptr;
  if(!d.check(sceGxmReserveFragmentDefaultUniformBuffer(d.context,&fragment),"reserve fragment uniforms")) return false;
  const auto upload=[&](const SceGxmProgramParameter* param,unsigned count,const float* data) {
    if(!param || !count) return true;
    count=std::min(count,sceGxmProgramParameterGetComponentCount(param)*sceGxmProgramParameterGetArraySize(param));
    return d.check(sceGxmSetUniformDataF(fragment,param,0,count,data),"upload fragment uniform");
  };
  if(!upload(p.kcolor,16,u.kcolor[0].data()) || !upload(p.tevreg,16,u.tevreg[0].data()) || !upload(p.clip,4,clip)) return false;
  if(!upload(p.fogColor,4,u.fogColor.data()) || !upload(p.fogParams,4,u.fogParams.data()) ||
     !upload(p.fogRange,10,u.fogRangeK.data()) || !upload(p.viewportWidth,1,&u.renderViewportWidth) ||
     !upload(p.indirectMatrices,MaxIndMatrices*8,u.indirectMatrices[0].data()) ||
     !upload(p.texcoordScale,usedTextureCount*4,u.texcoordScale[0].data()) ||
     !upload(p.textureSizeBias,usedTextureCount*4,u.textureSizeBias[0].data()))return false;
  if(p.textureTransform) {
    std::array<std::array<float,4>,MaxTextures> transform{};
    for(unsigned i=0;i<usedTextureCount;++i) {
      const bool flipX=textures&&(*textures)[i].flipX;
      const bool flipY=textures&&(*textures)[i].flipY;
      transform[i]={flipX?-1.f:1.f,flipY?-1.f:1.f,flipX?1.f:0.f,flipY?1.f:0.f};
    }
    if(!upload(p.textureTransform,usedTextureCount*4,transform[0].data()))return false;
  }
  if(p.textureWrap) {
    std::array<std::array<float,4>,MaxTextures> wrap{};
    for(unsigned i=0;i<usedTextureCount;++i) {
      const auto wrapS=textures?(*textures)[i].sampler.wrapS:WrapMode::Clamp;
      const auto wrapT=textures?(*textures)[i].sampler.wrapT:WrapMode::Clamp;
      wrap[i]={float(static_cast<unsigned>(wrapS)),float(static_cast<unsigned>(wrapT)),0.f,0.f};
    }
    if(!upload(p.textureWrap,usedTextureCount*4,wrap[0].data()))return false;
  }
  if(p.textureForceOpaque) {
    std::array<float,MaxTextures> opaque{};
    for(unsigned i=0;i<usedTextureCount;++i)opaque[i]=textures&&(*textures)[i].forceOpaque?1.f:0.f;
    if(!upload(p.textureForceOpaque,usedTextureCount,opaque.data()))return false;
  }
  if(p.textureCopyMode) {
    std::array<float,MaxTextures> mode{};
    for(unsigned i=0;i<usedTextureCount;++i)
      mode[i]=textures&&(*textures)[i].sampleFormat==EfbCopyFormat::R4?1.f:0.f;
    if(!upload(p.textureCopyMode,usedTextureCount,mode.data()))return false;
  }
  p.inFlight=true;
  return true;
}

bool Renderer::draw(const DrawPacket& packet) {
  auto& d = *impl_;
  if (!d.ensure_scene()) return false;
  const auto pi = d.pipelines.find(packet.pipelineKey);
  const auto vi = d.buffers.find(packet.vertices.buffer), ii = d.buffers.find(packet.indices.buffer);
  if (pi == d.pipelines.end() || vi == d.buffers.end() || ii == d.buffers.end()) return d.fail("unknown pipeline or buffer handle");
  const auto& p = *pi->second;
  const auto& pipeline = p.desc;
  for (unsigned unit=0;unit<MaxTextures;++unit) if (pipeline.nativeTextureWrapMask&(1u<<unit)) {
    const auto texture=d.textures.find(packet.textures[unit].texture);
    if(texture==d.textures.end() || !texture->second->swizzled)
      return d.fail("native wrapping requires a swizzled texture");
  }
  if (packet.instanceCount != 1 || (pipeline.fixedVertexOnGpu != (packet.fixedVertexUniforms != nullptr)) ||
      !packet.indexCount || packet.firstVertex ||
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
#if !defined(NDEBUG)
  const size_t end = size_t(packet.vertices.offset) + packet.vertices.size;
  for (uint32_t i = 0; i < packet.indexCount; ++i) {
    const size_t offset = base + size_t(indices[i]) * stride;
    // GXM's INDEX_16BIT stream has a documented limit below 64000.
    if (indices[i] >= 64000 || offset < packet.vertices.offset || offset > end || stride > end - offset)
      return d.fail("vertex index outside the supplied slice or GXM range");
  }
#endif
  if (pipeline.cull == CullMode::All || packet.scissor.width <= 0 || packet.scissor.height <= 0) return true;
  if (!pipeline.fragmentScissor &&
      (packet.scissor.x > 0 || packet.scissor.y > 0 ||
       int64_t(packet.scissor.x) + packet.scissor.width < d.width() ||
       int64_t(packet.scissor.y) + packet.scissor.height < d.height()))
    return d.fail("full-target pipeline requires a full-target scissor");
  uint64_t profileTick = d.profileDraws ? sceKernelGetProcessTimeWide() : 0;
  if(!bind_pipeline(packet.pipelineKey,packet.uniforms,packet.scissor,packet.fixedVertexUniforms,&packet.textures)) return false;
  if(d.profileDraws) { const auto now=sceKernelGetProcessTimeWide(); d.stats.nativePipelineUs+=now-profileTick; profileTick=now; }
  for(unsigned i=0;i<MaxTextures;++i) if(p.textureMask&(1u<<i))
    if(!bind_texture(packet.textures[i].texture,i,packet.textures[i].sampler)) return false;
  if(d.profileDraws) { const auto now=sceKernelGetProcessTimeWide(); d.stats.nativeTextureUs+=now-profileTick; profileTick=now; }
  if(!d.viewportValid||!same_viewport(d.cachedViewport,vp)) {
    const float low = std::min(vp.znear, vp.zfar), high = std::max(vp.znear, vp.zfar);
    sceGxmSetViewport(d.context, vp.x + vp.width * .5f, vp.width * .5f,
        vp.y + vp.height * .5f, -vp.height * .5f, (low + high) * .5f, (high - low) * .5f);
    d.cachedViewport=vp;d.viewportValid=true;
  }
  if(!d.vertexStreamValid||d.boundVertexBuffer!=packet.vertices.buffer||d.boundVertexBase!=base) {
    if (!d.check(sceGxmSetVertexStream(d.context, 0, static_cast<uint8_t*>(vi->second.memory.data()) + base), "bind native vertex stream")) return false;
    d.vertexStreamValid=true;d.boundVertexBuffer=packet.vertices.buffer;d.boundVertexBase=base;
  }
  const auto primitive = pipeline.primitive == Primitive::Triangles ? SCE_GXM_PRIMITIVE_TRIANGLES :
      pipeline.primitive == Primitive::TriangleFan ? SCE_GXM_PRIMITIVE_TRIANGLE_FAN : SCE_GXM_PRIMITIVE_TRIANGLE_STRIP;
  if (!d.check(sceGxmDraw(d.context, primitive, SCE_GXM_INDEX_FORMAT_U16, indices, packet.indexCount), "draw indexed")) return false;
  if(d.profileDraws) d.stats.nativeDrawUs+=sceKernelGetProcessTimeWide()-profileTick;
  pi->second->inFlight = true;
  vi->second.inFlight = true; ii->second.inFlight = true;
  for(unsigned i=0;i<MaxTextures;++i) if(p.textureMask&(1u<<i))
    d.textures.at(packet.textures[i].texture)->inFlight=true;
  ++d.stats.drawCalls;
  d.stats.triangles += pipeline.primitive == Primitive::Triangles ? packet.indexCount / 3 : packet.indexCount - 2;
  return true;
}

bool Renderer::end_frame(bool present) {
  auto& d = *impl_;
  if (!d.frameActive) return d.fail("end_frame outside a frame");
  const uint64_t timingStart=sceKernelGetProcessTimeWide();
  if (!d.end_scene()) return false;
  const uint64_t afterEnd=sceKernelGetProcessTimeWide();
  d.frameActive = false;
  auto& surface = d.surfaces[d.back];
  if (present) {
    const DisplayRequest request{surface.memory.data(), d.config.width, d.config.height, d.stride, d.config.waitVblank, &d.displayError};
    if (!d.check(sceGxmDisplayQueueAddEntry(d.surfaces[d.front].sync, surface.sync, &request), "queue present")) return false;
    d.displayed = true; d.front = d.back; d.back = (d.back + 1) % d.config.displayBuffers;
  } else {
    // Discarded presents retain the current display; complete writes before the
    // same offscreen surface can become the next scene's target.
    if(!finish()) return false;
  }
  const uint64_t afterQueue=sceKernelGetProcessTimeWide();
  static uint64_t presentCount=0;
  const uint64_t count=++presentCount;
  if(present && (count<=8 || (count&(count-1))==0))
    std::fprintf(stderr,"[aurora-gxm] present_timing n=%llu end_us=%llu queue_us=%llu total_us=%llu\n",
      static_cast<unsigned long long>(count),
      static_cast<unsigned long long>(afterEnd-timingStart),
      static_cast<unsigned long long>(afterQueue-afterEnd),
      static_cast<unsigned long long>(afterQueue-timingStart));
  d.stats.cpuFrameUs = sceKernelGetProcessTimeWide() - d.frameStarted;
  if (d.profileDraws) log_memory_state("profile-frame");
  return true;
}

bool Renderer::readback_rgba8(std::vector<uint8_t>& pixels) {
  auto& d = *impl_;
  if (!d.initialized || d.inScene || !d.displayed) return d.fail("readback requires a completed presented frame");
  if(!finish()) return false;
  pixels.resize(size_t(d.config.width) * d.config.height * 4);
  const auto* source = static_cast<const uint8_t*>(d.surfaces[d.front].memory.data());
  for (uint32_t y = 0; y < d.config.height; ++y)
    std::memcpy(pixels.data() + size_t(y) * d.config.width * 4,
        source + size_t(y) * d.stride * 4, size_t(d.config.width) * 4);
  return true;
}

bool Renderer::finish() {
  auto& d=*impl_;
  if(!d.context) return true;
  if(!d.end_scene()) return false;
  if(d.pendingFragmentTransferSync) {
    if(!d.check(sceGxmTransferFinish(),"finish pending native transfer"))return false;
    d.pendingFragmentTransferSync=false;
  }
  sceGxmFinish(d.context);
  d.pendingVertexDependency=nullptr;
  for(auto& [_,b]:d.buffers) b.inFlight=false;
  for(auto& [_,t]:d.textures) t->inFlight=false;
  for(auto& [_,p]:d.pipelines) p->inFlight=false;
  return true;
}

bool Renderer::update_buffer(Handle handle,const void* data,size_t bytes,size_t offset,bool storageRetired) {
  auto& d=*impl_;
  auto it=d.buffers.find(handle);
  if(it==d.buffers.end() || offset>it->second.bytes || bytes>it->second.bytes-offset || (!data&&bytes))
    return d.fail("invalid buffer update");
  if(!bytes) return true;
  if(it->second.inFlight && !storageRetired && !finish()) return false;
  // StreamingArena only sets storageRetired after its frames-in-flight interval.
  // At that point the corresponding display work has already rotated through
  // GXM's bounded queue, so a second global finish here would serialize every
  // third frame and defeat the ring buffer.
  if(storageRetired) it->second.inFlight=false;
  std::memcpy(static_cast<uint8_t*>(it->second.memory.data())+offset,data,bytes);
  return true;
}

void Renderer::destroy_buffer(Handle handle) {
  auto& d=*impl_;
  auto it=d.buffers.find(handle);
  if(it==d.buffers.end()) return;
  if(it->second.inFlight && !finish()) return;
  d.resourceBytes-=it->second.memory.size();
  d.buffers.erase(it);
}

void Renderer::destroy_pipeline(uint64_t key) {
  auto& d=*impl_;
  auto it=d.pipelines.find(key);
  if(it==d.pipelines.end() || key==d.clearPipeline) return;
  if(it->second->inFlight && !finish()) return;
  d.destroy_pipeline(*it->second);
  d.pipelines.erase(it);
}

size_t Renderer::texture_bytes(Handle handle) const noexcept {
  const auto& d=*impl_;
  const auto it=d.textures.find(handle);
  return it==d.textures.end()?0:it->second->memory.size()+it->second->depth.size();
}

void Renderer::destroy_texture(Handle handle) {
  auto& d=*impl_;
  auto it=d.textures.find(handle);
  if(it==d.textures.end()) return;
  if((it->second->inFlight || handle==d.boundTarget) && !finish()) return;
  if(handle==d.boundTarget) d.boundTarget=0;
  d.resourceBytes-=it->second->memory.size()+it->second->depth.size();
  for(auto c=d.textureCache.begin();c!=d.textureCache.end();)
    if(c->second==handle) c=d.textureCache.erase(c); else ++c;
  d.textures.erase(it);
}

Handle Renderer::create_target(uint32_t width,uint32_t height,bool useDepth) {
  auto& d=*impl_;
  if(!d.initialized || !width || !height || width>4096 || height>4096 || !d.nextHandle) {
    d.fail("invalid render target size"); return 0;
  }
  const uint32_t stride=(width+7u)&~7u;
  const uint32_t dw=(width+31u)&~31u,dh=(height+31u)&~31u;
  const size_t colorBytes=(size_t(stride)*height*4+4095u)&~size_t(4095u);
  const size_t depthBytes=useDepth?((size_t(dw)*dh*4+4095u)&~size_t(4095u)):0;
  if(!d.has_budget(colorBytes+depthBytes)) {d.fail("render target budget exhausted");return 0;}
  auto texture=std::make_unique<Impl::Texture>();
  auto& t=*texture;
  t.width=width;t.height=height;t.stride=stride;
  if(!d.alloc(t.memory,colorBytes,MemoryKind::GpuResource)) return 0;
  std::memset(t.memory.data(),0,t.memory.size());
  if(!d.check(sceGxmTextureInitLinear(&t.descriptor,t.memory.data(),SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
          width,height,0),"initialize target texture") ||
     !d.check(sceGxmColorSurfaceInit(&t.color,SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR,
          SCE_GXM_COLOR_SURFACE_LINEAR,SCE_GXM_COLOR_SURFACE_SCALE_NONE,SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
          width,height,stride,t.memory.data()),"initialize target color surface")) return 0;
  if(useDepth) {
    if(!d.alloc(t.depth,depthBytes,MemoryKind::GpuResource) ||
       !d.check(sceGxmDepthStencilSurfaceInit(&t.depthSurface,SCE_GXM_DEPTH_STENCIL_FORMAT_DF32,
          SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,dw,t.depth.data(),nullptr),"initialize target depth")) return 0;
    sceGxmDepthStencilSurfaceSetBackgroundDepth(&t.depthSurface,1.f);
  }
  SceGxmRenderTargetParams params{};
  params.width=width;params.height=height;params.scenesPerFrame=1;
  params.multisampleMode=SCE_GXM_MULTISAMPLE_NONE;params.driverMemBlock=-1;
  if(!d.check(sceGxmCreateRenderTarget(&params,&t.target),"create offscreen render target") ||
     !d.check(sceGxmSyncObjectCreate(&t.sync),"create offscreen sync")) return 0;
  const Handle h=d.nextHandle++;
  d.resourceBytes+=t.memory.size()+t.depth.size();
  d.textures.emplace(h,std::move(texture));
  return h;
}

bool Renderer::bind_target(Handle handle) {
  auto& d=*impl_;
  if(!d.initialized) return d.fail("bind target before initialization");
  const auto it=d.textures.find(handle);
  if(handle && (it==d.textures.end() || !it->second->target)) return d.fail("unknown render target");
  if(handle==d.boundTarget) return true;
  // Target switches only need GPU ordering, not a CPU-wide finish. End the
  // current scene and make the next one wait on its fragment sync object. This
  // preserves render-to-texture and depth ordering while allowing the CPU to
  // continue preparing the next GX scene in parallel with the GPU.
  if(d.inScene) {
    SceGxmSyncObject* previousSync=d.boundTarget?
        d.textures.at(d.boundTarget)->sync:d.surfaces[d.back].sync;
    if(!d.end_scene()) return false;
    d.pendingVertexDependency=previousSync;
  }
  d.boundTarget=handle;
  return true;
}

bool Renderer::read_target(Handle handle,std::vector<uint8_t>& pixels) {
  auto& d=*impl_;
  const auto it=d.textures.find(handle);
  if(it==d.textures.end()) return d.fail("unknown readback texture");
  if(!finish()) return false;
  const auto& t=*it->second;
  pixels.resize(size_t(t.width)*t.height*4);
  for(uint32_t y=0;y<t.height;++y)
    std::memcpy(pixels.data()+size_t(y)*t.width*4,
        static_cast<const uint8_t*>(t.memory.data())+size_t(y)*t.stride*4,size_t(t.width)*4);
  return true;
}

bool Renderer::read_current(std::vector<uint8_t>& pixels,uint32_t& width,uint32_t& height) {
  auto& d=*impl_;
  if(!d.initialized || !d.frameActive) return d.fail("read current requires an active frame");
  width=d.width();height=d.height();
  if(d.boundTarget) return read_target(d.boundTarget,pixels);
  if(!finish()) return false;
  pixels.resize(size_t(width)*height*4);
  const auto* source=static_cast<const uint8_t*>(d.surfaces[d.back].memory.data());
  for(uint32_t y=0;y<height;++y)
    std::memcpy(pixels.data()+size_t(y)*width*4,source+size_t(y)*d.stride*4,size_t(width)*4);
  return true;
}

bool Renderer::upload_target(Handle handle,const void* rgba,uint32_t width,uint32_t height) {
  auto& d=*impl_;
  const auto it=d.textures.find(handle);
  if(!rgba || it==d.textures.end() || it->second->width!=width || it->second->height!=height)
    return d.fail("invalid EFB upload");
  auto& t=*it->second;
  if((t.inFlight || handle==d.boundTarget) && !finish()) return false;
  for(uint32_t y=0;y<height;++y)
    std::memcpy(static_cast<uint8_t*>(t.memory.data())+size_t(y)*t.stride*4,
      static_cast<const uint8_t*>(rgba)+size_t(y)*width*4,size_t(width)*4);
  return true;
}

bool Renderer::copy_current_to_target(Handle handle,const Scissor& source,EfbCopyFormat format,
                                      bool flipX,bool flipY) {
  auto& d=*impl_;
  const auto destinationIt=d.textures.find(handle);
  if(!d.initialized || !d.frameActive || destinationIt==d.textures.end() ||
     !destinationIt->second->target || source.x<0 || source.y<0 ||
     source.width<=0 || source.height<=0)
    return d.fail("invalid native EFB GPU copy");
  auto& destination=*destinationIt->second;
  const uint32_t sourceWidth=d.width(),sourceHeight=d.height();
  const bool sameSize=uint32_t(source.width)==destination.width &&
      uint32_t(source.height)==destination.height;
  const bool halfScale=uint32_t(source.width)==destination.width*2u &&
      uint32_t(source.height)==destination.height*2u;
  if(uint64_t(source.x)+uint64_t(source.width)>sourceWidth ||
     uint64_t(source.y)+uint64_t(source.height)>sourceHeight ||
     (format!=EfbCopyFormat::Passthrough && format!=EfbCopyFormat::RGB565) ||
     (!sameSize && !halfScale))
    return d.fail("unsupported native EFB GPU copy");

  const Handle originalTarget=d.boundTarget;
  const void* sourceData=nullptr;
  uint32_t sourceStride=0;
  SceGxmSyncObject* sourceSync=nullptr;
  if(originalTarget) {
    const auto sourceIt=d.textures.find(originalTarget);
    if(sourceIt==d.textures.end())return d.fail("missing native EFB source");
    sourceData=sourceIt->second->memory.data();
    sourceStride=sourceIt->second->stride;
    sourceSync=sourceIt->second->sync;
  } else {
    sourceData=d.surfaces[d.back].memory.data();
    sourceStride=d.stride;
    sourceSync=d.surfaces[d.back].sync;
  }

  // Melee commonly copies a GX EFB rectangle 1:1 after the logical-to-Vita
  // framebuffer mapping. Keep both that case and the Strikers 2x downscale on
  // the transfer engine so neither path falls back to a full framebuffer CPU
  // readback between ordered GXCopyTex boundaries.
  const uint64_t copyStarted=sceKernelGetProcessTimeWide();
  const bool asyncCopy=format==EfbCopyFormat::Passthrough && !flipX && !flipY;
  if(asyncCopy) {
    if(!d.end_scene())return false;
  } else if(!finish()) return false;
  const uint64_t afterSourceFinish=sceKernelGetProcessTimeWide();
  const int transferResult=sameSize?
      sceGxmTransferCopy(destination.width,destination.height,0,0,SCE_GXM_TRANSFER_COLORKEY_NONE,
          SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR,SCE_GXM_TRANSFER_LINEAR,sourceData,
          static_cast<unsigned>(source.x),static_cast<unsigned>(source.y),static_cast<int>(sourceStride*4u),
          SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR,SCE_GXM_TRANSFER_LINEAR,destination.memory.data(),
          0,0,static_cast<int>(destination.stride*4u),asyncCopy?sourceSync:nullptr,
          asyncCopy?SCE_GXM_TRANSFER_FRAGMENT_SYNC:0u,nullptr):
      sceGxmTransferDownscale(SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR,
          sourceData,static_cast<unsigned>(source.x),static_cast<unsigned>(source.y),
          static_cast<unsigned>(source.width),static_cast<unsigned>(source.height),
          static_cast<int>(sourceStride*4u),SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR,
          destination.memory.data(),0,0,static_cast<int>(destination.stride*4u),
          asyncCopy?sourceSync:nullptr,asyncCopy?SCE_GXM_TRANSFER_FRAGMENT_SYNC:0u,nullptr);
  if(!d.check(transferResult,sameSize?"native EFB copy transfer":"native EFB downscale transfer"))return false;
  const uint64_t afterTransferSubmit=sceKernelGetProcessTimeWide();
  if(!asyncCopy && !d.check(sceGxmTransferFinish(),"finish native EFB downscale transfer"))return false;
  const uint64_t afterTransferFinish=sceKernelGetProcessTimeWide();

  // Preserve the shared GXCopyTex orientation contract. TransferDownscale has
  // no mirror flags, so apply the requested mirror to the already-downscaled
  // uncached CPU/GPU surface. This touches only the 480x272 destination.
  auto* pixels=static_cast<uint32_t*>(destination.memory.data());
  if(format==EfbCopyFormat::RGB565) {
    for(uint32_t y=0;y<destination.height;++y) {
      auto* row=pixels+size_t(y)*destination.stride;
      for(uint32_t x=0;x<destination.width;++x)row[x]|=0xff000000u;
    }
  }
  if(flipX) {
    for(uint32_t y=0;y<destination.height;++y) {
      auto* row=pixels+size_t(y)*destination.stride;
      for(uint32_t x=0;x<destination.width/2u;++x)
        std::swap(row[x],row[destination.width-1u-x]);
    }
  }
  if(flipY) {
    for(uint32_t y=0;y<destination.height/2u;++y) {
      auto* top=pixels+size_t(y)*destination.stride;
      auto* bottom=pixels+size_t(destination.height-1u-y)*destination.stride;
      for(uint32_t x=0;x<destination.width;++x)std::swap(top[x],bottom[x]);
    }
  }
  const uint64_t afterCpuFixup=sceKernelGetProcessTimeWide();
  ++d.stats.nativeEfbCopies;
  d.stats.nativeEfbEndSceneUs+=afterSourceFinish-copyStarted;
  d.stats.nativeEfbTransferSubmitUs+=afterTransferSubmit-afterSourceFinish;
  d.stats.nativeEfbTransferWaitUs+=afterTransferFinish-afterTransferSubmit;
  d.stats.nativeEfbCpuFixupUs+=afterCpuFixup-afterTransferFinish;
  if(asyncCopy) {
    d.pendingFragmentTransferSync=true;
    destination.inFlight=true;
  }
  static uint64_t copyTimingCount=0;
  const uint64_t copyN=++copyTimingCount;
  if(copyN<=8 || (copyN&(copyN-1u))==0)
    std::fprintf(stderr,
      "[aurora-gxm] efb_transfer_timing n=%llu source_finish_us=%llu transfer_submit_us=%llu transfer_wait_us=%llu cpu_fixup_us=%llu total_us=%llu\n",
      static_cast<unsigned long long>(copyN),
      static_cast<unsigned long long>(afterSourceFinish-copyStarted),
      static_cast<unsigned long long>(afterTransferSubmit-afterSourceFinish),
      static_cast<unsigned long long>(afterTransferFinish-afterTransferSubmit),
      static_cast<unsigned long long>(afterCpuFixup-afterTransferFinish),
      static_cast<unsigned long long>(afterCpuFixup-copyStarted));
  destination.inFlight=false;
  d.pendingVertexDependency=nullptr;
  d.boundTarget=originalTarget;
  return true;
}

bool Renderer::blit_to_default(Handle handle) {
  auto& d=*impl_;
  if(d.textures.find(handle)==d.textures.end()) return d.fail("unknown blit texture");
  if(!bind_target(0)) return false;
  PipelineDesc p{};
  p.cull=CullMode::None;p.depthTest=false;p.depthWrite=false;p.reversedZ=false;
  p.layout.count=2;
  p.layout.attributes[0]={0,4,VertexScalar::F32,false,28,0};
  p.layout.attributes[1]={3,3,VertexScalar::F32,false,28,16};
  p.tev.stages[0].texture=0;p.tev.stages[0].texCoord=0;
  p.tev.stages[0].color.d=TevColorArg::TexColor;p.tev.stages[0].alpha.d=TevAlphaArg::TexAlpha;
  const auto key=create_pipeline(p);
  if(!key) return false;
  if(!d.blitVertices) {
    // Match the legacy vitaGL display-copy convention. Native render targets are
    // sampled with the opposite vertical origin from GL FBO textures, so the
    // fullscreen copy must invert V once when presenting a captured EFB.
    const float vertices[]{-1,-1,-.5f,1, 0,0,1, 3,-1,-.5f,1, 2,0,1, -1,3,-.5f,1, 0,2,1};
    d.blitVertices=create_buffer(vertices,sizeof(vertices));
  }
  if(!d.blitVertices) return false;
  DrawPacket packet{};
  packet.pipelineKey=key;
  packet.vertices={d.blitVertices,0,84};packet.indices={d.clearIndices,0,6};
  packet.vertexCount=packet.indexCount=3;
  packet.viewport.width=float(d.config.width);packet.viewport.height=float(d.config.height);
  packet.scissor.width=d.config.width;packet.scissor.height=d.config.height;
  packet.textures[0].texture=handle;
  packet.textures[0].sampler.wrapS=packet.textures[0].sampler.wrapT=WrapMode::Clamp;
  return draw(packet);
}

bool Renderer::copy_display_region(const Scissor& source) {
  auto& d=*impl_;
  const uint64_t timingStart=sceKernelGetProcessTimeWide();
  if(!d.initialized || !d.frameActive || d.boundTarget || source.x<0 || source.y<0 ||
     source.width<=0 || source.height<=0 || uint64_t(source.x)+uint64_t(source.width)>d.config.width ||
     uint64_t(source.y)+uint64_t(source.height)>d.config.height)
    return d.fail("invalid native display-copy region");
  if(d.config.displayBuffers<3) return d.fail("native display copy requires three display buffers");

  // The Vita frontend already maps the logical GX EFB into the native render
  // target. When GXCopyDisp selects that complete image, the EFB is already the
  // exact XFB we need to scan out; a second textured scene would only resample
  // the same pixels and force an unnecessary scene dependency.
  if(source.x==0 && source.y==0 && uint32_t(source.width)==d.config.width &&
     uint32_t(source.height)==d.config.height) {
    static uint64_t passthroughCount=0;
    const uint64_t count=++passthroughCount;
    if(count<=4 || (count&(count-1))==0)
      std::fprintf(stderr,"[aurora-gxm] display_copy passthrough n=%llu source=%d,%d %dx%d\n",
                   static_cast<unsigned long long>(count),source.x,source.y,source.width,source.height);
    return true;
  }

  // GXCopyDisp needs arbitrary crop/scale. Keep it on the GPU: complete the EFB
  // scene, sample that display surface as a texture, and render into the third
  // display buffer. This replaces the old full-frame GPU->CPU->GPU round-trip.
  const uint32_t sourceBuffer=d.back;
  // Submit the EFB scene without stalling the CPU. The following display-copy
  // scene waits on its fragment dependency in the GPU command stream.
  if(!d.end_scene()) return false;
  const uint64_t afterEnd=sceKernelGetProcessTimeWide();
  uint32_t destination=UINT32_MAX;
  for(uint32_t i=0;i<d.config.displayBuffers;++i)
    if(i!=sourceBuffer && i!=d.front) {destination=i;break;}
  if(destination==UINT32_MAX) return d.fail("no free display-copy destination");

  Impl::Texture* alias=nullptr;
  if(!d.displaySource) {
    auto texture=std::make_unique<Impl::Texture>();
    texture->width=d.config.width;texture->height=d.config.height;texture->stride=d.stride;
    d.displaySource=d.nextHandle++;
    alias=texture.get();
    d.textures.emplace(d.displaySource,std::move(texture));
  } else alias=d.textures.at(d.displaySource).get();
  const uint32_t expectedStride=(d.config.width+7u)&~7u;
  if(d.stride!=expectedStride) return d.fail("display surface stride cannot be sampled linearly");
  if(!d.check(sceGxmTextureInitLinear(&alias->descriptor,d.surfaces[sourceBuffer].memory.data(),
      SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,d.config.width,d.config.height,0),"initialize display source texture")) return false;

  d.back=destination;
  d.pendingVertexDependency=d.surfaces[sourceBuffer].sync;
  PipelineDesc p{};
  p.cull=CullMode::None;p.depthTest=false;p.depthWrite=false;p.reversedZ=false;
  p.layout.count=2;
  p.layout.attributes[0]={0,4,VertexScalar::F32,false,28,0};
  p.layout.attributes[1]={3,3,VertexScalar::F32,false,28,16};
  p.tev.stages[0].texture=0;p.tev.stages[0].texCoord=0;
  p.tev.stages[0].color.d=TevColorArg::TexColor;p.tev.stages[0].alpha.d=TevAlphaArg::TexAlpha;
  const auto key=create_pipeline(p);
  if(!key) return false;
  const float u0=float(source.x)/float(d.config.width);
  const float u1=(float(source.x)+float(source.width))/float(d.config.width);
  const float v0=float(source.y)/float(d.config.height);
  const float v1=(float(source.y)+float(source.height))/float(d.config.height);
  const float du=u1-u0,dv=v1-v0;
  // GXM's viewport uses negative Y scale, so bottom clip vertices take the
  // source rectangle's bottom V. This preserves the top-left GX image origin.
  const float vertices[]{-1,-1,-.5f,1, u0,v1,1,
                          3,-1,-.5f,1, u0+2*du,v1,1,
                         -1, 3,-.5f,1, u0,v1-2*dv,1};
  if(!d.blitVertices) d.blitVertices=create_buffer(vertices,sizeof(vertices));
  else if(!d.displayCopySourceValid || source.x!=d.displayCopySource.x || source.y!=d.displayCopySource.y ||
          source.width!=d.displayCopySource.width || source.height!=d.displayCopySource.height) {
    if(!update_buffer(d.blitVertices,vertices,sizeof(vertices),0,false)) return false;
  }
  if(!d.blitVertices) return false;
  d.displayCopySource=source;d.displayCopySourceValid=true;
  DrawPacket packet{};packet.pipelineKey=key;
  packet.vertices={d.blitVertices,0,84};packet.indices={d.clearIndices,0,6};
  packet.vertexCount=packet.indexCount=3;
  packet.viewport.width=float(d.config.width);packet.viewport.height=float(d.config.height);
  packet.scissor.width=d.config.width;packet.scissor.height=d.config.height;
  packet.textures[0].texture=d.displaySource;
  packet.textures[0].sampler.wrapS=packet.textures[0].sampler.wrapT=WrapMode::Clamp;
  packet.textures[0].sampler.minFilter=packet.textures[0].sampler.magFilter=Filter::Linear;
  const uint64_t beforeDraw=sceKernelGetProcessTimeWide();
  const bool ok=draw(packet);
  const uint64_t afterDraw=sceKernelGetProcessTimeWide();
  static uint64_t displayCopyCount=0;
  const uint64_t count=++displayCopyCount;
  if(count<=8 || (count&(count-1))==0) {
    std::fprintf(stderr,
      "[aurora-gxm] display_copy n=%llu source=%d,%d %dx%d total_us=%llu end_us=%llu prep_us=%llu draw_us=%llu\n",
      static_cast<unsigned long long>(count),
      source.x,source.y,source.width,source.height,
      static_cast<unsigned long long>(afterDraw-timingStart),
      static_cast<unsigned long long>(afterEnd-timingStart),
      static_cast<unsigned long long>(beforeDraw-afterEnd),
      static_cast<unsigned long long>(afterDraw-beforeDraw));
  }
  return ok;
}

void Renderer::shutdown() noexcept {
  auto& d = *impl_;
  if (d.inScene && d.context) { sceGxmEndScene(d.context, nullptr, nullptr); d.inScene = false; }
  if (d.context) sceGxmFinish(d.context);
  if (d.ownsGxm) sceGxmDisplayQueueFinish();
  // Stop scanout before releasing display backing storage.
  if (d.displayed) { sceDisplaySetFrameBuf(nullptr, SCE_DISPLAY_SETBUF_NEXTFRAME); sceDisplayWaitVblankStart(); }
  for (auto& entry : d.pipelines) d.destroy_pipeline(*entry.second);
  d.pipelines.clear(); d.stageCache.clear(); d.textures.clear(); d.textureCache.clear(); d.buffers.clear();
  if (d.patcher) { sceGxmShaderPatcherDestroy(d.patcher); d.patcher = nullptr; }
  if (d.ownsCompiler) { shark_clear_output(); shark_install_log_cb(nullptr); shark_end(); d.ownsCompiler = false; }
  if (d.target) { sceGxmDestroyRenderTarget(d.target); d.target = nullptr; }
  if (d.context) { sceGxmDestroyContext(d.context); d.context = nullptr; }
  for (auto& s : d.surfaces) { if (s.sync) sceGxmSyncObjectDestroy(s.sync); s.sync = nullptr; s.memory.reset(); }
  d.depth.reset(); d.patchBuffer.reset(); d.vertexUsse.reset(); d.fragmentUsse.reset();
  d.vdm.reset(); d.vertexRing.reset(); d.fragmentRing.reset(); d.fragmentUsseRing.reset();
  shutdown_cdram_pool();
  std::free(d.hostMemory); d.hostMemory = nullptr;
  if (d.ownsGxm) { sceGxmTerminate(); d.ownsGxm = false; }
  d.initialized = false; d.displayed = false; d.resourceBytes = 0;
  d.frameActive=false;d.depthValid=false;d.boundTarget=0;d.blitVertices=0;d.displaySource=0;d.copyVertices=0;
  d.pendingVertexDependency=nullptr;d.displayCopySourceValid=false;d.efbCopyGeometryValid=false;
  d.stageMemoryHits=0;d.stageCompiles=0;
  d.clearVertices = d.clearIndices = 0; d.clearPipeline = 0;
  d.front = 0; d.back = 1;
}
} // namespace aurora::vita::gxm
