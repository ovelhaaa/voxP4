#pragma once
class Gate {
public:
  void init(float sr);
  void reset();
  void set(float threshold_db, float attack_ms, float hold_ms, float release_ms,
           float range_db);
  float process(float x);
  static float db_to_linear(float d);

private:
  float sr_ = 48000, threshold_ = 0, attack_ = 0, release_ = 0, min_gain_ = 0,
        env_ = 0, gain_ = 1;
  unsigned hold_samples_ = 0, hold_ = 0;
};
