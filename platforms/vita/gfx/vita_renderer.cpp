#include "vita_renderer.hpp"
#include "vita_pipeline_key.hpp"
#include <algorithm>
#if defined(__vita__)
#include <vitaGL.h>
#include <psp2/display.h>
#endif
namespace aurora::vita::gfx {
namespace {
#if defined(__vita__)
GLenum prim(Primitive p){switch(p){case Primitive::Triangles:return GL_TRIANGLES;case Primitive::TriangleStrip:return GL_TRIANGLE_STRIP;case Primitive::TriangleFan:return GL_TRIANGLE_FAN;case Primitive::Lines:return GL_LINES;case Primitive::LineStrip:return GL_LINE_STRIP;case Primitive::Points:return GL_POINTS;}return GL_TRIANGLES;}
GLenum scalar(VertexScalar s){switch(s){case VertexScalar::F32:return GL_FLOAT;case VertexScalar::S8:return GL_BYTE;case VertexScalar::U8:return GL_UNSIGNED_BYTE;case VertexScalar::S16:return GL_SHORT;case VertexScalar::U16:return GL_UNSIGNED_SHORT;}return GL_FLOAT;}
bool same_viewport(const Viewport&a,const Viewport&b) noexcept{return a.x==b.x&&a.y==b.y&&a.width==b.width&&a.height==b.height&&a.znear==b.znear&&a.zfar==b.zfar;}
bool same_scissor(const Scissor&a,const Scissor&b) noexcept{return a.x==b.x&&a.y==b.y&&a.width==b.width&&a.height==b.height;}
bool same_sampler(const SamplerDesc&a,const SamplerDesc&b) noexcept{return a.wrapS==b.wrapS&&a.wrapT==b.wrapT&&a.minFilter==b.minFilter&&a.magFilter==b.magFilter&&a.lodBias==b.lodBias;}
bool same_texture_binding(const TextureBinding&a,const TextureBinding&b) noexcept{return a.texture==b.texture&&a.source==b.source&&a.flipX==b.flipX&&a.flipY==b.flipY&&a.forceOpaque==b.forceOpaque&&same_sampler(a.sampler,b.sampler);}
#endif
}
Renderer::Renderer(const RendererConfig&cfg):cfg_(cfg),pipelines_(cfg.pipelineBudget),textures_(cfg.textureBudget){}Renderer::~Renderer(){shutdown();}
bool Renderer::initialize() noexcept {targetWidth_=cfg_.width;targetHeight_=cfg_.height;initialized_=true;return true;}
void Renderer::shutdown() noexcept {if(!initialized_)return;pipelines_.clear();textures_.clear();buffers_.clear();efb_.clear();maskedClearVertices_=maskedClearIndices_=InvalidHandle;invalidate_draw_state();initialized_=false;}
void Renderer::begin_frame() noexcept {pipelines_.clear_pins();pipelines_.trim_to_budget();stats_={};invalidate_draw_state();}
void Renderer::end_frame() noexcept {textures_.trim(frame_);frame_++;}
const char* Renderer::last_error() const noexcept { return ""; }
bool Renderer::present(bool display) noexcept {
#if defined(__vita__)
  if(display) vglSwapBuffers(GL_FALSE);
#else
  (void)display;
#endif
  return true;
}
bool Renderer::readback_rgba8(std::vector<uint8_t>& pixels) noexcept {
#if defined(__vita__)
  glFinish();
  sceGxmDisplayQueueFinish();
  SceDisplayFrameBuf fb{}; fb.size=sizeof(fb);
  if(sceDisplayGetFrameBuf(&fb,SCE_DISPLAY_SETBUF_IMMEDIATE)<0 || !fb.base ||
     fb.width!=cfg_.width || fb.height!=cfg_.height || fb.pitch<fb.width || fb.pixelformat!=0) return false;
  pixels.resize(size_t(fb.width)*fb.height*4);
  for(uint32_t y=0;y<fb.height;++y)
    std::memcpy(pixels.data()+size_t(y)*fb.width*4,
                static_cast<const uint8_t*>(fb.base)+size_t(y)*fb.pitch*4,size_t(fb.width)*4);
  return true;
#else
  (void)pixels; return false;
#endif
}
uint64_t Renderer::create_pipeline(const PipelineDesc&d) noexcept {auto*p=pipelines_.get_or_create(d,&stats_);if(!p)return 0;pipelines_.pin(p->key);return p->key;}
Handle Renderer::create_texture(const TextureDesc&d) noexcept {
  const uint32_t missesBefore=stats_.textureMisses;
  const Handle h=textures_.get_or_upload(d,frame_,&stats_);
  // A cache hit is a pure lookup and does not touch vitaGL. Miss paths may bind
  // upload scratch even when allocation/decode eventually fails, so invalidate
  // conservatively for every miss but keep bindings hot for the common hit case.
  if(stats_.textureMisses!=missesBefore)invalidate_texture_bindings();
  return h;
}
size_t Renderer::invalidate_texture_source_range(uint64_t start,size_t bytes) noexcept {const size_t n=textures_.invalidate_source_range(start,bytes);if(n)invalidate_texture_bindings();return n;}
Handle Renderer::create_vertex_buffer(const void*d,size_t n,bool dynamic) noexcept {const Handle h=buffers_.create_vertex(d,n,dynamic);invalidate_buffer_bindings();return h;}
Handle Renderer::create_index_buffer(const void*d,size_t n,bool dynamic) noexcept {const Handle h=buffers_.create_index(d,n,dynamic);invalidate_buffer_bindings();return h;}
bool Renderer::update_buffer(Handle h,const void*d,size_t n,size_t off) noexcept {const bool ok=buffers_.update(h,d,n,off);if(ok)invalidate_buffer_bindings();return ok;}
Handle Renderer::create_efb(uint32_t w,uint32_t h,bool depth) noexcept {const Handle out=efb_.create(w,h,depth);invalidate_texture_bindings();return out;}
void Renderer::invalidate_texture_bindings() noexcept {
#if defined(__vita__)
  textureStateValid_.fill(false);
#endif
}
void Renderer::invalidate_buffer_bindings() noexcept {
#if defined(__vita__)
  vertexStateValid_=false;indexStateValid_=false;
#endif
}
void Renderer::invalidate_resource_bindings() noexcept {invalidate_texture_bindings();invalidate_buffer_bindings();}
void Renderer::invalidate_draw_state() noexcept {
#if defined(__vita__)
  viewportValid_=false;scissorValid_=false;vertexStateValid_=false;indexStateValid_=false;textureStateValid_.fill(false);
  vertexAttribMaskValid_=false;vertexAttribDefaultsValid_=false;
#endif
  pipelines_.invalidate_bound();
}
bool Renderer::bind_efb(Handle h) noexcept {uint32_t w=0,hgt=0;if(!efb_.dimensions(h,w,hgt)||!efb_.bind(h))return false;boundEfb_=h;targetWidth_=w;targetHeight_=hgt;invalidate_draw_state();return true;}
void Renderer::bind_default() noexcept {efb_.bind_default(cfg_.width,cfg_.height);boundEfb_=InvalidHandle;targetWidth_=cfg_.width;targetHeight_=cfg_.height;invalidate_draw_state();}
bool Renderer::blit_efb(Handle h) noexcept {
  const bool ok=efb_.blit_to_default(h,cfg_.width,cfg_.height);
  if(ok){boundEfb_=InvalidHandle;targetWidth_=cfg_.width;targetHeight_=cfg_.height;
#if defined(__vita__)
    scissorEnabled_=false;
#endif
    invalidate_draw_state();}
  return ok;
}
bool Renderer::display_copy(const Scissor& src) noexcept {
  displayCopy_=capture_current(displayCopy_,src,cfg_.width,cfg_.height,EfbCopyFormat::Passthrough);
  return displayCopy_!=InvalidHandle && blit_efb(displayCopy_);
}
Handle Renderer::capture_current(Handle existing,const Scissor& src,uint32_t dstWidth,uint32_t dstHeight,EfbCopyFormat format,bool flipX,bool flipY) noexcept {if(src.width<=0||src.height<=0||!dstWidth||!dstHeight)return InvalidHandle;const int32_t y=static_cast<int32_t>(targetHeight_)-(src.y+src.height);const int32_t x=std::max<int32_t>(0,src.x);const int32_t sy=std::max<int32_t>(0,y);const uint32_t sw=std::min<uint32_t>(static_cast<uint32_t>(src.width),targetWidth_-std::min<uint32_t>(static_cast<uint32_t>(x),targetWidth_));const uint32_t sh=std::min<uint32_t>(static_cast<uint32_t>(src.height),targetHeight_-std::min<uint32_t>(static_cast<uint32_t>(sy),targetHeight_));if(!sw||!sh)return InvalidHandle;
#if defined(__vita__)
  const bool scissorWasEnabled=scissorEnabled_;
#else
  const bool scissorWasEnabled=false;
#endif
  Handle out=efb_.capture_from_bound(existing,x,sy,sw,sh,dstWidth,dstHeight,format,scissorWasEnabled,flipX,flipY);/* EFB allocation/capture may bind a raw GL texture, so the logical texture cache cannot survive the copy. Keep the much more expensive pipeline/vertex state hot for passthrough copies. */invalidate_texture_bindings();if(format!=EfbCopyFormat::Passthrough){
#if defined(__vita__)
    scissorEnabled_=false;
#endif
    invalidate_draw_state();}return out;}
Handle Renderer::upload_efb_rgba(Handle existing,uint32_t width,uint32_t height,const void* rgba) noexcept {Handle out=efb_.upload_rgba(existing,width,height,rgba);invalidate_texture_bindings();return out;}
void Renderer::clear_current(const Color& color,float depth,bool clearRgb,bool clearAlpha,bool clearDepth) noexcept {
#if defined(__vita__)
  if(!clearRgb&&!clearAlpha&&!clearDepth)return;
  // A GX copy clear covers the target, not the preceding draw's scissor.
  glDisable(GL_SCISSOR_TEST);scissorEnabled_=false;scissorValid_=false;
  if(clearRgb!=clearAlpha) {
    // vitaGL's glClear color path does not preserve glColorMask on hardware.
    // A normal draw does: use a constant-color triangle for partial clears.
    if(!maskedClearVertices_) {
      const float vertices[]{-1,-1,0,1, 3,-1,0,1, -1,3,0,1};
      maskedClearVertices_=create_vertex_buffer(vertices,sizeof(vertices));
    }
    if(!maskedClearIndices_) {
      const uint16_t indices[]{0,1,2};
      maskedClearIndices_=create_index_buffer(indices,sizeof(indices));
    }
    PipelineDesc desc{};
    desc.cull=CullMode::None;desc.reversedZ=false;
    desc.depthTest=clearDepth;desc.depthWrite=clearDepth;desc.depthFunc=Compare::Always;
    desc.colorWrite=clearRgb;desc.alphaWrite=clearAlpha;
    desc.layout.count=1;desc.layout.attributes[0]={0,4,VertexScalar::F32,false,16,0};
    desc.tev.stages[0].color.d=TevColorArg::Konst;desc.tev.stages[0].konstColor=KonstColorSel::K0;
    desc.tev.stages[0].alpha.d=TevAlphaArg::Konst;desc.tev.stages[0].konstAlpha=KonstAlphaSel::K0A;
    const auto key=create_pipeline(desc);
    if(!key||!maskedClearVertices_||!maskedClearIndices_) {failed_=true;return;}
    DrawPacket packet{};packet.pipelineKey=key;
    packet.vertices={maskedClearVertices_,0,48};packet.indices={maskedClearIndices_,0,6};
    packet.vertexCount=3;packet.indexCount=3;
    packet.viewport.width=float(targetWidth_);packet.viewport.height=float(targetHeight_);
    packet.scissor.width=targetWidth_;packet.scissor.height=targetHeight_;
    packet.uniforms.kcolor[0]={color.r,color.g,color.b,color.a};packet.uniforms.mvp[14]=depth-1.f;
    draw(packet);
  } else {
    glColorMask(clearRgb?GL_TRUE:GL_FALSE,clearRgb?GL_TRUE:GL_FALSE,clearRgb?GL_TRUE:GL_FALSE,clearAlpha?GL_TRUE:GL_FALSE);
    glDepthMask(clearDepth?GL_TRUE:GL_FALSE);glClearColor(color.r,color.g,color.b,color.a);glClearDepth(depth);
    const GLbitfield mask=(clearRgb?GL_COLOR_BUFFER_BIT:0)|(clearDepth?GL_DEPTH_BUFFER_BIT:0);
    glClear(mask);pipelines_.invalidate_bound();
  }
#else
  (void)color;(void)depth;(void)clearRgb;(void)clearAlpha;(void)clearDepth;
#endif
}
void Renderer::execute(const CommandStream&s) noexcept {for(const auto&c:s.commands())switch(c.type){case CommandType::Clear:
  clear_current(c.clear.color,c.clear.depth,c.clear.colorEnable,c.clear.colorEnable,c.clear.depthEnable);break;
case CommandType::Draw:draw(c.draw);break;case CommandType::SetRenderTarget:if(c.target.target)bind_efb(c.target.target);else bind_default();break;case CommandType::CopyEfb:if(c.copy.destination)blit_efb(c.copy.destination);break;case CommandType::Barrier:
#if defined(__vita__)
  glFlush();
#endif
  break;}pipelines_.clear_pins();pipelines_.trim_to_budget();}
void Renderer::draw(const DrawPacket&d) noexcept {const auto*p=pipelines_.find(d.pipelineKey);if(!p)return;if(p->desc.fixedVertexOnGpu&&!d.fixedVertexUniforms)return;pipelines_.bind(*p,d.uniforms,&stats_,d.fixedVertexUniforms);
#if defined(__vita__)
  if(!viewportValid_||!same_viewport(cachedViewport_,d.viewport)){const GLint vy=static_cast<GLint>(targetHeight_)-static_cast<GLint>(d.viewport.y+d.viewport.height);glViewport((GLint)d.viewport.x,vy,(GLsizei)d.viewport.width,(GLsizei)d.viewport.height);const float minDepth=std::clamp(std::min(d.viewport.znear,d.viewport.zfar),0.0f,1.0f);const float maxDepth=std::clamp(std::max(d.viewport.znear,d.viewport.zfar),0.0f,1.0f);glDepthRangef(minDepth,maxDepth);cachedViewport_=d.viewport;viewportValid_=true;}
  if(!scissorEnabled_){glEnable(GL_SCISSOR_TEST);scissorEnabled_=true;}
  if(!scissorValid_||!same_scissor(cachedScissor_,d.scissor)){const GLint sy=static_cast<GLint>(targetHeight_)-(d.scissor.y+d.scissor.height);glScissor(d.scissor.x,sy,d.scissor.width,d.scissor.height);cachedScissor_=d.scissor;scissorValid_=true;}
  for(unsigned i=0;i<MaxTextures;i++)if(d.textures[i].texture&&(!textureStateValid_[i]||!same_texture_binding(cachedTextures_[i],d.textures[i]))){if(d.textures[i].source==TextureSource::Efb)efb_.bind_texture(d.textures[i].texture,i,d.textures[i].sampler);else textures_.bind(d.textures[i].texture,i,d.textures[i].sampler);cachedTextures_[i]=d.textures[i];textureStateValid_[i]=true;}
  GLuint vb=buffers_.gl_id(d.vertices.buffer);if(!vb)return;
  const uint32_t vertexBaseOffset=d.absoluteVertexIndices?0:d.vertices.offset;
  if(!vertexStateValid_||cachedVertexBuffer_!=d.vertices.buffer||cachedVertexOffset_!=vertexBaseOffset||cachedVertexPipeline_!=p->key){
    glBindBuffer(GL_ARRAY_BUFFER,vb);
    const auto&l=p->desc.layout;
    uint32_t desiredMask=0;
    for(unsigned i=0;i<l.count&&i<MaxVertexAttributes;i++){
      const auto&a=l.attributes[i];
      if(a.location<MaxVertexAttributes&&a.components)desiredMask|=1u<<a.location;
    }
    if(!vertexAttribMaskValid_){
      for(unsigned i=0;i<MaxVertexAttributes;i++){
        if(desiredMask&(1u<<i))glEnableVertexAttribArray(i);else glDisableVertexAttribArray(i);
      }
      enabledVertexAttribMask_=desiredMask;
      vertexAttribMaskValid_=true;
    }else if(desiredMask!=enabledVertexAttribMask_){
      const uint32_t changed=desiredMask^enabledVertexAttribMask_;
      for(unsigned i=0;i<MaxVertexAttributes;i++)if(changed&(1u<<i)){
        if(desiredMask&(1u<<i))glEnableVertexAttribArray(i);else glDisableVertexAttribArray(i);
      }
      enabledVertexAttribMask_=desiredMask;
    }
    if(!vertexAttribDefaultsValid_){
      glVertexAttrib4f(1,1,1,1,1);glVertexAttrib4f(2,1,1,1,1);
      for(unsigned i=3;i<11;i++)glVertexAttrib4f(i,0,0,0,1);
      vertexAttribDefaultsValid_=true;
    }
    for(unsigned i=0;i<l.count&&i<MaxVertexAttributes;i++){
      const auto&a=l.attributes[i];if(a.location>=MaxVertexAttributes||!a.components)continue;
      const uintptr_t off=(uintptr_t)vertexBaseOffset+a.offset;
      glVertexAttribPointer(a.location,a.components,scalar(a.scalar),a.normalized?GL_TRUE:GL_FALSE,a.stride,(const void*)off);
    }
    cachedVertexBuffer_=d.vertices.buffer;cachedVertexOffset_=vertexBaseOffset;cachedVertexPipeline_=p->key;vertexStateValid_=true;
  }
  const GLenum mode=prim(p->desc.primitive);const unsigned reps=std::max(1u,d.instanceCount);if(d.indexCount){GLuint ib=buffers_.gl_id(d.indices.buffer);if(!ib)return;if(!indexStateValid_||cachedIndexBuffer_!=d.indices.buffer){glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ib);cachedIndexBuffer_=d.indices.buffer;indexStateValid_=true;}for(unsigned i=0;i<reps;i++)glDrawElements(mode,d.indexCount,GL_UNSIGNED_SHORT,(const void*)(uintptr_t)d.indices.offset);}else for(unsigned i=0;i<reps;i++)glDrawArrays(mode,d.firstVertex,d.vertexCount);
#else
  (void)d;
#endif
  stats_.drawCalls+=std::max(1u,d.instanceCount);const uint32_t n=d.indexCount?d.indexCount:d.vertexCount;if(p->desc.primitive==Primitive::Triangles)stats_.triangles+=(n/3)*std::max(1u,d.instanceCount);else if(p->desc.primitive==Primitive::TriangleStrip||p->desc.primitive==Primitive::TriangleFan)stats_.triangles+=(n>2?n-2:0)*std::max(1u,d.instanceCount);
}
} // namespace aurora::vita::gfx
