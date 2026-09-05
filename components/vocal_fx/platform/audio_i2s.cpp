#include "audio_i2s.h"
#include "vocal_fx.h"
#include <algorithm>
#ifdef ESP_PLATFORM
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
namespace vocal_fx_platform {
float AudioI2s::pcm32_to_float(int32_t v) { return (float)v / 2147483648.0f; }
int32_t AudioI2s::float_to_pcm32(float v) {
  v = std::clamp(v, -1.0f, 0.99999994f);
  return (int32_t)(v * 2147483648.0f);
}
float AudioI2s::pcm24_to_float(int32_t v) {
  // ESP-IDF places 24-bit standard-mode samples in the high 24 bits of a
  // 32-bit container.
  return (float)v / 2147483648.0f;
}
int32_t AudioI2s::float_to_pcm24(float v) {
  return float_to_pcm32(v) & (int32_t)0xffffff00;
}
float AudioI2s::pcm16_to_float(int16_t v) { return (float)v / 32768.0f; }
int16_t AudioI2s::float_to_pcm16(float v) {
  v = std::clamp(v, -1.0f, 0.9999695f);
  return (int16_t)(v * 32768.0f);
}
bool AudioI2s::init(const AudioI2sConfig &c, size_t bs) {
  if (bs == 0 || bs > 256)
    return false;
  block_size_ = bs;
  width_ = c.width;
  stereo_input_ = c.stereo_input;
#ifdef ESP_PLATFORM
  i2s_chan_config_t cc =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  i2s_chan_handle_t tx, rx;
  if (i2s_new_channel(&cc, &tx, &rx) != ESP_OK)
    return false;
  tx_ = tx;
  rx_ = rx;
  i2s_std_config_t s = {};
  s.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(c.sample_rate);
  i2s_data_bit_width_t data_width;
  switch (width_) {
  case PcmWidth::Bits16:
    data_width = I2S_DATA_BIT_WIDTH_16BIT;
    break;
  case PcmWidth::Bits24:
    data_width = I2S_DATA_BIT_WIDTH_24BIT;
    break;
  case PcmWidth::Bits32:
    data_width = I2S_DATA_BIT_WIDTH_32BIT;
    break;
  default:
    return false;
  }
  s.slot_cfg =
      I2S_STD_MSB_SLOT_DEFAULT_CONFIG(data_width, I2S_SLOT_MODE_STEREO);
  s.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  s.gpio_cfg.bclk = (gpio_num_t)c.bclk_pin;
  s.gpio_cfg.ws = (gpio_num_t)c.ws_pin;
  s.gpio_cfg.dout = (gpio_num_t)c.dout_pin;
  s.gpio_cfg.din = (gpio_num_t)c.din_pin;
  s.gpio_cfg.invert_flags = {};
  return i2s_channel_init_std_mode(tx, &s) == ESP_OK &&
         i2s_channel_init_std_mode(rx, &s) == ESP_OK &&
         i2s_channel_enable(tx) == ESP_OK && i2s_channel_enable(rx) == ESP_OK;
#else
  (void)c;
  return false;
#endif
}
void AudioI2s::run() {
#ifdef ESP_PLATFORM
  static int16_t in16[256 * 2], out16[256 * 2];
  static int32_t in32[256 * 2], out32[256 * 2];
  static float mono[256], l[256], r[256];
  auto rx = (i2s_chan_handle_t)rx_, tx = (i2s_chan_handle_t)tx_;
  for (;;) {
    size_t got = 0;
    size_t n;
    if (width_ == PcmWidth::Bits16) {
      if (i2s_channel_read(rx, in16, block_size_ * 2 * sizeof(int16_t), &got,
                           portMAX_DELAY) != ESP_OK)
        continue;
      n = got / (2 * sizeof(int16_t));
      for (size_t i = 0; i < n; ++i) {
        const float left = pcm16_to_float(in16[2 * i]);
        const float right = pcm16_to_float(in16[2 * i + 1]);
        mono[i] = stereo_input_ ? (left + right) * 0.5f : left;
      }
    } else {
      if (i2s_channel_read(rx, in32, block_size_ * 2 * sizeof(int32_t), &got,
                           portMAX_DELAY) != ESP_OK)
        continue;
      n = got / (2 * sizeof(int32_t));
      for (size_t i = 0; i < n; ++i) {
        const float left = width_ == PcmWidth::Bits24
                               ? pcm24_to_float(in32[2 * i])
                               : pcm32_to_float(in32[2 * i]);
        const float right = width_ == PcmWidth::Bits24
                                ? pcm24_to_float(in32[2 * i + 1])
                                : pcm32_to_float(in32[2 * i + 1]);
        mono[i] = stereo_input_ ? (left + right) * 0.5f : left;
      }
    }
    vocal_fx_process(mono, l, r, n);
    size_t sent;
    if (width_ == PcmWidth::Bits16) {
      for (size_t i = 0; i < n; ++i) {
        out16[2 * i] = float_to_pcm16(l[i]);
        out16[2 * i + 1] = float_to_pcm16(r[i]);
      }
      i2s_channel_write(tx, out16, n * 2 * sizeof(int16_t), &sent,
                        portMAX_DELAY);
    } else {
      for (size_t i = 0; i < n; ++i) {
        out32[2 * i] = width_ == PcmWidth::Bits24 ? float_to_pcm24(l[i])
                                                  : float_to_pcm32(l[i]);
        out32[2 * i + 1] = width_ == PcmWidth::Bits24 ? float_to_pcm24(r[i])
                                                      : float_to_pcm32(r[i]);
      }
      i2s_channel_write(tx, out32, n * 2 * sizeof(int32_t), &sent,
                        portMAX_DELAY);
    }
  }
#endif
}
} // namespace vocal_fx_platform
