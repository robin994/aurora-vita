#pragma once
#include "vita_gfx_types.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <cstring>
#include <utility>
#include <vector>

namespace aurora::vita::gfx {
enum class CommandType : uint8_t { Clear, Draw, SetRenderTarget, CopyEfb, Barrier };
struct ClearCommand { Color color{}; float depth=1.f; bool colorEnable=true; bool depthEnable=true; };
struct RenderTargetCommand { Handle target=InvalidHandle; };
struct CopyEfbCommand { Handle destination=InvalidHandle; uint32_t width=0,height=0; };
struct Command {
  CommandType type=CommandType::Barrier;
  ClearCommand clear{};
  uint32_t drawIndex=0;
  RenderTargetCommand target{};
  CopyEfbCommand copy{};
};
class CommandStream {
public:
  CommandStream()=default;
  CommandStream(const CommandStream& other):commands_(other.commands_),draws_(other.draws_),
      stateCount_(other.stateCount_),drawCount_(other.drawCount_){
    // A copied batch owns its state. Materialize shared views rather than
    // retaining pointers into the source stream, which may reset immediately.
    for(uint32_t i=0;i<drawCount_;++i)materialize(draws_[i],other.draws_[i]);
  }
  CommandStream& operator=(const CommandStream& other){
    if(this!=&other){CommandStream copy(other);swap(copy);}return *this;
  }
  CommandStream(CommandStream&& other) noexcept {swap(other);other.reset();}
  CommandStream& operator=(CommandStream&& other) noexcept {
    if(this!=&other){swap(other);other.reset();}return *this;
  }
  // Keep packet storage at its high-water mark. A DrawPacket exceeds deque's
  // usual block size: clearing it used to free/allocate once per draw per frame.
  void reset() noexcept { commands_.clear(); drawCount_=0;stateCount_=0;lastState_=nullptr; }
  void reserve(size_t n){commands_.reserve(n);}
  void clear(const ClearCommand& c){commands_.emplace_back();auto&x=commands_.back();x.type=CommandType::Clear;x.clear=c;}
  DrawPacket& emplace_draw(){
    if(drawCount_==draws_.size())draws_.emplace_back();
    else draws_[drawCount_]=DrawPacket{};
    commands_.emplace_back();auto&x=commands_.back();x.type=CommandType::Draw;
    x.drawIndex=drawCount_++;
    return draws_[x.drawIndex];
  }
  void draw(const DrawPacket& d){
    if(drawCount_==draws_.size())draws_.push_back(d);
    else draws_[drawCount_]=d;
    materialize(draws_[drawCount_],d);
    commands_.emplace_back();auto&x=commands_.back();x.type=CommandType::Draw;
    x.drawIndex=drawCount_++;
  }
  // The streamed producer fills all geometry fields. Inline uniform/texture
  // storage is deliberately untouched because sharedState supplies both views.
  DrawPacket& emplace_geometry_draw(){
    if(drawCount_==draws_.size())draws_.emplace_back();
    commands_.emplace_back();auto& x=commands_.back();x.type=CommandType::Draw;
    x.drawIndex=drawCount_++;
    return draws_[x.drawIndex];
  }
  void discard_tail_draw() noexcept {
    if(!commands_.empty()&&commands_.back().type==CommandType::Draw){
      const bool droppedSource=lastState_==&draws_[commands_.back().drawIndex];
      commands_.pop_back();--drawCount_;
      if(droppedSource){
        auto* tail=tail_draw();lastState_=tail?(tail->sharedState?tail->sharedState:tail):nullptr;
      }
    }
  }
  void share_draw_state(DrawPacket& packet,const DrawUniforms& uniforms,
                        const std::array<TextureBinding,MaxTextures>& textures,uint64_t revision,
                        uint64_t textureRevision=0){
    if(gxm_disabled(GxmDisableSharedState)||gxm_disabled(GxmDisableUniformRevision)){
      ++stateCount_;packet.uniforms=uniforms;packet.textures=textures;packet.sharedState=nullptr;
      packet.uniformRevision=revision;packet.textureBindingRevision=textureRevision;
      lastState_=&packet;lastStateRevision_=revision;lastTextureRevision_=textureRevision;return;
    }
    const bool sameUniforms=lastState_&&((revision&&revision==lastStateRevision_)||
        (!revision&&!std::memcmp(&lastState_->uniforms,&uniforms,sizeof(GpuDrawUniforms))));
    const bool sameTextures=lastState_&&((textureRevision&&textureRevision==lastTextureRevision_)||
        (!textureRevision&&!std::memcmp(lastState_->textures.data(),textures.data(),sizeof(textures))));
    if(!sameUniforms||!sameTextures){
      ++stateCount_;packet.uniforms=uniforms;packet.textures=textures;
      packet.sharedState=nullptr;lastState_=&packet;
    }else packet.sharedState=lastState_;
    packet.uniformRevision=revision;packet.textureBindingRevision=textureRevision;
    lastStateRevision_=revision;lastTextureRevision_=textureRevision;
  }
  size_t state_snapshot_count() const noexcept {return stateCount_;}
  void set_render_target(Handle h){commands_.emplace_back();auto&x=commands_.back();x.type=CommandType::SetRenderTarget;x.target.target=h;}
  void copy_efb(const CopyEfbCommand& c){commands_.emplace_back();auto&x=commands_.back();x.type=CommandType::CopyEfb;x.copy=c;}
  void barrier(){commands_.emplace_back();commands_.back().type=CommandType::Barrier;}
  DrawPacket* tail_draw() noexcept {
    return !commands_.empty()&&commands_.back().type==CommandType::Draw?&draws_[commands_.back().drawIndex]:nullptr;
  }
  const DrawPacket& draw_packet(uint32_t index) const noexcept{return draws_[index];}
  const std::vector<Command>& commands() const noexcept{return commands_;}
  size_t size() const noexcept{return commands_.size();}
private:
  static void materialize(DrawPacket& destination,const DrawPacket& source) noexcept {
    if(source.sharedState){destination.uniforms=source.gpu_uniforms();destination.textures=source.texture_bindings();}
    destination.sharedState=nullptr;
  }
  void swap(CommandStream& other) noexcept {
    commands_.swap(other.commands_);draws_.swap(other.draws_);
    std::swap(stateCount_,other.stateCount_);std::swap(drawCount_,other.drawCount_);
    std::swap(lastState_,other.lastState_);std::swap(lastStateRevision_,other.lastStateRevision_);
    std::swap(lastTextureRevision_,other.lastTextureRevision_);
  }
  std::vector<Command> commands_;
  std::deque<DrawPacket> draws_;
  size_t stateCount_=0;
  const DrawPacket* lastState_=nullptr;
  uint64_t lastStateRevision_=0;
  uint64_t lastTextureRevision_=0;
  uint32_t drawCount_=0;
};
} // namespace aurora::vita::gfx
