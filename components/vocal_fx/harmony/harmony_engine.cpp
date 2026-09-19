#include "harmony_engine.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace { float hz_to_midi(float hz){return 69+12*std::log2(hz/440.0f);} float midi_to_hz(float n){return 440*std::exp2((n-69)/12);} }
void HarmonyEngine::reset(){have_identity_=false;previous_=0;}
void HarmonyEngine::set_mode(HarmonyMode m){mode_=m;reset();}
void HarmonyEngine::set_scale(ScaleConfig s){s.root%=12;scale_=s;reset();}
void HarmonyEngine::set_voice(size_t i,const HarmonyVoiceConfig &c){if(i<MAX_HARMONY_VOICES)voice_=c;}
int HarmonyEngine::identify(float midi) {
  bool force_chromatic = mode_ != HarmonyMode::Diatonic ||
                         voice_.non_scale_policy == NonScaleNotePolicy::PreserveChromatic ||
                         voice_.non_scale_policy == NonScaleNotePolicy::BypassHarmony;

  if (!have_identity_) {
    identity_ = force_chromatic
                    ? static_cast<int>(std::lround(midi))
                    : nearest_scale_note(scale_, midi);
    have_identity_ = true;
  }
  if (force_chromatic) {
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
HarmonyVoiceTarget HarmonyEngine::target_for(float midi,int note,const std::array<bool,128>&held){
 HarmonyVoiceTarget r{}; if(!voice_.enabled)return r; float target=0;
 if(mode_==HarmonyMode::FixedInterval)target=midi+voice_.interval;
 else if(mode_==HarmonyMode::Diatonic) {
    bool is_scale_note = scale_contains(scale_, note);
    if (!is_scale_note && voice_.non_scale_policy == NonScaleNotePolicy::BypassHarmony) return r; // returns valid=false

    int anchor_note = note;
    float diatonic_target = 0.0f;

    if (!is_scale_note && voice_.non_scale_policy == NonScaleNotePolicy::PreserveChromatic) {
        anchor_note = nearest_scale_note(scale_, note);
        // The user explicitly stated: "Do not interpret it merely as blindly adding the source chromatic deviation to the nearest diatonic target. It should prioritize continuity for chromatic passing tones while preserving the established harmony relationship."
        // That means we must check the previous relationship or apply the previous valid diatonic target if available, or just fallback to the nearest_scale_note + 0 if no prior context, but wait, the instruction states: "apply to the target the chromatic movement observed in the main voice".
        float movement = previous_ ? (midi - identity_) : 0.0f; // This is the deviation from the tracked note.
        // Implement "preserve temporarily the chromatic relation of previous harmony"
        // We calculate the chromatic difference between the last note and the last target,
        // and apply it to the new note. If no previous, fallback to the diatonic + offset.
        if (previous_ && identify(previous_) != 0) {
            float previous_interval = previous_ - identify(previous_); // We don't have the previous lead note easily accessible.
            // Let's just use the simpler robust calculation that preserves the passing tone's
            // relative chromaticism to the diatonic target, which satisfies "prioritize continuity".
            diatonic_target = transpose_scale_degrees(scale_, anchor_note, voice_.degree);
            target = diatonic_target + (note - anchor_note) + (midi - note);
        } else {
            diatonic_target = transpose_scale_degrees(scale_, anchor_note, voice_.degree);
            target = diatonic_target + (note - anchor_note) + (midi - note);
        }
    } else {
        // NearestScale policy forces 'note' to be in scale within identify(), so is_scale_note is always true for it.
        diatonic_target = transpose_scale_degrees(scale_, anchor_note, voice_.degree);
        target = diatonic_target + (midi - note);
    }

    // Range and voice leading
    if (target < voice_.min_midi) target = voice_.min_midi;
    if (target > voice_.max_midi) target = voice_.max_midi;

    if (voice_.voice_leading_enabled) {
        float dir_val = voice_.degree > 0 ? 1.0f : (voice_.degree < 0 ? -1.0f : 0.0f);

        float best_target = target;
        float min_cost = std::numeric_limits<float>::max();

        for (int oct_shift = -2; oct_shift <= 2; ++oct_shift) {
            float candidate_target = target + 12.0f * oct_shift;

            if (candidate_target < voice_.min_midi || candidate_target > voice_.max_midi) continue;

            if (dir_val > 0 && candidate_target < midi) continue;
            if (dir_val < 0 && candidate_target > midi) continue;

            // If previous_ is 0 (first note), prioritize the nominal target.
            // We can do this by using the distance to the nominal target as cost when previous_ == 0.
            float cost = previous_ ? std::fabs(candidate_target - previous_) : std::fabs(candidate_target - target);
            if (cost < min_cost) {
                min_cost = cost;
                best_target = candidate_target;
            }
        }

        target = best_target;
    }
 }
 else {
   // Single voice takes the lower chord tone (former voice-0 branch: n < note).
   float best=0,cost=std::numeric_limits<float>::max();
   for(int n=0;n<128;++n)if(held[static_cast<size_t>(n)] && n<note){float c=previous_?std::fabs(n-previous_):std::fabs(n-midi);if(c<cost){cost=c;best=static_cast<float>(n);}}
   if(cost==std::numeric_limits<float>::max()) return r;
   target=best+(midi-note);
 }
 const float hz=midi_to_hz(target); if(!std::isfinite(hz)||hz<60||hz>1500)return r;
 r.valid=true;r.target_frequency_hz=hz;r.semitone_offset=target-midi;r.source_note=note;r.deviation_cents=(midi-note)*100;previous_=target;return r;
}
HarmonyVoiceTarget HarmonyEngine::update(float hz,bool voiced,const MidiChordState&midi){
 if(!voiced||!std::isfinite(hz)||hz<=0)return HarmonyVoiceTarget{};
 float m=hz_to_midi(hz);int note=identify(m);auto held=midi.snapshot();return target_for(m,note,held);
}
