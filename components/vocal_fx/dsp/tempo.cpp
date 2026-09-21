#include "tempo.h"

float tempo_subdivision_ratio(TempoSubdivision subdivision) {
  switch (subdivision) {
  case TempoSubdivision::Whole:
    return 4.0f;
  case TempoSubdivision::Half:
    return 2.0f;
  case TempoSubdivision::Quarter:
    return 1.0f;
  case TempoSubdivision::Eighth:
    return 0.5f;
  case TempoSubdivision::Sixteenth:
    return 0.25f;
  case TempoSubdivision::ThirtySecond:
    return 0.125f;
  case TempoSubdivision::DottedHalf:
    return 3.0f;
  case TempoSubdivision::DottedQuarter:
    return 1.5f;
  case TempoSubdivision::DottedEighth:
    return 0.75f;
  case TempoSubdivision::DottedSixteenth:
    return 0.375f;
  case TempoSubdivision::TripletQuarter:
    return 2.0f / 3.0f;
  case TempoSubdivision::TripletEighth:
    return 1.0f / 3.0f;
  case TempoSubdivision::TripletSixteenth:
    return 1.0f / 6.0f;
  default:
    return 1.0f;
  }
}

float tempo_subdivision_ms(float bpm, TempoSubdivision subdivision) {
  const float clamped_bpm = tempo_clamp_bpm(bpm);
  const float quarter_ms = 60000.0f / clamped_bpm;
  return quarter_ms * tempo_subdivision_ratio(subdivision);
}

const char *tempo_subdivision_name(TempoSubdivision subdivision) {
  switch (subdivision) {
  case TempoSubdivision::Whole:
    return "1/1";
  case TempoSubdivision::Half:
    return "1/2";
  case TempoSubdivision::Quarter:
    return "1/4";
  case TempoSubdivision::Eighth:
    return "1/8";
  case TempoSubdivision::Sixteenth:
    return "1/16";
  case TempoSubdivision::ThirtySecond:
    return "1/32";
  case TempoSubdivision::DottedHalf:
    return "1/2D";
  case TempoSubdivision::DottedQuarter:
    return "1/4D";
  case TempoSubdivision::DottedEighth:
    return "1/8D";
  case TempoSubdivision::DottedSixteenth:
    return "1/16D";
  case TempoSubdivision::TripletQuarter:
    return "1/4T";
  case TempoSubdivision::TripletEighth:
    return "1/8T";
  case TempoSubdivision::TripletSixteenth:
    return "1/16T";
  default:
    return "unknown";
  }
}
