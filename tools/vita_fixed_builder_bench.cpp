// Portable Cortex-A9/AArch32 microbenchmark. Linux ELF, not a Vita SELF.
// Run the same two real production kernels with identical inputs/compiler.
#include "gfx/vita_fixed_vertex.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace aurora::vita::gfx;
static volatile float checksum;
static void consume(const FixedVertexUniforms& v) {
#if defined(__GNUC__) || defined(__clang__)
  // Preserve the complete externally consumed payload, like DrawSink's snapshot
  // publication. Sampling a single field alone would allow dead-store removal.
  asm volatile("" : : "r"(&v) : "memory");
#endif
  checksum=v.position[3]+v.texture[0][7]+v.material[0][0];
}
static double run(bool optimized,size_t iterations) {
  PipelineDesc p;p.fixedVertexOnGpu=true;p.fixedVertexIndexedPn=true;p.fixedVertexTexMtxMask=3;
  p.texgenCount=2;p.tev.stageCount=2;
  for(unsigned i=0;i<2;++i){p.tev.stages[i].color.d=TevColorArg::TexColor;p.tev.stages[i].texture=i;p.tev.stages[i].texCoord=i;p.texgens[i].matrix=i;}
  p.tev.stages[1].alpha.d=TevAlphaArg::RasAlpha;
  VertexTransformState state;FixedVertexUniforms reference;FixedVertexUniformBuilder builder;
  state.channelMaterial[0]={.25f,.5f,.75f,1.f};
  const auto start=std::chrono::steady_clock::now();
  for(size_t i=0;i<iterations;++i){state.currentPnMatrix=i%10;state.postexMatrices[i%10].v[3]=float(i%65536)*.01f;
    if(optimized)consume(builder.build(p,state));
    else{fixed_vertex_uniforms_into(reference,p,state);consume(reference);}
  }
  return std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/iterations;
}
int main(int argc,char** argv){
  const size_t n=argc>1?std::strtoul(argv[1],nullptr,10):200000;
  if(!n||n>100000000)return 2;
  std::vector<double> ref,opt;
  run(false,1000);run(true,1000);
  for(unsigned i=0;i<9;++i){if(i&1){opt.push_back(run(true,n));ref.push_back(run(false,n));}else{ref.push_back(run(false,n));opt.push_back(run(true,n));}}
  std::sort(ref.begin(),ref.end());std::sort(opt.begin(),opt.end());
#if defined(__arm__)
  const char* arch="AArch32";
#elif defined(__aarch64__)
  const char* arch="AArch64";
#else
  const char* arch="other";
#endif
  std::printf("{\"architecture\":\"%s\",\"compiler\":\"%s\",\"pointer_bits\":%u,\"iterations\":%lu,\"trials\":9,\"reference_median_ns\":%.3f,\"candidate_median_ns\":%.3f,\"kernel_speedup\":%.4f}\n",
      arch,__VERSION__,unsigned(sizeof(void*)*8),(unsigned long)n,ref[4],opt[4],ref[4]/opt[4]);
}
