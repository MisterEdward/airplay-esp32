# Spec: USB Audio Input Source + PC Wake — `airplay-esp32`

**Repo:** `/Users/edward/airplay-esp32-v020` — WIP, v0.2.0, baseline HEAD `4a045aa`
**Target:** ESP32-S3 + external I2S DAC (PCM5102A), permanently USB-connected to a Windows PC
**Env:** `esp32s3` (PlatformIO default), ESP-IDF ≥ 5.5

Every claim in §2 was verified by reading the source at that commit and is cited `file:line`. Where something was **derived by reasoning rather than observed running**, it is labelled **[derived]** — treat those as high-confidence predictions to confirm empirically, not as measurements.

---

## §0 — Prime directive: the trap that will waste your first day

The repo **already contains a `CONFIG_AUDIO_OUTPUT_USB` backend** at `main/audio/audio_output_usb.c`. **It is the opposite of this task.** Its own header comment (`audio_output_usb.c:2-4`):

> "The ESP32 enumerates as a USB **microphone** (audio input device) on the connected host. Decoded AirPlay audio is fed to the host via the UAC input callback."

and `audio_output_usb.c:169-170`:

```c
.output_cb = NULL,        /* not a speaker — no host-to-device audio */
.input_cb = usb_input_cb, /* device-to-host: AirPlay audio out */
```

That feature is **AirPlay → USB host**. Your task is the reverse: **PC → USB → ESP32 → I2S DAC**, with the ESP32 appearing to Windows as a **speaker**.

Rules that follow:

1. **Do not modify `audio_output_usb.c`.** It is a separate, working feature with its own users.
2. **Do not touch the `CONFIG_AUDIO_OUTPUT_USB` Kconfig branch**, nor the `elseif(CONFIG_AUDIO_OUTPUT_USB)` arm in `main/CMakeLists.txt:64`.
3. What you build is **not an output backend**. It is a **new audio source**, sibling to AirPlay and Bluetooth A2DP, that writes into the existing `CONFIG_AUDIO_OUTPUT_I2S` backend (`main/audio/audio_output.c`).
4. Acceptance criterion **A12** exists solely to prove you respected this. It is not optional.

If you find yourself editing anything under `AUDIO_OUTPUT_USB`, stop — you have taken a wrong turn.

---

## §1 — Goal

One device, permanently plugged into a desktop PC, that is simultaneously:

- an **AirPlay 2 receiver** over WiFi — existing functionality, must not regress;
- a **USB speaker** for the PC — driverless, low latency, good enough for gaming;
- a **wake device** that can turn the PC on from sleep or from fully off.

Both sources feed one I2S DAC. **Success means zero software installed on Windows** — the PC sees a standard USB sound card and nothing else.

The bar is "works every time, without hesitation". A design that works for twenty minutes and then clicks is a failed design, not a partial success. §3 is where that bar is actually won or lost; read it before writing code.

---

## §2 — Verified facts

### 2.1 What already exists and must be reused

| Capability | Evidence | Why it matters |
|---|---|---|
| **Second-source precedent** | `main/audio/a2dp_sink.c` (whole file) | Bluetooth A2DP is already a second source that takes the I2S output away from AirPlay. **This is your architectural template.** Read it before designing anything. |
| Ring-buffer → I2S writer task | `a2dp_sink.c:171-190` (`bt_i2s_writer_task`) | Exact shape of the task you need: `xRingbufferReceiveUpTo` → `audio_output_write` → silence on underrun. |
| Non-blocking producer from a foreign context | `a2dp_sink.c:250-260` (`bt_a2dp_data_cb`) | Shows the ringbuffer handle snapshot pattern that avoids a use-after-free race against `i2s_task_stop`. You need the same discipline (see ADR-4). |
| Source arbiter enum | `main/playback_control.h:19-23` | `playback_source_t { NONE, AIRPLAY, BLUETOOTH }` + `playback_control_set_source()`. Extend it; do not invent a parallel arbiter. |
| Write path for non-AirPlay sources | `audio_output.h:58-70`, impl `audio_output.c:389` | Documented as "Can be used by any audio source (BT A2DP, etc.)". |
| Start/stop of the AirPlay writer task | `audio_output.c:360-386` | `audio_output_start()` / `audio_output_stop()`. Stop is cooperative and **polls at 50 ms granularity, up to 2 s**. |
| Runtime I2S sample-rate change | `audio_output.c:396-407` | Works, but carries an explicit precondition — see 2.3. |
| **Measured DMA queue depth** | `audio_output.c:82-90` (`audio_output_on_sent`), `:436-452` (`audio_output_get_pipeline_us`) | A real hardware completion cursor driven by the TX DMA ISR. This is your clock-loop error signal. Do not build another. |
| Underrun counter | `audio_output.c:454-456` | Already exposed and already correct. |
| Zipper-free gain ramp | `audio_output.c:145-172` (`apply_volume`) | Exponential approach, ~3 ms constant, stepped per stereo frame. Reuse the approach for your fades. |
| Sinc resampler, 44.1→48 | `components/audio-resampler`, `main/audio/audio_resample.c`, wired at `audio_output.c:355` | First-class, Kconfig-tunable (`RESAMPLER_TAPS`). Central to ADR-2. |
| UAC component **with the speaker direction already built** | `managed_components/espressif__usb_device_uac/` | See 2.2 — this is much further along than it looks. |
| Metrics pattern | `main/airplay_metrics.h` | The established way this project exposes diagnostics. |

### 2.2 The UAC component — what is actually there

This is the part most likely to be mis-estimated in both directions. Precisely:

