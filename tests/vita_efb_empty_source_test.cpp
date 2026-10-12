#include "../platforms/vita/gfx/vita_efb_copy.hpp"

using aurora::vita::gfx::Scissor;
using aurora::vita::gfx::efb_copy_source_empty;
using aurora::vita::gfx::efb_snap_one_to_one_capture;

// Visible VI height is not the EFB storage bound. WiiCompiled light maps at
// logical row 456 become valid captures once scratch rows are retained.
static_assert(!efb_copy_source_empty(Scissor{0,544,51,39},1024,640));
static_assert(!efb_copy_source_empty(Scissor{50,544,52,39},1024,640));
static_assert(efb_copy_source_empty(Scissor{0,544,51,0},960,544));
static_assert(efb_copy_source_empty(Scissor{50,544,52,0},960,544));
static_assert(efb_copy_source_empty(Scissor{0,544,51,38},960,544));
static_assert(efb_copy_source_empty(Scissor{-64,20,48,40},960,544));
static_assert(efb_copy_source_empty(Scissor{960,20,16,40},960,544));
static_assert(!efb_copy_source_empty(Scissor{0,0,960,544},960,544));
static_assert(!efb_copy_source_empty(Scissor{0,0,202,153},960,544));
static_assert(!efb_copy_source_empty(Scissor{940,520,40,40},960,544));
static_assert(!efb_copy_source_empty(Scissor{-10,20,20,40},960,544));

constexpr bool validate_one_to_one_snap() {
  Scissor cropped{};
  const Scissor first{0,544,51,39},second{50,544,52,39};
  if(!efb_snap_one_to_one_capture(first,32,32,32,32,51,38,1024,640,cropped)||
     cropped.width!=51||cropped.height!=38||first.height!=39)return false;
  if(!efb_snap_one_to_one_capture(second,32,32,32,32,51,38,1024,640,cropped)||
     cropped.x!=50||cropped.width!=51||cropped.height!=38||second.width!=52)return false;
  if(efb_snap_one_to_one_capture({0,0,202,153},128,128,128,128,202,153,1024,640,cropped)||
     efb_snap_one_to_one_capture({0,0,152,114},64,48,32,24,76,57,1024,640,cropped)||
     efb_snap_one_to_one_capture({0,0,53,39},32,32,32,32,51,38,1024,640,cropped)||
     efb_snap_one_to_one_capture({999,544,52,39},32,32,32,32,51,38,1024,640,cropped)||
     efb_snap_one_to_one_capture({-1,544,52,39},32,32,32,32,51,38,1024,640,cropped))return false;
  return true;
}
static_assert(validate_one_to_one_snap());

int main() { return 0; }
