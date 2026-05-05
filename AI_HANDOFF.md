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

## 3. Investigating Low Audio Volume on ES8388 (CURRENTLY UNSOLVED)

**Problem:** 
While audio playback works flawlessly over AirPlay, the absolute volume level on the 3.5mm earphone jack (`LOUT1`/`ROUT1`) is extremely quiet, even when the volume on the source device (iPhone/Mac) is pushed to 100%.

**Investigation & Attempted Fixes:**

1.  **Software Volume Curve (Failed):** 
    *   *Hypothesis:* The `esp_codec_dev` library defaults to a conservative software-to-decibel mapping. 
    *   *Action:* We injected a fake hardware gain compensation (`hw_gain.pa_gain = -15.0`) into the `es8388_codec_cfg_t` struct in `components/dac_es8388/dac_es8388.c`.
    *   *Outcome:* This merely shifted the volume curve. The digital volume reached its absolute maximum (0dB) prematurely when the phone was at 50% volume. Pushing the phone from 50% to 100% resulted in no volume increase, and the absolute maximum volume was still far too quiet. **This change was reverted.**

2.  **Analog Mixer & Output Overrides (Failed):**
    *   *Hypothesis:* The `esp_codec_dev` ES8388 driver initializes the analog mixer and output registers with high attenuation.
    *   *Action:* We examined `es8388_codec.c` inside the ESP-IDF managed components and found that it defaults the analog mixer (`DACCONTROL17`, `DACCONTROL20`) to `-6dB` (`0x90`). It also defaults `LOUT1/ROUT1` (`DACCONTROL24`, `DACCONTROL25`) to `0dB` (`0x1E`).
    *   *Action Taken:* We added manual I2C overrides in `dac_es8388_set_power_mode()` (inside `dac_es8388.c`) right after the codec is enabled:
        *   Overrode Reg `0x11` (DACCONTROL17) and Reg `0x14` (DACCONTROL20) to `0x80` (0dB mixer attenuation).
        *   Overrode Reg `0x2E` (DACCONTROL24) and Reg `0x2F` (DACCONTROL25) to `0x21` (+3dB maximum analog gain for LOUT1/ROUT1).
    *   *Outcome:* Despite forcing the analog stages to their absolute maximum gain settings, the user reports the 3.5mm headphone output is **still extremely quiet at 100% volume.**

**Next Steps for the AI Agent:**
*   The ESP32-A1S AudioKit board has specific hardware routing quirks. Ensure that the I2S digital PCM data being fed to the ES8388 isn't being aggressively scaled down in software *before* it reaches the DAC (check `airplay_get_volume_q15()` or `apply_volume()` in `main/audio/audio_output.c`).
*   Verify the exact I2S data format (16-bit vs 32-bit). If the ESP32 is sending 16-bit samples but the ES8388 is configured to expect 24-bit or 32-bit words, the samples will be shifted down significantly (e.g., a 16-bit sample read as the top 16 bits of a 32-bit word, or vice-versa, depending on alignment), resulting in massive volume loss.
*   Check the ES8388 datasheet to ensure there isn't another global analog attenuation register (like an ALC or DRC setting) squashing the signal.
*   Investigate if the ES8388's internal ADC-to-DAC loopback might be active and interfering with the digital I2S input.