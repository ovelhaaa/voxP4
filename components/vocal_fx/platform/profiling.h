#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
enum class ProfileSection : uint8_t {
  Input,
  Compressor,
  Delay,
  Reverb,
  Pipeline,
  Harmony,
  Master,
  ParameterQueue,
  PitchLpcTap,
  PitchMarkSync,
  DryAlignment,
  HarmonySlewPan,
  HarmonyLimiter,
  BusMixing,
  DelayPrep,
  ReverbPrep,
  AnalysisDecimator,
  AnalysisFifoDrain,
  AnalysisRollingWindow,
  AnalysisLinearWindowCopy,
  YinEnergy,
  YinDifference,
  YinCmnd,
  YinSearch,
  YinInterpolation,
  YinTotal,
  VoicedFeatures,
  VoicedClassifier,
  PitchSmoother,
  PitchMarkSearch,
  PitchMarkCorrelation,
  PitchPublication,
  AnalysisTotal,
  AnalysisRunTotal,
  PitchShiftMarkSelection,
  PitchShiftGrainScheduling,
  PitchShiftGrainHistoryLookup,
  PitchShiftPlainWindowOLA,
  PitchShiftLpcResidualFIR,
  PitchShiftLpcModelLookup,
  PitchShiftLpcModelWarpPolynomial,
  PitchShiftLpcModelWarpGainNorm,
  PitchShiftLpcWindowOLA,
  PitchShiftLpcSynthesisAllPole,
  PitchShiftLpcStateShift,
  PitchShiftFormantGainMatcher,
  PitchShiftFormantSoftClip,
  PitchShiftFormantBlend,
  PitchShiftFallback,
  PitchShiftArticulation,
  PitchShiftPlosiveBridge,
  PitchShiftTelemetry,
  PitchShiftOther,
  PitchShiftTotal,
  LpcFifoDrain, LpcRingWrite, LpcFrameLinearization, LpcSolveWindowing,
  LpcAutocorrelation, LpcLevinsonDurbin, LpcPublication, LpcSolveTotal, LpcTotal,
  HarmonyVoice0, HarmonyVoice1,
  InputHpf, InputGate,
  MasterMix, MasterLimiter,
  Chorus, Drive,
  Count
};
struct ProfileStats {
  uint64_t calls = 0, total_us = 0, max_us = 0, deadline_misses = 0;
  uint64_t total_cycles = 0, max_cycles = 0;
};
struct ProfileDistributionStats {
  uint32_t p50_us = 0, p95_us = 0, p99_us = 0, samples = 0;
};
class Profiler {
public:
  ~Profiler();
  static uint64_t now_us();
  static uint32_t now_cycles();
  static uint32_t cycles_per_us();
  // Audit-only bounded reservoir. Callers opt in explicitly so normal
  // firmware pays neither the memory nor the update cost.
  bool enable_distribution_range(ProfileSection first, ProfileSection last);
  void begin(ProfileSection s);
  void end(ProfileSection s, uint64_t deadline_us = 0);
  void record_cycles(ProfileSection s, uint64_t cycles,
                     uint64_t calls = 1);
  ProfileStats stats(ProfileSection s) const;
  // B4D.4: internal cumulative cycles (not the published snapshot), for
  // per-block delta attribution of record_cycles sections.
  uint64_t raw_total_cycles(ProfileSection s) const {
    return stats_[static_cast<size_t>(s)].total_cycles;
  }
  ProfileDistributionStats distribution_stats(ProfileSection s) const;
  void reset();

private:
  struct Atomic64Parts {
    std::atomic<uint32_t> low{0};
    std::atomic<uint32_t> high{0};
  };
  struct PublishedStats {
    std::atomic<uint32_t> sequence{0};
    Atomic64Parts calls, total_us, max_us, deadline_misses, total_cycles,
        max_cycles;
  };

  static void store(Atomic64Parts &destination, uint64_t value);
  static uint64_t load(const Atomic64Parts &source);
  void publish(size_t index);

  std::array<uint64_t, (size_t)ProfileSection::Count> start_{};
  std::array<uint32_t, (size_t)ProfileSection::Count> start_cycles_{};
  std::array<ProfileStats, (size_t)ProfileSection::Count> stats_{};
  std::array<PublishedStats, (size_t)ProfileSection::Count> published_{};
  void record_distribution(size_t index, uint64_t elapsed_us);
};
#if VOCAL_FX_ENABLE_PROFILING
#define VF_PROFILE_BEGIN(p, s) (p).begin(s)
#define VF_PROFILE_END(p, s, d) (p).end(s, d)
#else
#define VF_PROFILE_BEGIN(p, s) ((void)0)
#define VF_PROFILE_END(p, s, d) ((void)0)
#endif
