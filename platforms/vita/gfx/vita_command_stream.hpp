#pragma once
#include "vita_gfx_types.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace aurora::vita::gfx {
enum class CommandType : uint8_t { Clear, Draw, SetRenderTarget, CopyEfb, Barrier };
struct ClearCommand { Color color{}; float depth=1.f; bool colorEnable=true; bool depthEnable=true; };
struct RenderTargetCommand { Handle target=InvalidHandle; };
struct CopyEfbCommand { Handle destination=InvalidHandle; uint32_t width=0,height=0; };
struct Command {
  CommandType type=CommandType::Barrier;
  ClearCommand clear{};
  DrawPacket draw{};
  RenderTargetCommand target{};
  CopyEfbCommand copy{};
};
class CommandStream {
public:
  void reset() noexcept { commands_.clear(); }
  void reserve(size_t n){commands_.reserve(n);}
  void clear(const ClearCommand& c){commands_.emplace_back();auto&x=commands_.back();x.type=CommandType::Clear;x.clear=c;}
  void draw(const DrawPacket& d){commands_.emplace_back();auto&x=commands_.back();x.type=CommandType::Draw;x.draw=d;}
  void set_render_target(Handle h){commands_.emplace_back();auto&x=commands_.back();x.type=CommandType::SetRenderTarget;x.target.target=h;}
  void copy_efb(const CopyEfbCommand& c){commands_.emplace_back();auto&x=commands_.back();x.type=CommandType::CopyEfb;x.copy=c;}
  void barrier(){commands_.emplace_back();commands_.back().type=CommandType::Barrier;}
  DrawPacket* tail_draw() noexcept {
    return !commands_.empty()&&commands_.back().type==CommandType::Draw?&commands_.back().draw:nullptr;
  }
  const std::vector<Command>& commands() const noexcept{return commands_;}
  size_t size() const noexcept{return commands_.size();}
private:std::vector<Command> commands_;
};
} // namespace aurora::vita::gfx
