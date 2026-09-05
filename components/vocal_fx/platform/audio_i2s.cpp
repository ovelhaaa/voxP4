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
bool AudioI2s::init(const AudioI2sConfig &c, size_t bs) {
  block_size_ = bs;
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
  s.slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                               I2S_SLOT_MODE_STEREO);
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
  static int32_t in[256 * 2], out[256 * 2];
  static float mono[256], l[256], r[256];
  auto rx = (i2s_chan_handle_t)rx_, tx = (i2s_chan_handle_t)tx_;
  for (;;) {
    size_t got = 0;
    if (i2s_channel_read(rx, in, block_size_ * 2 * sizeof(int32_t), &got,
                         portMAX_DELAY) != ESP_OK)
      continue;
    size_t n = got / (2 * sizeof(int32_t));
    for (size_t i = 0; i < n; i++)
      mono[i] = pcm32_to_float(in[2 * i]);
    vocal_fx_process(mono, l, r, n);
    for (size_t i = 0; i < n; i++) {
      out[2 * i] = float_to_pcm32(l[i]);
      out[2 * i + 1] = float_to_pcm32(r[i]);
    }
    size_t sent;
    i2s_channel_write(tx, out, n * 2 * sizeof(int32_t), &sent, portMAX_DELAY);
  }
#endif
}
} // namespace vocal_fx_platform