| Fact | Evidence |
|---|---|
| The speaker (host→device) callback exists in the public API | `include/usb_device_uac.h:28` — `uac_output_cb_t output_cb` |
| **The S3 build already configures a stereo speaker interface** | `sdkconfig.defaults.esp32s3:10-12` — `CONFIG_UAC_SPEAKER_CHANNEL_NUM=2`, `CONFIG_UAC_MIC_CHANNEL_NUM=2`, `CONFIG_UAC_SAMPLE_RATE=48000` |
| The descriptors already enumerate a speaker streaming interface | `tusb_uac/uac_descriptors.h:37-44` — `ITF_NUM_AUDIO_STREAMING_SPK` |
| **The isochronous feedback endpoint is enabled** | `tusb_uac/tusb_config_uac.h:22` — `CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP 1` |
| A feedback method is already registered | `usb_device_uac.c:196-205` — `tud_audio_feedback_params_cb()` sets `AUDIO_FEEDBACK_METHOD_FIFO_COUNT` |
| TinyUSB version | `espressif__tinyusb` **0.19.0~3** (`idf_component.yml:27`) |
| Manual feedback override is available | `class/audio/audio_device.h:316` — `tud_audio_n_fb_set()`; `:443` — `tud_audio_fb_set()` |
| Full-speed only on this target | `sdkconfig.esp32s3:2565` — `CONFIG_TINYUSB_RHPORT_FS=y` |
| Feedback format correction is OFF | `CONFIG_UAC_SUPPORT_MACOS` not set → `CFG_TUD_AUDIO_ENABLE_FEEDBACK_FORMAT_CORRECTION` undefined (`tusb_config_uac.h:24-26`) |
| Composite mode exists but changes ownership | `Kconfig:3` `USB_DEVICE_UAC_AS_PART`; `CMakeLists.txt:13-25` injects the component's own `tusb_config.h` + `usb_descriptors.c` into the tinyusb lib **only when it is off** |
| The component claims the bus callbacks — but only in non-composite mode | `usb_device_uac.c:87-127`, all inside `#if !CONFIG_USB_DEVICE_UAC_AS_PART` |
| Dependency already declared for S2/S3/P4 | `main/idf_component.yml:14-18` |

**Net:** the USB speaker endpoint is already enumerated and wired to a callback that is simply `NULL`. The protocol layer is not the work. The work is everything in §3.

### 2.3 Constraints in the existing code that will bite

| Constraint | Evidence | Consequence |
|---|---|---|
| `audio_output_set_sample_rate()` is not concurrency-safe | `audio_output.c:396-398` comment: *"Only safe to call when no writer task is actively using I2S"* | Rate changes must be sequenced: stop writer → change rate → start writer. |
| `audio_output_get_pipeline_us()` and `..._hardware_latency_us()` divide by the **compile-time** `OUTPUT_RATE` | `audio_output.c:451`, `:429-434` | If you switch the I2S rate at runtime, both report wrong values and the AirPlay timing servo is fed a lie. **This is a strong argument for ADR-2.** |
| `apply_volume()` reads `airplay_get_volume_q15()` unconditionally | `audio_output.c:147,151` | It lives inside the AirPlay playback task. Audio written via `audio_output_write()` bypasses it entirely — A2DP relies on that. Your USB path gets **no volume** unless you apply your own. |
| DMA ring is 8 × 256 = **2048 frames** | `audio_output.c:46-47` | 42.7 ms at 48 kHz. Modelled steady-state latency `(2·8−1)·256/2 = 1920` frames = **40.0 ms**. This is the dominant term in your gaming latency. See ADR-8. |
| `auto_clear = true` with a documented 46 ms stall tolerance | `audio_output.c:288-298` | The ring is deliberately deep to survive NVS/flash cache-disable stalls and web-server CPU bursts. Shrinking it trades AirPlay robustness for USB latency — a real tradeoff, not free. |
| `audio_output_stop()` polls at 50 ms | `audio_output.c:378-380` | Minimum switch cost ~50 ms; worst case 2 s before it gives up and logs a warning. |
| **`CONFIG_FREERTOS_HZ=100`** | `sdkconfig.esp32s3:1671` | One tick = **10 ms**. `vTaskDelay(1)` is 10 ms. Any tick-based pacing on a 1 ms USB deadline is useless — everything on the USB path must be event-driven (task notifications), never delay-driven. |
| WiFi is pinned to core 0 | `sdkconfig.esp32s3:1553` — `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y` | Good news for ADR-7. |
| **lwIP TCP/IP task has no affinity** | `sdkconfig.esp32s3:1920-1923` — `CONFIG_LWIP_TCPIP_TASK_AFFINITY_NO_AFFINITY=y` | It floats onto core 1 and *will* land on top of your USB tasks. Pinning WiFi alone is not enough. See ADR-7. |
| Audio tasks are on core 1 | `audio_output.c:56-58` (`PLAYBACK_CORE 1`), priority doctrine at `audio_output.h:9-24` | Playback = 9, realtime UDP = 8, control = 7, buffered TCP = 5, A2DP I2S writer = 7 (`a2dp_sink.c:58`). |

### 2.4 The AirPlay disconnect signal is not what it looks like

`RTSP_EVENT_DISCONNECTED` is emitted from three sites in `rtsp_server.c` (`:348`, `:353`, `:358`), but for **AirPlay v1 sessions with a DACP remote** (i.e. iOS over legacy RAOP) it is gated behind a grace period (`rtsp_server.c:284-341`):

- Phase 1: 3 s settle.
- Phase 2: re-probe `dacp_probe_service()` every 5 s and **keep waiting for as long as the phone still advertises DACP** — potentially indefinitely.

For **v2 (AirPlay 2) sessions there is no grace period** — `rtsp_server.c:356-359`, *"v2 / unknown — no grace period, clear immediately."*

