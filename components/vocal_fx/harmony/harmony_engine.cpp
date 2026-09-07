#include "harmony_engine.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace { float hz_to_midi(float hz){return 69+12*std::log2(hz/440.0f);} float midi_to_hz(float n){return 440*std::exp2((n-69)/12);} }
void HarmonyEngine::reset(){have_identity_=false;previous_={0,0};}
void HarmonyEngine::set_mode(HarmonyMode m){mode_=m;reset();}
void HarmonyEngine::set_scale(ScaleConfig s){s.root%=12;scale_=s;reset();}
void HarmonyEngine::set_voice(size_t i,const HarmonyVoiceConfig &c){if(i<2)voices_[i]=c;}
int HarmonyEngine::identify(float midi) {
  if (!have_identity_) {
    identity_ = mode_ == HarmonyMode::Diatonic
                    ? nearest_scale_note(scale_, midi)
                    : static_cast<int>(std::lround(midi));
    have_identity_ = true;
  }
  if (mode_ != HarmonyMode::Diatonic) {
    // Preserve the existing chromatic 65-cent Schmitt threshold.
    if (std::fabs(midi - identity_) > .65f)
      identity_ = static_cast<int>(std::lround(midi));
    return identity_;
  }

  const int candidate = nearest_scale_note(scale_, midi);
  if (candidate != identity_) {
    // Add 15 cents beyond the midpoint. Unlike distance from the current note,
    // this remains a 30-cent Schmitt band across both semitone and whole-tone
    // gaps in the scale.
    const float midpoint = .5f * static_cast<float>(identity_ + candidate);
    const bool crossed = candidate > identity_ ? midi > midpoint + .15f
                                                : midi < midpoint - .15f;
    if (crossed)
      identity_ = candidate;
  }
  return identity_;
}
HarmonyVoiceTarget HarmonyEngine::target_for(size_t i,float midi,int note,const std::array<bool,128>&held){
 HarmonyVoiceTarget r{}; if(!voices_[i].enabled)return r; float target=0;
 if(mode_==HarmonyMode::FixedInterval)target=midi+voices_[i].interval;
 else if(mode_==HarmonyMode::Diatonic)target=transpose_scale_degrees(scale_,note,voices_[i].degree)+(midi-note);
 else {
   float best=0,cost=std::numeric_limits<float>::max();
   for(int n=0;n<128;++n)if(held[static_cast<size_t>(n)] && ((i==0&&n<note)||(i==1&&n>note))){float c=previous_[i]?std::fabs(n-previous_[i]):std::fabs(n-midi);if(c<cost){cost=c;best=static_cast<float>(n);}}
   if(cost==std::numeric_limits<float>::max()) return r;
   target=best+(midi-note);
 }
 const float hz=midi_to_hz(target); if(!std::isfinite(hz)||hz<60||hz>1500)return r;
 r.valid=true;r.target_frequency_hz=hz;r.semitone_offset=target-midi;r.source_note=note;r.deviation_cents=(midi-note)*100;previous_[i]=target;return r;
}
std::array<HarmonyVoiceTarget,2> HarmonyEngine::update(float hz,bool voiced,const MidiChordState&midi){
 std::array<HarmonyVoiceTarget,2> out{};if(!voiced||!std::isfinite(hz)||hz<=0)return out;float m=hz_to_midi(hz);int note=identify(m);auto held=midi.snapshot();for(size_t i=0;i<2;++i)out[i]=target_for(i,m,note,held);return out;
}
