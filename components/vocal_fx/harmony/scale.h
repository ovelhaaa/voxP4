#pragma once
#include <cstdint>

enum class ScaleType : uint8_t {
    Major = 0,
    NaturalMinor = 1,
    HarmonicMinor = 2,
    MelodicMinor = 3,
    Dorian = 4,
    Phrygian = 5,
    Lydian = 6,
    Mixolydian = 7,
    Locrian = 8,
    MajorPentatonic = 9,
    MinorPentatonic = 10,
    BluesMinor = 11,
    Count
};

struct ScaleConfig { uint8_t root = 0; ScaleType type = ScaleType::Major; };

int chromatic_class(int midi_note);
bool scale_contains(const ScaleConfig &scale, int midi_note);
int nearest_scale_note(const ScaleConfig &scale, float midi_note);
int transpose_scale_degrees(const ScaleConfig &scale, int note, int degrees);
