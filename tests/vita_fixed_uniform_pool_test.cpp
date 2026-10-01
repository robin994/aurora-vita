#include <gtest/gtest.h>
#include "../platforms/vita/gfx/vita_fixed_uniform_pool.hpp"
#include "../platforms/vita/gfx/vita_fixed_vertex.hpp"
#include <vector>

namespace {
using namespace aurora::vita::gfx;

void expect_same(const FixedVertexUniforms& a, const FixedVertexUniforms& b) {
  EXPECT_EQ(a.position, b.position); EXPECT_EQ(a.normal, b.normal);
  EXPECT_EQ(a.positionPalette, b.positionPalette); EXPECT_EQ(a.normalPalette, b.normalPalette);
  EXPECT_EQ(a.texturePalette, b.texturePalette); EXPECT_EQ(a.texture, b.texture);
  EXPECT_EQ(a.post, b.post); EXPECT_EQ(a.material, b.material); EXPECT_EQ(a.ambient, b.ambient);
  EXPECT_EQ(a.light, b.light); EXPECT_EQ(a.primitiveExpand, b.primitiveExpand);
  EXPECT_EQ(a.revision, b.revision);
}

TEST(FixedUniformPool, GrowthKeepsEveryQueuedSnapshotDistinctAndUnchanged) {
  FixedUniformPool pool;
  pool.configure(FixedUniformPool::MaxRetainedBytes);
  std::vector<FixedVertexUniforms*> queued;
  for (unsigned i=0; i<200; ++i) {
    auto& snapshot=pool.emplace_back();
    snapshot.position[0]=float(i+1); snapshot.revision=i+1;
    for (const auto* older:queued) EXPECT_NE(older, &snapshot);
    queued.push_back(&snapshot);
  }
  for (unsigned i=0; i<queued.size(); ++i) {
    EXPECT_EQ(queued[i]->position[0], float(i+1)); EXPECT_EQ(queued[i]->revision, i+1);
  }
  EXPECT_EQ(pool.stats().allocations, 200u);
  EXPECT_EQ(pool.stats().fallbacks, 0u);
}

TEST(FixedUniformPool, ResetRetainsStorageAndAcquisitionClearsAllOldContents) {
  FixedUniformPool pool;
  pool.configure(2*sizeof(FixedVertexUniforms));
  auto* first=&pool.emplace_back(); auto* second=&pool.emplace_back();
  first->position.fill(9); first->normal.fill(8); first->material[0].fill(7);
  first->positionPalette[0].fill(6); first->normalPalette[0].fill(5);
  first->texturePalette[0].fill(4); first->texture[0].fill(3); first->post[0].fill(2);
  first->ambient[0].fill(1); first->light[0].fill(10); first->primitiveExpand.fill(11);
  first->revision=123;
  const auto allocations=pool.stats().allocations;
  pool.reset(); EXPECT_EQ(pool.size(), 0u);
  auto& fresh=pool.emplace_back(); EXPECT_EQ(&fresh, first);
  expect_same(fresh, FixedVertexUniforms{});
  EXPECT_EQ(&pool.emplace_back(), second);
  EXPECT_EQ(pool.stats().allocations, allocations); EXPECT_EQ(pool.stats().reuses, 2u);
}

TEST(FixedUniformPool, BudgetOverflowUsesDistinctDequeStorageWithoutOverwritingLiveSlots) {
  FixedUniformPool pool;
  pool.configure(2*sizeof(FixedVertexUniforms));
  std::vector<FixedVertexUniforms*> queued;
  for (unsigned i=0; i<8; ++i) {
    auto& snapshot=pool.emplace_back(); snapshot.revision=i+1;
    for (const auto* older:queued) EXPECT_NE(older, &snapshot);
    queued.push_back(&snapshot);
  }
  for (unsigned i=0; i<queued.size(); ++i) EXPECT_EQ(queued[i]->revision, i+1);
  EXPECT_EQ(pool.stats().retainedBytes, 2*sizeof(FixedVertexUniforms));
  EXPECT_EQ(pool.stats().fallbacks, 6u);
  pool.reset(); EXPECT_EQ(&pool.emplace_back(), queued[0]);
}

TEST(FixedUniformPool, DiscardingDuplicateCandidatePreservesTheSharedOlderSnapshot) {
  FixedUniformPool pool;
  pool.configure(3*sizeof(FixedVertexUniforms));
  auto* shared=&pool.emplace_back(); shared->revision=45; shared->position[0]=123;
  auto* candidate=&pool.emplace_back(); candidate->revision=46;
  pool.pop_back(); EXPECT_EQ(pool.size(), 1u);
  auto& next=pool.emplace_back(); EXPECT_EQ(&next, candidate);
  next.position[0]=321;
  EXPECT_EQ(shared->revision, 45u); EXPECT_EQ(shared->position[0], 123);
}

TEST(FixedUniformPool, PopHandlesOverflowThenRetainedSlotsInCommandOrder) {
  FixedUniformPool pool;
  pool.configure(sizeof(FixedVertexUniforms));
  auto* retained=&pool.emplace_back(); retained->revision=7;
  pool.emplace_back(); pool.emplace_back();
  pool.pop_back(); EXPECT_EQ(pool.size(), 2u);
  pool.pop_back(); EXPECT_EQ(pool.size(), 1u); EXPECT_EQ(retained->revision, 7u);
  pool.pop_back(); EXPECT_EQ(pool.size(), 0u);
  EXPECT_EQ(&pool.emplace_back(), retained); EXPECT_EQ(retained->revision, 0u);
}

TEST(FixedUniformPool, ZeroBudgetRetainsTheOriginalDequeFallback) {
  FixedUniformPool pool;
  pool.configure(0);
  std::vector<FixedVertexUniforms*> queued;
  for (unsigned i=0; i<200; ++i) {
    auto& snapshot=pool.emplace_back(); snapshot.revision=i+1; queued.push_back(&snapshot);
  }
  for (unsigned i=0; i<queued.size(); ++i) EXPECT_EQ(queued[i]->revision, i+1);
  EXPECT_FALSE(pool.stats().enabled); EXPECT_EQ(pool.stats().retainedBytes, 0u);
  EXPECT_EQ(pool.stats().allocations, 0u); EXPECT_EQ(pool.stats().reuses, 0u);
  pool.reset(); expect_same(pool.emplace_back(), FixedVertexUniforms{});
}

TEST(FixedUniformPool, WarmFlushesStopAllocatingAndRetentionIsCappedAtOneMiB) {
  FixedUniformPool pool;
  pool.configure(4*FixedUniformPool::MaxRetainedBytes);
  const size_t count=FixedUniformPool::MaxRetainedBytes/sizeof(FixedVertexUniforms);
  for (size_t i=0; i<count+1; ++i) pool.emplace_back();
  EXPECT_LE(pool.stats().retainedBytes, FixedUniformPool::MaxRetainedBytes);
  EXPECT_EQ(pool.stats().allocations, count); EXPECT_EQ(pool.stats().fallbacks, 1u);
  for (unsigned frame=0; frame<20; ++frame) {
    pool.reset(); for (size_t i=0; i<count; ++i) pool.emplace_back();
  }
  EXPECT_EQ(pool.stats().allocations, count); EXPECT_EQ(pool.stats().reuses, 20*count);
}

TEST(FixedUniformPool, ReusedStorageRebuildsChangingPalettesLightsAndTextureMatricesExactly) {
  FixedUniformPool pool;
  pool.configure(sizeof(FixedVertexUniforms));
  PipelineDesc pipeline{};
  pipeline.fixedVertexOnGpu=true; pipeline.fixedVertexIndexedPn=true;
  pipeline.fixedVertexTexMtxMask=1;
  pipeline.texgenCount=1; pipeline.texgens[0].source=TexGenSource::Position;
  pipeline.texgens[0].matrix=0; pipeline.texgens[0].postMatrix=0;
  pipeline.colorChannels[0].lightingEnabled=true; pipeline.colorChannels[0].lightMask=1;
  VertexTransformState state{};
  for (unsigned frame=0; frame<20; ++frame) {
    state.postexMatrices[0].v[0]=float(frame+1);
    state.normalMatrices[0].v[0]=float(frame+2);
    state.postexMatrices[10].v[0]=float(frame+3);
    state.postMatrices[0].v[0]=float(frame+4);
    state.channelMaterial[0][0]=float(frame+5);
    state.channelAmbient[0][0]=float(frame+6);
    state.lights[0].color[0]=float(frame+7);
    pool.reset(); auto& snapshot=pool.emplace_back();
    fixed_vertex_uniforms_into(snapshot, pipeline, state);
    expect_same(snapshot, fixed_vertex_uniforms(pipeline, state));
  }
  EXPECT_EQ(pool.stats().allocations, 1u); EXPECT_EQ(pool.stats().reuses, 19u);
}

TEST(FixedUniformPool, ClearAndReconfigurationReleaseRetainedStorageAndCounters) {
  FixedUniformPool pool;
  pool.configure(2*sizeof(FixedVertexUniforms)); pool.emplace_back(); pool.emplace_back();
  pool.reset(); pool.emplace_back();
  pool.clear(); EXPECT_EQ(pool.size(), 0u); EXPECT_EQ(pool.stats().retainedBytes, 0u);
  EXPECT_FALSE(pool.stats().enabled); EXPECT_EQ(pool.stats().allocations, 0u);
  EXPECT_EQ(pool.stats().reuses, 0u); EXPECT_EQ(pool.stats().fallbacks, 0u);
  pool.configure(sizeof(FixedVertexUniforms)-1);
  EXPECT_FALSE(pool.stats().enabled); expect_same(pool.emplace_back(), FixedVertexUniforms{});
}
} // namespace
