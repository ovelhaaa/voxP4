#pragma once
#include "midi_harmony.h"
#include "scale.h"
#include <array>
#include <cstddef>

constexpr size_t MAX_HARMONY_VOICES = 2;
enum class HarmonyMode : uint8_t { FixedInterval, Diatonic, MidiChord };
struct HarmonyVoiceConfig { bool enabled=false; float gain=0.5f; float pan=0; float smoothing_ms=50; float interval=0; int degree=0; };
struct HarmonyVoiceTarget { bool valid=false; float target_frequency_hz=0; float semitone_offset=0; int source_note=0; float deviation_cents=0; };

class HarmonyEngine {
public:
  void reset(); void set_mode(HarmonyMode m); void set_scale(ScaleConfig s);
  void set_root(uint8_t root) { scale_.root=root%12; reset(); }
  void set_scale_type(ScaleType type) { scale_.type=type; reset(); }
  void set_voice(size_t i,const HarmonyVoiceConfig &c);
  std::array<HarmonyVoiceTarget,2> update(float source_hz,bool voiced,const MidiChordState &midi);
  const HarmonyVoiceConfig &voice(size_t i) const { return voices_[i]; }
private:
  int identify(float midi); HarmonyVoiceTarget target_for(size_t i,float midi,int note,const std::array<bool,128>&held);
  HarmonyMode mode_=HarmonyMode::FixedInterval; ScaleConfig scale_{};
  std::array<HarmonyVoiceConfig,2> voices_{{{false,.501187f,-.25f,50,4,2},{false,.354813f,.25f,50,7,4}}};
  int identity_=0; bool have_identity_=false; std::array<float,2> previous_{{0,0}};
};
