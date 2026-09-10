#include "harmony_engine.h"
#include "harmony_articulation_envelope.h"
#include <cassert>
#include <cmath>
#include <iostream>
namespace { float hz(float midi){return 440*std::exp2((midi-69)/12);} float midi(float h){return 69+12*std::log2(h/440);} void near(float a,float b,float e=.02f){assert(std::fabs(a-b)<e);} }
int main(){
 ScaleConfig c{0,ScaleType::Major}; const int notes[]{60,62,64,65,67,69,71}; const int thirds[]{64,65,67,69,71,72,74};
 for(int i=0;i<7;++i){assert(transpose_scale_degrees(c,notes[i],2)==thirds[i]);assert(transpose_scale_degrees(c,thirds[i],-2)==notes[i]);}
 ScaleConfig am{9,ScaleType::NaturalMinor};const int an[]{57,59,60,62,64,65,67};const int at[]{60,62,64,65,67,69,71};for(int i=0;i<7;++i)assert(transpose_scale_degrees(am,an[i],2)==at[i]);
 HarmonyEngine e;HarmonyVoiceConfig v{};v.enabled=true;v.degree=2;e.set_voice(0,v);e.set_scale(c);e.set_mode(HarmonyMode::Diatonic);MidiChordState chord;
 auto t=e.update(hz(62.2f),true,chord)[0];assert(t.valid);near(midi(t.target_frequency_hz),65.2f);near(t.deviation_cents,20,.1f);
 // Hysteresis: movement around the 50-cent boundary must retain C identity.
 e.reset();auto a=e.update(hz(60.49f),true,chord)[0];auto b=e.update(hz(60.51f),true,chord)[0];near(midi(a.target_frequency_hz),64.49f);near(midi(b.target_frequency_hz),64.51f);
 // Across the C-D whole-tone gap, remain C past the midpoint and change only
 // after crossing the upper Schmitt boundary at 61.15.
 e.reset();
 assert(e.update(hz(60.9f), true, chord)[0].source_note == 60);
 assert(e.update(hz(61.1f), true, chord)[0].source_note == 60);
 assert(e.update(hz(61.16f), true, chord)[0].source_note == 62);
 assert(e.update(hz(60.9f), true, chord)[0].source_note == 62);
 assert(e.update(hz(60.84f), true, chord)[0].source_note == 60);
 e.set_mode(HarmonyMode::FixedInterval);v.interval=4;e.set_voice(0,v);auto fixed=e.update(220,true,chord)[0];near(fixed.target_frequency_hz,277.183f,.03f);v.interval=7;e.set_voice(1,v);auto two=e.update(220,true,chord);near(two[1].target_frequency_hz,329.628f,.03f);
 chord.note_on(48,100);chord.note_on(52,100);chord.note_on(55,100);e.set_mode(HarmonyMode::MidiChord);auto mt=e.update(hz(52),true,chord);assert(mt[0].valid&&mt[1].valid);near(midi(mt[0].target_frequency_hz),48);near(midi(mt[1].target_frequency_hz),55);const auto before_clear=chord.generation();assert((before_clear&1U)==0);chord.all_notes_off();assert(chord.generation()==before_clear+2);mt=e.update(hz(52),true,chord);assert(!mt[0].valid&&!mt[1].valid);
 HarmonyArticulationEnvelope articulation;
 HarmonyArticulationConfig articulation_config{};
 articulation.init(1000.0f, articulation_config);
 articulation.begin_block(true, true);
 assert(!articulation.enabled());
 assert(!articulation.keep_target_active());
 articulation_config.enabled=true;
 articulation_config.pitch_loss_grace_ms=40;
 articulation_config.unvoiced_hold_ms=20;
 articulation_config.attack_ms=10;
 articulation_config.release_ms=40;
 articulation_config.onset_min_gain=.2f;
 articulation_config.unvoiced_min_gain=.1f;
 articulation.init(1000.0f,articulation_config);
 articulation.begin_block(true,true);
 for(int i=0;i<10;++i) articulation.process_sample(true,true,false);
 near(articulation.gain(),1.0f,.001f);
 articulation.begin_block(false,false);
 assert(articulation.keep_target_active());
 for(int i=0;i<20;++i) articulation.process_sample(false,false,false);
 assert(articulation.gain()>.99f);
 for(int i=0;i<20;++i) articulation.process_sample(false,false,false);
 for(int i=0;i<40;++i) articulation.process_sample(false,false,false);
 near(articulation.gain(),.1f,.001f);
 articulation.reset();near(articulation.gain(),0,.001f);
 std::cout<<"harmony tests passed\n";
}
