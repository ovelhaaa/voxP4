#pragma once
class SmoothedValue {
public:
  void init(float value, float sample_rate, float time_ms = 20.0f);
  void set_target(float value);
  void set_time(float sample_rate, float time_ms);
  float next();
  float current() const { return current_; }
  void reset(float value);

private:
  float current_ = 0, target_ = 0, coefficient_ = 0;
};
