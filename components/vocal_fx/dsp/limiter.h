#pragma once
class Limiter {
public:
  void init(float sr, float ceiling = .98f);
  void reset();
  void set_ceiling(float v);
  void process(float &l, float &r);

private:
  float ceiling_ = .98f, gain_ = 1, release_ = .999f;
};
