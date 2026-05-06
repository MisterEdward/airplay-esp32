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
