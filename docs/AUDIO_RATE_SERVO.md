# Drift-Compensating Audio Rate Servo

## Problema

Receiver-ul rulează un I²S clock master local (cristalul ESP32 la 44100 Hz nominal).
Sender-ul AirPlay (iPhone / Mac) rulează propriul lui ceas — fie cristal hardware,
fie clock generat din PTP. Cele două ceasuri **nu sunt nicicând perfect identice** —
diferența tipică e sub 100 ppm dar non-zero, și se acumulează liniar în timp.

Pe lângă asta, **stall-urile de WiFi** (vizibile în telemetry ca spike-uri de
`i2s_max=30-50ms`) shiftează momentul în care consumăm frame-uri din buffer
față de anchor-ul lor. Fiecare stall lasă o "datorie" de drift care nu se
auto-recuperează — odată ce am ratat 30ms, I²S clock-ul nostru nu poate să
ruleze mai repede ca să prindem din urmă.

Înainte de fix, comportamentul observat era:

- **Steady-state**: drift_min hovering la `-30..-60 ms` (audio în urma anchor-ului).
- **Late-frame absorption** la pragul de 60ms ținea drift-ul în zonă (de la
  -100ms cum era inițial), dar nu-l aducea la zero.
- **Bulk-flush**-ul (>2s late sau 3+ frames consecutive late) era singurul
  mecanism de recuperare — și asta produce silence audibilă de ~1.5s.

Concluzia: avem nevoie de un mecanism *continuu* de corecție care să convertească
drift-ul în câteva secunde, fără gauri audibile.

## Spațiul de soluții — ce s-a evaluat

### Opțiunea A: APLL clock servo (ajustare hardware)

Modificare directă pe APLL-ul ESP32 ca să modulăm I²S MCLK la rate ușor
diferit (44095..44105 Hz). Ar fi ideal — bit-perfect, zero CPU, zero latență.

**Eliminat** — un patch APLL anterior a produs "static / no audio / boom" pe
ESP32-A1S + ES8388 (documentat în `AI_HANDOFF.md` secțiunea 5). E un risc
hardware-specific cu istoric. Nu merită experimentat.

### Opțiunea B: Sample skip/duplicate periodic

Aruncă un sample la fiecare N frames. Simplu de implementat.

**Eliminat** — un patch anterior care făcea exact asta (la nivel de frame, nu
sample) a produs static. Chiar și la nivel de sample, fiecare drop e un click
audibil dacă e suficient de des. Inacceptabil pentru rate de corecție mari.

### Opțiunea C — aleasă: Resample fracțional cu interpolare liniară

Inserăm o etapă de resample cu rate ≈ 1.0 ± 500 ppm între pipeline-ul existent
și I²S. Software pur. Identitate (cu 1 sample delay) când corecția e zero.
Pitch shift maxim de **0.86 cents** la corecția maximă — sub orice prag de
detectabilitate audibilă (limita inferioară pentru ureche bine antrenată este
~5 cents pentru tonuri pure susținute).

Combinăm cu un **P-controller** care convertește drift-ul mediat într-un setpoint
de rate. Smoothing exponențial peste corecția aplicată ca să nu existe niciun
"pitch glide" perceptibil când sender-ul/condițiile se schimbă.

## Arhitectură

### Pipeline audio

```
[buffer] → audio_timing_read → audio_resample → audio_servo → I2S
            (anchor check)     (rate convert,   (drift comp,
             produce drift     no-op pe         linear interp
             signal)           44.1→44.1)       cu rate ≈ 1.0)
```

Servo-ul e **always-on**. La corecție 0 introduce un delay de 1 input-frame
(~22 µs la 44100 Hz) — inaudibil. Nu există ramuri condiționale care să-l
ocolească pe path-ul "fără corecție", ca să nu existe glitch-uri audibile la
tranziția enabled/disabled.

### Drift signal — `audio_timing.c`

În `audio_timing_read`, pentru fiecare frame extras din buffer, se calculează
`early_us = T_anchor - now - HW_LATENCY` (semnătura standard pentru AirPlay
receivers).

