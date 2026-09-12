#pragma once

#ifdef ESP_PLATFORM
#include "soc/gpio_num.h"
#else
// Host test stub definitions
#ifndef GPIO_NUM_19
#define GPIO_NUM_19 19
#define GPIO_NUM_20 20
#define GPIO_NUM_21 21
#define GPIO_NUM_22 22
#define GPIO_NUM_23 23
#define GPIO_NUM_37 37
#define GPIO_NUM_38 38
#endif
#endif

// =============================================================================
// Wireless-Tag WT9932P4-TINY Board Audio Pinout
// =============================================================================
// Header mapping (LEFT HEADER):
// Pin 4   GPIO19   MCLK   ─────► PCM1808 SCKI/SCK
// Pin 5   GPIO20   BCLK   ──┬──► PCM1808 BCK
//                           └──► PCM5102 BCK
// Pin 6   GPIO21   WS/LRCK ─┬──► PCM1808 LRCK
//                           └──► PCM5102 LCK/LRCK
// Pin 7   GPIO22   DOUT   ─────► PCM5102 DIN
// Pin 8   GPIO23   DIN    ◄───── PCM1808 DOUT
// Pin 9   GND      GND    ────── Common Ground
//
// Rules:
// - Do NOT use GPIO37 or GPIO38 for audio.
// - Do NOT use strapping pins.
// - Centralized definition for board audio pins.
// =============================================================================

#define VOXP4_I2S_MCLK_GPIO   (GPIO_NUM_19)
#define VOXP4_I2S_BCLK_GPIO   (GPIO_NUM_20)
#define VOXP4_I2S_WS_GPIO     (GPIO_NUM_21)
#define VOXP4_I2S_DOUT_GPIO   (GPIO_NUM_22)
#define VOXP4_I2S_DIN_GPIO    (GPIO_NUM_23)

// Ground connection reference pin
#define VOXP4_I2S_GND_PIN_DESC "Pin 9 (GND) Common Ground"
