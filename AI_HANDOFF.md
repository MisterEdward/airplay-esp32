# Debugging and Development Handoff

This document summarizes the debugging efforts and code changes made to the `airplay-esp32` project up to this point. It is intended to serve as a comprehensive context handoff for the next AI agent continuing this work.

## 1. Fixed RTSP Decryption Failure (AirPlay Transient Pairing)

**Problem:** 
The user reported that the AirPlay connection would fail immediately upon connecting. The device logs showed an error: `rtsp_crypto: Failed to decrypt frame: len=158 nonce=0 pending=0`.

**Investigation:**
*   AirPlay 2 uses transient pairing (skipping M1-M6 pair-setup and pair-verify) for standard audio streaming, jumping straight to deriving session keys from a 64-byte SRP shared secret.
*   The key derivation for the RTSP control channel uses HKDF-SHA512.
*   The project used a custom `hap_hkdf_sha512` implementation in `main/hap/hap_crypto.c` built on top of `libsodium`'s `crypto_auth_hmacsha512`.
*   Testing the custom implementation against standard HKDF output revealed it was generating incorrect output keys (`Control-Read-Encryption-Key` and `Control-Write-Encryption-Key`) when the Input Keying Material (IKM) was 64 bytes long. Because the keys were wrong, ChaCha20-Poly1305 decryption of the very first RTSP `SETUP` packet failed.

**Resolution:**
*   Replaced the buggy custom libsodium implementation in `main/hap/hap_crypto.c` with a robust, manual HKDF-SHA512 implementation using the standard `mbedtls_md_hmac_*` primitives (`mbedtls_md_hmac_starts`, `mbedtls_md_hmac_update`, `mbedtls_md_hmac_finish`). 
*   *(Note: We couldn't use `mbedtls_hkdf` directly because the `CONFIG_MBEDTLS_HKDF_C` option is not enabled by default in the project's ESP-IDF SDK configuration).*
*   **Result:** The derived keys now match the AirPlay specification, and the audio stream connects and plays successfully.

---

## 2. Automated SPIFFS Filesystem Upload (Web UI 404 Error)

**Problem:** 
After successfully connecting and trying to access the device's IP in a browser, the user encountered a "404 Page Not Found" error for the setup page.

**Investigation:**
*   The web server (`main/network/web_server.c`) serves HTML files (like the captive portal `index.html`) from a SPIFFS partition mounted at `/spiffs`.
*   The raw HTML files are stored in the `data/www/` directory of the project.
*   Running the standard `pio run -t upload` command only flashes the application firmware, leaving the SPIFFS partition completely empty.

**Resolution:**
*   Created a PlatformIO extra script at `scripts/uploadfs_hook.py` that hooks into the "upload" post-action.
*   Modified `platformio.ini` to include `extra_scripts = post:scripts/uploadfs_hook.py` in the common `[env]` section.
*   **Result:** Whenever `pio run -e <env> -t upload` is executed, PlatformIO now automatically runs the `uploadfs` target immediately afterward, ensuring the Web UI is always deployed alongside the firmware.

---

## 3. Low Audio Volume on ES8388 (FIX APPLIED, NEEDS HARDWARE LISTEN TEST)

**Problem:** 
While audio playback works flawlessly over AirPlay, the absolute volume level on the 3.5mm earphone jack (`LOUT1`/`ROUT1`) is extremely quiet, even when the volume on the source device (iPhone/Mac) is pushed to 100%.

**Investigation & Attempted Fixes:**

1.  **Software Volume Curve (Failed):** 
    *   *Hypothesis:* The `esp_codec_dev` library defaults to a conservative software-to-decibel mapping. 
    *   *Action:* We injected a fake hardware gain compensation (`hw_gain.pa_gain = -15.0`) into the `es8388_codec_cfg_t` struct in `components/dac_es8388/dac_es8388.c`.
    *   *Outcome:* This merely shifted the volume curve. The digital volume reached its absolute maximum (0dB) prematurely when the phone was at 50% volume. Pushing the phone from 50% to 100% resulted in no volume increase, and the absolute maximum volume was still far too quiet. **This change was reverted.**

