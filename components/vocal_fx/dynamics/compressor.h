#pragma once
class Compressor {
public:
  void init(float sr);
  void reset();
  void set(float threshold_db, float ratio, float attack_ms, float release_ms,
           float makeup_db, float knee_db);
  float process(float x);
  float gain_db_for(float input_db) const;

private:
  float sr_ = 48000, threshold_ = -18, ratio_ = 3, knee_ = 6, makeup_ = 1,
        attack_ = 0, release_ = 0, env_ = 0;
};
