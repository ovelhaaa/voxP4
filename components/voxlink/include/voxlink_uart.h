#pragma once
// ESP32-P4 UART transport for the VoxLink server. This is the only part of the
// control plane that touches hardware. The RX/TX task is deliberately isolated
// from the realtime audio task: it never locks, allocates or calls into DSP.
#include "voxlink_session.h"
#include <cstdint>

#if defined(ESP_PLATFORM)
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace voxlink {

struct UartConfig {
  int port = 1; // UART_NUM_1
  int baud = 921600;
  int tx_gpio = 20;
  int rx_gpio = 21;
  size_t rx_buffer = 4096;
  size_t tx_buffer = 8192;
  int task_core = 1;
  int task_priority = 5;
};

class UartTransport {
public:
  bool start(const UartConfig &config, Session *session);
  void stop();

private:
#if defined(ESP_PLATFORM)
  static void task_entry(void *arg);
  void run();
  UartConfig config_{};
  Session *session_ = nullptr;
  TaskHandle_t task_ = nullptr;
  volatile bool running_ = false;
#endif
};

} // namespace voxlink
