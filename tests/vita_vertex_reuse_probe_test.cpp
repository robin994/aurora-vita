#include "gfx/vita_draw_adapter.hpp"
#include "gfx/vita_vertex_reuse_probe.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace aurora::vita::gfx;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
int main(){
  VertexReuseProbe probe;CHECK(probe.initialize());
  constexpr auto pn=vertex_semantic_bit(VertexSemantic::Position)|vertex_semantic_bit(VertexSemantic::Normal);
  CanonicalVertex v{};v.position[0]=4;v.normal[1]=2;
  std::array<uint8_t,sizeof v> before{};std::memcpy(before.data(),&v,sizeof v);
  const auto original=VertexReuseProbe::fingerprint(v);
  CHECK(std::memcmp(before.data(),&v,sizeof v)==0);
  v.color0[0]=12;
  const auto material=VertexReuseProbe::fingerprint(v);
  CHECK(original.full!=material.full&&original.positionNormal==material.positionNormal);
  v.position[0]=5;CHECK(VertexReuseProbe::fingerprint(v).positionNormal!=material.positionNormal);
  probe.begin_frame(1);CHECK(!probe.prepare(3));
  probe.begin_frame(16);auto* hashes=probe.prepare(3);CHECK(hashes);
  for(unsigned i=0;i<3;++i)hashes[i]=original;
  probe.observe(3,pn);probe.observe(3,pn);
  CHECK(probe.stats().positionNormalCrossDrawVertices==3); // none within the first draw
  CHECK(probe.stats().fullHits==1&&probe.stats().fullHitVertices==3);
  CHECK(probe.stats().positionNormalHits==1);
  for(unsigned i=0;i<3;++i)hashes[i]=material;
  probe.observe(3,pn);CHECK(probe.stats().fullHits==1&&probe.stats().positionNormalHits==2);
  CHECK(probe.stats().positionNormalCrossDrawVertices==6);
  probe.observe(3,vertex_semantic_bit(VertexSemantic::Position));
  CHECK(probe.stats().positionNormalDraws==3); // no false default-normal observation
  probe.begin_frame(32);hashes=probe.prepare(3);for(unsigned i=0;i<3;++i)hashes[i]=original;
  probe.observe(3,pn);CHECK(probe.stats().fullHits==1); // no cross-frame reuse
  CHECK(!probe.prepare(VertexReuseProbe::MaxVertices+1));CHECK(probe.stats().skippedDraws==1);
  probe.begin_frame(48);
  for(unsigned n=0;n<VertexReuseProbe::TableSize+1;++n){
    hashes=probe.prepare(1);hashes[0]={n+1,n+1};probe.observe(1,pn);
  }
  CHECK(probe.stats().tableOverflows==2); // both bounded tables overflow safely

  // Integration: probing sees data BEFORE live matrices and preserves stream
  // allocation/counts. Fingerprinting itself does not mutate its input.
  Renderer renderer;CHECK(renderer.initialize());
  StreamingArena arena(renderer.buffers());CHECK(arena.initialize());arena.begin_frame(0);
  std::array<float,9> raw{1,2,3,4,5,6,7,8,9};
  VertexDecodeLayout layout{};layout.count=1;layout.streamStride=12;layout.streamLittleEndian=true;
  layout.attributes[0]={VertexSemantic::Position,VertexSource::Direct,VertexComponent::F32,3};
  compile_vertex_decode_layout(layout);
  PipelineDesc p{};VertexTransformState state{};DrawUniforms uniforms{};StreamedDraw reference{},measured{},moved{};
  VertexReuseProbe integrated;CHECK(integrated.initialize());integrated.begin_frame(16);
  auto draw=[&](StreamedDraw& out,VertexReuseProbe* census){
    CHECK(prepare_streamed_draw_into(out,arena,reinterpret_cast<const uint8_t*>(raw.data()),sizeof raw,3,
        SourcePrimitive::Triangles,nullptr,0,layout,p,state,&uniforms,nullptr,nullptr,census));
  };
  draw(reference,nullptr);draw(measured,&integrated);CHECK(arena.flush());
  CHECK(reference.vertices.size==measured.vertices.size);
  CHECK(reference.indices.size==measured.indices.size);
  CHECK(reference.vertexCount==measured.vertexCount&&reference.indexCount==measured.indexCount);
  state.postexMatrices[0].v[3]=9;draw(moved,&integrated);
  CHECK(integrated.stats().fullHits==1);
  raw[0]+=1;draw(moved,&integrated);CHECK(integrated.stats().fullHits==1);
  std::puts("vertex reuse probe: non-mutating fingerprints, live pose, matrix-independent census, normal admission and frame/table bounds");
}