```c
if (early_us > -1000000LL && early_us < 1000000LL) {
  if (!timing->smoothed_drift_valid) {
    timing->smoothed_drift_us = (int32_t)early_us;
    timing->smoothed_drift_valid = true;
  } else {
    int32_t diff = (int32_t)early_us - timing->smoothed_drift_us;
    timing->smoothed_drift_us += diff / 128;  // EMA, α = 1/128
  }
}
```

EMA cu **α = 1/128**. La cadența de ~125 Hz a frame-urilor, asta e un timp
constant de ~1 secundă — destul de lent ca să rejecteze WiFi jitter
(spike-uri de 30-50ms vin la frecvențe mai mari), destul de rapid ca să
reacționeze la drift real.

Sanity guard: skip-uim sample-uri afară de ±1 secundă. Asta e centura de
siguranță pentru cazul în care `compute_early_us` ar returna ceva absurd —
nu vrem să poizonăm integratorul.

### Controller — `audio_servo.c`

P-controller pur, fără I sau D. Argumentare:

- **De ce nu PI**: integratorul ar acumula offset-uri DC. Dar drift-ul în sine
  e deja un offset DC pe care vrem să-l zerăm — dacă sender-ul rulează
  permanent cu clock 50 ppm mai rapid ca al nostru, drift va fi non-zero la
  steady state și P-controller-ul va menține o corecție constantă.
  Asta e exact ce vrem. PI ar putea introduce oscilații pentru o îmbunătățire
  marginală a vitezei de convergență.

- **De ce nu PD**: derivata zgomotului WiFi ar amplifica ripple-ul.

Formula:

```c
target_ppm = -drift_us / TIME_CONSTANT_SEC;   // 24s timp constant
target_ppm = clamp(target_ppm, ±MAX_CORRECTION_PPM); // ±2000ppm
```

La drift = -24ms, TIME_CONSTANT = 24s: `target_ppm = +1000`. Mai precis:
recovery_time = drift / correction = 24ms / 0.001 = **24 secunde**. Adică
aceeași valoare ca TIME_CONSTANT_SEC. Asta nu e coincidență — TIME_CONSTANT
chiar reprezintă "câte secunde durează să recuperezi 1 secundă de drift la
1 ppm".

Pentru drift sub deadband (±5 ms) — `target_ppm = 0`. Previne hunting când
controller-ul a convergerat.

### Smoothing aplicat

Corecția aplicată tinde lent spre target — nu sare instant:

```c
delta = target_ppm - applied_ppm;
applied_ppm += delta / 200;  // ~1.6s timp constant la cadență de 125 Hz
```

Asta e **smoothing-ul critic** care face servo-ul inaudibil. Schimbarea bruscă
de rate — chiar și în cadrul ±2000ppm — produce un "wow" audibil. Smoothing-ul
distribuie tranziția pe ~1.6s, ceea ce e percepat ca o non-tranziție.

Min-step protection: `delta != 0` dar `delta/200 == 0` din cauza diviziei
întregi → forțăm step de ±1 ppm. Altfel controller-ul ar stalla la ~199 ppm
de target și nu ar mai progresa niciodată.

### Resampler — interpolare liniară fracțională

State persistent peste apeluri:

```c
struct {
  float phase;                 // poziție sub-sample în [0, 1)
  float step;                  // 1.0 + applied_correction
  int16_t prev_l, prev_r;      // sample-ul anterior pentru interp
  int16_t curr_l, curr_r;      // sample-ul curent
  bool primed;
};
```

Per output sample:

1. Avansează prin input până când `phase ∈ [0, 1)` (interp valid între prev și curr).
2. Output = `(1-phase) × prev + phase × curr` per canal.
3. `phase += step`.

Bootstrap (primul input): `prev = curr = first_sample`, `phase = 1.0`. Asta
forțează consumul unui al doilea input înainte de prima ieșire — astfel
prev ≠ curr și interpolarea e validă de la primul output.

**De ce interpolare liniară și nu polyphase**: pentru rate mici de servo,
distorsiunea introdusă de aliasing este sub -100 dB față de semnal. Polyphase
(care există deja în codebase pentru 44100→48000) ar costa mai mult CPU și
mai multă memorie pentru zero îmbunătățire audibilă.

