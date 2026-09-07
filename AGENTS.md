# Codex agent instructions

This repository targets ESP32-P4 using the pinned ESP-IDF version. The single
machine-readable version pin is `scripts/idf-version.sh`; ESP-IDF v5.3 is kept
because it was already required by this project's firmware CI and supports the
ESP32-P4 standard-mode I2S driver.

Before firmware work, verify the environment with:

```sh
./scripts/check-environment.sh
```

If ESP-IDF is unavailable, run:

```sh
./scripts/setup-codex.sh
```

Build firmware using:

```sh
./scripts/build-p4.sh
```

Do not install or silently switch to another ESP-IDF version.

Before completing firmware changes, run host tests and an ESP32-P4 firmware
build whenever the environment permits.

Host tests do not require ESP-IDF and are run with `make test`. Build scripts
activate ESP-IDF themselves because ephemeral Codex Cloud shells do not share
environment exports. Compilation is supported without attached hardware;
flashing and real-time hardware measurements are not.
