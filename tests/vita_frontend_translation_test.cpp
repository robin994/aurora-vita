#include "../lib/gx/gx.hpp"
#include "gx/aurora_gx_bridge.hpp"
#include "gx/aurora_vita_draw_sink.hpp"
#include "aurora_vita_backend.hpp"
#include "../lib/gx/command_processor.hpp"
#include "../lib/gx/fifo.hpp"
#include <array>
#include <bit>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include "../lib/gx/display_list_shadow.hpp"
#include "gfx/vita_view_draw_capture.hpp"
#include "gfx/vita_fixed_vertex.hpp"
#include <cstring>
#include <vector>
#include <dolphin/vi.h>

extern "C" void aurora_vita_notify_memory_write(const void* address,size_t bytes) noexcept;
extern "C" void aurora_vita_prepare_memory_write(const void* address,size_t bytes) noexcept;

namespace {
using namespace aurora::vita;
unsigned failures=0,checks=0;
#undef CHECK
#define CHECK(x) do {++checks;if(!(x)){++failures;std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);}} while(0)
void pooled_snapshot_transitions() {
  using gfx::FixedUniformPool;
  for(size_t budget:{size_t(0),sizeof(gfx::FixedVertexUniforms),FixedUniformPool::MaxRetainedBytes}) {
    FixedUniformPool pool;pool.configure(budget);
    pool.scratch().position[0]=11;
    auto* first=&pool.publish();const auto firstRevision=first->revision;
    const auto counters=pool.stats();
    for(unsigned i=0;i<10000;++i)CHECK(&pool.publish()==first);
    CHECK(pool.size()==1);CHECK(first->position[0]==11);
    CHECK(pool.stats().allocations==counters.allocations&&pool.stats().reuses==counters.reuses&&pool.stats().fallbacks==counters.fallbacks);
    pool.scratch().position[0]=12;auto* next=&pool.publish();
    CHECK(next!=first&&next->revision>firstRevision&&first->position[0]==11);
    pool.scratch().normalPalette[3][4]=22;auto* palette=&pool.publish();CHECK(palette!=next);
    pool.scratch().texture[1][2]=33;auto* texture=&pool.publish();CHECK(texture!=palette);
    pool.scratch().light[0][0]=44;auto* light=&pool.publish();CHECK(light!=texture);
    CHECK(&pool.publish(false)!=light);
    auto* sprite=&pool.publish(true,true);CHECK(&pool.publish()!=sprite);
    const auto revision=pool.publish().revision;
    pool.reset();CHECK(pool.size()==0);CHECK(pool.publish().revision>revision);
    pool.clear();CHECK(pool.stats().retainedBytes==0);CHECK(pool.publish().revision>revision);
  }
}
void layouts() {
  auto& g=aurora::gx::g_gxState;
  std::array<uint8_t,512> array{};
  for(auto source:{GX_DIRECT,GX_INDEX8,GX_INDEX16})
    for(auto normal:{GX_NRM_XYZ,GX_NRM_NBT,GX_NRM_NBT3})
      for(auto component:{GX_S8,GX_S16,GX_F32})for(unsigned fmt=0;fmt<8;++fmt){
        g=aurora::gx::GXState{};
        g.vtxDesc[GX_VA_POS]=source;g.vtxDesc[GX_VA_NRM]=source;g.vtxDesc[GX_VA_TEX0]=source;
        g.vtxFmts[fmt].attrs[GX_VA_POS]={GX_POS_XYZ,component,3};
        g.vtxFmts[fmt].attrs[GX_VA_NRM]={normal,component,0};
        g.vtxFmts[fmt].attrs[GX_VA_TEX0]={GX_TEX_ST,component,2};
        for(auto attr:{GX_VA_POS,GX_VA_NRM,GX_VA_TEX0})g.arrays[attr]={array.data(),array.size(),32,false};
        gfx::PipelineDesc pipeline{};gfx::VertexDecodeLayout reference{};uint64_t key=0;
        gxbridge::translate_current_pipeline_and_layout(GX_TRIANGLES,fmt,pipeline,reference,key);
        const auto narrow=gxbridge::translate_current_vertex_layout(fmt);
        CHECK(!std::memcmp(&narrow,&reference,sizeof(reference)));
        // Array rebinding must affect layout while leaving TEV fields irrelevant.
        g.arrays[GX_VA_POS].stride=48;g.arrays[GX_VA_POS].le=true;
        const auto rebound=gxbridge::translate_current_vertex_layout(fmt);
        CHECK(source==GX_DIRECT||rebound.attributes[0].array.stride==48);
        CHECK(source==GX_DIRECT||rebound.attributes[0].array.littleEndian);
      }
}
void fragment_updates() {
  auto& g=aurora::gx::g_gxState;g=aurora::gx::GXState{};
  g.proj.m0={1,0,0,0};g.proj.m1={0,1,0,0};g.proj.m2={0,0,1,0};g.proj.m3={0,0,0,1};
  g.renderViewport={0,0,960,544,0,1};g.logicalViewport=g.renderViewport;
  gfx::PipelineDesc pipeline{};pipeline.fogMode=gfx::FogMode::Linear;pipeline.fogRangeEnabled=true;
  pipeline.tev.indirectStageCount=1;
  gfx::VertexDecodeLayout layout{};gfx::VertexTransformState state{};
  gfx::DrawUniforms reference{},split{};
  gxbridge::translate_vertex_state(state,reference,pipeline,layout);
  gxbridge::translate_vertex_state(state,split,pipeline,layout,false);
  gxbridge::translate_fragment_uniforms(split,pipeline);
  CHECK(!std::memcmp(&split,&reference,sizeof(reference)));
  g.colorRegs[0]={0.25f,0.5f,0.75f,1.f};g.fog.a=2.f;g.fog.color={0.1f,0.2f,0.3f,1.f};
  gxbridge::translate_vertex_state(state,reference,pipeline,layout);
  gxbridge::translate_fragment_uniforms(split,pipeline);
  CHECK(!std::memcmp(&split,&reference,sizeof(reference)));
}

// A1a: exercise the *real* decoder with frozen, big-endian GX XF commands.
// The expected 32-bit words are independently specified, rather than rebuilt
// through the candidate or through translate_current_pipeline_and_layout().
void encoded_xf_matrix_oracles() {
  auto& g=aurora::gx::g_gxState;
  g=aurora::gx::GXState{};
  const auto emit=[](uint16_t address,const std::vector<uint32_t>& words) {
    std::vector<uint8_t> fifo;
    const auto write32=[&](uint32_t value){
      fifo.push_back(static_cast<uint8_t>(value>>24));
      fifo.push_back(static_cast<uint8_t>(value>>16));
      fifo.push_back(static_cast<uint8_t>(value>>8));
      fifo.push_back(static_cast<uint8_t>(value));
    };
    fifo.push_back(0x10); // LOAD_XF_REG, not a helper that skips the decoder.
    write32((static_cast<uint32_t>(words.size()-1)<<16)|address);
    for(uint32_t value:words)write32(value);
    aurora::gx::fifo::process_sync(fifo.data(),static_cast<uint32_t>(fifo.size()),true);
  };
  const auto bits=[](float f){return std::bit_cast<uint32_t>(f);};
  const auto readBits=[](const float& value){uint32_t u=0;std::memcpy(&u,&value,sizeof(u));return u;};

  std::vector<uint32_t> pose(12);
  for(unsigned i=0;i<12;++i)pose[i]=bits(static_cast<float>(i+1));
  emit(0x0000,pose);
  for(unsigned i=0;i<12;++i)CHECK(readBits(reinterpret_cast<const float*>(&g.pnMtx[0].pos)[i])==pose[i]);
  const auto firstRevision=g.stateRevisions.vertex;
  emit(0x0000,pose); // repeated bytes must never change the decoded matrix.
  CHECK(g.stateRevisions.vertex>=firstRevision);
  for(unsigned i=0;i<12;++i)CHECK(readBits(reinterpret_cast<const float*>(&g.pnMtx[0].pos)[i])==pose[i]);

  // A change to one element of an indexed-PN palette must remain visible.
  pose[11]=bits(-57.5f);
  emit(0x0000,pose);
  CHECK(readBits(g.pnMtx[0].pos.m2[3])==pose[11]);
  // -0 and +0 compare numerically equal, but must *not* be merged bitwise.
  pose[1]=0x80000000u;
  emit(0x0000,pose);
  CHECK(readBits(g.pnMtx[0].pos.m0[1])==0x80000000u);
  // Two NaNs with distinct payloads must not be silently collapsed either.
  pose[2]=0x7fc12345u;emit(0x0000,pose);
  CHECK(readBits(g.pnMtx[0].pos.m0[2])==0x7fc12345u);
  pose[2]=0x7fc54321u;emit(0x0000,pose);
  CHECK(readBits(g.pnMtx[0].pos.m0[2])==0x7fc54321u);

  // Normal XF uses 3x3 words packed into three rows of a 3x4 CPU matrix.
  std::vector<uint32_t> normal(9);
  for(unsigned i=0;i<9;++i)normal[i]=bits(static_cast<float>(31+i));
  emit(0x0400,normal);
  const float* nrm=reinterpret_cast<const float*>(&g.pnMtx[0].nrm);
  for(unsigned i=0;i<9;++i)CHECK(readBits(nrm[(i/3)*4+i%3])==normal[i]);
  CHECK(readBits(nrm[3])==0&&readBits(nrm[7])==0);

  // Texture and post-texture palettes must keep their independent indices.
  emit(0x0078,pose);
  CHECK(readBits(reinterpret_cast<const float*>(&g.texMtxs[0])[11])==pose[11]);
  emit(0x0500,pose);
  CHECK(readBits(reinterpret_cast<const float*>(&g.ptTexMtxs[0])[1])==pose[1]);
  CHECK(readBits(reinterpret_cast<const float*>(&g.texMtxs[0])[1])==pose[1]);

  // Replacing GXState must not inherit a prior matrix identity or revision.
  const auto identity=g.stateIdentity;
  g=aurora::gx::GXState{};
  CHECK(g.stateIdentity!=identity);
  emit(0x0000,pose);
  CHECK(readBits(g.pnMtx[0].pos.m0[2])==pose[2]);
}


// A2a acceptance oracle: independently replay identical *encoded* XF streams
// with optimization OFF and ON, comparing 10 complete PN matrices after every
// decoded command. Revision counts may differ; no value may differ.
void xf_equal_position_writes_oracle() {
  using aurora::gx::fifo::set_xf_equal_position_writes;
  using aurora::gx::fifo::xf_equal_position_stats;
  struct Result {
    std::vector<std::array<uint32_t,120>> snapshots;
    uint64_t finalVertexRevision=0;
    aurora::gx::fifo::XfEqualPositionStats counters{};
  };
  const auto run=[](bool candidate) {
    Result result;
    auto& g=aurora::gx::g_gxState;
    g=aurora::gx::GXState{};
    set_xf_equal_position_writes(candidate);
    const auto snapshot=[&] {
      std::array<uint32_t,120> values{};
      for(unsigned m=0;m<10;++m)
        for(unsigned i=0;i<12;++i) {
          const float* flat=reinterpret_cast<const float*>(&g.pnMtx[m].pos);
          std::memcpy(&values[m*12+i],&flat[i],sizeof(uint32_t));
        }
      result.snapshots.push_back(values);
    };
    const auto emit=[&](unsigned matrix,uint32_t changedWord,unsigned changedIndex) {
      std::array<uint8_t,53> raw{};
      const auto put=[&](unsigned offset,uint32_t word) {
        raw[offset]=static_cast<uint8_t>(word>>24);
        raw[offset+1]=static_cast<uint8_t>(word>>16);
        raw[offset+2]=static_cast<uint8_t>(word>>8);
        raw[offset+3]=static_cast<uint8_t>(word);
      };
      raw[0]=0x10;put(1,(11u<<16)|(matrix*12));
      for(unsigned i=0;i<12;++i)
        put(5+i*4,i==changedIndex?changedWord:0x3f800000u+(matrix<<15)+(i<<11));
      aurora::gx::fifo::process_sync(raw.data(),static_cast<uint32_t>(raw.size()),true);
      snapshot();
    };
    for(unsigned m=0;m<10;++m)emit(m,0,99); // Fresh indexed-PN palette.
    for(unsigned m=0;m<10;++m)emit(m,0,99); // Exactly identical writes.
    emit(7,0x80000000u,2); // Signed zero changes bits.
    emit(7,0x80000000u,2); // Same signed zero must be elided.
    emit(7,0x7fc12345u,2); // Distinct NaN payloads must remain observable.
    emit(7,0x7fc54321u,2);
    result.counters=xf_equal_position_stats();
    result.finalVertexRevision=g.stateRevisions.vertex;
    return result;
  };
  const auto reference=run(false);
  const auto candidate=run(true);
  CHECK(candidate.snapshots.size()==reference.snapshots.size());
  CHECK(candidate.snapshots==reference.snapshots); // All 24 canonical checkpoints.
  CHECK(reference.finalVertexRevision>candidate.finalVertexRevision);
  CHECK(candidate.counters.inspected==24);
  CHECK(candidate.counters.unchanged==11);
  CHECK(candidate.counters.changed==13);
  // Verify that the optimization can be disabled again without cached residue.
  set_xf_equal_position_writes(false);
  CHECK(xf_equal_position_stats().inspected==0);
}

// A5: send real big-endian XF writes through the command processor, including
// 3x3 normal packing, 2x4 texture matrices, distinct NaN payloads and -0.
// Check every destination word after every command, not only the final pose.
void xf_equal_matrix_writes_oracle() {
  using aurora::gx::fifo::set_xf_equal_matrix_writes;
  using aurora::gx::fifo::xf_equal_matrix_stats;
  struct Result {
    std::vector<std::array<uint32_t,12*4>> snapshots;
    uint64_t vertexRevision=0;
    aurora::gx::fifo::XfEqualMatrixStats counters{};
  };
  const auto replay=[](bool enable) {
    auto& g=aurora::gx::g_gxState;g=aurora::gx::GXState{};
    set_xf_equal_matrix_writes(enable);
    Result out{};
    auto write=[&](unsigned address,const std::vector<uint32_t>& payload) {
      std::vector<uint8_t> fifo;
      const auto word=[&](uint32_t value){
        fifo.push_back(uint8_t(value>>24));fifo.push_back(uint8_t(value>>16));
        fifo.push_back(uint8_t(value>>8));fifo.push_back(uint8_t(value));
      };
      fifo.push_back(0x10);
      word((uint32_t(payload.size()-1)<<16)|address);
      for(auto value:payload)word(value);
      aurora::gx::fifo::process_sync(fifo.data(),uint32_t(fifo.size()),true);
      std::array<uint32_t,12*4> snapshot{};
      const auto capture=[&](unsigned offset,const auto& matrix){
        const float* floats=reinterpret_cast<const float*>(&matrix);
        for(unsigned i=0;i<12;++i)std::memcpy(&snapshot[offset+i],floats+i,4);
      };
      capture(0,g.texMtxs[0]);capture(12,g.texMtxs[1]);
      capture(24,g.pnMtx[0].nrm);capture(36,g.ptTexMtxs[0]);
      out.snapshots.push_back(snapshot);
    };
    std::vector<uint32_t> tex(12),normal(9);
    for(unsigned i=0;i<12;++i)tex[i]=std::bit_cast<uint32_t>(float(i+1));
    for(unsigned i=0;i<9;++i)normal[i]=std::bit_cast<uint32_t>(float(i+33));
    write(0x78,tex);write(0x78,tex); // exact matrix reuse
    tex[1]=0x80000000u;write(0x78,tex);write(0x78,tex); // -0 != +0
    tex[1]=0x7fc12345u;write(0x78,tex); // NaN payload preserved
    tex[1]=0x7fc54321u;write(0x78,tex);
    write(0x84,std::vector<uint32_t>(tex.begin(),tex.begin()+8));
    write(0x84,std::vector<uint32_t>(tex.begin(),tex.begin()+8)); // 2x4
    write(0x400,normal);write(0x400,normal); // packed normal
    normal[4]=0x80000000u;write(0x400,normal);write(0x400,normal);
    normal[4]=0x7fc01234u;write(0x400,normal);
    normal[4]=0x7fc56789u;write(0x400,normal);
    write(0x500,tex);write(0x500,tex);
    tex[10]=0x80000000u;write(0x500,tex);write(0x500,tex);
    out.vertexRevision=g.stateRevisions.vertex;
    out.counters=xf_equal_matrix_stats();
    return out;
  };
  const auto reference=replay(false),candidate=replay(true);
  CHECK(candidate.snapshots==reference.snapshots);
  CHECK(reference.vertexRevision>candidate.vertexRevision);
  CHECK(candidate.counters.texInspected==8 && candidate.counters.texUnchanged==3);
  CHECK(candidate.counters.normalInspected==6 && candidate.counters.normalUnchanged==2);
  CHECK(candidate.counters.postInspected==4 && candidate.counters.postUnchanged==2);
  CHECK(candidate.counters.texChanged==5 && candidate.counters.normalChanged==4 &&
        candidate.counters.postChanged==2);
  set_xf_equal_matrix_writes(false);
  CHECK(xf_equal_matrix_stats().texInspected==0);
}

// A5: true GX BP streams changing inactive and subsequently active TEV stages.
// Only dirtiness revisions may differ; decoded material and order must not.
void tev_decoded_write_gate_oracle() {
  using aurora::gx::fifo::set_tev_decoded_write_gate;
  using aurora::gx::fifo::tev_decoded_write_stats;
  struct Result {
    std::vector<std::array<uint8_t,sizeof(aurora::gx::g_gxState.tevStages)>> snapshots;
    std::vector<uint8_t> stageCounts;
    std::vector<uint64_t> nativePipelineKeys;
    uint64_t vertexRevision=0;
    aurora::gx::fifo::TevDecodedWriteStats counters{};
  };
  const auto replay=[](bool enabled){
    auto& g=aurora::gx::g_gxState;g=aurora::gx::GXState{};
    g.numTevStages=1;
    set_tev_decoded_write_gate(enabled);
    Result out{};
    const auto emit=[&](uint32_t bp){
      std::array<uint8_t,5> fifo={0x61,uint8_t(bp>>24),uint8_t(bp>>16),uint8_t(bp>>8),uint8_t(bp)};
      aurora::gx::fifo::process_sync(fifo.data(),uint32_t(fifo.size()),true);
      std::array<uint8_t,sizeof(g.tevStages)> stage{};
      std::memcpy(stage.data(),g.tevStages.data(),sizeof(g.tevStages));
      out.snapshots.push_back(stage);
      out.stageCounts.push_back(g.numTevStages);
      // The Cg/GXM pipeline must remain identical across reference and
      // candidate, including inactive-to-active TEV transitions.
      gfx::PipelineDesc translated{};gfx::VertexDecodeLayout decode{};
      uint64_t key=0;
      gxbridge::translate_current_pipeline_and_layout(GX_TRIANGLES,0,translated,decode,key);
      out.nativePipelineKeys.push_back(key);
    };
    emit(0xc6000123u); // stage 3 RGB, initially inactive
    emit(0xc7000901u); // stage 3 alpha, initially inactive
    emit(0xc0000234u); // stage 0 RGB, active
    emit(0xc1001005u); // stage 0 alpha, active
    emit(0x28000637u); // TEV order for stages 0,1
    emit(0xc6000456u); // inactive, distinct hardware payload
    emit(0x00000c00u); // genMode activates 4 stages (0xC00 / 0x400+1)
    emit(0xc6000111u); // now active stage 3 RGB: MUST invalidate
    emit(0xc7000907u); // now active stage 3 alpha: MUST invalidate
    out.vertexRevision=g.stateRevisions.vertex;
    out.counters=tev_decoded_write_stats();
    return out;
  };
  const auto reference=replay(false),candidate=replay(true);
  CHECK(reference.stageCounts==candidate.stageCounts);
  CHECK(reference.snapshots==candidate.snapshots);
  CHECK(reference.nativePipelineKeys==candidate.nativePipelineKeys);
  CHECK(reference.vertexRevision>candidate.vertexRevision);
  CHECK(candidate.counters.inspected>=7);
  CHECK(candidate.counters.skipped>=2);
  CHECK(candidate.counters.changed>=2);
  set_tev_decoded_write_gate(false);
  CHECK(tev_decoded_write_stats().inspected==0);
}
// Compare dirty decisions against an independent, full decoded-state snapshot.
// This guards field-sized comparisons: inactive stages/texgens, redundant
// writes, BP masks and CP/XF aliases must retain the original decisions.
void decoded_snapshot_field_oracle() {
  using namespace aurora::gx;
  using namespace aurora::gx::fifo;
  auto& g=g_gxState;
  uint32_t random=0x53545249u;
  const auto next=[&]() {random^=random<<13;random^=random>>17;random^=random<<5;return random;};
  uint64_t digest=1469598103934665603ull;
  const auto hash=[&](const void* p,size_t n) {
    const auto* bytes=static_cast<const uint8_t*>(p);
    for(size_t i=0;i<n;++i){digest^=bytes[i];digest*=1099511628211ull;}
  };
  const auto emitBp=[](uint32_t word) {
    const uint8_t bytes[]={0x61,uint8_t(word>>24),uint8_t(word>>16),uint8_t(word>>8),uint8_t(word)};
    process_sync(bytes,sizeof(bytes),true);
  };
  for(bool gate:{false,true}) {
    set_tev_decoded_write_gate(gate);
    for(unsigned count=1;count<=16;++count) {
      g=GXState{};g.numTevStages=count;
      for(unsigned iteration=0;iteration<512;++iteration) {
        const unsigned kind=iteration%3;
        const unsigned stage=next()%16;
        const unsigned reg=kind==2?0x28+stage/2:0xc0+stage*2+kind;
        // Alternating mask writes exercise cached-register merging as well.
        if(iteration%17==0)emitBp(0xfe00ffffu);
        const auto before=g.tevStages;
        const auto generation=g.pipelineStateGeneration;
        const uint32_t word=(reg<<24)|(next()&0x00ffffffu);
        const uint32_t mask=g.bpRegCache[0xfe];
        const uint32_t merged=(g.bpRegCache[reg]&~mask)|(word&mask);
        const bool decoded=g.bpRegCache[reg]!=((reg<<24)|(merged&0x00ffffffu));
        g.stateDirty=false;
        emitBp(word);
        bool activeChanged=false;
        for(unsigned i=0;i<count;++i)
          activeChanged=activeChanged||std::memcmp(&before[i],&g.tevStages[i],sizeof(before[i]))!=0;
        CHECK((g.pipelineStateGeneration!=generation)==activeChanged);
        CHECK(g.stateDirty==(gate?activeChanged:decoded));
        hash(g.tevStages.data(),sizeof(g.tevStages));
        hash(&activeChanged,sizeof(activeChanged));
        // Exact repeated register payload must retain its cache no-op.
        const auto repeatedGeneration=g.pipelineStateGeneration;
        g.stateDirty=false;emitBp(word);
        // A preceding partial mask is single-use, so the second write may
        // still change the remaining bits; only unmasked repeats are no-ops.
        if(mask==0x00ffffffu) {
          CHECK(g.pipelineStateGeneration==repeatedGeneration&&!g.stateDirty);
        }
      }
    }
  }
  set_tev_decoded_write_gate(false);
  for(unsigned active=0;active<=8;++active) {
    g=GXState{};g.numTexGens=active;
    for(unsigned iteration=0;iteration<512;++iteration) {
      const unsigned bank=iteration%4;
      const unsigned first=(bank&1)?4:0;
      const bool cp=bank<2;
      // Keep unrelated fields and padding nonzero: a narrow comparison must
      // not depend on how the rest of TcgConfig was initialized.
      for(unsigned i=0;i<8;++i){g.tcgs[i].postMtx=GX_PTTEXMTX0;g.tcgs[i]._p2=uint8_t(i+1);}
      const auto before=g.tcgs;
      const uint32_t oldPn=g.currentPnMtx;
      const auto generation=g.vertexProgramStateGeneration;
      const auto pipelineGeneration=g.pipelineStateGeneration;
      const uint64_t vertex=g.stateRevisions.vertex;
      uint32_t value=(bank&1)?0u:(next()%10)*3;
      for(unsigned i=0;i<4;++i)value|=(next()%61)<<((bank&1)?i*6:6+i*6);
      const uint16_t addr=(bank&1)?0x19:0x18;
      std::vector<uint8_t> bytes;
      const auto write32=[&](uint32_t v){for(int shift:{24,16,8,0})bytes.push_back(uint8_t(v>>shift));};
      if(cp){bytes={0x08,uint8_t((bank&1)?0x40:0x30)};write32(value);}
      else{bytes={0x10};write32(0x1000+addr);write32(value);g.invalidateXfReg(addr);}
      g.stateDirty=false;process_sync(bytes.data(),uint32_t(bytes.size()),true);
      bool activeChanged=false;
      for(unsigned i=first;i<first+4&&i<active;++i)activeChanged|=before[i].mtx!=g.tcgs[i].mtx;
      const bool changed=std::memcmp(before.data(),g.tcgs.data(),sizeof(before))!=0;
      CHECK((g.vertexProgramStateGeneration!=generation)==activeChanged);
      CHECK(g.pipelineStateGeneration==pipelineGeneration);
      CHECK((g.stateRevisions.vertex!=vertex)==(changed||oldPn!=g.currentPnMtx));
      for(unsigned i=0;i<8;++i) {
        auto expected=before[i];
        if(i>=first&&i<first+4)expected.mtx=g.tcgs[i].mtx;
        CHECK(std::memcmp(&expected,&g.tcgs[i],sizeof(expected))==0);
      }
      hash(g.tcgs.data(),sizeof(g.tcgs));hash(&g.currentPnMtx,sizeof(g.currentPnMtx));
      hash(&activeChanged,sizeof(activeChanged));
    }
  }
  // Frozen from the original full-snapshot decoder before the narrow patch.
  CHECK(digest==0xab552d3a579e1db2ull);
  std::printf("decoded snapshot corpus: digest=%016llx\n",static_cast<unsigned long long>(digest));
}
void state_transitions() {
  using aurora::gx::StateDomain;
  auto& g=aurora::gx::g_gxState;
  const auto reset=[] {
    auto& state=aurora::gx::g_gxState;state=aurora::gx::GXState{};
    state.proj.m0={1,0,0,0};state.proj.m1={0,1,0,0};
    state.proj.m2={0,0,1,0};state.proj.m3={0,0,0,1};
    state.pnMtx[0].pos.m0={1,0,0,0};state.pnMtx[0].pos.m1={0,1,0,0};state.pnMtx[0].pos.m2={0,0,1,0};
    state.renderViewport={0,0,960,544,0,1};state.logicalViewport=state.renderViewport;
    state.renderScissor={0,0,960,544};state.logicalScissor=state.renderScissor;
    state.vtxDesc[GX_VA_POS]=GX_DIRECT;state.vtxFmts[0].attrs[GX_VA_POS]={GX_POS_XYZ,GX_F32,0};
  };
  reset();
  std::array<uint8_t,36> raw{};
  const std::array<float,9> positions{0,0,0,1,0,0,0,1,0};
  for(unsigned i=0;i<positions.size();++i){const auto bits=std::bit_cast<uint32_t>(positions[i]);
    for(unsigned b=0;b<4;++b)raw[i*4+b]=uint8_t(bits>>(24-8*b));}
  gfx::Renderer renderer;CHECK(renderer.initialize());
  gfx::Telemetry telemetry;telemetry.begin_frame(0);
  gxbridge::DrawSink sink;gxbridge::DrawSinkConfig config{};config.telemetry=&telemetry;
  CHECK(sink.initialize(renderer,config));renderer.begin_frame();sink.begin_frame(0);
  const auto submit=[&] {auto result=sink.submit(GX_TRIANGLES,0,raw.data(),raw.size(),3);
    CHECK(result.ok);if(result.ok)g.stateDirty=false;};
  submit();const auto first=telemetry.frame().counters;
  const auto firstUniforms=sink.stream().tail_draw()->gpu_uniforms();
  g.renderScissor={12,13,90,91};g.mark_dirty(StateDomain::Raster);submit();
  CHECK(telemetry.frame().counters.vertexTranslations==first.vertexTranslations);
  CHECK(telemetry.frame().counters.fragmentTranslations==first.fragmentTranslations);
  CHECK(telemetry.frame().counters.textureResolves==first.textureResolves);
  CHECK(sink.stream().tail_draw()->scissor.x==12);
  CHECK(!std::memcmp(&firstUniforms,&sink.stream().tail_draw()->gpu_uniforms(),sizeof(firstUniforms)));
  g.colorRegs[0]={0.2f,0.4f,0.6f,0.8f};g.mark_dirty(StateDomain::Fragment);submit();
  CHECK(telemetry.frame().counters.vertexTranslations==first.vertexTranslations);
  CHECK(telemetry.frame().counters.fragmentTranslations==first.fragmentTranslations+1);
  CHECK(telemetry.frame().counters.textureResolves==first.textureResolves);
  CHECK(sink.stream().tail_draw()->gpu_uniforms().tevreg[0][0]==0.2f);
  g.proj.m0[0]=2;g.mark_dirty(StateDomain::Vertex);submit();
  CHECK(telemetry.frame().counters.vertexTranslations==first.vertexTranslations+1);
  CHECK(sink.stream().tail_draw()->gpu_uniforms().mvp[0]==2);
  CHECK(telemetry.frame().counters.textureResolves==first.textureResolves);
  const auto beforeClear=telemetry.frame().counters;
  g.clearDepth=100;g.mark_dirty(StateDomain::Clear);submit();
  CHECK(telemetry.frame().counters.vertexTranslations==beforeClear.vertexTranslations);
  CHECK(telemetry.frame().counters.fragmentTranslations==beforeClear.fragmentTranslations);
  CHECK(telemetry.frame().counters.textureResolves==beforeClear.textureResolves);
  g.arrays[GX_VA_POS].stride=20;g.layoutStateGeneration=aurora::gx::next_gx_state_epoch();
  g.mark_dirty(StateDomain::Layout);submit();
  CHECK(telemetry.frame().counters.layoutTranslations==first.layoutTranslations+1);
  CHECK(telemetry.frame().counters.pipelineTranslations==first.pipelineTranslations);
  CHECK(telemetry.frame().counters.vertexTranslations==beforeClear.vertexTranslations);
  auto before=telemetry.frame().counters;
  g.stateDirty=true;submit(); // Legacy untracked writer retains a broad rebuild.
  CHECK(telemetry.frame().counters.vertexTranslations==before.vertexTranslations+1);
  CHECK(telemetry.frame().counters.textureResolves==before.textureResolves+1);
  before=telemetry.frame().counters;const auto oldIdentity=g.stateIdentity;reset();submit();
  CHECK(g.stateIdentity!=oldIdentity);
  CHECK(telemetry.frame().counters.textureResolves==before.textureResolves+1);
  CHECK(sink.stream().tail_draw()->gpu_uniforms().mvp[0]==1);
  before=telemetry.frame().counters;
  gfx::gxm_disable_mask()=gfx::GxmDisableStateDomains;
  g.renderScissor.x=7;g.mark_dirty(StateDomain::Raster);submit();
  CHECK(telemetry.frame().counters.vertexTranslations==before.vertexTranslations+1);
  CHECK(telemetry.frame().counters.textureResolves==before.textureResolves+1);
  gfx::gxm_disable_mask()=0;
  for(unsigned mode=0;mode<6;++mode){
    g.numTevStages=1;g.numTexGens=1;
    auto& stage=g.tevStages[0];stage.channelId=GX_COLOR0A0;
    stage.colorPass.d=(mode&1)?GX_CC_TEXC:GX_CC_RASC;
    stage.texCoordId=(mode&1)?GX_TEXCOORD0:GX_TEXCOORD_NULL;
    stage.texMapId=(mode&1)?GX_TEXMAP0:GX_TEXMAP_NULL;
    g.pipelineStateGeneration=aurora::gx::next_gx_state_epoch();g.mark_dirty();
    const auto prior=telemetry.frame().counters.pipelineTranslations;
    const auto priorLayout=telemetry.frame().counters.layoutTranslations;submit();
    gfx::PipelineDesc expected{};gfx::VertexDecodeLayout decode{};uint64_t key=0;
    gxbridge::translate_current_pipeline_and_layout(GX_TRIANGLES,0,expected,decode,key);
    const auto recipe=gfx::build_draw_recipe(expected);
    CHECK(sink.stream().tail_draw()->vertices.size==3*recipe.gpuStride);
    CHECK(telemetry.frame().counters.pipelineTranslations==prior+1);
    CHECK(telemetry.frame().counters.layoutTranslations==priorLayout);
  }
  sink.flush();sink.shutdown();renderer.shutdown();
}
// Real BP commands, including masked writes, versus a full uniform translation.
// Cover lit/indexed-PN CPU and the host backend's unlit resident/streamed GPU
// contracts independently (lit GPU rendering itself requires Vita GXM).
void raster_pipeline_uniform_reuse_oracle() {
  using namespace aurora::gx;
  using namespace aurora::gx::fifo;
  for(bool enabled:{false,true})for(unsigned mode=0;mode<3;++mode) {
    auto& g=g_gxState;g=GXState{};
    g.proj.m0={1,0,0,0};g.proj.m1={0,1,0,0};g.proj.m2={0,0,1,0};g.proj.m3={0,0,0,1};
    g.logicalViewport=g.renderViewport={0,0,960,544,0,1};
    g.logicalScissor=g.renderScissor={0,0,960,544};
    g.numChans=1;g.numTevStages=1;
    g.tevStages[0].channelId=GX_COLOR0A0;
    g.tevStages[0].colorPass.d=GX_CC_RASC;g.tevStages[0].alphaPass.d=GX_CA_RASA;
    g.colorChannelConfig[GX_COLOR0].lightingEnabled=mode==0;
    g.colorChannelState[GX_COLOR0].lightMask=1;
    g.colorChannelState[GX_COLOR0].matColor={1,0.5f,0.25f,1};
    g.lights[0].color={0.5f,1,0.25f,1};g.lights[0].pos={1,2,3,1};
    g.vtxDesc[GX_VA_PNMTXIDX]=mode==0?GX_DIRECT:GX_NONE;
    g.vtxDesc[GX_VA_POS]=GX_DIRECT;g.vtxDesc[GX_VA_NRM]=GX_DIRECT;
    g.vtxFmts[0].attrs[GX_VA_POS]={GX_POS_XYZ,GX_F32,0};
    g.vtxFmts[0].attrs[GX_VA_NRM]={GX_NRM_XYZ,GX_F32,0};g.vtxFmts[1]=g.vtxFmts[0];
    std::array<uint8_t,75> raw{};
    const unsigned stride=mode==0?25:24,positionOffset=mode==0?1:0;
    for(unsigned v=0;v<3;++v) {
      if(mode==0)raw[v*stride]=uint8_t(v*3);
      const std::array<float,6> values{float(v==1),float(v==2),0,0,0,1};
      for(unsigned i=0;i<values.size();++i) {
        const auto bits=std::bit_cast<uint32_t>(values[i]);
        for(unsigned b=0;b<4;++b)raw[v*stride+positionOffset+i*4+b]=uint8_t(bits>>(24-8*b));
      }
    }
    gfx::note_memory_write(raw.data(),raw.size());set_raster_uniform_reuse(enabled);
    gfx::Renderer renderer;CHECK(renderer.initialize());renderer.begin_frame();
    gfx::Telemetry telemetry;telemetry.begin_frame(0);
    gxbridge::DrawSink sink;gxbridge::DrawSinkConfig config{};config.telemetry=&telemetry;
    config.staticGeometryBudget=mode?1024*1024:0;config.staticGeometryMinVertices=3;
    config.staticGeometryStableOnly=mode==2;config.allowStreamedFixedVertexGpu=mode==2;
    config.allowLitFixedVertexGpu=true;config.fixedUniformPoolBytes=gfx::FixedUniformPool::MaxRetainedBytes;
    CHECK(sink.initialize(renderer,config));sink.begin_frame(0);
    const auto bp=[&](uint8_t reg,uint32_t value) {
      const std::array<uint8_t,5> command{0x61,reg,uint8_t(value>>16),uint8_t(value>>8),uint8_t(value)};
      process(command.data(),command.size(),true);
    };
    const auto submit=[&](uint8_t fmt=0) {
      const auto before=telemetry.frame().counters.draws;
      CHECK(sink.submit(GX_TRIANGLES,fmt,raw.data(),3*stride,3,nullptr,0,mode==1?raw.data():nullptr).ok);
      g.stateDirty=false;CHECK(telemetry.frame().counters.draws==before+1);
      const auto* draw=sink.stream().tail_draw();CHECK(draw);if(!draw)return;
      const auto* compiled=renderer.pipelines().find(draw->pipelineKey);CHECK(compiled);if(!compiled)return;
      CHECK(compiled->desc.depthTest==g.depthCompare);
      CHECK(compiled->desc.depthWrite==(g.depthCompare&&g.depthUpdate));
      CHECK(compiled->desc.colorWrite==g.colorUpdate&&compiled->desc.alphaWrite==g.alphaUpdate);
      CHECK(bool(draw->fixedVertexUniforms)==bool(mode));
      gfx::VertexTransformState vertex{};gfx::DrawUniforms uniforms{};
      if(mode)gxbridge::translate_fixed_vertex_state(vertex,uniforms,compiled->desc,true);
      else gxbridge::translate_vertex_state(vertex,uniforms,compiled->desc,
                                            gxbridge::translate_current_vertex_layout(fmt),true);
      const gfx::GpuDrawUniforms expected(uniforms);
      CHECK(!std::memcmp(&draw->gpu_uniforms(),&expected,sizeof(expected)));
      if(draw->fixedVertexUniforms) {
        auto expectedFixed=gfx::fixed_vertex_uniforms(compiled->desc,vertex);auto actual=*draw->fixedVertexUniforms;
        actual.revision=expectedFixed.revision=0;
        CHECK(!std::memcmp(&actual,&expectedFixed,sizeof(actual)));
      }
    };
    submit();const auto before=telemetry.frame().counters;
    const auto* frozen=sink.stream().tail_draw();const auto firstUniforms=frozen->gpu_uniforms();
    uint32_t random=0x31415926;
    for(unsigned i=0;i<128;++i) {
      random=random*1664525u+1013904223u;
      const auto revisions=g.stateRevisions;const auto contract=g.pipelineUniformGeneration;
      if(i%5==0)bp(0xfe,0x00000f);
      bp(i&1?0x40:0x41,random&0x00ffff);
      CHECK(g.stateRevisions.raster>revisions.raster);
      if(enabled) {
        CHECK(g.stateRevisions.vertex==revisions.vertex&&g.stateRevisions.fragment==revisions.fragment);
        CHECK(g.stateRevisions.textures==revisions.textures);CHECK(g.pipelineUniformGeneration==contract);
      }else CHECK(g.pipelineUniformGeneration!=contract);
      submit();
    }
    CHECK(telemetry.frame().counters.vertexTranslations==before.vertexTranslations+(enabled?0:128));
    CHECK(telemetry.frame().counters.fragmentTranslations==before.fragmentTranslations+(enabled?0:128));
    CHECK(!std::memcmp(&frozen->gpu_uniforms(),&firstUniforms,sizeof(firstUniforms)));
    const auto rebuild=[&](auto&& change,uint8_t fmt=0) {
      const auto counters=telemetry.frame().counters;change();submit(fmt);
      CHECK(telemetry.frame().counters.vertexTranslations==counters.vertexTranslations+1);
    };
    // Interposed full changes cannot be hidden by a later raster write.
    rebuild([&]{const auto epoch=g.pipelineUniformGeneration;bp(0x42,0x180);bp(0x41,0x0318);
      CHECK(g.pipelineUniformGeneration!=epoch);});
    rebuild([&]{g.pnMtx[1].pos.m0[3]=9;g.mark_dirty(StateDomain::Vertex);bp(0x40,0x17);});
    auto counters=telemetry.frame().counters;
    g.colorRegs[0]={0.2f,0.4f,0.6f,0.8f};g.mark_dirty(StateDomain::Fragment);bp(0x41,0x0319);submit();
    CHECK(telemetry.frame().counters.fragmentTranslations==counters.fragmentTranslations+1);
    rebuild([&]{g.layoutStateGeneration=next_gx_state_epoch();g.mark_dirty(StateDomain::Layout);bp(0x40,0x03);});
    rebuild([&]{g.vertexProgramStateGeneration=next_gx_state_epoch();g.mark_dirty(StateDomain::Vertex);bp(0x41,0x0318);});
    rebuild([&]{bp(0x40,0x17);},1);
    rebuild([&]{g.proj.m0[0]=2;g.stateDirty=true;},1); // Untracked legacy write.
    rebuild([&]{gfx::gxm_disable_mask()=gfx::GxmDisableStateDomains;bp(0x41,0x0319);});
    gfx::gxm_disable_mask()=0;
    rebuild([&]{g.stateIdentity=next_gx_state_epoch();bp(0x40,0x03);});
    // Fog reads the vertex-domain viewport; indirect TEV reads texture size.
    // Narrow raster writes must not mask those cross-domain dependencies.
    g.fog.type=GX_FOG_PERSP_LIN;g.fog.a=0.125f;g.fogRange[0]=1u<<10;
    g.numIndStages=1;g.pipelineStateGeneration=next_gx_state_epoch();g.mark_dirty();submit();
    counters=telemetry.frame().counters;
    g.logicalViewport.left=30;g.renderViewport.width=800;g.mark_dirty(StateDomain::Vertex);
    bp(0x41,0x0318);submit();
    CHECK(telemetry.frame().counters.fragmentTranslations==counters.fragmentTranslations+1);
    counters=telemetry.frame().counters;
    g.loadedTextures[0].image0=15u|(7u<<10);g.mark_dirty(StateDomain::Textures);
    bp(0x40,0x17);submit();
    CHECK(telemetry.frame().counters.fragmentTranslations==counters.fragmentTranslations+1);
    // An unclassified direct pipeline invalidation must remain conservative.
    rebuild([&]{g.pipelineStateGeneration=next_gx_state_epoch();g.mark_dirty(StateDomain::Fragment);});
    sink.flush();sink.shutdown();renderer.shutdown();set_raster_uniform_reuse(false);
  }
}

void fixed_geometry_vertex_program_transitions(bool streamed = false) {
  using aurora::gx::StateDomain;
  auto& g=aurora::gx::g_gxState;g=aurora::gx::GXState{};
  g.proj.m0={1,0,0,0};g.proj.m1={0,1,0,0};g.proj.m2={0,0,1,0};g.proj.m3={0,0,0,1};
  g.pnMtx[0].pos.m0={1,0,0,0};g.pnMtx[0].pos.m1={0,1,0,0};g.pnMtx[0].pos.m2={0,0,1,0};
  g.renderViewport={0,0,960,544,0,1};g.logicalViewport=g.renderViewport;
  g.renderScissor={0,0,960,544};g.logicalScissor=g.renderScissor;
  g.numChans=1;g.numTevStages=1;g.tevStages[0].channelId=GX_COLOR0A0;
  g.tevStages[0].colorPass.d=GX_CC_RASC;g.tevStages[0].alphaPass.d=GX_CA_RASA;
  g.vtxDesc[GX_VA_POS]=GX_DIRECT;g.vtxFmts[0].attrs[GX_VA_POS]={GX_POS_XYZ,GX_F32,0};
  g.vtxDesc[GX_VA_CLR0]=GX_DIRECT;g.vtxFmts[0].attrs[GX_VA_CLR0]={GX_CLR_RGBA,GX_RGBA8,0};
  std::array<uint8_t,48> raw{};
  for(unsigned i=0;i<3;++i){
    const std::array<float,3> pos{float(i==1),float(i==2),0};
    for(unsigned j=0;j<3;++j){const auto bits=std::bit_cast<uint32_t>(pos[j]);
      for(unsigned b=0;b<4;++b)raw[i*16+j*4+b]=uint8_t(bits>>(24-b*8));}
    raw[i*16+12]=32;raw[i*16+13]=64;raw[i*16+14]=128;raw[i*16+15]=255;
  }
  gfx::Renderer renderer;CHECK(renderer.initialize());renderer.begin_frame();
  gxbridge::DrawSink sink;gxbridge::DrawSinkConfig config{};
  config.staticGeometryBudget=1024*1024;config.staticGeometryMinVertices=3;
  config.staticGeometryStableOnly=streamed;
  config.allowStreamedFixedVertexGpu=streamed;
  CHECK(sink.initialize(renderer,config));sink.begin_frame(0);
  const auto baseGeneration=g.pipelineStateGeneration;
  for(unsigned mode=0;mode<8;++mode){
    // Channel source changes invalidate the vertex program, without changing
    // the fragment/base pipeline. The GPU input stride must still follow it.
    g.colorChannelConfig[GX_COLOR0].matSrc=(mode&1)?GX_SRC_VTX:GX_SRC_REG;
    g.vertexProgramStateGeneration=aurora::gx::next_gx_state_epoch();g.mark_dirty(StateDomain::Vertex);
    // A dynamic source has no resident geometry identity. It must use the
    // streamed GPU layout and still follow every vertex-program transition.
    const auto result=sink.submit(GX_TRIANGLES,0,raw.data(),raw.size(),3,nullptr,0,
                                  streamed?nullptr:raw.data());
    CHECK(result.ok);g.stateDirty=false;CHECK(g.pipelineStateGeneration==baseGeneration);
    const auto* draw=sink.stream().tail_draw();CHECK(draw&&draw->fixedVertexUniforms);
    if(!draw)continue;
    const auto* compiled=renderer.pipelines().find(draw->pipelineKey);CHECK(compiled);
    if(!compiled)continue;
    const auto expected=gfx::fixed_vertex_gpu_layout(compiled->desc);
    CHECK(compiled->desc.fixedVertexOnGpu);
    CHECK(compiled->desc.layout.attributes[0].stride==expected.attributes[0].stride);
    CHECK(draw->vertices.size==size_t(draw->vertexCount)*expected.attributes[0].stride);
    if(streamed){
      CHECK(sink.memory_budget().staticGeometryBytes==0);
      // Live OFF/ON transitions must restore the full CPU state after the
      // lightweight GPU translation, without changing the base pipeline.
      const auto features=sink.runtime_feature_flags();
      sink.set_runtime_feature_flags(features&~gxbridge::RuntimeStreamedFixedVertex);
      CHECK(sink.submit(GX_TRIANGLES,0,raw.data(),raw.size(),3).ok);
      const auto* cpuDraw=sink.stream().tail_draw();CHECK(cpuDraw&&!cpuDraw->fixedVertexUniforms);
      if(cpuDraw){const auto* cpuPipeline=renderer.pipelines().find(cpuDraw->pipelineKey);
        CHECK(cpuPipeline&&!cpuPipeline->desc.fixedVertexOnGpu);}
      sink.set_runtime_feature_flags(features);
    }
  }
  sink.flush();sink.shutdown();renderer.shutdown();
}
void resident_geometry_has_one_draw_per_submission() {
  for(const bool cached:{false,true}) {
    auto& g=aurora::gx::g_gxState;g=aurora::gx::GXState{};
    g.proj.m0={1,0,0,0};g.proj.m1={0,1,0,0};g.proj.m2={0,0,1,0};g.proj.m3={0,0,0,1};
    g.pnMtx[0].pos.m0={1,0,0,0};g.pnMtx[0].pos.m1={0,1,0,0};g.pnMtx[0].pos.m2={0,0,1,0};
    g.renderViewport={0,0,960,544,0,1};g.renderScissor={0,0,960,544};
    g.numChans=1;g.numTevStages=1;g.tevStages[0].channelId=GX_COLOR0A0;
    g.tevStages[0].colorPass.d=GX_CC_RASC;g.tevStages[0].alphaPass.d=GX_CA_RASA;
    g.vtxDesc[GX_VA_POS]=GX_DIRECT;g.vtxFmts[0].attrs[GX_VA_POS]={GX_POS_XYZ,GX_F32,0};
    std::array<uint8_t,36> raw{};gfx::note_memory_write(raw.data(),raw.size());
    gfx::Renderer renderer;CHECK(renderer.initialize());renderer.begin_frame();
    gxbridge::DrawSink sink;gxbridge::DrawSinkConfig config{};
    config.staticGeometryBudget=cached?1024*1024:0;config.staticGeometryMinVertices=3;
    config.staticGeometryStableOnly=true;config.localDrawBatching=false;
    CHECK(sink.initialize(renderer,config));sink.begin_frame(0);
    for(unsigned draw=0;draw<32;++draw){
      CHECK(sink.submit(GX_TRIANGLES,0,raw.data(),raw.size(),3,nullptr,0,raw.data()).ok);
      CHECK(sink.stream().tail_draw()->instanceCount==1);
      CHECK(bool(sink.stream().tail_draw()->fixedVertexUniforms)==cached);
      g.stateDirty=false;
    }
    CHECK(sink.submitted_draws()==32);CHECK(renderer.stats().drawCalls==0);
    CHECK(sink.memory_budget().staticGeometryHits==(cached?31u:0u));
    // A tracked mutation must fall back exactly once, without also drawing
    // the previously cached mesh. Keep all older queued snapshots alive.
    raw[3]=1;gfx::note_memory_write(raw.data(),raw.size());
    CHECK(sink.submit(GX_TRIANGLES,0,raw.data(),raw.size(),3,nullptr,0,raw.data()).ok);
    CHECK(!sink.stream().tail_draw()->fixedVertexUniforms);
    size_t packets=0;
    for(const auto& command:sink.stream().commands())if(command.type==gfx::CommandType::Draw)++packets;
    CHECK(packets==33);CHECK(sink.submitted_draws()==33);
    sink.flush();CHECK(renderer.stats().drawCalls==33);CHECK(renderer.stats().triangles==33);
    sink.shutdown();renderer.shutdown();
  }
}
void completed_frame_does_not_fence() {
  BackendConfig config{};config.cpu_worker_threads=0;config.wait_vblank=false;
  config.log_level=RuntimeLogLevel::Silent;
  CHECK(initialize(config));CHECK(!completed_performance_snapshot().completedFrame);CHECK(!completed_memory_snapshot().completedFrame);
  CHECK(begin_frame());end_frame();
  const auto first=completed_performance_snapshot();CHECK(first.completedFrame&&first.frameIndex==1);
  const auto firstMemory=completed_memory_snapshot();CHECK(firstMemory.completedFrame&&firstMemory.frameIndex==1);
  CHECK(firstMemory.budget.staticGeometryBytes==memory_budget().staticGeometryBytes);
  CHECK(aurora::gx::fifo::start_worker());
  struct Blocker {std::atomic<bool>* entered;std::atomic<bool>* release;};
  std::atomic<bool> entered=false,release=false,readerDone=false;
  Blocker blocker{&entered,&release};
  aurora::gx::fifo::run_async([](void* opaque){auto args=*static_cast<Blocker*>(opaque);
    args.entered->store(true,std::memory_order_release);
    while(!args.release->load(std::memory_order_acquire))std::this_thread::yield();},&blocker,sizeof blocker);
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(!entered.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  CHECK(entered.load());
  std::thread reader([&]{const auto value=completed_performance_snapshot();
    const auto memory=completed_memory_snapshot();
    readerDone.store(value.completedFrame&&value.frameIndex==first.frameIndex&&memory.completedFrame&&memory.frameIndex==firstMemory.frameIndex,std::memory_order_release);});
  while(!readerDone.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  const bool beforeRelease=readerDone.load(std::memory_order_acquire);
  release.store(true,std::memory_order_release);reader.join();CHECK(beforeRelease);
  aurora::gx::fifo::drain_sync();CHECK(begin_frame());end_frame();aurora::gx::fifo::drain_sync();
  CHECK(completed_performance_snapshot().frameIndex==2);CHECK(completed_memory_snapshot().frameIndex==2);
  shutdown();CHECK(!completed_performance_snapshot().completedFrame);CHECK(!completed_memory_snapshot().completedFrame);
  BackendConfig invalid=config;invalid.render_width=invalid.width+1;
  CHECK(!initialize(invalid));CHECK(!completed_memory_snapshot().completedFrame);
  CHECK(initialize(config));CHECK(!completed_memory_snapshot().completedFrame);
  CHECK(begin_frame());end_frame();CHECK(completed_memory_snapshot().frameIndex==1);
  shutdown();CHECK(!completed_memory_snapshot().completedFrame);
}

void internal_resolution_restart_and_efb_mapping() {
  GXRenderModeObj mode{};mode.fbWidth=640;mode.efbHeight=480;
  mode.viWidth=640;mode.viHeight=480;VIConfigure(&mode);
  for(const bool reduced:{false,true,false}) {
    BackendConfig config{};config.cpu_worker_threads=0;config.wait_vblank=false;
    config.log_level=RuntimeLogLevel::Silent;
    if(reduced){config.render_width=640;config.render_height=480;}
    CHECK(initialize(config));CHECK(begin_frame());
    auto& g=aurora::gx::g_gxState;g=aurora::gx::GXState{};
    const auto size=aurora::gfx::get_render_target_size();
    const auto display=aurora::gfx::get_frame_buffer_size();
    CHECK(size.x==(reduced?640u:960u)&&size.y==(reduced?480u:544u));
    CHECK(display.x==960&&display.y==544);
    aurora::gx::set_logical_viewport({0,0,640,480,0.2f,0.8f});
    CHECK(g.renderViewport.width==float(size.x)&&g.renderViewport.height==float(size.y));
    CHECK(g.renderViewport.znear==0.2f&&g.renderViewport.zfar==0.8f);
    aurora::gx::set_logical_scissor({1,3,5,7});
    CHECK(g.renderScissor.x==1&&g.renderScissor.y==3);
    CHECK(g.renderScissor.width==(reduced?5:8)&&g.renderScissor.height==(reduced?7:9));
    const auto clipped=aurora::gx::map_logical_scissor({-5,-5,650,490});
    CHECK(clipped.x==0&&clipped.y==0&&clipped.width==int(size.x)&&clipped.height==int(size.y));
    // Exercise the real GXCopyTex mapping and live EFB texture resolver. The
    // shadow atlas's 2:1 copy must scale against the raster target, not scanout.
    std::array<uint8_t,32> destination{};
    g.texCopySrc={0,0,256,256};g.texCopyDstWidth=g.texCopyDstHeight=128;
    g.texCopyFmt=GX_TF_RGBA8;
    CHECK(draw_sink().copy_tex(destination.data(),false));
    g.copyTextures[destination.data()]={};
    auto& texture=g.loadedTextures[0];texture.data=destination.data();
    texture.mWidth=texture.mHeight=128;texture.mFormat=GX_TF_RGBA8;
    g.numTevStages=1;g.numTexGens=1;
    g.tevStages[0].texMapId=GX_TEXMAP0;g.tevStages[0].texCoordId=GX_TEXCOORD0;
    g.tevStages[0].colorPass.d=GX_CC_TEXC;g.tevStages[0].alphaPass.d=GX_CA_TEXA;
    g.vtxDesc[GX_VA_POS]=GX_DIRECT;g.vtxFmts[0].attrs[GX_VA_POS]={GX_POS_XYZ,GX_F32,0};
    g.mark_dirty();std::array<uint8_t,36> vertices{};
    CHECK(draw_sink().submit(GX_TRIANGLES,0,vertices.data(),vertices.size(),3).ok);
    const auto* packet=draw_sink().stream().tail_draw();CHECK(packet);
    if(packet){const auto& binding=packet->texture_bindings()[0];
      CHECK(binding.source==gfx::TextureSource::Efb);
      uint32_t width=0,height=0;CHECK(renderer().efb().dimensions(binding.texture,width,height));
      CHECK(width==(reduced?128u:192u)&&height==(reduced?128u:145u));}
    end_frame();shutdown();
  }
  VIConfigure(nullptr);
}

void guest_memory_prepare_fences_async_gx() {
  BackendConfig config{};config.cpu_worker_threads=0;config.wait_vblank=false;
  config.log_level=RuntimeLogLevel::Silent;
  CHECK(initialize(config));
  CHECK(aurora::gx::fifo::start_worker());

  struct Blocker {std::atomic<bool>* entered;std::atomic<bool>* release;};
  std::atomic<bool> entered=false,release=false,writeReturned=false;
  Blocker blocker{&entered,&release};
  aurora::gx::fifo::run_async([](void* opaque){auto args=*static_cast<Blocker*>(opaque);
    args.entered->store(true,std::memory_order_release);
    while(!args.release->load(std::memory_order_acquire))std::this_thread::yield();},&blocker,sizeof blocker);

  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(!entered.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  CHECK(entered.load(std::memory_order_acquire));

  std::array<uint8_t,64> guest{};
  std::thread writer([&]{
    aurora_vita_prepare_memory_write(guest.data(),guest.size());
    writeReturned.store(true,std::memory_order_release);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  CHECK(!writeReturned.load(std::memory_order_acquire));
  release.store(true,std::memory_order_release);
  writer.join();
  CHECK(writeReturned.load(std::memory_order_acquire));
  shutdown();
}

void memory_write_notification_does_not_fence_async_gx() {
  BackendConfig config{};config.cpu_worker_threads=0;config.wait_vblank=false;
  config.log_level=RuntimeLogLevel::Silent;
  CHECK(initialize(config));
  CHECK(aurora::gx::fifo::start_worker());

  struct Blocker {std::atomic<bool>* entered;std::atomic<bool>* release;};
  std::atomic<bool> entered=false,release=false;
  Blocker blocker{&entered,&release};
  aurora::gx::fifo::run_async([](void* opaque){auto args=*static_cast<Blocker*>(opaque);
    args.entered->store(true,std::memory_order_release);
    while(!args.release->load(std::memory_order_acquire))std::this_thread::yield();},&blocker,sizeof blocker);
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(!entered.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  CHECK(entered.load(std::memory_order_acquire));

  std::array<uint8_t,64> guest{};
  const auto start=std::chrono::steady_clock::now();
  aurora_vita_notify_memory_write(guest.data(),guest.size());
  CHECK(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(5));
  release.store(true,std::memory_order_release);
  aurora::gx::fifo::wait_idle();
  shutdown();
}

void multiple_waiters_recheck_completed_serial() {
  BackendConfig config{};config.cpu_worker_threads=0;config.wait_vblank=false;
  config.log_level=RuntimeLogLevel::Silent;
  CHECK(initialize(config));
  CHECK(aurora::gx::fifo::start_worker());

  struct Blocker {std::atomic<bool>* entered;std::atomic<bool>* release;};
  std::atomic<bool> entered=false,release=false;
  Blocker blocker{&entered,&release};
  const uint64_t serial=aurora::gx::fifo::run_async([](void* opaque){auto args=*static_cast<Blocker*>(opaque);
    args.entered->store(true,std::memory_order_release);
    while(!args.release->load(std::memory_order_acquire))std::this_thread::yield();},&blocker,sizeof blocker);
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(!entered.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  CHECK(entered.load(std::memory_order_acquire));

  std::atomic<unsigned> waiting=0,finished=0;
  const auto waiter=[&]{waiting.fetch_add(1,std::memory_order_release);aurora::gx::fifo::wait_marker(serial);finished.fetch_add(1,std::memory_order_release);};
  std::thread first(waiter),second(waiter);
  while(waiting.load(std::memory_order_acquire)!=2&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  CHECK(waiting.load(std::memory_order_acquire)==2);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  release.store(true,std::memory_order_release);
  first.join();second.join();
  CHECK(finished.load(std::memory_order_acquire)==2);
  shutdown();
}

void async_completion_signal_never_overflows() {
  BackendConfig config{};config.cpu_worker_threads=0;config.wait_vblank=false;
  config.log_level=RuntimeLogLevel::Silent;
  CHECK(initialize(config));
  CHECK(aurora::gx::fifo::start_worker());

  struct Counter {std::atomic<unsigned>* value;};
  std::atomic<unsigned> completed=0;
  Counter counter{&completed};
  constexpr unsigned Jobs=32;
  for(unsigned i=0;i<Jobs;++i) {
    CHECK(aurora::gx::fifo::run_async([](void* opaque){
      auto args=*static_cast<Counter*>(opaque);
      args.value->fetch_add(1,std::memory_order_release);
    },&counter,sizeof counter)!=0);
  }
  aurora::gx::fifo::wait_idle();
  CHECK(completed.load(std::memory_order_acquire)==Jobs);
  shutdown();
}

void async_end_frame_is_a_lifetime_barrier() {
  BackendConfig config{};config.cpu_worker_threads=0;config.wait_vblank=false;
  config.log_level=RuntimeLogLevel::Silent;
  CHECK(initialize(config));
  CHECK(aurora::gx::fifo::start_worker());
  CHECK(begin_frame());

  struct Blocker {std::atomic<bool>* entered;std::atomic<bool>* release;};
  std::atomic<bool> entered=false,release=false,endReturned=false;
  Blocker blocker{&entered,&release};
  aurora::gx::fifo::run_async([](void* opaque){auto args=*static_cast<Blocker*>(opaque);
    args.entered->store(true,std::memory_order_release);
    while(!args.release->load(std::memory_order_acquire))std::this_thread::yield();},&blocker,sizeof blocker);

  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(!entered.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  CHECK(entered.load(std::memory_order_acquire));

  std::thread frameEnd([&]{
    end_frame();
    endReturned.store(true,std::memory_order_release);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  CHECK(!endReturned.load(std::memory_order_acquire));
  release.store(true,std::memory_order_release);
  frameEnd.join();
  CHECK(endReturned.load(std::memory_order_acquire));
  CHECK(completed_memory_snapshot().completedFrame);
  shutdown();
}

void async_worker_rejects_direct_display_list_submit() {
  BackendConfig config{};config.cpu_worker_threads=0;config.wait_vblank=false;
  config.log_level=RuntimeLogLevel::Silent;
  CHECK(initialize(config));
  CHECK(aurora::gx::fifo::start_worker());

  // Contents are intentionally irrelevant: the ownership guard must reject a
  // producer-thread direct submit before it can inspect GX state or DrawSink.
  const std::array<uint8_t,3> list{0x90,0x00,0x00};
  CHECK(!aurora::gx::fifo::submit_simple_display_list(list.data(),list.size()));
  shutdown();
}

void async_display_list_uses_pinned_segment_without_fifo_copy() {
  BackendConfig config{};config.cpu_worker_threads=0;config.wait_vblank=false;
  config.log_level=RuntimeLogLevel::Silent;
  CHECK(initialize(config));
  // Strikers' validated INI currently carries gxm_dl_shadow=0. Async transport
  // must still pin immutable display-list payloads; this switch may only
  // disable the optional synchronous shadow cache.
  aurora::gx::fifo::set_display_list_shadow_enabled(false);
  CHECK(aurora::gx::fifo::start_worker());

  std::array<uint8_t,32> list{};
  list[0]=GX_NOP;
  aurora_vita_notify_memory_write(list.data(),list.size());
  const uint32_t before=aurora::gx::fifo::get_buffer_size();
  aurora::gx::fifo::write_stable_data(list.data(),static_cast<uint32_t>(list.size()));
  CHECK(aurora::gx::fifo::get_buffer_size()==before);
  aurora::gx::fifo::drain_sync();
  aurora::gx::fifo::set_display_list_shadow_enabled(true);
  shutdown();
}
void prepared_async_display_list_equivalence() {
  // Tokens survive hits but never recur after replacement, eviction or clear.
  {
    aurora::gx::fifo::DisplayListShadowCache cache(64);
    std::array<uint8_t,64> a{},b{};uint64_t first=0,hit=0,fresh=0;
    auto pin=cache.pin(a.data(),a.size(),&first);CHECK(pin&&first);
    CHECK(cache.pin(a.data(),a.size(),&hit)&&hit==first);
    a[1]=1;aurora_vita_notify_memory_write(a.data(),a.size());
    CHECK(cache.pin(a.data(),a.size(),&fresh)&&fresh>first);CHECK((*pin)[1]==0);
    first=fresh;CHECK(cache.pin(b.data(),b.size(),&fresh)&&fresh>first);
    first=fresh;cache.clear();CHECK(cache.pin(b.data(),b.size(),&fresh)&&fresh>first);
  }
  BackendConfig config{};config.cpu_worker_threads=0;config.wait_vblank=false;
  config.log_level=RuntimeLogLevel::Silent;config.prepared_display_lists=true;
  CHECK(initialize(config));CHECK(aurora::gx::fifo::start_worker());CHECK(begin_frame());
  // All state mutation stays on the GX owner. Producer never submits directly.
  aurora::gx::fifo::run_sync([](void*) {
    auto& g=aurora::gx::g_gxState;g=aurora::gx::GXState{};
    g.proj.m0={1,0,0,0};g.proj.m1={0,1,0,0};g.proj.m2={0,0,1,0};g.proj.m3={0,0,0,1};
    g.pnMtx[0].pos.m0={1,0,0,0};g.pnMtx[0].pos.m1={0,1,0,0};g.pnMtx[0].pos.m2={0,0,1,0};
    g.renderViewport={0,0,960,544,0,1};g.renderScissor={0,0,960,544};
    g.vtxDesc[GX_VA_POS]=GX_DIRECT;g.vtxFmts[0].attrs[GX_VA_POS]={GX_POS_XYZ,GX_F32,0};
    g.mark_dirty();
  },nullptr);
  std::array<uint8_t,64> list{};list[0]=0x90;list[2]=3;
  aurora_vita_notify_memory_write(list.data(),list.size());
  for(unsigned i=0;i<3;++i) {
    aurora::gx::fifo::write_stable_data(list.data(),list.size());
    aurora::gx::fifo::drain_sync();
  }
  end_frame();auto perf=completed_performance_snapshot();CHECK(perf.preparedListMisses==1);CHECK(perf.preparedListHits==2);
  CHECK(perf.preparedListRejected==0);
  // Revision changes replace the pin; a padding side effect uses process().
  CHECK(begin_frame());list[63]=0x48;
  aurora_vita_notify_memory_write(list.data(),list.size());
  aurora::gx::fifo::write_stable_data(list.data(),list.size());end_frame();
  perf=completed_performance_snapshot();CHECK(perf.preparedListRejected==1);
  shutdown();
}

void native_replay_exact_slot_gx_fallback_and_producer_epoch() {
  using namespace aurora::gx::fifo;
  BackendConfig config{};config.cpu_worker_threads=0;config.wait_vblank=false;
  config.log_level=RuntimeLogLevel::Silent;
  CHECK(initialize(config));CHECK(start_worker());CHECK(begin_frame());
  std::array<uint8_t,64> list{};list[0]=0x90;list[2]=3;
  aurora_vita_notify_memory_write(list.data(),list.size());
  CHECK(!write_native_draw_replay(list.data(),uint32_t(list.size())));
  CHECK(!native_draw_replay_tracking_enabled());
  set_native_draw_replay_tracking(true);
  CHECK(native_draw_replay_tracking_enabled());
  auto start=producer_write_epoch();
  write_u8(0);CHECK(producer_write_epoch()==start+1);
  write_data(list.data(),1);CHECK(producer_write_epoch()==start+2);
  // Precompiled GXCallDisplayList uses a pinned FIFO segment instead of
  // write_data(). It must also invalidate the producer's native replay stamp.
  std::array<uint8_t,64> noop{};
  write_stable_data(noop.data(),uint32_t(noop.size()));
  CHECK(producer_write_epoch()==start+3);
  run_async([](void*){},nullptr,0);
  CHECK(producer_write_epoch()==start+4);
  const uint8_t nop=0;
  process_sync(&nop,1,true);
  CHECK(producer_write_epoch()==start+5);
  CHECK(write_native_draw_replay(list.data(),uint32_t(list.size())));
  CHECK(producer_write_epoch()==start+5);
  // Compound GX command: it is never replayed as a single GXM draw.
  // A complete BP command after the draw must be decoded, not discarded.
  list[59]=0x61;
  aurora_vita_notify_memory_write(list.data(),list.size());
  CHECK(write_native_draw_replay(list.data(),uint32_t(list.size())));
  write_u8(0);CHECK(producer_write_epoch()==start+6);
  end_frame();
  const auto stats=native_replay_stats();
  CHECK(stats.attempted==2&&stats.replayed==0&&stats.fallback==2);
  set_native_draw_replay_tracking(false);
  CHECK(!native_draw_replay_tracking_enabled());
  start=producer_write_epoch();write_u8(0);
  CHECK(producer_write_epoch()==start);clear_buffer();
  shutdown();
}

void async_view_markers_preserve_fifo_order() {
  using aurora::vita::gfx::view_draw_capture;
  BackendConfig config{};config.cpu_worker_threads=0;config.wait_vblank=false;
  config.log_level=RuntimeLogLevel::Silent;
  CHECK(initialize(config));CHECK(aurora::gx::fifo::start_worker());
  CHECK(begin_frame());
  auto& trace=view_draw_capture();
  CHECK(trace.start(64));

  struct Blocker { std::atomic<bool>* entered;std::atomic<bool>* release; };
  std::atomic<bool> entered=false,release=false;
  Blocker args{&entered,&release};
  aurora::gx::fifo::run_async([](void* opaque){const auto block=*static_cast<Blocker*>(opaque);
    block.entered->store(true,std::memory_order_release);
    while(!block.release->load(std::memory_order_acquire))std::this_thread::yield();},&args,sizeof args);
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(!entered.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  CHECK(entered.load(std::memory_order_acquire));

  aurora::gx::fifo::write_u8(0); // Inline FIFO bytes before the first view.
  aurora::gx::fifo::write_view_marker(3,740);
  aurora::gx::fifo::write_u8(0);
  aurora::gx::fifo::write_view_marker(11,740);
  aurora::gx::fifo::write_u8(0);
  aurora::gx::fifo::write_view_marker(255,740);
  CHECK(trace.count()==0); // Producer's later view must not update the consumer.
  release.store(true,std::memory_order_release);
  end_frame(); // Hard lifetime fence: all FIFO segments were consumed.
  trace.stop();
  AuroraViewDrawRecord events[8]{};
  const size_t n=trace.read(0,events,8);
  CHECK(n==4&&trace.lost()==0);
  if(n==4) {
    CHECK(events[0].type==AURORA_VIEW_DRAW_MARKER&&events[0].view==3);
    CHECK(events[1].type==AURORA_VIEW_DRAW_MARKER&&events[1].view==11);
    CHECK(events[2].type==AURORA_VIEW_DRAW_MARKER&&events[2].view==255);
    CHECK(events[3].type==AURORA_VIEW_DRAW_FRAME_COMPLETE&&events[3].consumer_frame==1);
    for(unsigned i=0;i<4;++i)CHECK(events[i].sequence==i+1&&events[i].producer_frame==740);
  }
  shutdown();
}

}
int main(){pooled_snapshot_transitions();layouts();fragment_updates();encoded_xf_matrix_oracles();xf_equal_position_writes_oracle();xf_equal_matrix_writes_oracle();tev_decoded_write_gate_oracle();decoded_snapshot_field_oracle();state_transitions();raster_pipeline_uniform_reuse_oracle();fixed_geometry_vertex_program_transitions();fixed_geometry_vertex_program_transitions(true);resident_geometry_has_one_draw_per_submission();completed_frame_does_not_fence();internal_resolution_restart_and_efb_mapping();guest_memory_prepare_fences_async_gx();memory_write_notification_does_not_fence_async_gx();multiple_waiters_recheck_completed_serial();async_completion_signal_never_overflows();async_end_frame_is_a_lifetime_barrier();async_worker_rejects_direct_display_list_submit();async_display_list_uses_pinned_segment_without_fifo_copy();prepared_async_display_list_equivalence();native_replay_exact_slot_gx_fallback_and_producer_epoch();async_view_markers_preserve_fifo_order();
  std::printf("frontend translation: %u checks, %u failures\n",checks,failures);return failures?1:0;}
