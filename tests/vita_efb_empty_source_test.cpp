#include "../platforms/vita/gfx/vita_efb_copy.hpp"

using aurora::vita::gfx::Scissor;
using aurora::vita::gfx::efb_copy_source_empty;

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

int main() { return 0; }
