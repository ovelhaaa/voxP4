# VoxP4 Alpha 0.1B — PCM1808 & PCM5102 Hardware Wiring Guide

This document describes the physical wiring and jumper configuration between the **Wireless-Tag WT9932P4-TINY (ESP32-P4)** development board, the **PCM1808 stereo 24-bit ADC**, and the **PCM5102 / PCM5102A stereo DAC**.

---

## 1. Physical Pin Mapping (WT9932P4-TINY Left Header)

```text
WT9932P4-TINY LEFT HEADER

Pin 4   GPIO19   MCLK   ─────► PCM1808 SCKI/SCK
Pin 5   GPIO20   BCLK   ──┬──► PCM1808 BCK
                          └──► PCM5102 BCK

Pin 6   GPIO21   LRCK   ──┬──► PCM1808 LRCK
                          └──► PCM5102 LCK/LRCK

Pin 7   GPIO22   DOUT   ─────► PCM5102 DIN

Pin 8   GPIO23   DIN    ◄───── PCM1808 DOUT

Pin 9   GND      GND    ────── Common Ground (Connect to all GND pins)
```

> [!WARNING]
> **Pin Safety Rules:**
> - Do **NOT** use GPIO37 or GPIO38 for audio signals.
> - Do **NOT** use strapping pins on ESP32-P4.
> - Ensure all boards share a clean common ground (GND).

---

## 2. VERIFY ON MODULE BEFORE POWER-UP

Before connecting power to the breakouts, perform the following electrical verification on the module straps and jumpers using a multimeter or visual inspection:

### 2.1 PCM1808 ADC Module Straps
The PCM1808 must operate as a **Slave in Standard Philips I2S format (24-bit)**:

| Pin / Jumper | Required State | Function / Mode |
|---|---|---|
| **MD1** | **LOW (GND)** | Slave Mode select |
| **MD0** | **LOW (GND)** | Slave Mode select (MD1=0, MD0=0 -> Slave auto-detect) |
| **FMT** | **LOW (GND)** | Format select (FMT=0 -> I2S 24-bit Philips format) |

> [!CAUTION]
> **Check PCB Solder Jumpers on Breakout Boards:**
> Many commercially available purple PCM1808 breakout boards have pull-up resistors on `MD0` or `MD1` placing them into Master mode by default.
> Verify with a multimeter continuity check between MD0/MD1/FMT and GND. They **must be pulled LOW (0V)** so the ESP32-P4 acts as the sole clock master!

### 2.2 Breakout Power Rails (Chip vs Module Board)
- **PCM1808 IC Requirements:**
  - VCC (Analog core): 5.0 V typical (4.5 V – 5.5 V).
  - VDD (Digital logic): 3.3 V typical (2.7 V – 3.6 V).
- **Breakout Board Differences:**
  - Breakouts with an onboard AMS1117-3.3 regulator accept 5V at their `VCC`/`5V` pin and generate the 3.3V digital rail internally.
  - Breakouts without an onboard LDO require separate 5.0V (for analog) and 3.3V (for digital) connections.
  - **Action:** Inspect the breakout board for an onboard voltage regulator before powering on.

---

## 3. PCM5102 / PCM5102A DAC Configuration

The PCM5102 operates in **3-wire I2S mode with internal PLL system clock recovery**:

| Pin / Jumper | Connection | Function |
|---|---|---|
| **BCK** | Connect to WT9932P4 **GPIO20** | Bit Clock input (3.072 MHz) |
| **DIN** | Connect to WT9932P4 **GPIO22** | Serial Audio Data input |
| **LRCK / LCK** | Connect to WT9932P4 **GPIO21** | Left/Right Word Clock (48 kHz) |
| **SCK** | **GND (or NC if onboard bridged)** | Internal PLL clock generation |
| **FMT** | **LOW (GND)** | I2S Audio Format |
| **FLT** | **LOW (GND)** | Normal latency FIR filter |
| **DMP** | **LOW (GND)** | De-emphasis filter disabled |
| **XMT** | **HIGH (3.3V)** | Soft unmute (active high) |

> [!IMPORTANT]
> **Do NOT drive PCM5102 SCK with MCLK:**
> Most PCM5102 breakout boards tie SCK directly to GND on the PCB to force 3-wire operation. Driving SCK from GPIO19 would cause a direct short circuit to ground.
> Therefore: `GPIO19 MCLK -> PCM1808 ONLY`.

---

## 4. Single Clock Domain Summary

```text
                         ESP32-P4 (Master)
                            │
                   MCLK 12.288 MHz (GPIO19)
                            │
                            ▼
                         PCM1808 (SCKI)

ESP32-P4 BCLK 3.072 MHz ────┬────► PCM1808 (BCK)
(GPIO20)                    └────► PCM5102 (BCK)

ESP32-P4 LRCK 48 kHz ───────┬────► PCM1808 (LRCK)
(GPIO21)                    └────► PCM5102 (LCK)

PCM1808 DOUT ────────────────────► ESP32-P4 DIN (GPIO23)

ESP32-P4 DOUT (GPIO22) ──────────► PCM5102 DIN
```

ADC and DAC share BCLK and LRCK. This avoids sample drift and phase jitter between capture and playback.
