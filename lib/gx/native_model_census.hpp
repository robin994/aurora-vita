#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace aurora::gx::fifo {
enum class ModelReject : uint8_t {
  NullPacket, NoDraw, ViewTransition, NotCompiled, UserData, MissingStreams,
  NotDisplayList, Program, TexConfig, VertexCount, Primitive, StreamCount,
  View, DirtyState, DirtyTexture, Modifiers, TextureHandles, Matrix,
  StreamSemantic, StreamStorage, MissingPosition, DisplayListSize,
  WorkerInactive, WorkerContext, InsideDisplayList, RecordingBusy, RecipeLimit, Count
};
inline constexpr std::array<const char*,size_t(ModelReject::Count)> ModelRejectNames{
  "null_packet","no_draw","view_transition","not_compiled","user_data","missing_streams",
  "not_display_list","program","texconfig","vertex_count","primitive","stream_count",
  "view","dirty_state","dirty_texture","modifiers","texture_handles","matrix",
  "stream_semantic","stream_storage","missing_position","display_list_size",
  "worker_inactive","worker_context","inside_display_list","recording_busy","recipe_limit"};
enum class ModelStage : uint8_t {
  BeginAttempt, BeginRejected, KnownNeedsState, QueueAccepted, QueueRejected,
  CaptureSealed, CaptureAborted, Count
};
inline constexpr std::array<const char*,size_t(ModelStage::Count)> ModelStageNames{
  "begin_attempt","begin_rejected","known_needs_state","queue_accepted","queue_rejected",
  "capture_sealed","capture_aborted"};
inline constexpr size_t ModelViewCount=35; // Strikers views 0..33 plus unknown.
inline constexpr size_t ModelProgramCount=9; // Eight named GLX programs plus other.
inline constexpr uint32_t model_reject_bit(ModelReject r){return uint32_t(1)<<unsigned(r);}
inline constexpr uint32_t ModelPacketRejectMask=(uint32_t(1)<<unsigned(ModelReject::WorkerInactive))-1;

struct ModelAdmissionFacts {
  uint64_t frame=0;
  uint32_t flags=0,view=34,previousView=34,program=0,programKind=8;
  uint32_t texconfig=0,textureMask=0,streamMask=0,modifierMask=0;
  uint32_t vertices=0,streams=0,primitive=0,listBytes=0,transportRejects=0;
  bool packet=false,compiled=false,userData=false,hasStreams=false,displayList=false;
  bool allowedProgram=false,allowedView=false,dirtyState=false,dirtyTexture=false;
  bool ownedMatrix=false,validStreamSemantics=true,validStreamStorage=true,position=false;
};

// Independent diagnostic classifier. The production gate remains unchanged;
// observations compare its answer with these facts and count any mismatch.
inline uint32_t model_admission_rejections(const ModelAdmissionFacts& f) noexcept {
  uint32_t mask=f.transportRejects;
  const auto reject=[&](bool condition,ModelReject r){if(condition)mask|=model_reject_bit(r);};
  reject(!(f.flags&0x800),ModelReject::NoDraw);
  reject((f.flags&1)||f.view!=f.previousView,ModelReject::ViewTransition);
  reject(!f.packet,ModelReject::NullPacket);
  if(!f.packet)return mask;
  reject(!f.compiled,ModelReject::NotCompiled);reject(f.userData,ModelReject::UserData);
  reject(!f.hasStreams,ModelReject::MissingStreams);reject(!f.displayList,ModelReject::NotDisplayList);
  reject(!f.allowedProgram,ModelReject::Program);reject(f.texconfig!=0,ModelReject::TexConfig);
  reject(f.vertices<48,ModelReject::VertexCount);reject(f.primitive!=0,ModelReject::Primitive);
  reject(f.streams==0||f.streams>8,ModelReject::StreamCount);reject(!f.allowedView,ModelReject::View);
  reject(f.dirtyState,ModelReject::DirtyState);reject(f.dirtyTexture,ModelReject::DirtyTexture);
  reject(f.modifierMask!=0,ModelReject::Modifiers);reject(f.textureMask!=0,ModelReject::TextureHandles);
  reject(!f.ownedMatrix,ModelReject::Matrix);
  // A missing/oversized stream table is not dereferenced and has its own reason.
  if(f.hasStreams&&f.streams>0&&f.streams<=8){
    reject(!f.validStreamSemantics,ModelReject::StreamSemantic);
    reject(!f.validStreamStorage,ModelReject::StreamStorage);
    reject(!f.position,ModelReject::MissingPosition);
  }
  if(f.displayList)reject(f.listBytes<3||f.listBytes>256u*1024u,ModelReject::DisplayListSize);
  return mask;
}

struct ModelCensusSnapshot {
  uint64_t callbacks=0,drawCallbacks=0,filterPassed=0,transportReady=0,eligible=0,mismatches=0;
  std::array<uint64_t,size_t(ModelReject::Count)> rejects{};
  std::array<uint64_t,ModelViewCount> views{};
  std::array<uint64_t,ModelProgramCount> programs{};
  std::array<uint64_t,size_t(ModelStage::Count)> stages{};
};
struct ModelCensusExample {bool valid=false;uint32_t rejects=0;ModelAdmissionFacts facts{};};
struct ModelCensusEvidence {
  ModelCensusSnapshot counts{};
  std::array<ModelCensusExample,size_t(ModelReject::Count)> reasons{};
  std::array<ModelCensusExample,ModelViewCount> views{};
};
class ModelAdmissionCensus {
  mutable std::mutex mutex_;
  ModelCensusEvidence evidence_{};
public:
  void observe(const ModelAdmissionFacts& facts,bool productionEligible) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& s=evidence_.counts;++s.callbacks;
    if(!(facts.flags&0x800))return; // State-only callbacks are not draws.
    ++s.drawCallbacks;
    const uint32_t mask=model_admission_rejections(facts);
    const bool diagnosticEligible=(mask&ModelPacketRejectMask)==0;
    if(productionEligible)++s.filterPassed;
    if(diagnosticEligible!=productionEligible)++s.mismatches;
    if(!(mask&~ModelPacketRejectMask))++s.transportReady;
    if(productionEligible&&!(mask&~ModelPacketRejectMask))++s.eligible;
    const size_t view=facts.view<34?facts.view:34;
    ++s.views[view];++s.programs[facts.programKind<ModelProgramCount?facts.programKind:8];
    if(!evidence_.views[view].valid)evidence_.views[view]={true,mask,facts};
    for(size_t i=0;i<s.rejects.size();++i)if(mask&(uint32_t(1)<<i)){
      ++s.rejects[i];if(!evidence_.reasons[i].valid)evidence_.reasons[i]={true,mask,facts};
    }
  }
  void event(ModelStage stage) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);++evidence_.counts.stages[size_t(stage)];
  }
  ModelCensusSnapshot snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);return evidence_.counts;
  }
  ModelCensusEvidence evidence() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);return evidence_;
  }
};

uint32_t native_model_transport_rejections() noexcept;
void record_native_model_admission(const ModelAdmissionFacts&,bool productionEligible) noexcept;
void record_native_model_stage(ModelStage) noexcept;
ModelCensusSnapshot native_model_census_snapshot() noexcept;
ModelCensusEvidence native_model_census_evidence() noexcept;
} // namespace aurora::gx::fifo
