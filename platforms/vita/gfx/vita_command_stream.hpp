#pragma once
#include "vita_gfx_types.hpp"
#include <array>
#include <cstdint>
#include <deque>
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
  // Keep packet storage at its high-water mark. A deque preserves packet
  // addresses while the stream grows; fixed-vertex snapshots and regression
  // tooling rely on that stability across later emplace_draw() calls.
  void reset() noexcept {
    commands_.clear();drawCount_=0;
    drawOnly_=true;
    uniformStateCount_=textureStateCount_=0;
    lastUniformGeneration_=lastTextureGeneration_=0;
    lastUniformRef_=nullptr;lastTextureRef_=nullptr;
  }
  void reserve(size_t n){commands_.reserve(n);}
  void clear(const ClearCommand& c){drawOnly_=false;commands_.emplace_back();auto&x=commands_.back();x.type=CommandType::Clear;x.clear=c;}
  DrawPacket& emplace_draw(){
    if(drawCount_==draws_.size())draws_.emplace_back();
    else draws_[drawCount_]=DrawPacket{};
    commands_.emplace_back();auto&x=commands_.back();x.type=CommandType::Draw;
    x.drawIndex=drawCount_++;
    return draws_[x.drawIndex];
  }
  DrawPacket& emplace_draw_fast(){
    if(drawCount_==draws_.size())draws_.emplace_back();
    else reset_reused_draw(draws_[drawCount_]);
    commands_.emplace_back();auto&x=commands_.back();x.type=CommandType::Draw;
    x.drawIndex=drawCount_++;
    return draws_[x.drawIndex];
  }
  void draw(const DrawPacket& d){
    if(drawCount_==draws_.size())draws_.push_back(d);
    else draws_[drawCount_]=d;
    commands_.emplace_back();auto&x=commands_.back();x.type=CommandType::Draw;
    x.drawIndex=drawCount_++;
  }
  void set_render_target(Handle h){drawOnly_=false;commands_.emplace_back();auto&x=commands_.back();x.type=CommandType::SetRenderTarget;x.target.target=h;}
  void copy_efb(const CopyEfbCommand& c){drawOnly_=false;commands_.emplace_back();auto&x=commands_.back();x.type=CommandType::CopyEfb;x.copy=c;}
  void barrier(){drawOnly_=false;commands_.emplace_back();commands_.back().type=CommandType::Barrier;}
  void bind_state(DrawPacket& packet,const DrawUniforms& uniforms,
                  const std::array<TextureBinding,MaxTextures>& textures,
                  uint32_t uniformGeneration,uint32_t textureGeneration) {
    packet.uniformGeneration=uniformGeneration;
    packet.textureGeneration=textureGeneration;
    if(uniformGeneration) {
      if(!lastUniformRef_||lastUniformGeneration_!=uniformGeneration) {
        if(uniformStateCount_==uniformStates_.size())uniformStates_.emplace_back(uniforms);
        else uniformStates_[uniformStateCount_]=uniforms;
        lastUniformRef_=&uniformStates_[uniformStateCount_++];
        lastUniformGeneration_=uniformGeneration;
      }
      packet.uniformRef=lastUniformRef_;
    } else {
      packet.uniformRef=nullptr;
      packet.uniforms=uniforms;
    }
    if(textureGeneration) {
      if(!lastTextureRef_||lastTextureGeneration_!=textureGeneration) {
        if(textureStateCount_==textureStates_.size())textureStates_.push_back(textures);
        else textureStates_[textureStateCount_]=textures;
        lastTextureRef_=&textureStates_[textureStateCount_++];
        lastTextureGeneration_=textureGeneration;
      }
      packet.textureRef=lastTextureRef_;
    } else {
      packet.textureRef=nullptr;
      packet.textures=textures;
    }
  }
  DrawPacket* tail_draw() noexcept {
    return !commands_.empty()&&commands_.back().type==CommandType::Draw?&draws_[commands_.back().drawIndex]:nullptr;
  }
  const DrawPacket& draw_packet(uint32_t index) const noexcept{return draws_[index];}
  const std::vector<Command>& commands() const noexcept{return commands_;}
  bool draw_only() const noexcept{return drawOnly_;}
  uint32_t draw_count() const noexcept{return drawCount_;}
  size_t size() const noexcept{return commands_.size();}
private:
  static void reset_reused_draw(DrawPacket& d) noexcept {
    // Hot callers overwrite uniforms/textures/viewport/scissor immediately.
    // Reset only fields whose default value can affect a partially populated
    // packet; avoiding DrawPacket{} removes a ~1 KiB zero-fill per submitted draw.
    d.pipelineKey=0;
    d.vertices={};d.indices={};
    d.vertexCount=0;d.indexCount=0;d.firstVertex=0;d.instanceCount=1;
    d.uniformGeneration=0;d.textureGeneration=0;
    d.absoluteVertexIndices=false;
    d.uniformRef=nullptr;d.textureRef=nullptr;
    d.fixedVertexUniforms=nullptr;
  }
  std::vector<Command> commands_;
  std::deque<DrawPacket> draws_;
  std::deque<GpuDrawUniforms> uniformStates_;
  std::deque<std::array<TextureBinding,MaxTextures>> textureStates_;
  uint32_t drawCount_=0;
  uint32_t uniformStateCount_=0,textureStateCount_=0;
  uint32_t lastUniformGeneration_=0,lastTextureGeneration_=0;
  const GpuDrawUniforms* lastUniformRef_=nullptr;
  const std::array<TextureBinding,MaxTextures>* lastTextureRef_=nullptr;
  bool drawOnly_=true;
};
} // namespace aurora::vita::gfx
