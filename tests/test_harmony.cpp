#include "harmony_engine.h"
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
 e.set_mode(HarmonyMode::FixedInterval);v.interval=4;e.set_voice(0,v);auto fixed=e.update(220,true,chord)[0];near(fixed.target_frequency_hz,277.183f,.03f);v.interval=7;e.set_voice(1,v);auto two=e.update(220,true,chord);near(two[1].target_frequency_hz,329.628f,.03f);
 chord.note_on(48,100);chord.note_on(52,100);chord.note_on(55,100);e.set_mode(HarmonyMode::MidiChord);auto mt=e.update(hz(52),true,chord);assert(mt[0].valid&&mt[1].valid);near(midi(mt[0].target_frequency_hz),48);near(midi(mt[1].target_frequency_hz),55);chord.all_notes_off();mt=e.update(hz(52),true,chord);assert(!mt[0].valid&&!mt[1].valid);
 std::cout<<"harmony tests passed\n";
}
