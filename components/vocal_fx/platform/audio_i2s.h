#pragma once
#include <cstddef>
#include <cstdint>
namespace vocal_fx_platform {
enum class PcmWidth : uint8_t { Bits16 = 16, Bits24 = 24, Bits32 = 32 };
struct AudioI2sConfig {
  uint32_t sample_rate = 48000;
  PcmWidth width = PcmWidth::Bits32;
  bool stereo_input = false;
  int bclk_pin = -1, ws_pin = -1, dout_pin = -1, din_pin = -1;
};
class AudioI2s {
public:
  bool init(const AudioI2sConfig &, size_t block_size);
  void run();
  static float pcm32_to_float(int32_t v);
  static int32_t float_to_pcm32(float v);
  static float pcm24_to_float(int32_t left_aligned_v);
  static int32_t float_to_pcm24(float v);
  static float pcm16_to_float(int16_t v);
  static int16_t float_to_pcm16(float v);

private:
  size_t block_size_ = 0;
  PcmWidth width_ = PcmWidth::Bits32;
  bool stereo_input_ = false;
#ifdef ESP_PLATFORM
  void *tx_ = nullptr;
  void *rx_ = nullptr;
#endif
};
} // namespace vocal_fx_platform
