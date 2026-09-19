#include "harmony_engine.h"
#include "harmony_articulation_envelope.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

namespace {
    float hz(float midi){return 440*std::exp2((midi-69)/12);}
    float midi(float h){return 69+12*std::log2(h/440);}
    void near(float a,float b,float e=.02f){if (std::fabs(a-b)>=e) { std::cerr << "Expected " << a << " to be near " << b << std::endl; assert(false); }}
}

void test_regression_old_behavior() {
    ScaleConfig c{0,ScaleType::Major};
    HarmonyEngine e;
    HarmonyVoiceConfig v{};
    v.enabled=true;
    v.degree=2; // Third above
    v.non_scale_policy = NonScaleNotePolicy::NearestScale;
    v.voice_leading_enabled = false;
    e.set_voice(0,v);
    e.set_scale(c);
    e.set_mode(HarmonyMode::Diatonic);
    MidiChordState chord;

    // C Major, third above. Notes: C D E F G A B -> E F G A B C D
    const int leads[] = {60, 62, 64, 65, 67, 69, 71};
    const int expected[] = {64, 65, 67, 69, 71, 72, 74};
    for (int i=0; i<7; ++i) {
        auto t = e.update(hz(leads[i]), true, chord);
        assert(t.valid);
        near(midi(t.target_frequency_hz), expected[i]);
    }
}

void test_scale_contains() {
    ScaleConfig c{0, ScaleType::Major};
    assert(scale_contains(c, 60)); // C
    assert(!scale_contains(c, 61)); // C#
    assert(scale_contains(c, 62)); // D
    assert(scale_contains(c, 64)); // E
    assert(scale_contains(c, 65)); // F
    assert(scale_contains(c, 67)); // G
    assert(scale_contains(c, 69)); // A
    assert(scale_contains(c, 71)); // B
    assert(!scale_contains(c, 70)); // Bb
}

void test_all_roots() {
    for (int root=0; root<12; ++root) {
        ScaleConfig c{static_cast<uint8_t>(root), ScaleType::Major};
        assert(scale_contains(c, root + 60));
        assert(scale_contains(c, root + 60 + 2));
        assert(scale_contains(c, root + 60 + 4));
        assert(scale_contains(c, root + 60 + 5));
        assert(scale_contains(c, root + 60 + 7));
        assert(scale_contains(c, root + 60 + 9));
        assert(scale_contains(c, root + 60 + 11));
    }
}

void test_transposition_crossing() {
    ScaleConfig c{0, ScaleType::Major};
    // +14 degrees = 2 octaves up
    assert(transpose_scale_degrees(c, 60, 14) == 84);
    // -14 degrees = 2 octaves down
    assert(transpose_scale_degrees(c, 60, -14) == 36);
    // Transpose from B (71) + 1 degree = C (72)
    assert(transpose_scale_degrees(c, 71, 1) == 72);
}

void test_specific_scales() {
    // Harmonic Minor (A)
    ScaleConfig hm{9, ScaleType::HarmonicMinor};
    assert(scale_contains(hm, 57)); // A
    assert(scale_contains(hm, 59)); // B
    assert(scale_contains(hm, 60)); // C
    assert(scale_contains(hm, 62)); // D
    assert(scale_contains(hm, 64)); // E
    assert(scale_contains(hm, 65)); // F
    assert(scale_contains(hm, 68)); // G#
    assert(!scale_contains(hm, 67)); // G natural not in harmonic minor

    // Dorian (D)
    ScaleConfig dorian{2, ScaleType::Dorian};
    assert(scale_contains(dorian, 62)); // D
    assert(scale_contains(dorian, 64)); // E
    assert(scale_contains(dorian, 65)); // F
    assert(scale_contains(dorian, 67)); // G
    assert(scale_contains(dorian, 69)); // A
    assert(scale_contains(dorian, 71)); // B
    assert(scale_contains(dorian, 72)); // C

    // Blues (C)
    ScaleConfig blues{0, ScaleType::BluesMinor};
    assert(scale_contains(blues, 60)); // C
    assert(scale_contains(blues, 63)); // Eb
    assert(scale_contains(blues, 65)); // F
    assert(scale_contains(blues, 66)); // F#
    assert(scale_contains(blues, 67)); // G
    assert(scale_contains(blues, 70)); // Bb
}