Meanwhile the audio path is torn down **immediately** at socket close, before any of that: `audio_receiver_stop()` and `audio_output_flush()` at `rtsp_server.c:280-282`, and `RTSP_EVENT_PAUSED` is emitted at `:295`.

**[derived]** Therefore an arbiter gated purely on `RTSP_EVENT_DISCONNECTED` will hold the output hostage — silently, producing nothing — for the entire v1 grace period, which can be unbounded while the phone is on the same WiFi. That is a hard deadlock of PC audio from the user's point of view. See ADR-6.

---

## §3 — Architecture decision record

Each decision states the reasoning. If you disagree with one, say so and argue it **before** implementing something else — do not silently diverge.

### ADR-1 — Build a new *source* module, not an output backend

`main/CMakeLists.txt:58-66` selects exactly one output backend (`if SPDIF / elseif USB / else I2S`). The three are mutually exclusive by construction and all implement the same `audio_output.h` API.

USB-in is not a backend: it is a **producer**, and the I2S backend stays the consumer. Model it on `a2dp_sink.c`, which is a producer that writes through `audio_output_write()`.

Create `main/audio/usb_audio_source.{c,h}`, gated on a new Kconfig `CONFIG_USB_AUDIO_SOURCE`, appended to `SRC_FILES` unconditionally of the backend choice but guarded so it can only be enabled alongside `CONFIG_AUDIO_OUTPUT_I2S`.

> **Rationale.** Any other shape either forks the backend selection (breaking SPDIF and the existing USB-mic build) or duplicates the I2S driver. The producer/consumer split already exists and is proven by A2DP.

### ADR-2 — Run the entire device at a fixed 48 000 Hz. Never switch the I2S rate at runtime.

**Decision:** set `CONFIG_OUTPUT_SAMPLE_RATE_48000=y`. AirPlay's 44.1 kHz is resampled to 48 kHz by the existing sinc resampler. USB runs natively at 48 kHz. The I2S clock is configured once at init and never reconfigured.

Four independent reasons, any one of which would be sufficient:

**(a) The UAC component's rate arithmetic is only exact at multiples of 1000 Hz.**

`usb_device_uac.c:322` computes
```c
spk_bytes_per_ms = current_sample_rate / 1000 * SPEAK_CHANNEL_NUM * BYTES_PER_SAMPLE;
```
with integer division, and `tud_audio_rx_done_isr` (`:361`) drains **exactly that many bytes** from the TinyUSB EP-OUT FIFO on every received frame.

At 48 000: `48000/1000 = 48` exactly → 192 bytes/ms → drain rate == nominal rate. Correct.

At 44 100: `44100/1000 = 44` → 176 bytes/ms → an effective drain of **44 000 Hz**. **[derived]** Because `AUDIO_FEEDBACK_METHOD_FIFO_COUNT` regulates the host's send rate to whatever keeps that FIFO half full (`audio_device.c:530-531`, `:1205-1220`), the loop settles where arrival equals drain — i.e. it asks the host for 44 000 samples/s while the DAC consumes 44 100. That is a permanent 100 samples/s deficit, ~2270 ppm, and it sits exactly at the clamp `min_value = ((44100-1)/1000)<<16 = 44.0` (`audio_device.c:1195`), so the regulator has no headroom left to correct. A ~10 ms buffer runs dry roughly every 4 seconds, forever.

This is not currently a live bug — `CONFIG_UAC_SAMPLE_RATE` is already 48000 — but it is a landmine directly under the "just match AirPlay's 44.1 kHz" idea, which is otherwise the obvious first instinct. **Never set `CONFIG_UAC_SAMPLE_RATE` to a value that is not a multiple of 1000.**

**(b) The project's own maintainers already reached this conclusion.**

`main/Kconfig.projbuild:198-201`:
```
config OUTPUT_SAMPLE_RATE_HZ
    default 48000 if AUDIO_OUTPUT_USB
```
and the rate choice at `:187` is `depends on ... && !AUDIO_OUTPUT_USB` — i.e. when USB is involved, 48 kHz is forced and the user is not allowed to pick. `sdkconfig.defaults.esp32s3:7-9` says it in prose: *"Keep both streams stereo at 48 kHz so the UAC descriptors, feedback, and AirPlay USB backend all use the same resampled output rate."*

**(c) Runtime rate switching corrupts the AirPlay timing engine.**

`audio_output_get_pipeline_us()` divides the measured queue depth by the compile-time `OUTPUT_RATE` (`audio_output.c:451`). Switch the hardware to 48 kHz while `OUTPUT_RATE` is 44100 and every pipeline measurement is off by 8.8%, feeding a systematic bias straight into `compute_early_us()` and the position servo. Fixing that properly means threading a live rate through the timing engine — a much larger and riskier change than resampling 44.1→48.

**(d) Switching costs a mute, a disable/enable, and a pop, on every source change.**

`audio_output_set_sample_rate()` does `i2s_channel_disable` → reconfig → `i2s_channel_enable` (`:401-405`) and requires no writer to be running. A fixed rate removes the entire failure class.

**Cost, stated honestly:** the AirPlay path gains a sinc resample on every frame — CPU on core 1, and a small quality cost. The path is already supported and Kconfig-tunable (`RESAMPLER_TAPS`, `Kconfig.projbuild:205`). Measure the CPU headroom and report it (§7.3). If it turns out to be prohibitive, come back and argue — do not silently switch to runtime rate changes.

### ADR-3 — `CONFIG_USB_DEVICE_UAC_AS_PART=y` is mandatory