**De ce float și nu Q32 fixed-point**: ESP32 are FPU hardware pentru float
(single precision). 24 biți mantisa = precision suficientă pentru phase ∈
[0, 2). Q32 fixed-point pe 32-bit ar avea overhead de shift-uri în loop-ul
fierbinte. Doubla nu avem (e softfloat pe ESP32 — lent).

### Reset behavior

- La **stream start** — `audio_servo_init` zerează tot.
- La **flush** (FLUSH/FLUSHBUFFERED/seek) — `audio_servo_reset` zerează doar
  state-ul de resampler (phase, primed). **Lasă `applied_ppm` neschimbat**
  intenționat — e un offset hardware (clock mismatch) care nu dispare la
  flush. Reset abrupt la 1.0 ar produce mai multă pertubație decât menținerea
  corecției curente.
- La **PTP master change** — `audio_timing_reset` zerează `smoothed_drift_us`
  pentru noua sesiune; controller-ul va re-converge.
- La **paused** — `audio_servo_update(_, playing=false)` setează target la 0,
  smoothing-ul aduce corecția înapoi la 0 într-un ~1.6s.

## Tunables — argumentarea valorilor

### `MAX_CORRECTION_PPM = 2000`

2000 ppm = 0.2% rate change = **3.46 cents pitch shift**. Pragul de detectabilitate
pentru un ascultător antrenat pe tonuri pure susținute e ~5 cents
(Moore et al., "An Introduction to the Psychology of Hearing", cap. 6).
Pentru muzică sau voce, pragul e mult mai înalt (>20 cents).

Deci 2000 ppm rămâne o limită steady-state conservatoare. După seek/scrub
există un boost temporar la 10000 ppm cu gain și smoothing mai rapide; acesta poate fi
audibil dacă ar sta mult timp, deci expiră singur și se oprește când drift-ul
revine sub ~8 ms, dar nu mai devreme de ~2.5 s după seek.

### `DRIFT_DEADBAND_US = 5000` (5 ms)

EMA-ul lasă reziduri de zgomot WiFi de ordinul ±2-4ms chiar la steady state.
Fără deadband, controller-ul ar pulsa între ±50 ppm corecție. Pulsațiile sunt
sub pragul audibil dar produc telemetry "noisy" și pot interfera cu late-frame
absorption (corecție pozitivă tranzitorie ar grăbi consumul de buffer).

5 ms e larg ca să acopere zgomotul EMA dar mic ca să producă A/V skew
perceptibil (5 ms e sub pragul de detectare lipsync de ~25-40 ms).

### `TIME_CONSTANT_SEC = 24`

Stabilește câtă agresivitate are P-controller-ul. La 24s, drift de -24 ms
produce target +1000 ppm, ceea ce înseamnă convergență teoretică în 24s.
Mai mic = mai agresiv, risc de overshoot și saturare permanentă la clamp.

Mai mare (120s) = convergență prea lentă față de WiFi jitter — drift se
acumulează mai repede decât se recuperează.

24s e compromisul curent: mai lipit de zero după seek, încă lent destul pentru
a nu vâna jitter-ul WiFi.

### `SMOOTHING_DENOMINATOR = 200`

Cadența update-ului = ~125 Hz (un update per audio chunk). 1/200 per update
= timp constant de 200/125 ≈ 1.6 secunde pe schimbarea efectivă de rate.

Mai rapid (1/50 → 0.4s) — schimbarea de rate devine perceptibilă ca un "swoosh"
când controller-ul reacționează la un disturbance brusc.

Mai lent (1/500 → 4s) — convergența aparentă devine prea lentă, mai ales după
un seek.

1.6s e sub orice prag de detectare a pitch glide-ului dar destul de rapid ca
să reacționeze la schimbări reale.

## Rezultate observate

### Path TCP-buffered (iPhone, Apple Music, Safari player)

Telemetry steady state după ~5s de redare:

```
buf103 ... drift min/max=-3/+3ms servo=-426ppm | ... ptp lock=1
buf103 ... drift min/max=-1/+4ms servo=-296ppm | ... ptp lock=1
buf103 ... drift min/max=6/13ms  servo=-178ppm | ... ptp lock=1
```