void test_cents_preservation() {
    ScaleConfig c{0, ScaleType::Major};
    HarmonyEngine e;
    HarmonyVoiceConfig v{};
    v.enabled=true;
    v.degree=2; // Third above
    e.set_voice(0,v);
    e.set_scale(c);
    e.set_mode(HarmonyMode::Diatonic);
    MidiChordState chord;

    // +20 cents
    auto t = e.update(hz(60.2f), true, chord);
    near(midi(t.target_frequency_hz), 64.2f);
    near(t.deviation_cents, 20.0f, 0.1f);

    // -25 cents
    t = e.update(hz(64.0f - 0.25f), true, chord); // E - 25 cents
    near(midi(t.target_frequency_hz), 67.0f - 0.25f);
}

void test_non_scale_policies() {
    ScaleConfig c{0, ScaleType::Major};
    HarmonyEngine e;
    HarmonyVoiceConfig v{};
    v.enabled=true;
    v.degree=2; // Third above
    e.set_scale(c);
    e.set_mode(HarmonyMode::Diatonic);
    MidiChordState chord;

    // 1. NearestScale (Default)
    v.non_scale_policy = NonScaleNotePolicy::NearestScale;
    e.set_voice(0,v);
    e.reset();
    auto t1 = e.update(hz(61.0f), true, chord); // C# -> Nearest is C (60) or D (62).
    // Since 61 is exactly in the middle, lround rounds to nearest even for float, but nearest_scale_note uses lround.
    // Actually, C# (61) distance to C (60) is 1, distance to D (62) is 1. nearest_scale_note scans from best-2 to best+2.
    // It finds C first or D first?
    // Wait, let's test C + 40 cents.

    // NearestScale: C# (61) mapped to C(60) or D(62). Let's use 61.2 (closer to D).
    auto t_nearest = e.update(hz(61.2f), true, chord);
    // Anchor is D (62). Target from D is F (65).
    // Target = 65 + (61.2 - 62) = 65 - 0.8 = 64.2
    near(midi(t_nearest.target_frequency_hz), 64.2f);

    // 2. PreserveChromatic
    v.non_scale_policy = NonScaleNotePolicy::PreserveChromatic;
    e.set_voice(0,v);
    e.reset();
    // Use an unambiguous pitch: D# (63). Nearest is E (64) or D (62). Both 1 semitone away.
    // nearest_scale_note prefers lower note if equidistant? Let's trace.
    // best = 63. scale_contains(s, 63-1=62). c=1. cost=1. best=62.
    // scale_contains(s, 63+1=64). c=1. cost=1 < 1 is false. So best=62.
    // Anchor = 62. Diatonic target (degree +2) from 62 is 65 (F).
    // note = 63. anchor = 62. offset = +1. Target = 65 + 1 = 66 (F#).
    // Let's test F# (66). Nearest is G (67) or F (65).
    // F# (66): center=66. n=65, c=1, cost=1, best=65. n=67, c=1, cost=1 (not <1). best=65.
    // Anchor = 65. Diatonic (+2) from F (65) is A (69).
    // note=66. anchor=65. offset=+1. Target = 69 + 1 = 70 (A#).
    auto t_preserve = e.update(hz(66.0f), true, chord);
    near(midi(t_preserve.target_frequency_hz), 70.0f);

    // 3. BypassHarmony
    v.non_scale_policy = NonScaleNotePolicy::BypassHarmony;
    e.set_voice(0,v);
    e.reset();
    auto t_bypass = e.update(hz(63.0f), true, chord);
    assert(!t_bypass.valid);
}

