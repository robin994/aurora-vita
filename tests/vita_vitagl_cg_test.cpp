#include "gfx/vita_vitagl_cg.hpp"
#include "gfx/vita_vertex_decode.hpp"
#include "gfx/vita_program_binary_cache.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace aurora::vita::gfx;
namespace {
unsigned checks=0;
void require(bool ok,int line){++checks;if(!ok){std::fprintf(stderr,"FAIL line %d\n",line);std::exit(1);}}
#define REQUIRE(x) require(bool(x),__LINE__)
}
int main(){
  PipelineDesc d{};d.layout=gpu_vertex_layout();
  d.tev.stages[0].color.d=TevColorArg::TexColor;
  d.tev.stages[0].alpha.d=TevAlphaArg::TexAlpha;
  d.tev.stages[0].texture=0;d.tev.stages[0].texCoord=0;
  d.tev.texCoordCount=1;d.texgenCount=1;d.positionIsClipSpace=false;
  d.fogMode=FogMode::Linear;d.fogRangeEnabled=true;
  d.polygonOffset=true;
  const auto before=d;
  const auto source=build_vitagl_cg(d);
  REQUIRE(source.ok());
  REQUIRE(std::memcmp(&before,&d,sizeof(d))==0);
  REQUIRE(source.vertex.find("a_position : POSITION")!=std::string::npos);
  REQUIRE(source.vertex.find("u_mvp[4]")!=std::string::npos);
  REQUIRE(source.vertex.find("u_mvp[0]*a_position.x+u_mvp[1]*a_position.y")!=std::string::npos);
  REQUIRE(source.fragment.find("u_tex0 : TEXUNIT0")!=std::string::npos);
  REQUIRE(source.fragment.find("tex2D(u_tex0,gx_sample_uv(")!=std::string::npos);
  REQUIRE(source.fragment.find("if(window_position.x<u_clip_rect")==std::string::npos);
  REQUIRE(source.fragment.find("u_fog_range_k[3]")!=std::string::npos);

  // Non-symmetric column-major projection: Cg vector-array upload must match
  // the CPU matrix-vector oracle, including translation and homogeneous W.
  const std::array<float,16> m{{2,3,5,7,11,13,17,19,23,29,31,37,41,43,47,53}};
  const std::array<float,4> v{{-2,.25f,3,1}};
  const std::array<float,4> expected{{108.75f,127.25f,134.25f,154.75f}};
  for(unsigned row=0;row<4;++row){
    float dot=0;for(unsigned col=0;col<4;++col)dot+=m[col*4+row]*v[col];
    REQUIRE(dot==expected[row]);
  }

  const std::array<float,10> knots{{1,2,3,4,5,6,7,8,9,10}};
  const auto fog=vitagl_cg_fog_knots(knots);
  for(unsigned i=0;i<10;++i)REQUIRE(fog[i]==knots[i]);
  REQUIRE(fog[10]==0&&fog[11]==0);
  // Independent linear interpolation oracle for all 10 knots, including edges.
  for(unsigned step=0;step<=36;++step){
    const float index=step*.25f;float packedValue=0;
    for(unsigned k=0;k<10;++k)packedValue+=std::max(0.f,1.f-std::abs(index-k))*fog[k];
    REQUIRE(std::abs(packedValue-(index+1.f))<.00001f);
  }

  std::array<TextureBinding,MaxTextures> bindings{};
  bindings[0].source=TextureSource::Efb;
  bindings[0].uvScaleX=.5f;bindings[0].uvScaleY=.25f;
  bindings[0].uvBiasX=.125f;bindings[0].uvBiasY=.375f;
  bindings[0].flipX=true;bindings[0].flipY=true;
  const auto crop=vitagl_cg_texture_transforms(&bindings);
  for(const float uv:{0.f,.25f,1.f}){
    REQUIRE(crop[0][0]*uv+crop[0][2]==.125f+.5f*(1.f-uv));
    REQUIRE(crop[0][1]*uv+crop[0][3]==.375f+.25f*(1.f-uv));
  }
  // Switching back to a normal texture, or a missing binding, resets every UV
  // transform; an EFB crop/flip must not leak into the next use of the program.
  bindings[0]=TextureBinding{};
  const auto normal=vitagl_cg_texture_transforms(&bindings);
  REQUIRE(normal==vitagl_cg_texture_transforms(nullptr));
  REQUIRE(normal[0]==(std::array<float,4>{{1,1,0,0}}));
  REQUIRE(crop!=normal);

  const auto glslKey=program_source_hash("same-vs","same-fs");
  const auto cgKey=program_source_hash("same-vs","same-fs",ProgramShaderLanguage::Cg);
  REQUIRE(glslKey!=cgKey);
  REQUIRE(cgKey!=program_source_hash("same-vs2","same-fs",ProgramShaderLanguage::Cg));
  REQUIRE(program_source_hash("ab","c",ProgramShaderLanguage::Cg)!=
          program_source_hash("a","bc",ProgramShaderLanguage::Cg));
  d.layout.count=0;
  REQUIRE(!build_vitagl_cg(d).ok()); // Invalid input remains a hard shader failure.
  std::printf("vitaGL Cg: %u checks, 0 failures\n",checks);
}
