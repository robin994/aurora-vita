#pragma once
#include "vita_hash.hpp"
#include "vita_vertex_decode.hpp"
#include <array>
#include <memory>
#include <new>

namespace aurora::vita::gfx {

// Attribution only: fingerprints describe decoded object-space data, never
// authorize skipping decode, transformation, allocation or a GPU draw.
// Sample one whole frame out of sixteen so workers can report in disjoint
// slots, without atomics, locks or allocation in their hot loop.
class VertexReuseProbe {
public:
  struct VertexHashes { uint64_t full=0, positionNormal=0; };
  struct Stats {
    uint64_t sampledFrames=0,draws=0,vertices=0;
    uint64_t fullHits=0,fullHitVertices=0;
    uint64_t positionNormalDraws=0,positionNormalVertices=0;
    uint64_t positionNormalHits=0,positionNormalHitVertices=0;
    uint64_t positionNormalCrossDrawVertices=0,positionNormalVertexTableOverflows=0;
    uint64_t tableOverflows=0,skippedDraws=0;
  };
  static constexpr size_t MaxVertices=65536,TableSize=2048;
  bool initialize() noexcept {
    hashes_.reset(new(std::nothrow) VertexHashes[MaxVertices]);
    return hashes_!=nullptr;
  }
  void begin_frame(uint64_t frame) noexcept {
    active_=hashes_ && frame%16==0;
    if(!active_)return;
    ++stats_.sampledFrames;
    full_={};positionNormal_={};positionNormalVertices_={};
  }
  VertexHashes* prepare(uint32_t count) noexcept {
    if(!active_)return nullptr;
    if(!count||count>MaxVertices){++stats_.skippedDraws;return nullptr;}
    return hashes_.get();
  }
  static VertexHashes fingerprint(const CanonicalVertex& v) noexcept {
    // Exclude structure padding: only decoder-defined fields participate.
    uint64_t h=fnv1a64(v.position,sizeof v.position);
    h=fnv1a64(v.normal,sizeof v.normal,h);
    const uint64_t pn=h;
    h=fnv1a64(v.binormal,sizeof v.binormal,h);
    h=fnv1a64(v.tangent,sizeof v.tangent,h);
    h=fnv1a64(v.color0,sizeof v.color0,h);
    h=fnv1a64(v.color1,sizeof v.color1,h);
    h=fnv1a64(v.texcoord,sizeof v.texcoord,h);
    h=fnv1a64(&v.pnMatrixIndex,sizeof v.pnMatrixIndex,h);
    h=fnv1a64(v.texMatrixIndex,sizeof v.texMatrixIndex,h);
    return {h,pn};
  }
  void observe(uint32_t count,VertexSemanticMask semantics) noexcept {
    if(!active_||!count||count>MaxVertices)return;
    ++stats_.draws;stats_.vertices+=count;
    uint64_t full=hash_combine(count,semantics),pn=hash_combine(count,0);
    for(uint32_t i=0;i<count;++i){
      full=hash_combine(full,hashes_[i].full);
      pn=hash_combine(pn,hashes_[i].positionNormal);
    }
    if(seen(full_,full,count,semantics)){++stats_.fullHits;stats_.fullHitVertices+=count;}
    constexpr auto pnSemantics=vertex_semantic_bit(VertexSemantic::Position)|vertex_semantic_bit(VertexSemantic::Normal);
    // A shadow pass that does not decode normals must not report its default
    // normal as evidence of reusable source position/normal data.
    if((semantics&pnSemantics)==pnSemantics){
      ++stats_.positionNormalDraws;stats_.positionNormalVertices+=count;
      // Separate parts can overlap without having identical whole streams.
      // Count only matches with an earlier DRAW, excluding a draw's own
      // repeated triangle vertices. Probes stay bounded even when full.
      for(uint32_t i=0;i<count;++i){
        const uint64_t hash=hashes_[i].positionNormal;
        const size_t slot=(hash^(hash>>32))&(positionNormalVertices_.size()-1);
        bool found=false;
        for(size_t n=0;n<8;++n){
          auto& e=positionNormalVertices_[(slot+n)&(positionNormalVertices_.size()-1)];
          if(!e.firstDraw){e={hash,stats_.draws};found=true;break;}
          if(e.hash==hash){
            if(e.firstDraw!=stats_.draws)++stats_.positionNormalCrossDrawVertices;
            found=true;break;
          }
        }
        if(!found)++stats_.positionNormalVertexTableOverflows;
      }
      if(seen(positionNormal_,pn,count,pnSemantics)){
        ++stats_.positionNormalHits;stats_.positionNormalHitVertices+=count;
      }
    }
  }
  Stats stats() const noexcept { return stats_; }
private:
  struct Entry { uint64_t hash=0;uint32_t count=0;VertexSemanticMask semantics=0; };
  bool seen(std::array<Entry,TableSize>& table,uint64_t hash,uint32_t count,VertexSemanticMask semantics) noexcept {
    const size_t slot=(hash^(hash>>32))&(TableSize-1);
    for(size_t i=0;i<TableSize;++i){
      auto& e=table[(slot+i)&(TableSize-1)];
      if(!e.count){e={hash,count,semantics};return false;}
      if(e.hash==hash&&e.count==count&&e.semantics==semantics)return true;
    }
    ++stats_.tableOverflows;return false;
  }
  std::unique_ptr<VertexHashes[]> hashes_{};
  std::array<Entry,TableSize> full_{},positionNormal_{};
  struct VertexEntry { uint64_t hash=0,firstDraw=0; };
  std::array<VertexEntry,16384> positionNormalVertices_{};
  Stats stats_{};
  bool active_=false;
};
} // namespace aurora::vita::gfx
