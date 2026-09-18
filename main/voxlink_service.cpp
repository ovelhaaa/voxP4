#include "voxlink_service.h"

#include "sdkconfig.h"

#if defined(CONFIG_VOXLINK_ENABLE_SERVER) && CONFIG_VOXLINK_ENABLE_SERVER
#include "vocal_fx.h"
#include "vocal_fx_param_binding.h"
#include "voxlink_registry.h"
#include "voxlink_session.h"
#include "voxlink_state.h"
#include "voxlink_uart.h"
#include "esp_log.h"

static const char *kVoxlinkTag = "voxlink";

namespace {
// Boot-state coherence: once the engine is initialized, push every registry
// default through the bounded queue so registry, ProductState and effective
// engine targets agree. Runs on the control task, never the audio task.
void seed_defaults_when_ready(void *) {
  static bool seeded = false;
  if (seeded || !vocal_fx_is_ready())
    return;
  if (voxp4::voxlink_seed_defaults()) {
    seeded = true;
    ESP_LOGI(kVoxlinkTag, "registry defaults seeded");
  }
}
} // namespace

namespace {
voxlink::ProductState g_state;
voxlink::Session g_session;
voxlink::UartTransport g_transport;
bool g_started = false;
} // namespace

bool voxlink_service_start() {
  if (g_started)
    return true;

  g_state.init();

  voxlink::SessionConfig cfg;
  // Only implemented modules are advertised. Presets, MIDI and scenes are
  // deliberately absent in v1.
  cfg.capability_flags = voxlink::kCapHarmony | voxlink::kCapCompressor |
                         voxlink::kCapDelay | voxlink::kCapReverb |
                         voxlink::kCapLimiter | voxlink::kCapGate |
                         voxlink::kCapFormantPreservation;
  cfg.sample_rate = 44100;
  cfg.block_size = 64;
  cfg.harmony_voice_count = 1;
  cfg.product_id = 1; // VoxP4
  cfg.hw_target = 1;  // ESP32-P4
  cfg.fw_major = 0;
  cfg.fw_minor = 1;
  cfg.fw_patch = 0;
  cfg.git_sha = nullptr;
  cfg.submit = &voxp4::voxlink_submit;
  cfg.submit_user = nullptr;
  cfg.heartbeat_timeout_ms = 3000;
  cfg.heartbeat_interval_ms = 1000;

  g_session.init(cfg);
  g_session.set_product_state(&g_state);

  voxlink::UartConfig uart;
  uart.port = CONFIG_VOXLINK_UART_PORT;
  uart.baud = CONFIG_VOXLINK_UART_BAUD;
  uart.tx_gpio = CONFIG_VOXLINK_UART_TX_GPIO;
  uart.rx_gpio = CONFIG_VOXLINK_UART_RX_GPIO;
  uart.task_core = 1;
  uart.task_priority = 5;
  uart.tick_hook = &seed_defaults_when_ready;
  uart.tick_user = nullptr;

  // Final pins are intentionally unassigned until verified against the board
  // schematic. Refuse to start rather than risk binding an audio/console pin.
  if (uart.tx_gpio < 0 || uart.rx_gpio < 0) {
    ESP_LOGW(kVoxlinkTag,
             "server enabled but UART pins unassigned (tx=%d rx=%d); not "
             "starting",
             uart.tx_gpio, uart.rx_gpio);
    return false;
  }

  g_started = g_transport.start(uart, &g_session);
  return g_started;
}

#else

bool voxlink_service_start() { return false; }

#endif
