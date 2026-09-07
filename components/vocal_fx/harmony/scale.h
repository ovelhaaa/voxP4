#pragma once
#include <cstdint>

enum class ScaleType : uint8_t { Major, NaturalMinor };
struct ScaleConfig { uint8_t root = 0; ScaleType type = ScaleType::Major; };

int chromatic_class(int midi_note);
bool scale_contains(const ScaleConfig &scale, int midi_note);
int nearest_scale_note(const ScaleConfig &scale, float midi_note);
int transpose_scale_degrees(const ScaleConfig &scale, int note, int degrees);
