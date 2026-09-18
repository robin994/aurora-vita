#pragma once
#include "vita_vertex_pipeline.hpp"
#include <algorithm>

namespace aurora::vita::gfx {

inline bool vertex_layout_has_semantic(const VertexDecodeLayout& layout,
                                       VertexSemantic semantic) noexcept {
  for(unsigned i=0;i<layout.count&&i<layout.attributes.size();++i)
    if(layout.attributes[i].source!=VertexSource::None&&layout.attributes[i].semantic==semantic)return true;
  return false;
}

// Eligibility stays conservative around the remaining unsupported GX features.
// Position palettes and channel lighting are exact; emboss and per-vertex texture
// matrix selectors still fall back to the CPU vertex pipeline.
inline bool supports_fixed_vertex_gpu(const PipelineDesc& pipeline,
                                      const VertexDecodeLayout& layout,
                                      const VertexTransformState& state) noexcept {
  if (pipeline.positionIsClipSpace || state.currentPnMatrix >= state.postexMatrices.size()) return false;
  const auto requirements = vertex_pipeline_requirements(pipeline);
  if (requirements.needBumpBasis) return false;
  if (layout.count > layout.attributes.size() || !layout.streamStride) return false;
#if !defined(AURORA_VITA_RENDERER_GXM)
  // The expanded palette/lighting shader is native-GXM only for now. Keep the
  // vitaGL backend on its proven fixed-current-PN, unlit fast path.
  if(requirements.needNormal||vertex_layout_has_semantic(layout,VertexSemantic::PnMatrixIndex))return false;
#endif
  for (unsigned i=0; i<layout.count; ++i) {
    const auto& a=layout.attributes[i];
    if (a.source==VertexSource::None) continue;
    if (a.semantic>=VertexSemantic::TexMatrixIndex0 && a.semantic<=VertexSemantic::TexMatrixIndex7) return false;
  }
  const uint8_t generated=pipeline_texgen_compute_mask(pipeline);
  for (unsigned i=0; i<pipeline.texgenCount && i<MaxTextures; ++i) if (generated&(1u<<i)) {
    const auto& t=pipeline.texgens[i];
    if (texgen_type_is_bump(t.type) || t.matrixFromVertex) return false;
    if (t.type==TexGenType::SRTG && t.source!=TexGenSource::Color0 && t.source!=TexGenSource::Color1) return false;
    if (t.matrix>=0 && 10u+static_cast<unsigned>(t.matrix)>=state.postexMatrices.size()) return false;
    if (t.postMatrix>=static_cast<int>(state.postMatrices.size())) return false;
  }
  return true;
}

inline VertexSemanticMask fixed_vertex_gpu_inputs(const PipelineDesc& pipeline) noexcept {
  VertexSemanticMask mask=vertex_semantic_bit(VertexSemantic::Position);
  const auto requirements=vertex_pipeline_requirements(pipeline);
  if(requirements.needNormal)mask|=vertex_semantic_bit(VertexSemantic::Normal);
  if(pipeline.fixedVertexIndexedPn)mask|=vertex_semantic_bit(VertexSemantic::PnMatrixIndex);
  const uint8_t colors=pipeline_raster_color_mask(pipeline);
  for (unsigned i=0;i<2;++i) if (colors&(1u<<i)) {
    if (pipeline.colorChannels[i].materialSource==ColorSource::Vertex ||
        pipeline.colorChannels[i+2].materialSource==ColorSource::Vertex ||
        (pipeline.colorChannels[i].lightingEnabled&&pipeline.colorChannels[i].ambientSource==ColorSource::Vertex) ||
        (pipeline.colorChannels[i+2].lightingEnabled&&pipeline.colorChannels[i+2].ambientSource==ColorSource::Vertex))
      mask|=vertex_semantic_bit(i?VertexSemantic::Color1:VertexSemantic::Color0);
  }
  const uint8_t outputs=pipeline_texcoord_mask(pipeline);
  for (unsigned i=0;i<MaxTextures;++i) if(outputs&(1u<<i)) {
    if(i>=pipeline.texgenCount) {
      mask|=vertex_semantic_bit(static_cast<VertexSemantic>(static_cast<unsigned>(VertexSemantic::Tex0)+i));
      continue;
    }
    const auto source=pipeline.texgens[i].source;
    VertexSemantic semantic=VertexSemantic::Position;
    switch(source) {
      case TexGenSource::Position: semantic=VertexSemantic::Position; break;
      case TexGenSource::Normal: semantic=VertexSemantic::Normal; break;
      case TexGenSource::Binormal: semantic=VertexSemantic::Binormal; break;
      case TexGenSource::Tangent: semantic=VertexSemantic::Tangent; break;
      case TexGenSource::Color0: semantic=VertexSemantic::Color0; break;
      case TexGenSource::Color1: semantic=VertexSemantic::Color1; break;
      default: {
        const unsigned n=static_cast<unsigned>(source)-static_cast<unsigned>(TexGenSource::Tex0);
        if(n<MaxTextures)semantic=static_cast<VertexSemantic>(static_cast<unsigned>(VertexSemantic::Tex0)+n);
        break;
      }
    }
    mask|=vertex_semantic_bit(semantic);
  }
  return mask;
}

inline VertexLayout fixed_vertex_gpu_layout(const PipelineDesc& pipeline) noexcept {
  const auto mask=fixed_vertex_gpu_inputs(pipeline);
  VertexLayout result{};
  uint16_t stride=0;
  const auto add=[&](uint8_t location,uint8_t components,VertexScalar scalar,bool normalized) {
    auto& a=result.attributes[result.count++];
    a={location,components,scalar,normalized,0,stride};
    stride+=static_cast<uint16_t>(components*(scalar==VertexScalar::F32?4u:1u));
  };
  add(0,4,VertexScalar::F32,false);
  if(mask&vertex_semantic_bit(VertexSemantic::Color0))add(1,4,VertexScalar::U8,true);
  if(mask&vertex_semantic_bit(VertexSemantic::Color1))add(2,4,VertexScalar::U8,true);
  for(unsigned i=0;i<MaxTextures;++i)
    if(mask&vertex_semantic_bit(static_cast<VertexSemantic>(static_cast<unsigned>(VertexSemantic::Tex0)+i)))
      add(static_cast<uint8_t>(3+i),3,VertexScalar::F32,false);
  if(mask&vertex_semantic_bit(VertexSemantic::Normal))add(11,3,VertexScalar::F32,false);
  if(mask&vertex_semantic_bit(VertexSemantic::Binormal))add(12,3,VertexScalar::F32,false);
  if(mask&vertex_semantic_bit(VertexSemantic::Tangent))add(13,3,VertexScalar::F32,false);
  if(mask&vertex_semantic_bit(VertexSemantic::PnMatrixIndex))add(14,1,VertexScalar::U8,false);
  stride=static_cast<uint16_t>((stride+3u)&~3u);
  for(unsigned i=0;i<result.count;++i)result.attributes[i].stride=stride;
  return result;
}

inline VertexLayout effective_gpu_vertex_layout(const PipelineDesc& pipeline) noexcept {
  return pipeline.fixedVertexOnGpu?fixed_vertex_gpu_layout(pipeline):
      gpu_vertex_layout(pipeline_texcoord_mask(pipeline),pipeline_raster_color_mask(pipeline));
}

inline FixedVertexUniforms fixed_vertex_uniforms(const PipelineDesc& pipeline,
                                                const VertexTransformState& state) noexcept {
  FixedVertexUniforms result{};
  if(state.currentPnMatrix<state.postexMatrices.size())result.position=state.postexMatrices[state.currentPnMatrix].v;
  if(!state.normalMatrices.empty())result.normal=state.normalMatrices[std::min<unsigned>(state.currentPnMatrix,state.normalMatrices.size()-1)].v;
  if(pipeline.fixedVertexIndexedPn)for(unsigned i=0;i<10;++i){
    result.positionPalette[i]=state.postexMatrices[i].v;
    result.normalPalette[i]=state.normalMatrices[i].v;
  }
  result.material=state.channelMaterial;
  result.ambient=state.channelAmbient;
  uint8_t lightMask=0;
  for(unsigned ch=0;ch<4;++ch)if(pipeline.colorChannels[ch].lightingEnabled)
    lightMask|=pipeline.colorChannels[ch].lightMask;
  for(unsigned i=0;i<MaxLights;++i)if(lightMask&(1u<<i)){
    const auto& src=state.lights[i];
    result.light[i*5+0]=src.position;
    result.light[i*5+1]=src.direction;
    result.light[i*5+2]=src.color;
    result.light[i*5+3]=src.cosAtt;
    result.light[i*5+4]=src.distAtt;
  }
  const uint8_t outputs=pipeline_texgen_compute_mask(pipeline);
  for(unsigned i=0;i<pipeline.texgenCount&&i<MaxTextures;++i)if(outputs&(1u<<i)) {
    const auto& t=pipeline.texgens[i];
    if(t.matrix>=0&&10u+static_cast<unsigned>(t.matrix)<state.postexMatrices.size())
      result.texture[i]=state.postexMatrices[10u+static_cast<unsigned>(t.matrix)].v;
    if(t.postMatrix>=0&&static_cast<unsigned>(t.postMatrix)<state.postMatrices.size())
      result.post[i]=state.postMatrices[static_cast<unsigned>(t.postMatrix)].v;
  }
  return result;
}

} // namespace aurora::vita::gfx
