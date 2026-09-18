#include "voxlink_uart.h"

#if defined(ESP_PLATFORM)
#include "esp_timer.h"
#include <cstring>

namespace voxlink {
namespace {
inline uart_port_t to_port(int port) {
  return static_cast<uart_port_t>(port);
}
} // namespace

bool UartTransport::start(const UartConfig &config, Session *session) {
  if (session == nullptr || running_)
    return false;
  config_ = config;
  session_ = session;

  const uart_config_t uart_cfg = {
      .baud_rate = config.baud,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 0,
      .source_clk = UART_SCLK_DEFAULT,
  };
  if (uart_driver_install(to_port(config.port), config.rx_buffer, config.tx_buffer, 0,
                          nullptr, 0) != ESP_OK)
    return false;
  if (uart_param_config(to_port(config.port), &uart_cfg) != ESP_OK)
    return false;
  if (uart_set_pin(to_port(config.port), config.tx_gpio, config.rx_gpio,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK)
    return false;

  running_ = true;
  if (xTaskCreatePinnedToCore(task_entry, "voxlink", 4096, this,
                              config.task_priority, &task_,
                              config.task_core) != pdPASS) {
    running_ = false;
    uart_driver_delete(to_port(config.port));
    return false;
  }
  return true;
}

void UartTransport::stop() {
  if (!running_)
    return;
  running_ = false;
  if (task_ != nullptr) {
    vTaskDelay(pdMS_TO_TICKS(50));
    task_ = nullptr;
  }
  uart_driver_delete(to_port(config_.port));
}

void UartTransport::task_entry(void *arg) {
  static_cast<UartTransport *>(arg)->run();
}

void UartTransport::run() {
  uint8_t rx[256];
  uint8_t tx[512];
  while (running_) {
    const int n = uart_read_bytes(to_port(config_.port), rx, sizeof(rx),
                                  pdMS_TO_TICKS(5));
    if (n > 0)
      session_->feed(rx, static_cast<size_t>(n));
    size_t drained = 0;
    size_t chunk = 0;
    while ((chunk = session_->take_tx(tx, sizeof(tx))) > 0) {
      uart_write_bytes(to_port(config_.port), tx, chunk);
      drained += chunk;
      if (drained >= 4096)
        break;
    }
    session_->tick(static_cast<uint32_t>(esp_timer_get_time() / 1000));
  }
  vTaskDelete(nullptr);
}

} // namespace voxlink
#endif
