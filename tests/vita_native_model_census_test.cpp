#include "../lib/gx/native_model_census.hpp"
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace aurora::gx::fifo;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

ModelAdmissionFacts candidate(){
  ModelAdmissionFacts f{};f.packet=true;f.flags=0x800;f.view=f.previousView=16;
  f.compiled=true;f.hasStreams=true;f.displayList=true;f.allowedProgram=f.allowedView=true;
  f.vertices=48;f.streams=2;f.listBytes=64;f.ownedMatrix=true;f.position=true;
  return f;
}
int main(){
  auto f=candidate();CHECK(model_admission_rejections(f)==0);
  // Independent oracle: a textured, lit Characters packet must report all
  // simultaneous exclusions, not only whichever the production gate checks first.
  f.view=f.previousView=11;f.allowedView=f.allowedProgram=false;
  f.texconfig=1;f.textureMask=1;f.validStreamSemantics=false;f.streamMask=(1u<<0)|(1u<<1)|(1u<<3);
  const uint32_t expected=model_reject_bit(ModelReject::View)|model_reject_bit(ModelReject::Program)|
      model_reject_bit(ModelReject::TexConfig)|model_reject_bit(ModelReject::TextureHandles)|
      model_reject_bit(ModelReject::StreamSemantic);
  CHECK(model_admission_rejections(f)==expected);
  ModelAdmissionCensus census;census.observe(f,false);auto s=census.snapshot();
  CHECK(s.drawCallbacks==1&&s.filterPassed==0&&s.transportReady==1&&s.eligible==0&&s.mismatches==0);
  CHECK(s.rejects[size_t(ModelReject::View)]==1&&s.rejects[size_t(ModelReject::TextureHandles)]==1);
  CHECK(census.evidence().views[11].facts.streamMask==11);
  // Transport rejection is independent of an otherwise admitted material.
  f=candidate();f.transportRejects=model_reject_bit(ModelReject::WorkerContext);
  census.observe(f,true);s=census.snapshot();CHECK(s.filterPassed==1&&s.eligible==0&&s.mismatches==0);
  CHECK(s.rejects[size_t(ModelReject::WorkerContext)]==1);
  // Draw and stream bounds retain their own causes and never invent inspection
  // results for an absent/oversized array. State-only callbacks are excluded.
  f=candidate();f.streams=9;f.validStreamStorage=false;f.position=false;
  CHECK(model_admission_rejections(f)==model_reject_bit(ModelReject::StreamCount));
  f=candidate();f.listBytes=256u*1024u;CHECK(model_admission_rejections(f)==0);
  ++f.listBytes;CHECK(model_admission_rejections(f)==model_reject_bit(ModelReject::DisplayListSize));
  f=candidate();f.flags=0;census.observe(f,false);CHECK(census.snapshot().callbacks==3&&census.snapshot().drawCallbacks==2);
  // A disagreement with the original gate is surfaced, not silently accepted.
  f=candidate();census.observe(f,false);CHECK(census.snapshot().mismatches==1);
  // First examples remain immutable; counters grow and concurrent snapshots
  // describe one coherent observation boundary.
  ModelAdmissionCensus concurrent;
  const auto write=[&](unsigned view){auto draw=candidate();draw.view=draw.previousView=view;
    for(unsigned i=0;i<2000;++i){draw.frame=i;concurrent.observe(draw,true);}};
  std::thread first(write,16),second(write,26);
  for(unsigned i=0;i<100;++i){const auto sample=concurrent.snapshot();
    CHECK(sample.drawCallbacks==sample.filterPassed&&sample.drawCallbacks==sample.eligible);
    CHECK(sample.views[16]+sample.views[26]==sample.drawCallbacks);
  }
  first.join();second.join();s=concurrent.snapshot();
  CHECK(s.callbacks==4000&&s.mismatches==0&&s.views[16]==2000&&s.views[26]==2000);
  CHECK(concurrent.evidence().views[16].facts.frame==0);
  concurrent.event(ModelStage::BeginRejected);CHECK(concurrent.snapshot().stages[size_t(ModelStage::BeginRejected)]==1);
  puts("native model census: overlapping reasons, transport context, bounds, original-gate mismatch and coherent concurrent snapshots");
}