2.  **Analog Mixer & Output Overrides (Original Attempt Failed):**
    *   *Hypothesis:* The `esp_codec_dev` ES8388 driver initializes the analog mixer and output registers with high attenuation.
    *   *Action:* We examined `es8388_codec.c` inside the ESP-IDF managed components and found that it defaults the analog mixer (`DACCONTROL17`, `DACCONTROL20`) to `-6dB` (`0x90`). It also defaults `LOUT1/ROUT1` (`DACCONTROL24`, `DACCONTROL25`) to `0dB` (`0x1E`).
    *   *Action Taken:* We added manual I2C overrides in `dac_es8388_set_power_mode()` (inside `dac_es8388.c`) right after the codec is enabled:
        *   Overrode Reg `0x11` (DACCONTROL17) and Reg `0x14` (DACCONTROL20) to `0x80` (0dB mixer attenuation).
        *   Overrode Reg `0x2E` (DACCONTROL24) and Reg `0x2F` (DACCONTROL25) to `0x21` (+3dB maximum analog gain for LOUT1/ROUT1).
    *   *Outcome:* This attempt had a register-address bug. It meant to write `DACCONTROL17` and `DACCONTROL20`, but wrote `0x11` and `0x14`. In the local `esp_codec_dev` register map, the correct addresses are `0x27` and `0x2A`.

**Resolution Applied:**
*   Updated `components/dac_es8388/dac_es8388.c`.
*   Changed the ES8388 default volume from `-12 dB` to `0 dB`.
*   Clamped ES8388 volume requests to the codec's supported `-96..0 dB` range.
*   Corrected mixer override writes to `0x27` / `0x2A`.
*   Raised both output pairs to `0x21` (`+3 dB`): `LOUT1/ROUT1` and `LOUT2/ROUT2`. This matters because ESP32-A1S board revisions can route the physical jack differently.
*   Verified with `~/.platformio/penv/bin/pio run -e esp32-a1s`: build succeeds.

**Next Step:**
*   Flash and listen-test the earphone jack. If it is still too quiet, the remaining likely cause is hardware-level output/load behavior rather than AirPlay scaling or I2S format.

**Follow-up Volume Scale Tuning:**
*   The first fix made the jack too loud.
*   Updated `components/dac_es8388/dac_es8388.c` again:
    *   AirPlay minimum (`-30 dB`, source 0%) now forces ES8388 mute.
    *   AirPlay volume now uses a piecewise ES8388 curve:
        *   0% is silent.
        *   50% stays around ES8388 `-30 dB`.
        *   100% now reaches ES8388 `-6 dB`.
    *   Resulting intent: 50% remains comfortable, while max volume has more headroom.
*   Verified again with `~/.platformio/penv/bin/pio run -e esp32-a1s`: build succeeds.

---

## 4. HTTP Sound Alerts / Alarm Sounds

**Goal:**
Add local sound alerts that can be triggered over HTTP and mixed over AirPlay 2 playback or played while AirPlay is idle.

**Implementation Applied:**
*   Added `main/audio/audio_alert.c` and `main/audio/audio_alert.h`.
*   Alert sound is a single firmware-builtin `chime`.
*   The chime plays embedded mono PCM generated into `main/audio/chime_pcm.c`.
*   Mixed alerts into the normal I2S output path in `main/audio/audio_output.c`.
*   If AirPlay is playing, alert audio is added over the current PCM.
*   If AirPlay is idle, alert audio is mixed over the silence stream and powers the DAC on for the alert.
*   Added HTTP endpoints in `main/network/web_server.c`:
    *   `GET /api/alert/list`
    *   `GET /api/alert/play?name=chime&volume=10&repeat=1`
    *   `GET /api/alert/stop`
*   Only accepted sound name: `chime`.
*   Registered `audio_alert_init()` during app startup.
*   Verified with `~/.platformio/penv/bin/pio run -e esp32-a1s`: build succeeds.
*   OTA firmware artifact: `.pio/build/esp32-a1s/firmware.bin`.

---

## 5. Mac System AirPlay Audio Sync / Static Handoff

This section is a handoff for debugging the ESP32-A1S / ES8388 AirPlay 2 audio behavior.

### Hardware / Board

* Board: Ai-Thinker ESP32-A1S.
* Codec: ES8388.
* User says the board is v2.2 / version 1.
* Output is speakers connected to the A1S audio path.
* OTA is normally done from the Web UI.
* NVS is not believed to be the root cause. The bug depends on sender path and codec/stream mode.

### User-Observed Behavior

* iPhone / phone playback is basically perfect.
* Apple Music on Mac can sometimes be perfect.
* MacBook system audio tray AirPlay output is consistently problematic.
* Safari / YouTube / HBO Max through the macOS system audio tray has:
  * audible jitter / drift-like artifacts,
  * sometimes audio behind video by roughly 100 ms,
  * periodic sample drop / repeat feeling,
  * static-like distortion in bad firmware attempts.
* If the user selects AirPlay from Safari's video player / YouTube player itself, playback is good.
* Safari player seems to hold video briefly, then syncs cleanly.
* macOS system tray does not appear to do that same helpful buffering/sync behavior.
* Scrubbing / seeking can cause short silence, catch-up, another silence, then normal playback.