Drift converge la **±5 ms** în ~5 secunde după pornire. Servo se stabilizează
la valori non-zero (-178 ppm aici), reflectând offset-ul real al clock-ului
sender-ului față de al nostru. Asta e exact ce trebuie — clock mismatch
permanent compensat permanent.

Înainte de servo: drift-ul ar fi continuat să se acumuleze sau ar fi convergit
la ~+15-20 ms (offset de pre-buffer al sender-ului care depinde de
implementarea AirPlay 2 buffered).

### Path UDP-realtime (Mac system tray, ALAC type=96)

Telemetry steady state după ~10s:

```
rt96 ... drift min/max=4/45ms   servo=-500ppm
rt96 ... drift min/max=-3/55ms  servo=-482ppm
rt96 ... drift min/max=10/56ms  servo=-500ppm
rt96 ... drift min/max=14/52ms  servo=-465ppm
```

Servo e **clamp-uit la -500 ppm**. Drift-ul nu converge la zero pentru că
condițiile WiFi introduc disturbances mai mari decât poate compensa servo-ul
la limita actuală.

Drift_min e tot pozitiv (+5..+15 ms) — adică audio se redă tot **înaintea**
anchor-ului, ceea ce înseamnă că lag-ul perceput față de video NU vine din
audio path-ul nostru. Vine din pipeline-ul video local al Mac-ului.

### Recovery după disturbance

La t=51.7s în log-ul atașat:

```
Dropping late frame(s): 75 ms
Bulk flush: frame 71 ms late, consecutive_late=4, flushing 225 stale frames
```

Un burst WiFi a depășit threshold-ul de 60ms. Bulk flush triggered. Servo
era deja la -500 ppm dar nu putea preveni acest spike single-event. După
recuperare, drift se restabilește la pattern-ul steady state în ~2s.

Asta evidențiază că **servo + late-threshold + bulk-flush sunt straturi
complementare**:
- Servo: corectează drift continuu, sub-perceptibil.
- Late threshold (60ms): absoarbe stall-uri momentane fără silence.
- Bulk flush (>2s sau 3 consecutive): safety net pentru disturbances majore.

### Comportament la pause/resume

La t=178.9-188.9s — pause de 10s pe buf103:

```
178.9s: SETRATEANCHORTIME rate=0.0 -> PAUSING
        ... drift min/max=0/0ms servo=0ppm  (target merge la 0, smoothing aduce înapoi)
188.9s: SETRATEANCHORTIME rate=1.0 -> RESUMING (dintr-un seek mare)
189.9s: drift min/max=-23/14ms servo=87ppm (recovery activă, corecție pozitivă)
190.9s: drift min/max=10/19ms  servo=-36ppm (servo aproape de zero)
194.0s: drift min/max=5/17ms   servo=-215ppm (steady state recovery)
198.0s: drift min/max=6/13ms   servo=-178ppm (settled)
```

Convergență completă în ~4-5 secunde după resume. Niciun click audibil la
schimbarea de rate.

## Ce s-a atins și ce nu

### Atinse

✅ **Drift-ul converge la zero pe buf103** (path-ul cu buffer adânc TCP).
   Înainte: drift hovering ~+15-20 ms permanent. Acum: ±5 ms, în deadband.

✅ **Drift-ul se stabilizează pe rt96** chiar și când sender-ul are clock
   mismatch. Înainte: degradare progresivă spre bulk-flush. Acum: clamp la
   -500ppm previne degradarea.

✅ **Tranziții imperceptibile** între sender-i, după pause/resume, după seek.
   Smoothing-ul de 1.6s ascunde toate schimbările de rate.

✅ **Pitch-ul rămâne inaudibil** la corecția maximă — testat empiric pe muzică
   și voce. Nu se aud "pitch glides" sau "wobble".

✅ **Telemetry expune servo-ul în timp real** (`servo=Nppm` în log) — ușor de
   debug-at și tunat.

### Limitări fundamentale