In non-composite mode the component owns the USB identity: `CMakeLists.txt:13-25` injects its own `tusb_config.h` and `usb_descriptors.c` into the TinyUSB library, and `usb_device_uac.c:87-127` defines `tud_mount_cb`, `tud_umount_cb`, `tud_suspend_cb` and `tud_resume_cb` — all inside `#if !CONFIG_USB_DEVICE_UAC_AS_PART`.

Composite mode is required for **four separate requirements**, not one:

1. **HID interface for wake** (FR-4.1) — impossible while the component owns the descriptor set.
2. **`tud_suspend_cb` / `tud_mount_cb` for PC power-state detection** (FR-4.3) — the component takes those symbols otherwise.
3. **The remote-wakeup attribute** (`bmAttributes` bit 5) in the configuration descriptor (FR-1.5) — you must author the descriptor to set it.
4. **Device identity** — `CONFIG_UAC_TUSB_VID/PID/PRODUCT` are all inside `if !USB_DEVICE_UAC_AS_PART` (`Kconfig:3-40`), so in composite mode you write them yourself anyway. The current defaults are `0x303A:0x8000`, `"Espressif"`, `"ESP UAC Device"` (`sdkconfig.esp32s3:2560-2564`).

So: the project supplies `tusb_config.h` and `usb_descriptors.c`, calls `tusb_init()` itself, passes `skip_tinyusb_init = true` plus `spk_itf_num` / `mic_itf_num` to `uac_device_init()` (`usb_device_uac.h:34-36`, consumed at `usb_device_uac.c:398-401`).

> Start from the component's own `tusb/usb_descriptors.c` and `tusb/tusb_config.h` as the base and add the HID function — do not write descriptors from scratch.

**Set `CONFIG_UAC_MIC_CHANNEL_NUM=0` in this build.** The mic direction belongs to the other feature (§0) and is dead weight here: it costs two `CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ` buffers (`usb_device_uac.c:48-49`), an interface, an endpoint, and a task.

### ADR-4 — `output_cb` must be memcpy-only. This is a correctness requirement, not a style preference.

Trace the speaker path:

- `tud_audio_rx_done_isr` (`usb_device_uac.c:340-366`) runs **in ISR context**, reads into the single shared buffer `s_uac_device->spk_buf` (`:52`), sets `spk_data_size`, and notifies the task.
- `usb_spk_task` (`:406-424`) wakes, calls `output_cb(spk_buf, spk_data_size, ctx)`, then zeroes `spk_data_size`.

**There is exactly one `spk_buf` and no handshake.** **[derived]** If `output_cb` has not returned before the next isochronous packet arrives — 1 ms later — the ISR overwrites the buffer underneath it. The result is torn audio with no error, no counter, and no log line.

Therefore:

- `output_cb` **must** do nothing but copy into your own ring buffer and return. Budget well under 1 ms.
- **It must never call `audio_output_write()`**, which wraps `i2s_channel_write()` and blocks until DMA space frees — on a 42.7 ms ring that is a multi-millisecond block by design.
- A separate writer task drains your ring into `audio_output_write()`, exactly like `bt_i2s_writer_task` (`a2dp_sink.c:171-190`).

Calling `audio_output_write()` from `output_cb` is the single most likely mistake in this project, because it is the shortest path to hearing sound and it appears to work at low load.

Also copy the handle-snapshot discipline from `bt_a2dp_data_cb` (`a2dp_sink.c:250-255`): `output_cb` can fire after teardown has begun, so snapshot the ringbuffer handle once and null-check the snapshot.

### ADR-5 — Feedback: verify what the existing loop actually observes before changing it

The good news from 2.2: `CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP` is on, the descriptor carries the endpoint, and `tud_audio_feedback_params_cb` registers `AUDIO_FEEDBACK_METHOD_FIFO_COUNT`. Asynchronous operation is structurally present.

The concern: `AUDIO_FEEDBACK_METHOD_FIFO_COUNT` regulates **TinyUSB's EP-OUT FIFO** (`audio_device.c:530-531`), and that FIFO is drained by the component at a fixed nominal rate per frame (ADR-2a), *not* by your DAC. **[derived]** The quantity it stabilises may therefore be blind to the real sink clock: if your I2S consumes slightly slower than the host produces, the surplus accumulates **downstream** of the FIFO — in your ring buffer — where the regulator cannot see it.

At 48 kHz the nominal rates match exactly, so the loop is at least not *biased*; whether it actually tracks DAC drift is the open question.

**Do this, in order:**

1. **Measure first.** Instrument your ring-buffer fill level and `audio_output_get_pipeline_us()` and log both over 60 minutes of continuous playback. If the fill level is stationary, the existing loop is closing correctly on the real sink and **you are done — change nothing**.
2. **If it drifts**, switch to manual feedback: implement your own `tud_audio_feedback_params_cb` returning `AUDIO_FEEDBACK_METHOD_DISABLED`, and drive `tud_audio_n_fb_set()` (`audio_device.h:316`) from a slow PI controller whose error term is your ring fill level plus `audio_output_get_pipeline_us()`. Feedback is in 16.16 samples-per-frame; nominal at 48 kHz is `48 << 16`. TinyUSB clamps to `[47.0, 49.0]` (`audio_device.c:1195-1196`), i.e. ±2%, which is far more authority than any real crystal drift needs.
3. **Do not use `AUDIO_FEEDBACK_METHOD_FREQUENCY_*`.** It requires an MCLK cycle counter locked to the sample clock read from the SOF ISR (`audio_device.h:284-291`). The S3 has no APLL for I2S (see ADR-6 note) and no such counter, and the doc itself warns of long-term accumulated drift.