### Important Stream Difference Found in Logs

The bad path and good path choose different AirPlay stream types.

Bad path:

* macOS system audio tray selects stream `type=96`.
* Codec: ALAC.
* Transport: realtime UDP.
* Observed format: `sr=44100`, `spf=352`, `ct=2`.

Good path:

* Safari in-player / phone often selects stream `type=103`.
* Codec: AAC / AAC-ELD path.
* Transport: buffered TCP.
* Observed format: `sr=44100`, `spf=1024`, `ct=4`.

This means the current evidence does not point to a 48 kHz vs 44.1 kHz mismatch for these logs.
Both bad and good samples reported 44.1 kHz.

### What Codex Tried

#### APLL / I2S Clock

* A previous APLL-related patch was tried before this handoff.
* It caused severe static / no audio / speaker boom on connect.
* It was reverted.
* Recommendation: do not reintroduce APLL changes casually on ESP32-A1S + ES8388.

#### Realtime Timing Rebase

* Tried bypassing / rebasing realtime `type=96` frames that appeared too early.
* This reduced some waiting but did not fix the tray audio.
* It made behavior feel no better, sometimes worse.
* Reverted back to the safer timing behavior.

#### Advertising Buffered `type=103` to macOS Tray

* Tried advertising both `type=96` and `type=103` in `/info`.
* Tried changing RAOP TXT `tp` from `UDP` to `UDP,TCP`.
* macOS system tray still selected `type=96` in observed logs.
* This did not help and was reverted.

#### One-Sample Slip / Local Buffer Servo

* Tried dropping / duplicating one PCM sample based on realtime buffer depth.
* Logs showed repeated `Realtime buffer high: frames=..., dropping 1 sample`.
* User then reported only static.
* This was reverted immediately.
* Recommendation: do not use crude sample slip in `audio_output.c`.

### Current Codex Changes Left In Repo

The remaining changes are diagnostic-only.

* `main/rtsp/rtsp_handlers.c`
  * Logs `/info` advertised formats and latency.
  * Logs SETUP stream type, codec type, sample rate, samples per frame, key lengths.
  * Logs SETRATEANCHORTIME rate with more precision.
* `main/audio/audio_receiver.c`
  * Logs active decoded audio format.
* `main/audio/audio_output.c`
  * Logs source-rate changes.
* `main/audio/audio_resample.c`
  * Warns if source rate differs from output rate while the 44.1 kHz build has resampling compiled out.
* `main/audio/audio_timing.c`
  * Promoted early-frame timing logs to `INFO` with consecutive count and buffer state.

Build verified:

```bash
~/.platformio/penv/bin/pio run -e esp32-a1s
```

Result:

```text
SUCCESS
```

Firmware artifact:

```text
.pio/build/esp32-a1s/firmware.bin
```

### Current Best Hypotheses

1. The bug is mainly in the realtime UDP `type=96` path, not the decoder in general.
2. Buffered TCP `type=103` works because it has deeper sender/receiver buffering and different pacing.
3. macOS system tray may be sending a realtime stream that needs a real clock servo, not local pending-frame waits or crude sample dropping.
4. PTP/anchor math may be slightly wrong or too noisy for the realtime path.
5. The receiver may need better realtime packet-gap, NACK, underrun, buffer-depth, and playout-rate telemetry before another audio-changing patch.
6. ES8388/I2S hardware is probably not the main root cause because phone / Safari-player buffered audio can sound clean.
7. The loud static after the last attempt was almost certainly caused by the one-sample slip experiment, not by WiFi reconnect alone.

### Recommended Next Step

Do not start with another sound-changing fix.

First add low-risk diagnostics for the realtime `type=96` path:

* packet sequence gaps,
* retransmission request count,
* late packet count,
* buffer underruns,
* buffer depth min/max/average over 1-second windows,
* RTP timestamp delta per packet,
* actual I2S write cadence,
* PTP lock status and offset jitter over time.

Then compare:

* Mac system tray YouTube.
* Safari YouTube player AirPlay picker.
* iPhone Apple Music / YouTube.

Only after that, attempt a proper realtime correction.
The likely fix should be a controlled rate servo / resampler approach, not dropping individual samples in the output callback.

### Important Cleanup Note

Logs currently show SRP / encryption debug dumps such as:

* `DBG S`
* `DBG K`
* `DBG srp_K`
* `DBG enc(Read)`
* `DBG dec(Write)`

These leak key material and flood logs.
They should be removed or hidden behind a compile-time debug option.
This is not likely the audio bug, but it makes debugging harder and is unsafe.
