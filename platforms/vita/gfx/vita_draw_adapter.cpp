#include "vita_draw_adapter.hpp"
#include "vita_cpu_workers.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace aurora::vita::gfx {
namespace {
bool push(std::vector<uint16_t>&o,uint32_t v) noexcept {if(v>std::numeric_limits<uint16_t>::max())return false;o.push_back(static_cast<uint16_t>(v));return true;}
bool tri(std::vector<uint16_t>&o,uint32_t a,uint32_t b,uint32_t c) noexcept{return push(o,a)&&push(o,b)&&push(o,c);}
bool build_indices(std::vector<uint16_t>&o,SourcePrimitive p,uint32_t n) noexcept {
  if(n>std::numeric_limits<uint16_t>::max())return false;
  switch(p){
  case SourcePrimitive::Triangles: if(n%3)return false; o.reserve(n);for(uint32_t i=0;i<n;i++)if(!push(o,i))return false;return true;
  case SourcePrimitive::Quads: if(n%4)return false;o.reserve((n/4)*6);for(uint32_t i=0;i<n;i+=4)if(!tri(o,i,i+1,i+2)||!tri(o,i+2,i+3,i))return false;return true;
  case SourcePrimitive::TriangleFan: if(n<3)return true;o.reserve((n-2)*3);for(uint32_t i=2;i<n;i++)if(!tri(o,0,i-1,i))return false;return true;
  case SourcePrimitive::TriangleStrip: if(n<3)return true;o.reserve((n-2)*3);for(uint32_t i=2;i<n;i++){if(i&1){if(!tri(o,i-1,i-2,i))return false;}else if(!tri(o,i-2,i-1,i))return false;}return true;
  case SourcePrimitive::Lines: case SourcePrimitive::LineStrip: case SourcePrimitive::Points:return false;
  }
  return false;
}

struct Clip {float x=0,y=0,z=0,w=1;};
Clip project(const std::array<float,16>&m,const CanonicalVertex&v) noexcept {
  const float x=v.position[0],y=v.position[1],z=v.position[2],w=v.position[3];
  return {m[0]*x+m[4]*y+m[8]*z+m[12]*w,
          m[1]*x+m[5]*y+m[9]*z+m[13]*w,
          m[2]*x+m[6]*y+m[10]*z+m[14]*w,
          m[3]*x+m[7]*y+m[11]*z+m[15]*w};
}
void set_clip(CanonicalVertex&v,const Clip&c) noexcept {v.position[0]=c.x;v.position[1]=c.y;v.position[2]=c.z;v.position[3]=c.w;}
void tex_offset(CanonicalVertex&v,uint8_t mask,float s,float t) noexcept {for(unsigned i=0;i<MaxTextures;i++)if(mask&(1u<<i)){v.texcoord[i][0]+=s;v.texcoord[i][1]+=t;}}

bool append_quad_indices(std::vector<uint16_t>&idx,uint32_t base) noexcept {
  // Matches Aurora's line/point fixed quad index order: 0,1,3 / 3,2,0.
  return tri(idx,base,base+1,base+3)&&tri(idx,base+3,base+2,base);
}

bool expand_points(const std::vector<CanonicalVertex>&src,std::vector<CanonicalVertex>&dst,std::vector<uint16_t>&idx,
                   const std::array<float,16>&projection,const PrimitiveExpansionState&e) noexcept {
  if(src.size()>std::numeric_limits<uint16_t>::max()/4u)return false;
  const float vw=std::max(e.viewportWidth,1.f),vh=std::max(e.viewportHeight,1.f);
  const float half=std::max(e.pointSizePixels,0.f)*0.5f;
  dst.reserve(src.size()*4);idx.reserve(src.size()*6);
  for(const auto&v:src){const Clip c=project(projection,v);const uint32_t base=static_cast<uint32_t>(dst.size());
    for(unsigned q=0;q<4;q++){CanonicalVertex o=v;const float xs=(q&1)?1.f:-1.f,ys=(q>=2)?1.f:-1.f;Clip p=c;p.x+=(xs*half*2.f/vw)*c.w;p.y+=(ys*half*2.f/vh)*c.w;set_clip(o,p);if(q&1)tex_offset(o,e.pointTexcoordMask,e.pointTexOffset,0.f);if(q>=2)tex_offset(o,e.pointTexcoordMask,0.f,e.pointTexOffset);dst.push_back(o);}if(!append_quad_indices(idx,base))return false;}
  return true;
}

bool append_line_segment(const CanonicalVertex&a,const CanonicalVertex&b,std::vector<CanonicalVertex>&dst,std::vector<uint16_t>&idx,
                         const std::array<float,16>&projection,const PrimitiveExpansionState&e) noexcept {
  const Clip ca=project(projection,a),cb=project(projection,b);if(std::abs(ca.w)<1e-10f||std::abs(cb.w)<1e-10f)return true;
  const float vw=std::max(e.viewportWidth,1.f),vh=std::max(e.viewportHeight,1.f);
  const float ax=ca.x/ca.w,ay=ca.y/ca.w,bx=cb.x/cb.w,by=cb.y/cb.w;
  const float dx=(bx-ax)*0.5f*vw,dy=(by-ay)*0.5f*vh;const float dl=std::sqrt(dx*dx+dy*dy);
  const float ux=dl>1e-10f?dx/dl:1.f,uy=dl>1e-10f?dy/dl:0.f;const float px=-uy,py=ux;const float half=std::max(e.lineWidthPixels,0.f)*0.5f;
  const float ox=px*half*2.f/vw,oy=py*half*2.f/vh;const uint32_t base=static_cast<uint32_t>(dst.size());
  for(unsigned q=0;q<4;q++){const bool useB=q>=2,positive=q&1;CanonicalVertex o=useB?b:a;const Clip c=useB?cb:ca;const float sign=positive?1.f:-1.f;Clip p=c;p.x+=ox*sign*c.w;p.y+=oy*sign*c.w;set_clip(o,p);if(positive)tex_offset(o,e.lineTexcoordMask,0.f,e.lineTexOffset);dst.push_back(o);}return append_quad_indices(idx,base);
}

bool expand_lines(const std::vector<CanonicalVertex>&src,SourcePrimitive source,std::vector<CanonicalVertex>&dst,std::vector<uint16_t>&idx,
                  const std::array<float,16>&projection,const PrimitiveExpansionState&e) noexcept {
  const size_t segments=source==SourcePrimitive::Lines?src.size()/2:(src.size()>1?src.size()-1:0);if(segments>std::numeric_limits<uint16_t>::max()/4u)return false;
  dst.reserve(segments*4);idx.reserve(segments*6);
  if(source==SourcePrimitive::Lines){if(src.size()%2)return false;for(size_t i=0;i<src.size();i+=2)if(!append_line_segment(src[i],src[i+1],dst,idx,projection,e))return false;}
  else for(size_t i=0;i+1<src.size();i++)if(!append_line_segment(src[i],src[i+1],dst,idx,projection,e))return false;
  return true;
}

bool same_sampler(const SamplerDesc&a,const SamplerDesc&b) noexcept {
  return a.wrapS==b.wrapS&&a.wrapT==b.wrapT&&a.minFilter==b.minFilter&&a.magFilter==b.magFilter&&
         a.lodBias==b.lodBias;
}
bool same_texture(const TextureBinding&a,const TextureBinding&b) noexcept {
  return a.texture==b.texture&&a.source==b.source&&same_sampler(a.sampler,b.sampler);
}
bool same_viewport(const Viewport&a,const Viewport&b) noexcept {
  return a.x==b.x&&a.y==b.y&&a.width==b.width&&a.height==b.height&&a.znear==b.znear&&a.zfar==b.zfar;
}
bool same_scissor(const Scissor&a,const Scissor&b) noexcept {
  return a.x==b.x&&a.y==b.y&&a.width==b.width&&a.height==b.height;
}
bool batch_compatible(const DrawPacket&a,const DrawPacket&b) noexcept {
  if(!a.absoluteVertexIndices||!b.absoluteVertexIndices||a.pipelineKey!=b.pipelineKey||a.instanceCount!=1||b.instanceCount!=1)return false;
  if(a.vertices.buffer!=b.vertices.buffer||a.indices.buffer!=b.indices.buffer)return false;
  if(a.indices.offset+a.indices.size!=b.indices.offset)return false;
  if(!same_viewport(a.viewport,b.viewport)||!same_scissor(a.scissor,b.scissor))return false;
  // channel/light state is consumed by the CPU vertex pipeline before enqueue and
  // never reaches the Vita shader. Comparing it here both burns memory bandwidth
  // and prevents otherwise identical GPU draws from coalescing.
  if(std::memcmp(&a.uniforms,&b.uniforms,sizeof(GpuDrawUniforms))!=0)return false;
  for(unsigned i=0;i<MaxTextures;i++)if(!same_texture(a.textures[i],b.textures[i]))return false;
  return true;
}

struct FusedVertexContext {
  const uint8_t* raw=nullptr;
  size_t rawBytes=0;
  const VertexDecodeLayout* layout=nullptr;
  const PipelineDesc* pipeline=nullptr;
  const VertexTransformState* state=nullptr;
  VertexPipelineRequirements requirements{};
  CanonicalVertex* vertices=nullptr;
  std::array<uint8_t,3> error{{0,0,0}}; // 1=decode, 2=transform
};

bool decode_transform_range(void* opaque,size_t begin,size_t end,uint32_t lane) noexcept {
  auto&ctx=*static_cast<FusedVertexContext*>(opaque);
  if(lane>=ctx.error.size())return false;
  for(size_t i=begin;i<end;++i){
    auto&v=ctx.vertices[i];
    if(!decode_vertex_into(ctx.raw,ctx.rawBytes,static_cast<uint32_t>(i),*ctx.layout,v)){
      ctx.error[lane]=1;return false;
    }
    if(!transform_vertex_for_pipeline(v,*ctx.pipeline,*ctx.state,ctx.requirements)){
      ctx.error[lane]=2;return false;
    }
  }
  return true;
}
}
DrawFootprint estimate_draw_footprint(SourcePrimitive source,uint32_t count,uint32_t explicitIndexCount,size_t vertexStride) noexcept {
  DrawFootprint f{};
  if(!count)return f;
  uint64_t vertices=count,indices=0;
  switch(source){
  case SourcePrimitive::Triangles: if(count%3)return f;indices=count;break;
  case SourcePrimitive::Quads: if(count%4)return f;indices=(uint64_t(count)/4u)*6u;break;
  case SourcePrimitive::TriangleFan: case SourcePrimitive::TriangleStrip: indices=count<3?0:(uint64_t(count)-2u)*3u;break;
  case SourcePrimitive::Lines: if(count%2)return f;vertices=(uint64_t(count)/2u)*4u;indices=(uint64_t(count)/2u)*6u;break;
  case SourcePrimitive::LineStrip: {const uint64_t seg=count>1?uint64_t(count)-1u:0u;vertices=seg*4u;indices=seg*6u;break;}
  case SourcePrimitive::Points: vertices=uint64_t(count)*4u;indices=uint64_t(count)*6u;break;
  }
  if(explicitIndexCount)indices=explicitIndexCount;
  if(vertices>std::numeric_limits<uint16_t>::max()||indices>std::numeric_limits<uint32_t>::max())return f;
  f.vertexCount=static_cast<uint32_t>(vertices);f.indexCount=static_cast<uint32_t>(indices);
  if(vertexStride==0)return f;
  f.vertexBytes=static_cast<size_t>(vertices)*vertexStride;
  f.indexBytes=static_cast<size_t>(indices)*sizeof(uint16_t);
  f.valid=true;return f;
}
bool prepare_draw_into(PreparedDraw&out,const uint8_t*raw,size_t bytes,uint32_t count,SourcePrimitive source,const VertexDecodeLayout&layout,const PipelineDesc&pipeline,const VertexTransformState&state,DrawUniforms*uniforms,const PrimitiveExpansionState&expansion,Telemetry*telemetry) noexcept {
  out.error=PrepareDrawError::None;out.vertices.clear();out.indices.clear();out.scratch.clear();out.primitive=Primitive::Triangles;out.positionIsClipSpace=false;
  if(!raw||!count){out.error=PrepareDrawError::InvalidInput;return false;}
  if(size_t(count)*layout.streamStride>bytes){out.error=PrepareDrawError::VertexDecodeFailed;return false;}
  if(uniforms)uniforms->mvp=state.projection;
  const bool expanded=source==SourcePrimitive::Points||source==SourcePrimitive::Lines||source==SourcePrimitive::LineStrip;
  auto&transformed=expanded?out.scratch:out.vertices;
  transformed.resize(count);
  FusedVertexContext fused{raw,bytes,&layout,&pipeline,&state,vertex_pipeline_requirements(pipeline),transformed.data()};
  // Decode and GX vertex processing used to dispatch the worker pool twice and
  // walk the 168-byte canonical array twice. Fusing them keeps each vertex hot
  // in cache and makes medium-sized Strikers draws worth parallelizing.
  { ScopedTelemetryPhase phase(telemetry,TelemetryPhase::VertexDecode);
    if(!cpu_parallel_for(count,decode_transform_range,&fused)){
      bool transformFailed=false;for(const auto e:fused.error)transformFailed=transformFailed||e==2;
      out.error=transformFailed?PrepareDrawError::VertexTransformFailed:PrepareDrawError::VertexDecodeFailed;
      return false;
    }
  }
  { ScopedTelemetryPhase phase(telemetry,TelemetryPhase::CommandBuild);
    if(source==SourcePrimitive::Points){if(!expand_points(transformed,out.vertices,out.indices,state.projection,expansion)){out.error=PrepareDrawError::TooManyVertices;return false;}out.positionIsClipSpace=true;out.primitive=Primitive::Triangles;return true;}
    if(source==SourcePrimitive::Lines||source==SourcePrimitive::LineStrip){if(!expand_lines(transformed,source,out.vertices,out.indices,state.projection,expansion)){out.error=PrepareDrawError::TooManyVertices;return false;}out.positionIsClipSpace=true;out.primitive=Primitive::Triangles;return true;}
    if(!build_indices(out.indices,source,count)){out.error=PrepareDrawError::TooManyVertices;return false;}out.primitive=Primitive::Triangles;
  }
  return true;
}
PreparedDraw prepare_draw(const uint8_t*raw,size_t bytes,uint32_t count,SourcePrimitive source,const VertexDecodeLayout&layout,const PipelineDesc&pipeline,const VertexTransformState&state,DrawUniforms*uniforms,const PrimitiveExpansionState&expansion,Telemetry*telemetry) noexcept {
  PreparedDraw out{};
  (void)prepare_draw_into(out,raw,bytes,count,source,layout,pipeline,state,uniforms,expansion,telemetry);
  return out;
}
uint64_t resolve_draw_pipeline(Renderer&renderer,const PreparedDraw&prepared,const PipelineDesc&pipeline,Telemetry*telemetry) noexcept {
  auto desc=pipeline;
  desc.primitive=prepared.primitive;
  desc.positionIsClipSpace=prepared.positionIsClipSpace;
  desc.layout=gpu_vertex_layout(pipeline_texcoord_mask(desc),pipeline_raster_color_mask(desc));
  ScopedTelemetryPhase phase(telemetry,TelemetryPhase::PipelineResolve);
  return renderer.create_pipeline(desc);
}
bool enqueue_draw(Renderer&renderer,StreamingArena&arena,CommandStream&stream,const PreparedDraw&prepared,const PipelineDesc&pipeline,const DrawUniforms&uniforms,const Viewport&viewport,const Scissor&scissor,const std::array<TextureBinding,MaxTextures>&textures,PrepareDrawError*err,Telemetry*telemetry,uint64_t resolvedPipelineKey) noexcept {
  auto fail=[&](PrepareDrawError e){if(err)*err=e;return false;};
  if(!prepared.ok()||prepared.vertices.empty())return fail(prepared.error==PrepareDrawError::None?PrepareDrawError::InvalidInput:prepared.error);
  BufferSlice vb{},ib{};
  { ScopedTelemetryPhase phase(telemetry,TelemetryPhase::CommandBuild);
    // Only stream attributes consumed by the generated Vita shader. The CPU-only
    // normal/tangent/matrix fields in CanonicalVertex have already done their job.
    const VertexLayout gpuLayout=gpu_vertex_layout(pipeline_texcoord_mask(pipeline),pipeline_raster_color_mask(pipeline));
    if(gpuLayout.count<1||gpuLayout.attributes[0].stride==0)return fail(PrepareDrawError::InvalidInput);
    const size_t gpuStride=gpuLayout.attributes[0].stride;
    void* vertexDst=nullptr;
    vb=arena.reserve_vertices(prepared.vertices.size()*gpuStride,gpuStride,&vertexDst);if(!vb.buffer||!vertexDst)return fail(PrepareDrawError::StreamingOverflow);
    auto* gpu=static_cast<uint8_t*>(vertexDst);
    for(size_t i=0;i<prepared.vertices.size();++i){
      const auto&src=prepared.vertices[i];uint8_t*dst=gpu+i*gpuStride;
      for(unsigned ai=0;ai<gpuLayout.count;ai++){
        const auto&a=gpuLayout.attributes[ai];
        if(a.location==0)std::memcpy(dst+a.offset,src.position,sizeof(src.position));
        else if(a.location==1)std::memcpy(dst+a.offset,src.color0,sizeof(src.color0));
        else if(a.location==2)std::memcpy(dst+a.offset,src.color1,sizeof(src.color1));
        else if(a.location>=3&&a.location<3+MaxTextures)std::memcpy(dst+a.offset,src.texcoord[a.location-3],sizeof(src.texcoord[0]));
      }
    }
    if(!prepared.indices.empty()){
      if(vb.offset%gpuStride!=0)return fail(PrepareDrawError::StreamingOverflow);
      const uint32_t base=vb.offset/static_cast<uint32_t>(gpuStride);
      if(base>std::numeric_limits<uint16_t>::max())return fail(PrepareDrawError::TooManyVertices);
      for(const uint16_t idx:prepared.indices)if(base+idx>std::numeric_limits<uint16_t>::max())return fail(PrepareDrawError::TooManyVertices);
      ib=arena.upload_rebased_indices(prepared.indices.data(),prepared.indices.size(),base);if(!ib.buffer)return fail(PrepareDrawError::StreamingOverflow);
    }
  }
  uint64_t key=resolvedPipelineKey?resolvedPipelineKey:resolve_draw_pipeline(renderer,prepared,pipeline,telemetry);
  if(!key)return fail(PrepareDrawError::PipelineFailed);
  { ScopedTelemetryPhase phase(telemetry,TelemetryPhase::CommandBuild); DrawPacket d{};d.pipelineKey=key;d.vertices=vb;d.indices=ib;d.vertexCount=static_cast<uint32_t>(prepared.vertices.size());d.indexCount=static_cast<uint32_t>(prepared.indices.size());d.absoluteVertexIndices=!prepared.indices.empty();d.textures=textures;d.uniforms=uniforms;d.viewport=viewport;d.scissor=scissor;
    if(auto*tail=stream.tail_draw();tail&&batch_compatible(*tail,d)){
      tail->vertices.size=(d.vertices.offset+d.vertices.size)-tail->vertices.offset;
      tail->indices.size+=d.indices.size;
      tail->vertexCount+=d.vertexCount;
      tail->indexCount+=d.indexCount;
    }else stream.draw(d);
  }
  if(err) *err=PrepareDrawError::None;
  return true;
}
} // namespace aurora::vita::gfx
