#pragma once
#include "midi_harmony.h"
#include "scale.h"
#include <array>
#include <cstddef>

// B4D.6: the production product is a SINGLE harmony voice.  The former
// two-voice array/HarmonyEngine has been reduced to one voice so the second
// TD-PSOLA instance, its state, telemetry and hot-path loop are gone.
constexpr size_t MAX_HARMONY_VOICES = 1;
enum class HarmonyMode : uint8_t { FixedInterval, Diatonic, MidiChord };
enum class NonScaleNotePolicy : uint8_t { NearestScale, PreserveChromatic, BypassHarmony };
struct HarmonyVoiceConfig { bool enabled=false; float gain=0.5f; float pan=0; float smoothing_ms=50; float interval=0; int degree=0; NonScaleNotePolicy non_scale_policy=NonScaleNotePolicy::NearestScale; bool voice_leading_enabled=false; int min_midi=0; int max_midi=127; };
struct HarmonyVoiceTarget { bool valid=false; float target_frequency_hz=0; float semitone_offset=0; int source_note=0; float deviation_cents=0; };

class HarmonyEngine {
public:
  void reset(); void set_mode(HarmonyMode m); void set_scale(ScaleConfig s);
  void set_root(uint8_t root) { scale_.root=root%12; reset(); }
  void set_scale_type(ScaleType type) { scale_.type=type; reset(); }
  void set_voice(size_t i,const HarmonyVoiceConfig &c);
  HarmonyVoiceTarget update(float source_hz,bool voiced,const MidiChordState &midi);
  const HarmonyVoiceConfig &voice() const { return voice_; }
  // Legacy index form: only voice 0 exists, so index >= 1 is ignored.
  const HarmonyVoiceConfig &voice(size_t i) const { (void)i; return voice_; }
private:
  int identify(float midi); HarmonyVoiceTarget target_for(float midi,int note,const std::array<bool,128>&held);
  HarmonyMode mode_=HarmonyMode::FixedInterval; ScaleConfig scale_{};
  HarmonyVoiceConfig voice_{false,.501187f,-.25f,50,4,2,NonScaleNotePolicy::NearestScale,false,0,127};
  int identity_=0; bool have_identity_=false; float previous_=0;
};