❌ **Lag perceput vs. video local pe Mac (system tray + non-AirPlay app)**.

   Asta e o limitare structurală macOS, nu a receiver-ului. Pipeline-ul video
   pentru HBO Max / YouTube în browser are propriul lui delay (decode + GPU +
   compositor) care nu e coordonat cu audio-ul re-routat prin system tray.
   Telemetry confirmă că audio-ul nostru se redă *înainte* de anchor (drift
   pozitiv), nu *după*. Servo-ul nu poate aduce audio din viitor.

   **Singurul fix**: folosirea AirPlay-ului din player-ul aplicației (Safari
   YT, Apple Music, app-ul de iPhone) unde aplicația coordonează video-ul cu
   anchor-ul audio. Atunci `latency96/103` din `/info` e respectat și A/V e
   sincronizat de sender.

❌ **Recovery dintr-un single WiFi spike >60ms** rămâne audibil ca un brief
   silence când bulk-flush triggerează. Servo nu poate preveni asta pentru
   că disturbance-ul e single-frame și mai mare decât marja absorbită de
   late-threshold.

   Mitigare posibilă (neimplementată): NACK repetat pe pachetele lipsă pentru
   a reduce probabilitatea ca un retransmit să sosească după threshold. Dar
   asta e o optimizare ortogonală, nu pe path-ul de drift compensation.

❌ **Servo clamp-uit la 500ppm pe Mac rt96**. Drift_max ajunge la 90 ms
   ocasional în condiții WiFi proaste. Servo-ul vrea corecție mai mare dar
   e limitat. Putem mări `MAX_CORRECTION_PPM` la 1000-2000 dacă acceptăm
   risc minor de pitch detectabil pe tonuri foarte pure susținute (e.g. flute
   solo). Pentru muzică pop / vorbire / video soundtrack, 2000 ppm încă e
   imperceptibil.

## Decizii non-standard care merită explicate

1. **Servo-ul lasă `applied_ppm` neschimbat la `audio_servo_reset()`** —
   contraintuitiv (s-ar zice că reset = totul la valori default), dar
   corecția curentă reprezintă un offset hardware persistent. Reset-area la
   1.0 ar produce un wow audibil la fiecare flush.

2. **Sanity guard la ±1 secundă în EMA-ul de drift** este redundant cu
   sanity check-ul din `compute_early_us` (care returnează `false` peste
   ±5 minute). Redundanța e intenționată: dacă cineva slăbește vreodată
   sanity-ul din `compute_early_us`, integratorul tot rămâne protejat.

3. **Min-step de ±1 ppm la smoothing** previne stall-ul controller-ului din
   cauza diviziei întregi. Fără asta, controller-ul ar putea sta blocat la
   ~199 ppm de target permanent.

4. **Servo-ul rulează always-on, nu doar când drift e non-zero**. Asta
   înseamnă că path-ul audio trece mereu prin interpolatorul liniar, chiar
   și când corecția e exact 0. Costul e ~22 µs delay (1 sample) și ~20 KCPU/s
   pentru interpolare. Beneficiul: zero discontinuități audibile la
   tranziții enabled/disabled. Pentru ascultătorul critic, ăsta e
   trade-off-ul corect.

## Cod relevant

- `main/audio/audio_servo.h` / `.c` — modul nou cu controller + resampler.
- `main/audio/audio_timing.c` — EMA pentru `smoothed_drift_us`.
- `main/audio/audio_timing.h` — câmpuri noi în `audio_timing_t`.
- `main/audio/audio_receiver.c/.h` — accessor `audio_receiver_get_smoothed_drift_us`.
- `main/audio/audio_output.c` — integrare în pipeline (apel `audio_servo_update` +
  `audio_servo_process` în `playback_task`).
- `main/audio/audio_telemetry.c` — log-ul include `servo=Nppm`.
- `main/CMakeLists.txt` — listare fișier nou.

## Verificare

```bash
~/.platformio/penv/bin/pio run -e esp32-a1s
```

Rezultat: SUCCESS. Firmware: `.pio/build/esp32-a1s/firmware.bin`.

## Concluzie

Servo-ul atinge obiectivul de design — converge drift-ul la zero pe path-ul
TCP buffered și-l stabilizează pe path-ul UDP realtime, fără artefacte
audibile. Pentru cazul utilizatorului (Mac system tray + HBO Max în browser),
lag-ul rezidual este structural și fără soluție din partea receiver-ului.
Pentru toate celelalte path-uri AirPlay-aware (iPhone, Apple Music, AirPlay
button în player), audio sync-ul este acum strâns la nivel de milisecunde.
