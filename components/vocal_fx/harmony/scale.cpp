#include "scale.h"
#include <array>
#include <cmath>
#include <limits>

namespace {
struct ScaleDefinition {
    const int8_t *intervals;
    uint8_t degree_count;
    uint16_t pitch_class_mask;
};

constexpr int8_t major_intervals[] = {0, 2, 4, 5, 7, 9, 11};
constexpr int8_t natural_minor_intervals[] = {0, 2, 3, 5, 7, 8, 10};
constexpr int8_t harmonic_minor_intervals[] = {0, 2, 3, 5, 7, 8, 11};
constexpr int8_t melodic_minor_intervals[] = {0, 2, 3, 5, 7, 9, 11};
constexpr int8_t dorian_intervals[] = {0, 2, 3, 5, 7, 9, 10};
constexpr int8_t phrygian_intervals[] = {0, 1, 3, 5, 7, 8, 10};
constexpr int8_t lydian_intervals[] = {0, 2, 4, 6, 7, 9, 11};
constexpr int8_t mixolydian_intervals[] = {0, 2, 4, 5, 7, 9, 10};
constexpr int8_t locrian_intervals[] = {0, 1, 3, 5, 6, 8, 10};
constexpr int8_t major_pentatonic_intervals[] = {0, 2, 4, 7, 9};
constexpr int8_t minor_pentatonic_intervals[] = {0, 3, 5, 7, 10};
constexpr int8_t blues_minor_intervals[] = {0, 3, 5, 6, 7, 10};

constexpr uint16_t mask(const int8_t* intervals, uint8_t count) {
    uint16_t m = 0;
    for (uint8_t i = 0; i < count; ++i) m |= (1 << intervals[i]);
    return m;
}

constexpr ScaleDefinition scale_defs[] = {
    {major_intervals, 7, mask(major_intervals, 7)},
    {natural_minor_intervals, 7, mask(natural_minor_intervals, 7)},
    {harmonic_minor_intervals, 7, mask(harmonic_minor_intervals, 7)},
    {melodic_minor_intervals, 7, mask(melodic_minor_intervals, 7)},
    {dorian_intervals, 7, mask(dorian_intervals, 7)},
    {phrygian_intervals, 7, mask(phrygian_intervals, 7)},
    {lydian_intervals, 7, mask(lydian_intervals, 7)},
    {mixolydian_intervals, 7, mask(mixolydian_intervals, 7)},
    {locrian_intervals, 7, mask(locrian_intervals, 7)},
    {major_pentatonic_intervals, 5, mask(major_pentatonic_intervals, 5)},
    {minor_pentatonic_intervals, 5, mask(minor_pentatonic_intervals, 5)},
    {blues_minor_intervals, 6, mask(blues_minor_intervals, 6)},
};

const ScaleDefinition& get_def(ScaleType t) {
    size_t idx = static_cast<size_t>(t);
    if (idx >= std::size(scale_defs)) idx = 0;
    return scale_defs[idx];
}

int floor_div(int a, int b) { int q=a/b, r=a%b; return r < 0 ? q-1 : q; }
}

int chromatic_class(int note) { int r=note%12; return r < 0 ? r+12 : r; }

bool scale_contains(const ScaleConfig &s, int note) {
  const int relative=chromatic_class(note-static_cast<int>(s.root%12));
  return (get_def(s.type).pitch_class_mask & (1 << relative)) != 0;
}

int nearest_scale_note(const ScaleConfig &s, float note) {
  int best=static_cast<int>(std::lround(note)); float cost=std::numeric_limits<float>::max();
  const int center=best;
  for(int n=center-2;n<=center+2;++n) if(scale_contains(s,n)) {
    float c=std::fabs(note-n); if(c<cost){cost=c;best=n;}
  }
  return best;
}

int transpose_scale_degrees(const ScaleConfig &s, int note, int degrees) {
  const auto &def = get_def(s.type);
  int octave=floor_div(note-static_cast<int>(s.root%12),12);
  const int rel=chromatic_class(note-static_cast<int>(s.root%12)); int degree=0;
  for(size_t i=0;i<def.degree_count;++i) if(std::abs(def.intervals[i]-rel)<std::abs(def.intervals[degree]-rel)) degree=static_cast<int>(i);
  const int total=degree+degrees; octave+=floor_div(total,def.degree_count);
  const int wrapped=total-def.degree_count*floor_div(total,def.degree_count);
  return static_cast<int>(s.root%12)+12*octave+def.intervals[static_cast<size_t>(wrapped)];
}
