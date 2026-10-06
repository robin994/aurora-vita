#include "gxm/gxm_shader_gen.hpp"
#include "gfx/vita_fixed_vertex.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <random>
#include <string>
using namespace aurora::vita;
using namespace gfx;
static size_t checks;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
using RGBA=std::array<double,4>;
static double wrap(double v) { const double b=v*255.;return (b-std::floor(b/256.)*256.)/255.; }
// Independent scalar interpreter of the emitted material expressions. It does
// not call the generator's arithmetic or input helpers; each RGB lane is run
// separately. This catches SSA lifetime/read-before-write and wrapping errors.
struct Expression {
  const std::string& s;const std::map<std::string,double>& vars;size_t pos=0;
  double sum(){double v=product();while(pos<s.size()&&(s[pos]=='+'||s[pos]=='-')){char c=s[pos++];double b=product();v=c=='+'?v+b:v-b;}return v;}
  double product(){double v=atom();while(pos<s.size()&&s[pos]=='*'){++pos;v*=atom();}return v;}
  double atom(){
    if(s[pos]=='-'){++pos;return -atom();}
    if(s[pos]=='('){++pos;double v=sum();CHECK(s[pos++]==')');return v;}
    if(std::isdigit(s[pos])||s[pos]=='.') {char* end=nullptr;double v=std::strtod(s.c_str()+pos,&end);pos=size_t(end-s.c_str());return v;}
    const size_t start=pos;
    while(pos<s.size()&&(std::isalnum(s[pos])||s[pos]=='_'||s[pos]=='['||s[pos]==']'||s[pos]=='.'))++pos;
    const std::string name=s.substr(start,pos-start);
    if(pos==s.size()||s[pos]!='('){auto it=vars.find(name);CHECK(it!=vars.end());return it->second;}
    ++pos;const double a=sum();
    if(name=="float3"||name=="tev_wrap1"||name=="tev_wrap3"){CHECK(s[pos++]==')');return name=="float3"?a:wrap(a);}
    CHECK(s[pos++]==',');const double b=sum();CHECK(s[pos++]==',');const double c=sum();CHECK(s[pos++]==')');
    CHECK(name=="clamp"||name=="lerp");return name=="clamp"?std::clamp(a,b,c):a+(b-a)*c;
  }
  double value(){const double v=sum();CHECK(pos==s.size());return v;}
};
static double konst(unsigned k,bool alpha,unsigned lane,const std::array<RGBA,4>& colors) {
  if(k<8)return (8.-k)/8.;
  if(!alpha&&k<12)return colors[k-8][lane];
  k-=alpha?8:12;return colors[k%4][k/4];
}
static RGBA swapped(const RGBA& v,const TevSwapDesc& s) {
  return {v[unsigned(s.r)],v[unsigned(s.g)],v[unsigned(s.b)],v[unsigned(s.a)]};
}
static double color(unsigned arg,unsigned lane,const TevStage& s,const std::array<RGBA,4>& regs,
                    const std::array<bool,4>& cn,const std::array<bool,4>& an,
                    const RGBA& tex,const RGBA& ras,const std::array<RGBA,4>& kc,bool abc) {
  if(arg<8){bool alpha=arg&1;double v=regs[arg/2][alpha?3:lane];return abc&&!(alpha?an[arg/2]:cn[arg/2])?wrap(v):v;}
  switch(arg){case 8:return tex[lane];case 9:return tex[3];case 10:return ras[lane];case 11:return ras[3];
  case 12:return 1.;case 13:return .5;case 14:return konst(unsigned(s.konstColor),false,lane,kc);default:return 0.;}
}
static double alpha(unsigned arg,const TevStage& s,const std::array<RGBA,4>& regs,const std::array<bool,4>& an,
                    const RGBA& tex,const RGBA& ras,const std::array<RGBA,4>& kc,bool abc) {
  if(arg<4){double v=regs[arg][3];return abc&&!an[arg]?wrap(v):v;}
  switch(arg){case 4:return tex[3];case 5:return ras[3];case 6:return konst(unsigned(s.konstAlpha),true,0,kc);default:return 0.;}
}
static double reference(std::array<double,4> v,TevOp op,TevBias bias,TevScale scale,bool clamp) {
  const double b[]{0.,.5,-.5},sc[]{1.,2.,4.,.5};
  // Match the emitted/Cg lerp evaluation order. The algebraically equivalent
  // A*(1-C)+B*C form accumulates different host double rounding and made this
  // equivalence test depend on the standard library's random sample sequence.
  const double mix=v[0]+(v[1]-v[0])*v[2];
  return std::clamp((v[3]+(op==TevOp::Add?mix:-mix)+b[unsigned(bias)])*sc[unsigned(scale)],clamp?0.:-4.,clamp?1.:4.);
}
static std::string rhs(const std::string& source,const std::string& lhs) {
  const auto at=source.find(lhs+"=clamp(");CHECK(at!=std::string::npos);
  const auto begin=at+lhs.size()+1,end=source.find(';',begin);CHECK(end!=std::string::npos);
  return source.substr(begin,end-begin);
}
static void equivalence() {
  std::mt19937 rng(0x534d5356);std::uniform_real_distribution<double> normal(0.,1.),signedValue(-4.,4.);
  for(unsigned test=0;test<768;++test) {
    PipelineDesc d;d.layout=gpu_vertex_layout(255,3);d.texgenCount=8;
    d.tev.stageCount=1+test%16;d.tev.rasterColorCount=2;
    for(auto& sw:d.tev.swapTable)sw={TevChannel(rng()%4),TevChannel(rng()%4),TevChannel(rng()%4),TevChannel(rng()%4)};
    for(unsigned i=0;i<d.tev.stageCount;++i) {
      auto& s=d.tev.stages[i];s.texture=i%8;s.texCoord=i%8;s.texSwap=rng()%4;s.rasSwap=rng()%4;
      s.rasterSource=RasterSource(rng()%2);s.color={TevColorArg(rng()%16),TevColorArg(rng()%16),TevColorArg(rng()%16),TevColorArg(rng()%16)};
      s.alpha={TevAlphaArg(rng()%8),TevAlphaArg(rng()%8),TevAlphaArg(rng()%8),TevAlphaArg(rng()%8)};
      s.colorOp=TevOp(rng()%2);s.alphaOp=TevOp(rng()%2);s.colorOut=TevReg(rng()%4);s.alphaOut=TevReg(rng()%4);
      s.colorBias=TevBias(rng()%3);s.alphaBias=TevBias(rng()%3);s.colorScale=TevScale(rng()%4);s.alphaScale=TevScale(rng()%4);
      s.colorClamp=rng()%2;s.alphaClamp=rng()%2;s.konstColor=KonstColorSel(rng()%28);s.konstAlpha=KonstAlphaSel(rng()%24);
    }
    const auto cg=gxm::build_material_cg(d);CHECK(cg.ok());CHECK(cg.nativeMaterial);
    CHECK(cg.fragment.find("float4 prev=")==std::string::npos);
    CHECK(cg.vertex==gxm::build_tev_cg(d).vertex);
    for(unsigned sample=0;sample<8;++sample) {
      std::array<RGBA,4> regs,kc;for(auto& v:regs)for(auto& x:v)x=signedValue(rng);for(auto& v:kc)for(auto& x:v)x=normal(rng);
      std::array<std::map<std::string,double>,4> vars;
      for(unsigned lane=0;lane<4;++lane)for(unsigned r=0;r<4;++r) {
        vars[lane]["u_tevreg["+std::to_string(r)+"].rgb"]=regs[r][lane];vars[lane]["u_tevreg["+std::to_string(r)+"].a"]=regs[r][3];
        vars[lane]["u_kcolor["+std::to_string(r)+"].rgb"]=kc[r][lane];
        for(unsigned c=0;c<4;++c)vars[lane]["u_kcolor["+std::to_string(r)+"]."+"rgba"[c]]=kc[r][c];
      }
      std::array<bool,4> cn{},an{};
      for(unsigned i=0;i<d.tev.stageCount;++i) {
        const auto& s=d.tev.stages[i];RGBA tex,ras;for(auto& x:tex)x=normal(rng);for(auto& x:ras)x=normal(rng);
        tex=swapped(tex,d.tev.swapTable[s.texSwap]);ras=swapped(ras,d.tev.swapTable[s.rasSwap]);
        const unsigned ca[]{unsigned(s.color.a),unsigned(s.color.b),unsigned(s.color.c),unsigned(s.color.d)};
        const unsigned aa[]{unsigned(s.alpha.a),unsigned(s.alpha.b),unsigned(s.alpha.c),unsigned(s.alpha.d)};
        RGBA expected{},actual{};
        for(unsigned lane=0;lane<4;++lane) {
          auto& v=vars[lane];v["texc.rgb"]=tex[lane];v["texc.a"]=tex[3];v["rasc.rgb"]=ras[lane];v["rasc.a"]=ras[3];
          std::array<double,4> arg;
          for(unsigned j=0;j<4;++j)arg[j]=lane<3?color(ca[j],lane,s,regs,cn,an,tex,ras,kc,j!=3):alpha(aa[j],s,regs,an,tex,ras,kc,j!=3);
          expected[lane]=reference(arg,lane<3?s.colorOp:s.alphaOp,lane<3?s.colorBias:s.alphaBias,lane<3?s.colorScale:s.alphaScale,lane<3?s.colorClamp:s.alphaClamp);
          const std::string name=(lane<3?"material_c":"material_a")+std::to_string(i);
          const auto expr=rhs(cg.fragment,name);actual[lane]=Expression{expr,v}.value();
          CHECK(std::abs(actual[lane]-expected[lane])<1e-10);v[name]=actual[lane];
        }
        // A later RGB stage may read the alpha result of any earlier stage.
        for(auto& v:vars)v["material_a"+std::to_string(i)]=actual[3];
        for(unsigned lane=0;lane<3;++lane)regs[unsigned(s.colorOut)][lane]=expected[lane];
        regs[unsigned(s.alphaOut)][3]=expected[3];cn[unsigned(s.colorOut)]=s.colorClamp;an[unsigned(s.alphaOut)]=s.alphaClamp;
      }
    }
  }
  PipelineDesc d;d.layout=gpu_vertex_layout();d.tev.stages[0].colorOp=TevOp::CompRGB8Greater;
  CHECK(!gxm::build_material_cg(d).nativeMaterial);
  CHECK(gxm::build_material_cg(d).fragment==gxm::build_tev_cg(d).fragment);
  d.tev.stages[0].colorOp=TevOp::Add;d.tev.stages[0].indirectEnabled=true;
  CHECK(!gxm::native_material_supported(d));
}
static void manifest(const char* path) {
  struct Header{uint32_t magic,version,size,count;};struct Record{uint64_t key,hits;PipelineDesc desc;};
  std::ifstream f(path,std::ios::binary);Header h{};f.read(reinterpret_cast<char*>(&h),sizeof h);
  CHECK(f&&h.magic==0x48505641&&h.version==1&&h.size==sizeof(PipelineDesc)&&h.count<=2048);
  uint64_t hits=0,nativeHits=0,oldBytes=0,newBytes=0;unsigned supported=0;
  for(unsigned i=0;i<h.count;++i){Record r{};f.read(reinterpret_cast<char*>(&r),sizeof r);CHECK(f);
    const auto old=gxm::build_tev_cg(r.desc),now=gxm::build_material_cg(r.desc);CHECK(old.ok()&&now.ok());
    hits+=r.hits;oldBytes+=old.fragment.size();newBytes+=now.fragment.size();
    if(now.nativeMaterial){++supported;nativeHits+=r.hits;}
  }
  std::printf("manifest pipelines=%u native=%u hits=%llu native_hits=%llu coverage=%.3f%% cg_bytes=%llu->%llu\n",h.count,supported,
      (unsigned long long)hits,(unsigned long long)nativeHits,hits?100.*nativeHits/hits:0.,(unsigned long long)oldBytes,(unsigned long long)newBytes);
}
int main(int argc,char** argv){equivalence();if(argc>1)manifest(argv[1]);std::printf("native material equivalence: %zu checks\n",checks);}
