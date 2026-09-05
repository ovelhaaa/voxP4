#include "compressor.h"
#include <algorithm>
#include <cmath>
void Compressor::init(float sr) {
  sr_ = sr;
  set(-18, 3, 10, 100, 3, 6);
  reset();
}
void Compressor::reset() { env_ = 0; }
void Compressor::set(float t, float r, float a, float rel, float m, float k) {
  threshold_ = t;
  ratio_ = std::max(r, 1.0f);
  knee_ = std::max(k, 0.0f);
  makeup_ = std::pow(10.0f, m / 20);
  attack_ = std::exp(-1 / (sr_ * std::max(a, .01f) * .001f));
  release_ = std::exp(-1 / (sr_ * std::max(rel, .01f) * .001f));
}
float Compressor::gain_db_for(float x) const {
  float over = x - threshold_;
  if (knee_ > 0 && over > -knee_ / 2 && over < knee_ / 2) {
    float v = over + knee_ / 2;
    return (1 / ratio_ - 1) * v * v / (2 * knee_);
  }
  return over > 0 ? (1 / ratio_ - 1) * over : 0;
}
float Compressor::process(float x) {
  float p = std::fabs(x);
  float c = p > env_ ? attack_ : release_;
  env_ = p + (env_ - p) * c;
  float db = 20 * std::log10(std::max(env_, 1e-12f));
  return x * std::pow(10.0f, gain_db_for(db) / 20) * makeup_;
}
