<div align="center">

<img src="docs/logo_airplay_esp32.png" alt="airplay-ESP32" width="320">

# ESP32 AirPlay 2 Receiver

**An AirPlay 2 speaker for ~10 $: ESP32-S3 + a PCM5102A DAC.**<br>
Tight multiroom sync, sub-second seek, clean transitions, and a USB speaker for your PC.

[![License](https://img.shields.io/badge/license-Non--Commercial-blue?style=flat-square)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.5-red?style=flat-square)](https://docs.espressif.com/projects/esp-idf/)
[![Chip](https://img.shields.io/badge/chip-ESP32--S3-green?style=flat-square)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Fork of](https://img.shields.io/badge/fork%20of-rbouteiller%2Fairplay--esp32-lightgrey?style=flat-square)](https://github.com/rbouteiller/airplay-esp32)

</div>

---

## Why this fork

This fork started from [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32) v0.2.0
and reworks the parts you hear: timing, transitions and session handling.

| | Measured on the board |
|---|---|
| 🎯 **In sync** | first sample within tens of µs of the sender's schedule (PTP) |
| ⏩ **Fast seek** | sound 0.4–0.9 s after a seek, resumes at the exact spot |
| ⚡ **Quick start** | 1.7 s from tapping the speaker to sound (a TCL TV takes 2.2 s) |
| 🔇 **Clean transitions** | 150 ms fade-in on start and resume, 100 ms fade-out on pause, no click on play |
| 🖥️ **USB speaker** | plug it into a PC: it becomes its sound card, and can wake it |
| 🛡️ **Stays up** | 4 h stress run: 0 dropouts, 0 underruns; recovers from WiFi loss on its own |

## Quick start

```bash
git clone --recursive https://github.com/MisterEdward/airplay-esp32.git
cd airplay-esp32
pio run -e esp32s3 -t upload && pio run -e esp32s3 -t uploadfs
```

On first boot, join the **ESP32-AirPlay-Setup** WiFi, enter your network, and the
speaker shows up in AirPlay. Later updates go over WiFi:

```bash
curl -X POST --data-binary @.pio/build/esp32s3/firmware.bin http://<speaker-ip>/api/ota/update
```

## Hardware

<img src="docs/ESP_PCM_front.png" alt="ESP32-S3 with a PCM5102A DAC" width="220" align="right">

- ESP32-S3 dev board (16 MB flash, 8 MB PSRAM)
- PCM5102A I²S DAC board
- Any powered speakers or amplifier

No soldering needed: the DAC plugs onto the ESP32. Wiring, other boards (SqueezeAMP,
Esparagus), displays and buttons: see the [original README](docs/UPSTREAM-README.md).

<br clear="right">

## Docs

| | |
|---|---|
| [`docs/FABLE.md`](docs/FABLE.md) | what changed versus upstream, flashing, test plan |
| [`docs/FABLE-GUIDE.md`](docs/FABLE-GUIDE.md) | how the AirPlay 2 path works, how to measure, known issues |
| [`scripts/fable/stress`](scripts/fable/stress) | stress harness: scripted sender, USB capture, report |
| [`docs/UPSTREAM-README.md`](docs/UPSTREAM-README.md) | the original project's full documentation |

## Credits & license

Built on [airplay-esp32](https://github.com/rbouteiller/airplay-esp32) by **Rémi Bouteiller**
and its contributors, which in turn builds on [Shairport Sync](https://github.com/mikebrady/shairport-sync)
and [openairplay/airplay2-receiver](https://github.com/openairplay/airplay2-receiver).

**Non-commercial use only**, see [LICENSE](LICENSE) (© 2026 Rémi Bouteiller).
Not affiliated with Apple Inc.