**Format:** leave `CONFIG_UAC_SUPPORT_MACOS` **off**. TinyUSB's own comment (`audio_device.h:302-305`) states Windows requires 16.16 for all USB 2.0 devices, while macOS wants 10.14 on full speed. Off = 16.16 = correct for the target host. If macOS support is ever wanted, override the weak `tud_audio_feedback_format_correction_cb()` (`audio_device.h:357`) rather than flipping the Kconfig — but that is out of scope here.

> **Note on why hardware clock trimming is not an option:** `soc_caps.h` for esp32s3 defines `SOC_I2S_SUPPORTS_PLL_F160M` and does **not** define `SOC_I2S_SUPPORTS_APLL` — which is exactly why `audio_output_spdif.c:289-290` guards its APLL use on that macro. There is no fractional-divider trim available on this target. Rate control must live either in the host (feedback) or in software (resampling).

### ADR-6 — Arbitration: AirPlay is absolute; a lost transport is bounded

**Policy (as specified by the user):** while an AirPlay session is connected, AirPlay owns the output — **including while paused or idle**. USB audio is consumed and discarded, never queued.

**Why USB activity cannot be the arbitration signal.** Windows keeps a selected output device open and streams continuous silence to it. `spk_active` (`usb_device_uac.c:318`) and `CONFIG_UAC_SPK_NEW_PLAY_INTERVAL` will therefore report "playing" essentially permanently. Any arbiter keyed on USB liveness is keyed on a constant. AirPlay session state is the only signal carrying information.

**Why the precedent already agrees with the user.** `main.c:194-197` handles `RTSP_EVENT_PAUSED` by keeping Bluetooth suspended: *"Session still active — BT stays suspended and hidden so the phone reconnects to AirPlay rather than falling back to BT."* That is exactly the requested semantics, already implemented for the other source. Mirror `on_airplay_client_event` (`main.c:174-206`) rather than inventing a new state machine.

**The bound, and why it is required.** Per 2.4, `RTSP_EVENT_DISCONNECTED` can be delayed indefinitely for v1 sessions while the phone still advertises DACP. A normal pause is intentional and must retain ownership indefinitely. A closed transport is different: the sender is no longer delivering audio, so PC audio must not remain blocked by the v1 DACP grace period. Therefore add a distinct transport-close event at the socket-close site:

```
AIRPLAY_OWNS on:  RTSP_EVENT_CLIENT_CONNECTED, RTSP_EVENT_PLAYING
AIRPLAY_OWNS on:  RTSP_EVENT_PAUSED           — indefinitely; no timer
START TIMER   on: RTSP_EVENT_TRANSPORT_CLOSED — release after 3000 ms
RELEASE      on:  RTSP_EVENT_DISCONNECTED     — immediately
RELEASE      on:  release timer expiry
```

