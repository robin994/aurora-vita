#include "gxm_renderer.hpp"
#include "gfx/vita_renderer.hpp"
#include "gfx/vita_pipeline_key.hpp"
#include "gfx/vita_texture_decode.hpp"
#include "gfx/vita_efb_copy.hpp"
#include <algorithm>
#include <cstdio>
#include <limits>

// The same GX DrawSink and preparation/streaming code link these implementations
// instead of the vitaGL resource classes. No frontend or FIFO is duplicated.
namespace aurora::vita::gfx {
BufferPool::~BufferPool() { clear(); }
Handle BufferPool::create_vertex(const void* data,size_t bytes,bool dynamic) noexcept {
  if(!native_) return 0;
  const auto h=native_->create_buffer(data,bytes);
  if(h) map_.emplace(h,Entry{h,0,bytes,dynamic});
  return h;
}
Handle BufferPool::create_index(const void* data,size_t bytes,bool dynamic) noexcept {
  return create_vertex(data,bytes,dynamic);
}
bool BufferPool::update(Handle h,const void* data,size_t bytes,size_t offset) noexcept {
  const auto it=map_.find(h);
  return native_ && it!=map_.end() && native_->update_buffer(h,data,bytes,offset,it->second.dynamic);
}
void BufferPool::wait_idle() noexcept { if(native_) native_->finish(); }
void BufferPool::destroy(Handle h) noexcept {
  if(map_.erase(h) && native_) native_->destroy_buffer(h);
}
void BufferPool::clear() noexcept {
  if(native_) { native_->finish(); for(const auto& [h,_]:map_) native_->destroy_buffer(h); }
  map_.clear();
}

TextureCache::~TextureCache() { clear(); }
void TextureCache::pre_evict(size_t required,uint64_t frame,uint64_t protectKey) noexcept {
  while((required>budget_ || bytes_>budget_-required) && !byKey_.empty()) {
    auto victim=byKey_.end();
    for(auto it=byKey_.begin();it!=byKey_.end();++it)
      if(it->first!=protectKey && it->second.lastUse<frame &&
          (victim==byKey_.end() || it->second.lastUse<victim->second.lastUse)) victim=it;
    if(victim==byKey_.end()) break;
    ++preEvictions_;preEvictedBytes_+=victim->second.bytes;
    erase(victim->second.handle);
  }
}
Handle TextureCache::get_or_upload(const TextureDesc& desc,uint64_t frame,FrameStats* stats) noexcept {
  if(!native_) return 0;
  const uint64_t stableKey=texture_key(desc);
  if(desc.cacheable) {
    const auto it=byKey_.find(stableKey);
    if(it!=byKey_.end()) { it->second.lastUse=frame;if(stats)++stats->textureHits;return it->second.handle; }
  }
  if(stats)++stats->textureMisses;
  if(failedFrame_!=frame) {failedFrame_=frame;failedKeys_.clear();}
  if(failedKeys_.contains(stableKey)) {++retrySuppressTotal_;return 0;}
  if(!desc.width || !desc.height || desc.width>4096 || desc.height>4096 || !desc.data) return 0;
  // Native RGBA backing, including per-level row padding and one memblock page.
  size_t required=0;
  const unsigned levels=std::min<unsigned>(desc.generateMipmaps?13:std::max<unsigned>(desc.mipCount,1),13);
  for(unsigned i=0;i<levels;++i) {
    const auto w=std::max(1u,desc.width>>i),h=std::max(1u,desc.height>>i);
    required+=size_t((w+7u)&~7u)*h*4;
    if(w==1 && h==1) break;
  }
  required=(required+4095u)&~size_t(4095u);lastRequestedBytes_=required;
  pre_evict(required,frame,stableKey);
  if(required>budget_ || bytes_>budget_-required) {
    ++allocFailTotal_;failedKeys_.insert(stableKey);return 0;
  }
  auto nativeDesc=desc;
  nativeDesc.cacheable=false; // This facade is the only cache/retirement owner.
  const auto handle=native_->create_texture(nativeDesc);
  if(!handle) {++allocFailTotal_;failedKeys_.insert(stableKey);return 0;}
  uint64_t key=stableKey;
  if(!desc.cacheable) {
    key^=uint64_t(handle)*0x9e3779b97f4a7c15ull;
    while(byKey_.contains(key)) ++key;
  }
  Entry entry{};
  entry.handle=handle;entry.key=key;entry.lastUse=frame;entry.bytes=native_->texture_bytes(handle);
  entry.cacheable=desc.cacheable;entry.hasMipmaps=desc.mipCount>1 || desc.generateMipmaps;
  entry.sourceId=desc.sourceId;entry.paletteSourceId=desc.paletteSourceId;
  entry.sourceBytes=desc.dataSize;entry.paletteBytes=desc.paletteSize;
  auto [it,ok]=byKey_.emplace(key,entry);
  if(!ok) {native_->destroy_texture(handle);return 0;}
  byHandle_[handle]=&it->second;
  bytes_+=entry.bytes;highWaterBytes_=std::max(highWaterBytes_,bytes_);
  if(stats)++stats->textureUploads;
  return handle;
}
void TextureCache::bind(Handle h,unsigned unit,const SamplerDesc& sampler) noexcept {
  if(native_ && byHandle_.contains(h)) native_->bind_texture(h,unit,sampler);
}
void TextureCache::erase(Handle h) noexcept {
  const auto it=byHandle_.find(h);
  if(it==byHandle_.end()) return;
  const auto key=it->second->key;
  bytes_-=it->second->bytes;
  if(native_) native_->destroy_texture(h);
  byHandle_.erase(it);byKey_.erase(key);
}
void TextureCache::clear() noexcept {
  if(native_) {native_->finish();for(const auto& [h,_]:byHandle_)native_->destroy_texture(h);}
  byHandle_.clear();byKey_.clear();bytes_=0;failedKeys_.clear();failedFrame_=~uint64_t{0};
}
void TextureCache::trim(uint64_t frame) noexcept {
  for(auto it=byKey_.begin();it!=byKey_.end();) {
    if(!it->second.cacheable && it->second.lastUse<frame) {
      const auto h=it->second.handle;++it;erase(h);++evictions_;
    } else ++it;
  }
  pre_evict(0,frame,0);
}
size_t TextureCache::invalidate_source_range(uint64_t start,size_t bytes) noexcept {
  if(!bytes) return 0;
  const auto end=bytes>UINT64_MAX-start?UINT64_MAX:start+bytes;
  const auto overlaps=[&](uint64_t p,size_t n) {
    const auto pe=n>UINT64_MAX-p?UINT64_MAX:p+n;
    return p && n && p<end && start<pe;
  };
  std::vector<Handle> victims;
  for(const auto& [_,e]:byKey_)
    if(overlaps(e.sourceId,e.sourceBytes)||overlaps(e.paletteSourceId,e.paletteBytes))victims.push_back(e.handle);
  for(auto h:victims)erase(h);
  return victims.size();
}

PipelineCache::~PipelineCache() { clear(); }
void PipelineCache::destroy_pipeline(CompiledPipeline& p) noexcept {
  if(native_)native_->destroy_pipeline(p.key);
}
bool PipelineCache::evict_one() noexcept {
  auto victim=map_.end();
  for(auto it=map_.begin();it!=map_.end();++it)
    if(!pinned_.contains(it->first) && (victim==map_.end() || it->second.lastUsed<victim->second.lastUsed))victim=it;
  if(victim==map_.end())return false;
  if(victim->first==bound_)invalidate_bound();
  destroy_pipeline(victim->second);map_.erase(victim);++evictions_;return true;
}
void PipelineCache::set_max_entries(size_t n) noexcept {maxEntries_=n;trim_to_budget();}
void PipelineCache::trim_to_budget() noexcept {while(maxEntries_ && map_.size()>maxEntries_ && evict_one()) {}}
const CompiledPipeline* PipelineCache::get_or_create(const PipelineDesc& desc,FrameStats* stats) noexcept {
  const auto key=pipeline_key(desc);
  auto it=map_.find(key);
  if(it!=map_.end()) {it->second.lastUsed=++useSequence_;if(stats)++stats->pipelineHits;return &it->second;}
  if(stats)++stats->pipelineMisses;
  if(!native_ || failedKeys_.contains(key))return nullptr;
  if(maxEntries_ && map_.size()>=maxEntries_)evict_one();
  if(!native_->create_pipeline(desc)) {failedKeys_.insert(key);++compileFailures_;return nullptr;}
  CompiledPipeline p{};p.key=key;p.desc=desc;p.lastUsed=++useSequence_;
  const auto result=map_.emplace(key,std::move(p));
  highWaterEntries_=std::max(highWaterEntries_,map_.size());
  return &result.first->second;
}
const CompiledPipeline* PipelineCache::find(uint64_t key) noexcept {
  auto it=map_.find(key);if(it==map_.end())return nullptr;
  it->second.lastUsed=++useSequence_;return &it->second;
}
void PipelineCache::bind(const CompiledPipeline& p,const GpuDrawUniforms& u,FrameStats* stats,
                          const FixedVertexUniforms* fixed) noexcept {
  if(fixed || !native_ || !native_->bind_pipeline(p.key,u))return;
  if(bound_!=p.key && stats)++stats->stateChanges;
  bound_=p.key;boundPipeline_=const_cast<CompiledPipeline*>(&p);
}
void PipelineCache::clear() noexcept {
  if(native_)native_->finish();
  for(auto& [_,p]:map_)destroy_pipeline(p);
  map_.clear();pinned_.clear();failedKeys_.clear();invalidate_bound();
}

EfbManager::~EfbManager() {clear();}
Handle EfbManager::create(uint32_t width,uint32_t height,bool depth) noexcept {
  if(!native_)return 0;
  const auto h=native_->create_target(width,height,depth);
  if(!h)return 0;
  Entry e{};e.width=width;e.height=height;e.color=h;e.fbo=h;e.bytes=native_->texture_bytes(h);
  map_.emplace(h,e);bytes_+=e.bytes;highWaterBytes_=std::max(bytes_,highWaterBytes_);
  return h;
}
bool EfbManager::bind(Handle h) noexcept {
  if(!native_ || !map_.contains(h) || !native_->bind_target(h))return false;
  boundFbo_=h;return true;
}
void EfbManager::bind_default(uint32_t,uint32_t) noexcept {
  if(native_ && native_->bind_target(0))boundFbo_=0;
}
bool EfbManager::blit_to_default(Handle h,uint32_t,uint32_t) noexcept {
  if(!native_ || !map_.contains(h) || !native_->blit_to_default(h))return false;
  boundFbo_=0;return true;
}
Handle EfbManager::upload_rgba(Handle existing,uint32_t width,uint32_t height,const void* rgba) noexcept {
  if(!native_ || !rgba)return 0;
  auto it=map_.find(existing);
  if(it!=map_.end() && it->second.width==width && it->second.height==height)
    return native_->upload_target(existing,rgba,width,height)?existing:0;
  const auto h=create(width,height,false);
  if(!h)return 0;
  if(!native_->upload_target(h,rgba,width,height)) {destroy(h);return 0;}
  if(existing)destroy(existing);
  return h;
}
Handle EfbManager::capture_from_bound(Handle existing,int32_t x,int32_t y,uint32_t sw,uint32_t sh,
                                      uint32_t dw,uint32_t dh,EfbCopyFormat format,bool,bool flipX,bool flipY) noexcept {
  if(!native_)return 0;
  uint32_t width=0,height=0;
  if(boundFbo_) {
    const auto current=map_.find(boundFbo_);
    if(current==map_.end())return 0;
    width=current->second.width;height=current->second.height;
  } else {
    width=960;height=544;
  }
  const int64_t top=int64_t(height)-y-sh;
  if(top<0 || top>INT32_MAX || sw>INT32_MAX || sh>INT32_MAX)return 0;
  const Scissor rect{x,int32_t(top),int32_t(sw),int32_t(sh)};
  const bool nativeTransfer=(format==EfbCopyFormat::Passthrough || format==EfbCopyFormat::RGB565) &&
      ((sw==dw && sh==dh) || (sw==dw*2u && sh==dh*2u));
  if(nativeTransfer) {
    Handle target=0;
    const auto existingIt=map_.find(existing);
    if(existingIt!=map_.end() && existingIt->second.width==dw && existingIt->second.height==dh)target=existing;
    else target=create(dw,dh,false);
    if(!target)return 0;
    // CPU fallback below consumes top-left rows and applies !flipY after the
    // legacy framebuffer-origin normalization. Preserve that exact contract in
    // the native render-to-texture path so GXCopyTex is backend interchangeable.
    if(native_->copy_current_to_target(target,rect,format,flipX,!flipY)) {
      if(existing && existing!=target)destroy(existing);
      return target;
    }
    if(target!=existing)destroy(target);
    // Rare feedback/self-copy or an unsupported hardware edge can still use
    // the correctness path below. It converts in cached CPU memory and performs
    // only one bulk upload; the native hot path never edits uncached pixels.
  }

  std::vector<uint8_t> pixels,copy;
  // A failed GPU fixup may already have switched targets. The fallback must
  // read the original source, including when allocation/shader creation fails.
  if(!native_->bind_target(boundFbo_))return 0;
  if(!native_->read_current(pixels,width,height))return 0;
  // The shared capture API follows the existing render-texture convention:
  // its first sampled row is the framebuffer's bottom row. Native read_current
  // returns top-left scanout rows, so normalize that origin before user flips.
  if(!copy_efb_rgba8(pixels.data(),width,height,rect,dw,dh,format,flipX,!flipY,copy))return 0;
  return upload_rgba(existing,dw,dh,copy.data());
}
bool EfbManager::bind_texture(Handle h,unsigned unit,const SamplerDesc& s) noexcept {
  return native_ && map_.contains(h) && native_->bind_texture(h,unit,s);
}
bool EfbManager::read_rgba(Handle h,std::vector<uint8_t>& out) noexcept {
  return native_ && map_.contains(h) && native_->read_target(h,out);
}
bool EfbManager::dimensions(Handle h,uint32_t& w,uint32_t& ht) const noexcept {
  const auto it=map_.find(h);if(it==map_.end())return false;
  w=it->second.width;ht=it->second.height;return true;
}
void EfbManager::destroy(Handle h) noexcept {
  auto it=map_.find(h);if(it==map_.end())return;
  if(native_)native_->destroy_texture(h);
  bytes_-=it->second.bytes;map_.erase(it);
  if(boundFbo_==h)boundFbo_=0;
}
void EfbManager::clear() noexcept {
  if(native_) {native_->finish();for(const auto& [h,_]:map_)native_->destroy_texture(h);}
  map_.clear();bytes_=0;boundFbo_=0;
}

Renderer::Renderer(const RendererConfig& cfg):cfg_(cfg),native_(std::make_unique<gxm::Renderer>()),
    pipelines_(cfg.pipelineBudget),textures_(cfg.textureBudget) {}
Renderer::~Renderer() {shutdown();}
bool Renderer::initialize() noexcept {
  if(initialized_)return true;
  gxm::Config c{};c.width=cfg_.width;c.height=cfg_.height;c.displayBuffers=cfg_.displayBuffers;
  c.waitVblank=cfg_.waitVblank;c.resourceBudgetBytes=cfg_.nativeResourceBudget;
  c.cdramPoolBytes=cfg_.nativeCdramPoolBytes;
  c.cdramReserveBytes=cfg_.nativeCdramReserveBytes;
  c.d16Depth=cfg_.nativeD16Depth;
  c.programCachePath=cfg_.programBinaryCachePath;
  c.maxPipelines=cfg_.pipelineBudget+16; // Native clear/blit variants are not GX cache entries.
  if(!native_->initialize(c))return false;
  buffers_.native_=textures_.native_=pipelines_.native_=efb_.native_=native_.get();
  targetWidth_=cfg_.width;targetHeight_=cfg_.height;initialized_=true;failed_=false;
  return true;
}
void Renderer::shutdown() noexcept {
  if(!initialized_)return;
  native_->finish();pipelines_.clear();textures_.clear();buffers_.clear();efb_.clear();
  buffers_.native_=textures_.native_=pipelines_.native_=efb_.native_=nullptr;
  native_->shutdown();initialized_=false;
}
const char* Renderer::last_error() const noexcept {return native_->last_error();}
void Renderer::begin_frame() noexcept {
  pipelines_.clear_pins();pipelines_.trim_to_budget();stats_={};
  failed_=!native_->begin_frame();boundEfb_=0;targetWidth_=cfg_.width;targetHeight_=cfg_.height;
}
void Renderer::end_frame() noexcept {textures_.trim(frame_);++frame_;}
bool Renderer::present(bool display) noexcept {
  const bool ok=native_->end_frame(display);failed_=failed_||!ok;
  // Include internal clears, copies and the final display scene. The last GX
  // draw alone is not a complete snapshot of the native frame.
  const auto& s=native_->stats();
  stats_.nativeTimingsSampled=s.nativeTimingsSampled;
  stats_.nativePipelineUs=s.nativePipelineUs;stats_.nativeTextureUs=s.nativeTextureUs;
  stats_.nativeDrawUs=s.nativeDrawUs;stats_.nativeSceneCount=s.nativeSceneCount;
  stats_.nativeVertexUniformReuses=s.nativeVertexUniformReuses;
  stats_.nativeFragmentUniformReuses=s.nativeFragmentUniformReuses;
  stats_.nativeEfbCopies=s.nativeEfbCopies;stats_.nativeEfbEndSceneUs=s.nativeEfbEndSceneUs;
  stats_.nativeEfbTransferSubmitUs=s.nativeEfbTransferSubmitUs;
  stats_.nativeEfbTransferWaitUs=s.nativeEfbTransferWaitUs;
  stats_.nativeEfbCpuFixupUs=s.nativeEfbCpuFixupUs;
  return ok;
}
bool Renderer::readback_rgba8(std::vector<uint8_t>& pixels) noexcept {return native_->readback_rgba8(pixels);}
uint64_t Renderer::create_pipeline(const PipelineDesc& d) noexcept {
  const auto* p=pipelines_.get_or_create(d,&stats_);if(!p)return 0;pipelines_.pin(p->key);return p->key;
}
Handle Renderer::create_texture(const TextureDesc& d) noexcept {return textures_.get_or_upload(d,frame_,&stats_);}
size_t Renderer::invalidate_texture_source_range(uint64_t start,size_t bytes) noexcept {return textures_.invalidate_source_range(start,bytes);}
Handle Renderer::create_vertex_buffer(const void* data,size_t bytes,bool dynamic) noexcept {return buffers_.create_vertex(data,bytes,dynamic);}
Handle Renderer::create_index_buffer(const void* data,size_t bytes,bool dynamic) noexcept {return buffers_.create_index(data,bytes,dynamic);}
bool Renderer::update_buffer(Handle h,const void* data,size_t bytes,size_t offset) noexcept {return buffers_.update(h,data,bytes,offset);}
Handle Renderer::create_efb(uint32_t w,uint32_t h,bool depth) noexcept {return efb_.create(w,h,depth);}
bool Renderer::bind_efb(Handle h) noexcept {
  uint32_t w=0,ht=0;if(!efb_.dimensions(h,w,ht)||!efb_.bind(h))return false;
  boundEfb_=h;targetWidth_=w;targetHeight_=ht;return true;
}
void Renderer::bind_default() noexcept {
  efb_.bind_default(cfg_.width,cfg_.height);boundEfb_=0;targetWidth_=cfg_.width;targetHeight_=cfg_.height;
}
bool Renderer::blit_efb(Handle h) noexcept {
  if(!efb_.blit_to_default(h,cfg_.width,cfg_.height))return false;
  boundEfb_=0;targetWidth_=cfg_.width;targetHeight_=cfg_.height;return true;
}
bool Renderer::display_copy(const Scissor& src) noexcept {
  if(!native_->copy_display_region(src)) {failed_=true;return false;}
  boundEfb_=0;targetWidth_=cfg_.width;targetHeight_=cfg_.height;return true;
}
Handle Renderer::capture_current(Handle existing,const Scissor& s,uint32_t dw,uint32_t dh,EfbCopyFormat f,bool fx,bool fy) noexcept {
  const int64_t y=int64_t(targetHeight_)-s.y-s.height;
  if(s.width<=0||s.height<=0||y<INT32_MIN||y>INT32_MAX)return 0;
  const auto h=efb_.capture_from_bound(existing,s.x,int32_t(y),s.width,s.height,dw,dh,f,false,fx,fy);
  const auto& sNative=native_->stats();
  stats_.nativeEfbCopies=sNative.nativeEfbCopies;
  stats_.nativeEfbEndSceneUs=sNative.nativeEfbEndSceneUs;
  stats_.nativeEfbTransferSubmitUs=sNative.nativeEfbTransferSubmitUs;
  stats_.nativeEfbTransferWaitUs=sNative.nativeEfbTransferWaitUs;
  stats_.nativeEfbCpuFixupUs=sNative.nativeEfbCpuFixupUs;
  return h;
}
Handle Renderer::upload_efb_rgba(Handle existing,uint32_t w,uint32_t h,const void* data) noexcept {return efb_.upload_rgba(existing,w,h,data);}
void Renderer::clear_current(const Color& c,float z,bool rgb,bool alpha,bool depth) noexcept {
  if(!native_->clear(c,z,rgb,alpha,depth))failed_=true;
}
// GXM receives immutable native descriptors at each draw; there are no GL binding
// mirrors to invalidate. The GX cache and source generations remain shared.
void Renderer::invalidate_resource_bindings() noexcept {}
void Renderer::invalidate_texture_bindings() noexcept {}
void Renderer::invalidate_buffer_bindings() noexcept {}
void Renderer::invalidate_draw_state() noexcept {pipelines_.invalidate_bound();}
void Renderer::draw(const DrawPacket& packet) noexcept {
  if(failed_)return;
  if(!native_->draw(packet)) {failed_=true;return;}
  const auto& s=native_->stats();stats_.drawCalls=s.drawCalls;stats_.triangles=s.triangles;
  stats_.nativePipelineUs=s.nativePipelineUs;stats_.nativeTextureUs=s.nativeTextureUs;stats_.nativeDrawUs=s.nativeDrawUs;
  stats_.nativeVertexUniformReuses=s.nativeVertexUniformReuses;
  stats_.nativeFragmentUniformReuses=s.nativeFragmentUniformReuses;
  stats_.nativeSceneCount=s.nativeSceneCount;
}
void Renderer::execute(const CommandStream& stream) noexcept {
  execute_range(stream,0,stream.size(),true);
}
void Renderer::execute_range(const CommandStream& stream,size_t begin,size_t end,bool finalize) noexcept {
  const auto& commands=stream.commands();
  begin=std::min(begin,commands.size());end=std::min(std::max(end,begin),commands.size());
  for(size_t index=begin;index<end;++index) {
    const auto& c=commands[index];
    if(failed_)break;
    switch(c.type) {
    case CommandType::Clear:clear_current(c.clear.color,c.clear.depth,c.clear.colorEnable,c.clear.colorEnable,c.clear.depthEnable);break;
    case CommandType::Draw:draw(stream.draw_packet(c.drawIndex));break;
    case CommandType::SetRenderTarget:
      if(c.target.target) {if(!bind_efb(c.target.target))failed_=true;} else bind_default();break;
    case CommandType::CopyEfb:if(c.copy.destination&&!blit_efb(c.copy.destination))failed_=true;break;
    case CommandType::Barrier:if(!native_->finish())failed_=true;break;
    }
  }
  if(finalize){pipelines_.clear_pins();pipelines_.trim_to_budget();}
}
}
