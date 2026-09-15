#pragma once
#include "vita_command_stream.hpp"
#include "vita_renderer.hpp"
#include "vita_streaming_arena.hpp"
#include "vita_telemetry.hpp"
#include "vita_vertex_decode.hpp"
#include "vita_vertex_pipeline.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aurora::vita::gfx {

enum class SourcePrimitive : uint8_t { Quads, Triangles, TriangleStrip, TriangleFan, Lines, LineStrip, Points };

enum class PrepareDrawError : uint8_t { None, InvalidInput, VertexDecodeFailed, VertexTransformFailed, TooManyVertices, UnsupportedLineExpansion, StreamingOverflow, PipelineFailed };

// Runtime state used by Aurora's WGSL line/point expansion, expressed in render-target pixels.
struct PrimitiveExpansionState {
  float viewportWidth = 960.f;
  float viewportHeight = 544.f;
  float lineWidthPixels = 1.f;
  float pointSizePixels = 1.f;
  float lineTexOffset = 0.f;
  float pointTexOffset = 0.f;
  uint8_t lineTexcoordMask = 0;
  uint8_t pointTexcoordMask = 0;
};

struct PreparedDraw {
  PrepareDrawError error = PrepareDrawError::None;
  std::vector<CanonicalVertex> vertices{};
  std::vector<uint16_t> indices{};
  // Reused decode storage for line/point expansion. Keeping it with the prepared
  // draw lets the Vita submit path retain allocations across thousands of draws.
  std::vector<CanonicalVertex> scratch{};
  Primitive primitive = Primitive::Triangles;
  bool positionIsClipSpace = false;
  bool ok() const noexcept { return error == PrepareDrawError::None; }
};

struct DrawFootprint {
  size_t vertexBytes=0;
  size_t indexBytes=0;
  uint32_t vertexCount=0;
  uint32_t indexCount=0;
  bool valid=false;
};

// Fast-path result for ordinary GX primitives. Vertices are decoded,
// transformed and packed directly into the streaming arena instead of first
// materializing a 168-byte CanonicalVertex array for the whole draw.
struct StreamedDraw {
  PrepareDrawError error = PrepareDrawError::None;
  BufferSlice vertices{};
  BufferSlice indices{};
  uint32_t vertexCount = 0;
  uint32_t indexCount = 0;
  Primitive primitive = Primitive::Triangles;
  bool positionIsClipSpace = false;
  bool ok() const noexcept { return error == PrepareDrawError::None; }
};

DrawFootprint estimate_draw_footprint(SourcePrimitive source,uint32_t vertexCount,uint32_t explicitIndexCount=0,
                                      size_t vertexStride=sizeof(GpuVertex)) noexcept;

PreparedDraw prepare_draw(const uint8_t* rawVertices,size_t rawBytes,uint32_t vertexCount,SourcePrimitive source,
                          const VertexDecodeLayout& layout,const PipelineDesc& pipeline,
                          const VertexTransformState& state,DrawUniforms* uniforms=nullptr,
                          const PrimitiveExpansionState& expansion={},Telemetry* telemetry=nullptr) noexcept;
bool prepare_draw_into(PreparedDraw& out,const uint8_t* rawVertices,size_t rawBytes,uint32_t vertexCount,SourcePrimitive source,
                       const VertexDecodeLayout& layout,const PipelineDesc& pipeline,
                       const VertexTransformState& state,DrawUniforms* uniforms=nullptr,
                       const PrimitiveExpansionState& expansion={},Telemetry* telemetry=nullptr) noexcept;

// Direct streaming path for triangles/quads/fans/strips. Lines and points still
// use PreparedDraw because their Vita representation expands the vertex count.
bool prepare_streamed_draw_into(StreamedDraw& out,StreamingArena& arena,
                                const uint8_t* rawVertices,size_t rawBytes,uint32_t vertexCount,
                                SourcePrimitive source,const uint16_t* rawIndices,uint32_t rawIndexCount,
                                const VertexDecodeLayout& layout,const PipelineDesc& pipeline,
                                const VertexTransformState& state,DrawUniforms* uniforms=nullptr,
                                Telemetry* telemetry=nullptr) noexcept;

// Resolves the effective post-conversion pipeline once. Callers submitting a
// consecutive run with unchanged GX state may reuse the returned key.
uint64_t resolve_draw_pipeline(Renderer& renderer,const PreparedDraw& prepared,
                               const PipelineDesc& pipeline,Telemetry* telemetry=nullptr) noexcept;
uint64_t resolve_draw_pipeline(Renderer& renderer,Primitive primitive,bool positionIsClipSpace,
                               const PipelineDesc& pipeline,Telemetry* telemetry=nullptr) noexcept;

// Uploads a prepared draw into the multi-buffered stream arena and appends one DrawPacket.
bool enqueue_draw(Renderer& renderer,StreamingArena& arena,CommandStream& stream,const PreparedDraw& prepared,
                  const PipelineDesc& pipeline,const DrawUniforms& uniforms,const Viewport& viewport,
                  const Scissor& scissor,const std::array<TextureBinding,MaxTextures>& textures={},
                  PrepareDrawError* error=nullptr,Telemetry* telemetry=nullptr,
                  uint64_t resolvedPipelineKey=0) noexcept;

bool enqueue_streamed_draw(CommandStream& stream,const StreamedDraw& prepared,uint64_t resolvedPipelineKey,
                           const DrawUniforms& uniforms,const Viewport& viewport,const Scissor& scissor,
                           const std::array<TextureBinding,MaxTextures>& textures={},
                           PrepareDrawError* error=nullptr) noexcept;

} // namespace aurora::vita::gfx