- `AIRPLAY_HOLD_ON_PAUSE` — always **true** (the user's stated policy).
- `AIRPLAY_TRANSPORT_CLOSE_GRACE_MS` — default **3000**. It only starts after the RTSP transport closes; it never applies to an ordinary pause.

The existing v1 DACP grace may continue to preserve reconnect semantics. Releasing USB ownership does not tear down that logical session. If the sender reconnects later, `CLIENT_CONNECTED` or `PLAYING` immediately gives AirPlay ownership again.

Keep the transport-close grace as a build-time constant in one place, with a comment naming the stuck-ownership failure it prevents.

**Ownership transfer mechanics.** The AirPlay playback task (`audio_output.c:200-263`) writes silence to I2S whenever the receiver has no data. If it runs while your USB writer also writes, both interleave into the same DMA ring and you get garbage. So exactly one writer task may exist at a time:

- **USB takes over:** stop the AirPlay writer with `audio_output_stop()`, wait for it to exit, then start the USB writer task.
- **AirPlay takes over:** stop the USB writer task, wait for it to exit, then `audio_output_start()`.

**Critical difference from the A2DP precedent:** A2DP calls `stop_airplay_services()` (`main.c:82-95`), which also calls `rtsp_server_stop()`. **You must not do that.** AirPlay has to stay discoverable and connectable while the PC is playing. Call `audio_output_stop()` alone, and leave the RTSP server running. Verify that the AirPlay writer task is genuinely idle-safe to stop while the RTSP server is up — `audio_output_start()` already resets the output cursor on restart (`audio_output.c:366-368`) with a comment describing exactly this "DMA has been free-running (A2DP, or plain silence)" case, which suggests it is, but confirm it.

**Extend `playback_source_t`** with `PLAYBACK_SOURCE_USB` (`playback_control.h:19-23`) and audit every `switch` over that enum for unhandled cases — `playback_control.c` routes button actions by source and will otherwise silently do the wrong thing.

### ADR-7 — Task and core assignment, decided up front

Established facts: WiFi is pinned to core 0 (`sdkconfig.esp32s3:1553`); all audio runs on core 1 (`PLAYBACK_CORE 1`, `audio_output.c:56-58`); the priority doctrine is documented at `audio_output.h:9-24` (playback 9 > realtime UDP 8 > control 7 > buffered TCP 5).

USB isochronous transfers have a **hard 1 ms deadline**. The AirPlay playback task has ~40 ms of DMA slack and, by its own documentation, "blocks on the DMA write for all but a few hundred microseconds of each ~8 ms frame period". Deadline urgency, not importance, should set the priority here.

Proposed assignment — **verify and report as a table (§7.5), and change it only with an argument**:

| Task | Core | Priority | Reason |
|---|---|---|---|
| TinyUSB stack (`CONFIG_UAC_TINYUSB_TASK_CORE/PRIORITY`) | 1 | **10** | Away from WiFi; above audio playback because a missed 1 ms isochronous deadline is unrecoverable while a 40 ms DMA ring is not. Also services control transfers — starving it makes Windows report device errors. |
| UAC speaker task (`CONFIG_UAC_SPK_TASK_CORE/PRIORITY`) | 1 | 8 | Does a single memcpy (ADR-4). Below playback (9), honouring the source-task doctrine. |
| USB → I2S writer (yours) | 1 | 9 | It *is* the playback task while USB owns the output; the AirPlay one is stopped. (Note A2DP uses 7 at `a2dp_sink.c:58` and works, but it has no competing writer either.) |

**Also pin lwIP.** `CONFIG_LWIP_TCPIP_TASK_AFFINITY_NO_AFFINITY=y` (`sdkconfig.esp32s3:1920`) lets the TCP/IP task float onto core 1 and preempt the USB path in bursts. Set `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y` in the new build profile. Pinning WiFi without pinning lwIP leaves the hole half-closed — and acceptance test **A10** exists precisely to catch this.

**Everything on the USB path must be event-driven.** `CONFIG_FREERTOS_HZ=100` means one tick is 10 ms (`sdkconfig.esp32s3:1671`). Task notifications and semaphores only; no `vTaskDelay`-based pacing anywhere near a 1 ms deadline.

### ADR-8 — Make the DMA ring depth a build-time profile, and state the tradeoff

The 2048-frame ring (`audio_output.c:46-47`) is **40 ms of the USB latency budget** and dwarfs every other term:

| Term | ms @48 kHz | Source |
|---|---|---|
| Windows audio engine (shared mode) | ~10 | host, not controllable |
| UAC EP-OUT FIFO, regulated half-full | ~5.6 | `196 B × 11 = 2156 B` = 11.2 ms (`tusb_config_uac.h:52`, `CONFIG_UAC_SPK_INTERVAL_MS=10`) |
| Your ring buffer | ~5 | your choice |
| **I2S DMA ring** | **40.0** | `(2·8−1)·256/2 = 1920` frames |
| **Total** | **~60** | |

Dropping to `DESC_NUM=4, FRAME_NUM=128` gives 512 frames = 10.7 ms ring, ~9.3 ms modelled — total **~30 ms**, which is genuinely good for gaming.

**But it is not free.** `audio_output.c:288-298` documents the deep ring as protection against writer stalls longer than the ring — NVS/flash writes that disable the cache, web-server CPU bursts — where `auto_clear` degrades the stall to silence instead of a stuttering replay. A 10.7 ms ring makes those stalls audible on the AirPlay path too.

So: expose `I2S_DMA_DESC_NUM` / `I2S_DMA_FRAME_NUM` as Kconfig with the current values as default, and ship the low-latency values only in the USB build profile. `audio_output_get_hardware_latency_us()` already derives from the macros (`:429-434`), so the timing engine follows automatically — verify that it does. Report measured end-to-end latency at both settings (§7.5).

---

## §4 — Functional requirements

### FR-1 — USB speaker device

- **FR-1.1** Enumerate as a **UAC2 playback (speaker)** device: `output_cb` populated, host→device. It must appear under **Playback** in Windows Sound settings. (`output_cb` is currently `NULL` at `audio_output_usb.c:169` — that is the line that tells you the direction convention.)
- **FR-1.2** Driverless on Windows 11. UAC2 over full speed. The bundled Espressif component already implements UAC2; converting it to UAC1 would replace a working protocol layer without helping the target PC.
- **FR-1.3** 48 000 Hz, 16-bit, stereo. `CONFIG_UAC_SAMPLE_RATE=48000`, `CONFIG_UAC_SPEAKER_CHANNEL_NUM=2`, `CONFIG_UAC_MIC_CHANNEL_NUM=0`. Per ADR-2, never a rate that is not a multiple of 1000.
- **FR-1.4** Composite device: UAC + HID, without CDC. Requires `CONFIG_USB_DEVICE_UAC_AS_PART=y` (ADR-3).
- **FR-1.5** Configuration descriptor declares remote wakeup (`bmAttributes` bit 5).
- **FR-1.6** Set a distinct VID/PID and product string. Do not ship `0x303A:0x8000 "ESP UAC Device"`. If no VID is available, keep Espressif's VID and pick an unused PID, and say so in the README.
- **FR-1.7** Handle `set_volume_cb` and `set_mute_cb` (`usb_device_uac.h:30-31`). The host's volume slider must work. Note `apply_volume()` is AirPlay-only and is bypassed on this path (2.3) — apply your own gain in the writer task, reusing the ramp shape from `audio_output.c:145-172` so volume changes do not zipper. Respect `CONFIG_DAC_CONTROLS_VOLUME` the same way the existing code does.
- **FR-1.8** Feed `led_audio_feed()` from the USB path so the VU meter and LED behaviour match the other sources (`a2dp_sink.c:260` does this).

### FR-2 — Clock synchronisation

- **FR-2.1** Follow ADR-5: measure before changing. Report the 60-minute fill-level trace either way.
- **FR-2.2** Use `audio_output_get_pipeline_us()` as the error signal. Do not add a second measurement path.
- **FR-2.3** Ring buffer target depth configurable at build time; start at ~10 ms. Do not change any AirPlay-path buffer values.
- **FR-2.4** On underrun, emit silence and **count it** — never block, never stall the USB path.

### FR-3 — Source arbitration

- **FR-3.1** AirPlay is absolute, per ADR-6, including while paused. A normal pause never releases ownership. A lost transport is bounded to 3000 ms before USB resumes.
- **FR-3.2** Ownership is driven by `rtsp_events` (`main/rtsp/rtsp_events.h:13-19`), registered via `rtsp_events_register()`. Mirror `on_airplay_client_event` (`main.c:174-206`).
- **FR-3.3** Exactly one I2S writer task at any time (ADR-6). Never call `rtsp_server_stop()`.
- **FR-3.4** While AirPlay owns the output, keep consuming and discarding USB data (§5.2).
- **FR-3.5** Fade out / fade in (~5–10 ms) across every switch, using the ramp approach from `apply_volume()`. Never a hard cut.
- **FR-3.6** With no source active, output digital silence and leave I2S enabled — do not disable the channel (avoids a DAC pop and keeps `dac_on_i2s_started()` semantics intact).
- **FR-3.7** Add `PLAYBACK_SOURCE_USB` to `playback_source_t` and audit every `switch` over it.

### FR-4 — PC wake

- **FR-4.1 Primary — USB remote wakeup.** The mechanism that wakes a suspended host is **USB resume signalling**, not the keypress: call TinyUSB's `tud_remote_wakeup()`. Optionally follow, after the bus resumes, with one benign HID report — a bare **Left Ctrl** press/release, inert in essentially every application. Never a key that could trigger an action.
- **FR-4.2 Fallback — Wake-on-LAN.** UDP broadcast to port 9; payload = 6 × `0xFF` then the target MAC repeated 16 times. **MAC must be configurable via NVS**, following the `settings.c` pattern — never hardcoded. Expose it in the web UI alongside the other settings.
- **FR-4.3 State detection.** `tud_mounted()` true and bus active → PC on. Enumerated but suspended (`tud_suspend_cb`) → S3 → FR-4.1. VBUS present but not enumerated → off → FR-4.2. These callbacks are yours to define in composite mode (ADR-3).
- **FR-4.4 Trigger.** Use an HTTP endpoint and the existing web UI. This board has no physical buttons.
- **FR-4.5 README must document the host-side settings**, all commonly wrong by default:
  - **BIOS: ErP / EuP Ready = Disabled.** Required *both* for +5VSB on the USB ports (so the device stays alive when the PC is off) *and* for WoL from S5. One setting, both features — if it is enabled, neither works.
  - **Windows: Fast Startup = Disabled.** Hybrid shutdown is not a true S5 and commonly breaks WoL.
  - **Device Manager → the device → Power Management → "Allow this device to wake the computer"** must be checked for FR-4.1.
  - Note that FR-4.1 wakes from S3 only; S5 needs FR-4.2 unless the board supports USB power-on.
  - Note which USB ports on the board stay powered — on many boards it is only some of them.

### FR-5 — Diagnostics

- **FR-5.1** Expose: active source, ring fill level, USB feedback value in ppm relative to nominal, underruns, overruns, discarded-while-AirPlay-owns byte count, detected PC power state, and any active transport-close grace time.
- **FR-5.2** Extend `main/airplay_metrics.c` — the established pattern. Do not add a new telemetry path. Surface it on the existing web UI.
- **FR-5.3** Underrun/overrun counters are mandatory; A2 and A10 depend on them.
- **FR-5.4** Log the resolved task/core/priority map once at boot. It is the first thing to check when someone reports clicks.

### FR-6 — Build integration

- **FR-6.1** New `sdkconfig.defaults.usbaudio` layering the USB-source options, and a `[env:esp32s3-usbaudio]` in `platformio.ini` extending `env:esp32s3` with the layered `cmake_extra_args` (follow `platformio.ini:33-37`).
- **FR-6.2** New `CONFIG_USB_AUDIO_SOURCE` in `Kconfig.projbuild` under the existing "Audio Output" menu (`:158`), with `depends on AUDIO_OUTPUT_I2S && !AUDIO_OUTPUT_USB` and a target guard for S2/S3/P4 mirroring `main/idf_component.yml:14-18`.
- **FR-6.3** The default `esp32s3` env must build **unchanged and byte-for-byte functionally identical**. Do not put USB-source options into `sdkconfig.defaults.esp32s3`.
- **FR-6.4** Add the new env to the CI build matrix (`.github/workflows/ci-release.yml`), which currently builds esp32s3, squeezeamp-bt, squeezeamp-4m, esparagus-audio-brick-bt.

---

## §5 — Pitfalls

**5.1 — WiFi/lwIP versus isochronous deadlines.** See ADR-7. Pin lwIP as well as WiFi. Test A10 exists to catch this and it is the test most likely to fail first.

**5.2 — Never apply backpressure on the USB path.** While AirPlay owns the output, keep draining and discarding. Stalling `output_cb` makes the ISR overwrite `spk_buf` (ADR-4) and makes Windows report device errors.

**5.3 — This becomes the PC's only sound card.** A crash kills PC audio mid-game. Keep the failure mode graceful: task watchdog is already on with a 5 s timeout (`sdkconfig.esp32s3:1497`), clean re-enumeration on recovery, and never a state where the device is enumerated but silently dead with no counter moving.

**5.4 — Flash over the UART port, not the native USB port** — the native port is the live audio device. The `esp32s3` devkit has both.

**5.5 — Power.** WiFi TX bursts draw up to ~500 mA. Ensure bulk capacitance near the module. Random reboots under load: suspect this before suspecting firmware. Keep the device USB-powered rather than externally powered — a second supply plus the USB ground plus an analogue output to an amplifier is a ground loop, and USB-powered is what every USB DAC does for exactly that reason.

**5.6 — `printf` in the UAC hot path.** `usb_device_uac.c:325` and `:336` call bare `printf()` from `tud_audio_set_itf_cb`. That is a blocking UART write in a USB control-transfer path. Harmless at enumeration, but if you copy that file as a starting point for anything, do not carry those lines forward.

---

## §6 — Implementation order

Land as separate, individually testable increments. Do not start the next until the current one is verified on hardware.

1. **Survey and report (§7).** No code.
2. **Build profile + composite descriptors.** `CONFIG_USB_DEVICE_UAC_AS_PART=y`, UAC + HID, correct VID/PID, remote-wakeup bit. Verify enumeration under Playback with **no audio path yet**. Windows must show the device cleanly, Device Manager error-free.
3. **Audio out.** Wire `output_cb` → ring → writer task → `audio_output_write()`. Ignore drift and AirPlay. Verify A1.
4. **Clock verification (ADR-5).** The 60-minute measurement. Only then decide whether to change the feedback method. Verify A2.
5. **Arbitration (ADR-6).** Verify A3–A7.
6. **Wake (FR-4).** Trivial by comparison. Verify A8, A9.
7. **Latency profile (ADR-8).** Measure at both DMA ring settings, pick a default, document.
8. **Diagnostics, README, CI.**

---

## §7 — Report before writing code

Answer from the actual source and from measurement. Do not assume, and do not answer from this document — it is a starting map, not ground truth.

1. **Confirm or refute the ADR-2a analysis** by reading `usb_device_uac.c:322` and `:361` yourself. If you think the integer-division reasoning is wrong, say so with the counter-argument before anything is built on it.
2. **Composite feasibility.** Having read `CMakeLists.txt:13-25` and `tusb/usb_descriptors.c`, is `CONFIG_USB_DEVICE_UAC_AS_PART=y` + a hand-written descriptor set with HID actually clean, or does the component's `uac_descriptors.h` interface numbering (`:37-44`) fight it? If it fights, propose the alternative before committing.
3. **CPU headroom for ADR-2.** Measure core-1 utilisation with the 44.1→48 resampler active during AirPlay playback, at the default `RESAMPLER_TAPS`. Report the number. This is the one place ADR-2 could fail.
4. **Free heap in the worst case** — AirPlay connected and USB streaming, with `CONFIG_SPIRAM=y` and OCT mode at 80 MHz. Report measured internal and PSRAM figures separately; TinyUSB buffers must be in internal RAM (DMA-capable).
5. **Your final task/core/priority table** (ADR-7), plus measured end-to-end USB latency at both DMA ring settings (ADR-8).
6. **Any conflict** between the arbitration model in ADR-6 and the actual behaviour of `audio_output_start()` / `audio_output_stop()` when the RTSP server is left running — in particular whether stopping the AirPlay writer while a session is *connected but idle* is safe.

---

## §8 — Acceptance criteria

Demonstrate each on hardware and report the actual result. **Do not report completion on a criterion you have not run** — say which ones you skipped and why.

| # | Test | Pass condition |
|---|---|---|
| A1 | Plug into Windows, no drivers installed | Appears under **Playback**, selectable, produces sound; Device Manager clean |
| A2 | **60 minutes of continuous PC playback** | Zero clicks, zero dropouts, underrun and overrun counters both 0, ring fill level stationary |
| A3 | Start AirPlay while PC audio is playing | AirPlay takes over within ~200 ms, clean fade, no pop |
| A4 | Pause the AirPlay sender without disconnecting | Output silent; PC audio does **not** resume (FR-3.1) |
| A5 | Disconnect an AirPlay **2** sender | PC audio resumes within ~1 s |
| A6 | Disconnect an AirPlay **1 / RAOP** sender that keeps advertising DACP | PC audio resumes within `AIRPLAY_TRANSPORT_CLOSE_GRACE_MS` — proves the ADR-6 bound |
| A7 | Kill a sender ungracefully (WiFi off on the phone) | PC audio resumes; no stuck state |
| A8 | Sleep the PC, trigger wake | PC wakes via remote wakeup |
| A9 | Power the PC fully off, trigger wake | Device stays powered; PC wakes via WoL |
| A10 | **AirPlay and USB audio concurrently, 10 minutes** | No clicks on either — validates ADR-7 including the lwIP pinning |
| A11 | Full AirPlay regression per `docs/BASELINE_TEST.md` | No regression |
| A12 | Build and run `CONFIG_AUDIO_OUTPUT_USB=y` (the USB-**mic** feature) | Still builds, still works — proves §0 was respected |
| A13 | Build the default `esp32s3` env | Unchanged and functionally identical (FR-6.3) |
| A14 | Unplug and replug USB 20 times while AirPlay is streaming | No crash, no leak, AirPlay unaffected, counters sane |
| A15 | Reboot the ESP32 while Windows has the device selected | Windows re-enumerates and audio returns without user action |

**A2, A10, A12 and A15 are the ones that matter.** A2 and A10 are where the design either holds or does not. A12 proves you did not conflate the two USB features. A15 is the difference between a demo and something the user can leave plugged in.

---

## §9 — Conventions

Read `CLAUDE.md` at the repo root and follow it.

- **Formatting:** LLVM style, 2-space indent, **80-column limit**, enforced by `.clang-format` and a `format-check` CI job. Install the hooks: `git config core.hooksPath .githooks`.
- **Linting:** `.clang-tidy` with bugprone/performance/portability/readability. `scripts/lint.sh` needs `build/compile_commands.json`.
- **No unit tests exist.** This is firmware; hardware testing is the only verification. That is why §8 is long — it is the test suite.
- **Comment style:** this codebase explains *why*, at length, citing specific failure mechanisms and issue numbers. Read `audio_output.h:9-24` (task priority doctrine, issue #122), `audio_output.c:288-298` (auto_clear rationale), and `apply_volume()` (`audio_output.c:145-156`, zipper-click rationale) to calibrate. **Match that density.** Terse code with no rationale will not fit, and in this project the comments are load-bearing — several of the findings in this spec came from reading them.
- The repo is **WIP with uncommitted work in progress**. Branch, keep commits scoped to one increment from §6, and do not reformat unrelated files.
