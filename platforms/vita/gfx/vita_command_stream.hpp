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
  // Keep packet storage at its high-water mark. A DrawPacket exceeds deque's
  // usual block size: clearing it used to free/allocate once per draw per frame.
  void reset() noexcept { commands_.clear(); drawCount_=0; }
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
    commands_.emplace_back();auto&x=commands_.back();x.type=CommandType::Draw;
    x.drawIndex=drawCount_++;
  }
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
  std::vector<Command> commands_;
  std::deque<DrawPacket> draws_;
  uint32_t drawCount_=0;
};
} // namespace aurora::vita::gfx