void test_voice_leading() {
    ScaleConfig c{0, ScaleType::Major};
    HarmonyEngine e;
    HarmonyVoiceConfig v{};
    v.enabled=true;
    v.degree=2; // Third above
    v.voice_leading_enabled=true;
    e.set_voice(0,v);
    e.set_scale(c);
    e.set_mode(HarmonyMode::Diatonic);
    MidiChordState chord;

    // First note: C (60) -> E (64)
    auto t1 = e.update(hz(60.0f), true, chord);
    near(midi(t1.target_frequency_hz), 64.0f);

    // Next note: G (67). Third above is B (71).
    // But voice leading might prefer B (59) if it was closer to 64.
    // However, degree=2 means it MUST be ABOVE the lead (67).
    // So B (59) is invalid because 59 < 67.
    // So it must be B (71) or B (83), etc. B (71) is closer.
    auto t2 = e.update(hz(67.0f), true, chord);
    near(midi(t2.target_frequency_hz), 71.0f);

    // Range test
    v.max_midi = 70; // Max note allowed is A# (70)
    e.set_voice(0,v);
    auto t3 = e.update(hz(67.0f), true, chord);
    // Best target is restricted to 70 before voice leading.
    // So the target starts as 70.0.
    // The loop won't find better valid targets because degree > 0 requires target >= 67.
    // And shift by 12 from 71 is 59 (invalid since < 67) or 83 (invalid since > 70).
    // So it stays at the clamped target 70.
    near(midi(t3.target_frequency_hz), 70.0f);

    // Let's test a downward degree.
    v.degree = -2; // Third below
    v.max_midi = 127;
    e.set_voice(0,v);
    e.reset();

    // C (72) -> A (69)
    auto t4 = e.update(hz(72.0f), true, chord);
    near(midi(t4.target_frequency_hz), 69.0f);

    // Next note: F (65). Third below is D (62).
    // Candidate D (74) is above lead (65), invalid.
    auto t5 = e.update(hz(65.0f), true, chord);
    near(midi(t5.target_frequency_hz), 62.0f);
}

int main(){
    test_regression_old_behavior();
    test_scale_contains();
    test_all_roots();
    test_transposition_crossing();
    test_specific_scales();
    test_cents_preservation();
    test_non_scale_policies();
    test_voice_leading();

    ScaleConfig c{0,ScaleType::Major}; const int notes[]{60,62,64,65,67,69,71}; const int thirds[]{64,65,67,69,71,72,74};
    for(int i=0;i<7;++i){assert(transpose_scale_degrees(c,notes[i],2)==thirds[i]);assert(transpose_scale_degrees(c,thirds[i],-2)==notes[i]);}
    ScaleConfig am{9,ScaleType::NaturalMinor};const int an[]{57,59,60,62,64,65,67};const int at[]{60,62,64,65,67,69,71};for(int i=0;i<7;++i)assert(transpose_scale_degrees(am,an[i],2)==at[i]);
    HarmonyEngine e;HarmonyVoiceConfig v{};v.enabled=true;v.degree=2;e.set_voice(0,v);e.set_scale(c);e.set_mode(HarmonyMode::Diatonic);MidiChordState chord;
    auto t=e.update(hz(62.2f),true,chord);assert(t.valid);near(midi(t.target_frequency_hz),65.2f);near(t.deviation_cents,20,.1f);
    e.reset();auto a=e.update(hz(60.49f),true,chord);auto b=e.update(hz(60.51f),true,chord);near(midi(a.target_frequency_hz),64.49f);near(midi(b.target_frequency_hz),64.51f);
    e.reset();
    assert(e.update(hz(60.9f), true, chord).source_note == 60);
    assert(e.update(hz(61.1f), true, chord).source_note == 60);
    assert(e.update(hz(61.16f), true, chord).source_note == 62);
    assert(e.update(hz(60.9f), true, chord).source_note == 62);
    assert(e.update(hz(60.84f), true, chord).source_note == 60);
    e.set_mode(HarmonyMode::FixedInterval);v.interval=4;e.set_voice(0,v);auto fixed=e.update(220,true,chord);near(fixed.target_frequency_hz,277.183f,.03f);
    chord.note_on(48,100);chord.note_on(52,100);chord.note_on(55,100);e.set_mode(HarmonyMode::MidiChord);auto mt=e.update(hz(52),true,chord);assert(mt.valid);near(midi(mt.target_frequency_hz),48);const auto before_clear=chord.generation();assert((before_clear&1U)==0);chord.all_notes_off();assert(chord.generation()==before_clear+2);mt=e.update(hz(52),true,chord);assert(!mt.valid);
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
    return 0;
}
