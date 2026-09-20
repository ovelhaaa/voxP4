# VoxLink UART bring-up (M6.2)

First physical link between the WT9932P4-TINY (ESP32-P4) and the ESP32 CYD.
This document is the wiring + procedure reference. It does not change the
VoxLink v1 protocol.

## Pinout

| Signal | ESP32-P4 (WT9932P4-TINY) | ESP32 CYD |
|---|---|---|
| VoxLink TX | GPIO16 | — |
| VoxLink RX | GPIO17 | — |
| VoxLink TX | — | GPIO18 (`VoxUartTx`) |
| VoxLink RX | — | GPIO19 (`VoxUartRx`) |
| Ground | GND | GND |

Configured on the P4 through Kconfig (see `sdkconfig.defaults`):

```text
CONFIG_VOXLINK_ENABLE_SERVER=y
CONFIG_VOXLINK_UART_PORT=1
CONFIG_VOXLINK_UART_BAUD=921600
CONFIG_VOXLINK_UART_TX_GPIO=16
CONFIG_VOXLINK_UART_RX_GPIO=17
```

The CYD side already uses `VoxUartTx=18` / `VoxUartRx=19` at 921600 (SD bus
reused, SD disabled). No CYD pin changes are required.

GPIO16/17 are free header GPIOs; the repo contains no other reference to them.
Audio stays on GPIO19–23 (I2S). GPIO37/38 (console/JTAG) and strapping pins are
not used.

## Wiring (crossed)

```text
P4 GPIO16 (TX) ──▶ CYD GPIO19 (RX)
P4 GPIO17 (RX) ◀── CYD GPIO18 (TX)
P4 GND         ───  CYD GND
```

* 3.3 V UART only. Never connect 5 V to a GPIO.
* Power both boards from their own USB; share only GND + TX/RX.
* Use short wires (< 20–30 cm) for the first test. If unstable at 921600,
  shorten the wires before blaming software.
* Do not use USB Host/VCP in this milestone.

## Boot diagnostics

The P4 prints on boot (control task only, never the audio task):

```text
I (…) voxlink: VoxLink enabled: UART1 TX=16 RX=17 baud=921600
I (…) voxlink: registry defaults seeded
```

If the UART fails to initialise the server logs an error and returns; **the
audio engine keeps running standalone** (no reboot, no audio impact).

Controller boot (DEBUG build) prints the VoxLink handshake milestones in
`docs/m6_voxlink_client.md`.

## Procedure

1. Flash the P4 (server enabled) and the CYD.
2. Wire TX/RX crossed + common GND.
3. Open the P4 and CYD serial monitors.
4. Expect: `HELLO → HELLO_ACK → CAPS (49) → GET_STATE → 49 STATE_PARAM →
   ACTIVE`, then `LINK = ACTIVE` on the CYD System screen.
5. Edits: enables (Harmony/Reverb/Delay/Harmony-limiter), continuous sliders,
   discrete enums, and a no-op write must produce `SET_PARAM → ACK →
   PARAM_CHANGED` and an authoritative value on the P4.
6. Reconnect: remove TX/RX, wait for `LinkDown`, confirm audio continues, then
   reconnect and confirm `HELLO → CAPS → GET_STATE → ACTIVE` (the CYD must not
   replay old state; the P4 snapshot wins).
7. Boot order both ways (P4 first / CYD first) and individual resets must reach
   `ACTIVE` automatically.
8. Stress: with real audio, exercise the UI for >= 10 minutes and confirm no new
   audio underrun/overrun, deadline misses, watchdog, heap leak or crash.

## Diagnostic counters to record

On the CYD: `event_drops`, `ui_queue_drops`, `crc_errors`, `parse_errors`,
`pending`, `queue_full`, `reconnects`. On the P4: audio underrun/overrun,
deadline misses, free/min heap, task high-water marks.

At 921600 with normal wiring, CRC/parse errors must be 0. If they appear,
investigate in order: wiring/GND/length → UART config → parser/client → server →
scheduler. Only then try 460800 manually. There is no auto-baud.

## Status

```text
Software (config, logs, docs, builds, CI):  prepared
Physical tests A–I / measurements:          NOT RUN — no hardware attached
```

The physical acceptance (hardware verified) requires a board and is pending.
