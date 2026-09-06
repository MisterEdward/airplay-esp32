# Plan de reparații — Fable 5.1 (ESP32-S3 + PCM5102A)

Handoff pentru un agent AI. **Doar reparații și optimizări. Features și
distracție vin într-o etapă ulterioară, nu le începe.**

---

## 0. Context obligatoriu

**Repo:** `/Users/edward/airplay-fable`, branch `fable-5.1`. Fork personal
pentru O SINGURĂ boxă: ESP32-S3 + PCM5102A, numită "Bedroom Speakers",
IP `192.168.68.104`. Citește `CLAUDE.md` și `docs/FABLE.md` înainte de orice.

**Utilizatorul e Edward. Răspunde-i în română.** El e singurul care poate
asculta boxa; tu nu ai microfon. Când ai nevoie de un test auditiv, cere-i-l
explicit, scurt și concret ("dă play, apoi pauză, apoi next, și spune-mi dacă
se aude tăietură"), nu-l pune să facă zece lucruri deodată.

### Cum se construiește și se încarcă

```bash
~/.platformio/penv/bin/pio run -e esp32s3
curl -X POST --data-binary @.pio/build/esp32s3/firmware.bin \
  http://192.168.68.104/api/ota/update
```

- După **orice** modificare în `sdkconfig.defaults*`, **șterge**
  `sdkconfig.esp32s3` (e generat, git-ignored) altfel noile valori nu se aplică.
- Calea proiectului nu are voie să conțină spații (PlatformIO refuză).
- Teste pe host pentru modulele pure (envelope, aliniere, jurnal, volum
  per-sursă, bucla de timing cu PCM și ceas fake): `tests/host/run.sh`.
  **Rulează-le înainte și după fiecare modificare în `main/audio/`.**

### Cum se verifică că a mers

- `curl -s http://192.168.68.104/api/status` — sursă activă, sesiune AirPlay,
  now playing, timing, PTP, USB, stare firmware.
  **`firmware.ota_state` trebuie să ajungă la `valid` după fiecare OTA.**
  Dacă rămâne `pending_verify` 180 s, bootloaderul face rollback.
- `curl -s http://192.168.68.104/api/system/info` — heap, RSSI, reset reason.
- `curl -s http://192.168.68.104/api/tasks` — stările task-urilor FreeRTOS.
- `curl -s http://192.168.68.104/api/logs/download` — tot jurnalul din PSRAM.
- Nivel de log per tag, la runtime:
  `curl -X POST -d '{"tag":"audio_output","level":"debug"}' http://192.168.68.104/api/logs/level`
  Taguri utile: `rtsp_handlers`, `audio_output`, `audio_recv`, `audio_buf`,
  `audio_time`, `ptp`.
  **NU seta niciodată tagul `*` pe debug** — httpd se loghează pe sine prin
  WebSocket, bucla de reacție blochează serverul HTTP (ping răspunde, HTTP nu).

### Reguli de lucru

1. **Un fix logic per commit.** Commit + push pe `origin fable-5.1` după
   fiecare pas care compilează și trece testele. Edward a pierdut deja muncă;
   nu acumula.
2. **Măsoară înainte și după.** Fiecare optimizare din Etapa 1 are un număr
   asociat. Notează-l înainte, notează-l după, pune-l în mesajul de commit.
3. **Nu atinge logica de timing în Etapele 1-2.** Sunt fix-uri de
   infrastructură. Timing-ul are propria etapă, cu teste scrise întâi.
4. **Nu ai consolă serială.** `CONFIG_ESP_CONSOLE_NONE=y`, deliberat: fiecare
   linie de log pe UART bloca task-ul ~13 ms și golea ring-ul DMA. Nu o
   reactiva "doar ca să depanezi". Tot ce vezi vine prin WiFi.
5. Formatare: `scripts/format.sh` înainte de commit (LLVM, 2 spații, 80 col).
6. Dacă un test pe telefon eșuează ciudat, **întreabă-l pe Edward dacă a dat
   airplane mode on/off**. Un iPhone care a trecut printr-un deadlock AirPlay
   rămâne stricat până își resetează sesiunea, și vei depana un bug inexistent.

---

## Etapa 1 — Fix-uri fără risc pentru calea audio

Toate cele șapte pot intra într-o singură sesiune. Niciunul nu schimbă logică
audio. Fă-le în ordinea asta.

### 1.1 BUG: un OTA eșuat omoară AirPlay definitiv `[15 min]`

**Unde:** `main/network/web_server.c:887-897`, `main/main.c:106-108`.

`ota_update_handler()` cheamă `rtsp_server_stop()` înainte de update. Pe calea
de succes urmează `esp_restart()`, deci nu contează. Pe calea de eroare:

```c
esp_err_t err = ota_start_from_http(req);
if (err != ESP_OK) {
  httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
  return ESP_FAIL;          // RTSP rămâne oprit pentru totdeauna
}
```

Și nu se recuperează niciodată, pentru că `s_airplay_started` a rămas `true`
și `start_airplay_services()` iese imediat pe acel flag. Rezultat pe o placă
fără serial: USB merge, AirPlay a dispărut, nimeni nu știe de ce.

**Fix:** pe calea de eroare repornește serviciile. Cel mai curat e ca `main.c`
să expună `airplay_services_mark_stopped(void)` care pune `s_airplay_started =
false`, iar handlerul să cheme asta plus `start_airplay_services()`. Dacă
repornirea în loc e complicată, `esp_restart()` pe eroare e acceptabil —
imaginea veche e intactă — dar spune-i lui Edward că ai ales varianta aia.

**Verificare:** trimite intenționat gunoi la `/api/ota/update`
(`curl -X POST --data-binary @README.md ...`), apoi confirmă că boxa mai apare
în lista AirPlay de pe telefon și că `/api/status` arată sesiunea disponibilă.

### 1.2 Bufferele de randare pleacă automat în PSRAM `[45 min, cel mai mare câștig]`

**Unde:** `sdkconfig.esp32s3` are `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024`,
deci **orice `malloc()` peste 1 KB aterizează în PSRAM**.

Afectate, toate pe calea cu deadline de 8 ms:
- `main/audio/audio_output.c:292-294` — `pcm` (~4.1 KB), `resample_buf`
  (~4.5 KB), `held` (~4.5 KB)
- `main/audio/audio_resample.c:35,40` — `float_in` (~8.2 KB), `float_out` (~9 KB)

Taskul de randare atinge ~30 KB de PSRAM per bloc, printr-un D-cache de 32 KB
împărțit cu lwIP, WiFi și httpd. Fiecare bloc e o rafală de cache miss. Asta e
suspectul principal pentru jitterul pe care servo-ul îl taie azi cu
`POS_SERVO_INNOV_CLAMP_US` (`audio_timing.c:74`) — al cărui comentariu descrie
fenomenul fără să-i numească cauza.

**Fix:** `heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)` pentru
cele cinci. ~30 KB de DRAM intern.

**Secundar, dacă mai e loc:** `receiver.decrypt_buffer` (8 KB,
`main/audio/audio_receiver.c:101`, azi explicit PSRAM) și
`buffer->frame_buffer` (16 KB, `main/audio/audio_buffer.c:159`) — scratch
per-cadru pe taskul decodor. **Pool-urile mari rămân în PSRAM**: coada de
pachete comprimate (~1.86 MB) și ringul PCM (~1.42 MB). Nu le muta.

**Verificare:**
- înainte de OTA, notează linia `Boot baseline: free heap ... internal
  (largest block ...)` din `main.c:458`. După, trebuie să scadă cu ~30 KB și
  `largest block` să rămână confortabil. Dacă e prea strâmt,
  `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` (azi 32768) poate crește.
- pune `audio_time` pe debug și colectează 2 minute de `Playout: err=... filt=...`
  înainte și după. Compară deviația. **Salvează ambele loguri**, e singura
  dovadă că a meritat.

### 1.3 I-cache la 16 KB `[10 min, pe gratis]`

**Unde:** `sdkconfig.esp32s3`: `CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB=y`,
`CONFIG_ESP32S3_DATA_CACHE_32KB=y`.

Pe ESP32-S3 memoria de cache e dedicată, **nu** se scade din cei 512 KB SRAM.
Rulează simultan WiFi, lwIP, TCP, decodare AAC, resampler sinc și TinyUSB
izocron, tot din flash. 16 KB e sub-dimensionat.

**Fix:** în `sdkconfig.defaults.esp32s3` adaugă
`CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y` și
`CONFIG_ESP32S3_DATA_CACHE_64KB=y`. **Șterge `sdkconfig.esp32s3`** și
reconstruiește. Confirmă în noul `sdkconfig.esp32s3` că valorile s-au aplicat.

**Verificare:** aceeași ca la 1.2 — se măsoară împreună, deci fă-le în commituri
separate dar măsoară după amândouă.

### 1.4 Fiecare linie de log se formatează de două ori `[10 min]`

**Unde:** `main/network/log_stream.c:120`.

```c
int ret = s_orig_vprintf ? s_orig_vprintf(fmt, uart_args) : 0;
```

`s_orig_vprintf` e `vprintf`-ul original. Cu `CONFIG_ESP_CONSOLE_NONE=y` și
`CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` ieșirea se aruncă, dar formatarea
completă se face oricum, apoi se repetă în `vsnprintf` la linia 124. Un
`Playout:` are 14 argumente, iar linia se emite din taskul de randare.

**Fix:** `#if !CONFIG_ESP_CONSOLE_NONE` în jurul apelului, sau nu instala deloc
`s_orig_vprintf` când consola e NONE. Păstrează `ret` coerent.

### 1.5 Buffer supradimensionat și cod inaccesibil în `usb_pull` `[10 min]`

**Unde:** `main/usb/usb_audio_source.c:286-288`.

```c
static int16_t tmp[2 * 1026];      // 4 KB
if (want_in > 1025) { want_in = 1025; }
```

Singurul apelant e `main/audio/audio_output.c:386`, care cere mereu
`FRAME_SAMPLES` = 352, deci `want_in <= 353` (vezi calculul de la
`usb_audio_source.c:276-280`). Ramura e inaccesibilă, iar bufferul e de 12 ori
prea mare — 3.7 KB de DRAM intern degeaba, exact resursa de care ai nevoie
la 1.2.

**Fix:** dimensionează după `FRAME_SAMPLES + 1` cu un `static_assert` care
prinde regresia dacă cineva schimbă `FRAME_SAMPLES`. Păstrează clamparea ca
plasă de siguranță, dar la noua limită.

### 1.6 Bugetul de socket-uri HTTP e prea strâns `[10 min]`

**Unde:** `main/network/web_server.c:1461-1463`.

```c
config.max_open_sockets = 3;
config.lru_purge_enable = true;
```

Pagina `/logs` ține un WebSocket permanent. Rămân două socket-uri pentru pagina
principală, care face polling pe `/api/status` și `/api/system/info`. Al treilea
client, sau un browser cu keep-alive, declanșează LRU purge — care poate tăia
exact WebSocket-ul de loguri, fix când te uiți la el. Edward va crede că a
crăpat firmware-ul.

`CONFIG_LWIP_MAX_SOCKETS=16`; RTSP, audio, PTP și NTP ocupă ~8. **`5` e sigur.**
Nu atinge ramura de BT de la linia 1458 (nu se compilează pe S3, dar las-o).

**Verificare:** deschide `/logs` și `/` în două tab-uri, plus un `curl` în
buclă pe `/api/status`, și confirmă că WebSocket-ul nu mai cade.

### 1.7 `/api/tasks` nu arată CPU% `[15 min]`

**Unde:** `sdkconfig.esp32s3`: `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` nu e
setat. `main/network/web_server.c:937` expune nume, stare, prioritate, core și
stack, dar nu procentaj de CPU.

Pe o placă unde singurul instrument de diagnostic e WebUI, "cine mănâncă
procesorul" e întrebarea numărul unu și azi n-are răspuns. Vei avea nevoie de
ea la Etapele 3-6.

**Fix:** `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y` în
`sdkconfig.defaults.esp32s3`, șterge `sdkconfig.esp32s3`, și adaugă
`ulRunTimeCounter` în JSON. Raportează-l ca **deltă între două apeluri**
(ține ultimul snapshot într-un static), nu ca total de la boot — altfel după
o oră de uptime toate procentele sunt inutile.

**Verificare:** `/api/tasks` de două ori la 5 s distanță în timp ce cântă;
suma procentelor ≈ 200% (două core-uri).

---

## Etapa 2 — Observabilitate: fă restul depanabil

Etapa asta nu repară nimic audibil. O faci pentru că fără ea, orice bug rar din
Etapele 3-6 îți lasă exact un număr de reset reason.

### 2.1 Coredump în flash + `/api/coredump` `[2-3 h]`

**Starea de azi:** `CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y` — dezactivat explicit,
nu din omisiune. Jurnalul de 192 KB e în PSRAM, deci **se pierde complet la
orice panică sau watchdog**. La boot, tot ce rămâne e
`ESP_LOGW(TAG, "Boot: reset reason %d")` (`main/main.c:335`).

**Fix:**
1. Adaugă o partiție `coredump` de 64 KB în `components/boards/partitions.csv`.
   Tabela curentă se termină la `0x800000`; `storage` (spiffs) e `0x620000`,
   `0x1E0000`. **Verifică întâi dimensiunea reală a flash-ului** — vezi 2.4 —
   apoi fie extinzi tabela, fie tai 64 KB din spiffs.
   ⚠️ Schimbarea tabelei de partiții **nu se poate face prin OTA**. Cere-i lui
   Edward un flash pe portul USB nativ, și **spune-i explicit** că după aceea
   trebuie și `pio run -e esp32s3 -t uploadfs`, altfel pierde paginile web.
2. `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` + `CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y`.
3. Endpoint `GET /api/coredump` care servește blobul brut (pentru
   `espcoredump.py info_corefile`) și `GET /api/coredump/summary` cu task-ul
   care a crăpat, PC-ul și backtrace-ul, via `esp_core_dump_get_summary()`.
   `DELETE` sau `POST /api/coredump/clear` ca să-l ștergi după ce l-ai citit.
4. `/api/status` să spună dacă există un coredump nedescărcat.

### 2.2 Jurnalul de loguri să supraviețuiască unei panici `[2 h]`

Ultimii ~8 KB din jurnalul PSRAM salvați într-o partiție mică la panică, apoi
re-injectați în jurnalul nou la boot cu un marcaj `--- previous boot ---`.

Atenție: la panică nu poți folosi API-ul normal de flash sau `esp_vfs`. Ai două
variante — un `__wrap_esp_panic_handler` (există deja scaffolding de `-Wl,--wrap`
în `main/CMakeLists.txt` pentru SqueezeAMP), sau `esp_core_dump`-ul din 2.1 cu
jurnalul ținut într-o zonă `RTC_NOINIT_ATTR`. **A doua e mult mai simplă și mai
sigură**: RTC RAM supraviețuiește unui reset software, e ~8 KB, și nu ai nevoie
de niciun cod în handlerul de panică — doar scrii în paralel ultimele linii
acolo, tot timpul, într-un inel.

Fă varianta cu RTC_NOINIT prima. Dacă se dovedește insuficientă, discută cu
Edward înainte să te apuci de panic handler.

### 2.3 Task watchdog `[30 min]`

`CONFIG_ESP_TASK_WDT_PANIC` nu e setat, deci un task blocat doar loghează — în
jurnalul care va dispărea. Înscrie explicit taskul de randare și perechea
reader/decodor la TWDT, cu `CONFIG_ESP_TASK_WDT_PANIC=y` **după** ce 2.1 și 2.2
funcționează, nu înainte. Ordinea contează: fără coredump, un panic de watchdog
îți dă mai puțină informație decât un task blocat.

### 2.4 Nepotrivire de flash — de lămurit înainte de 2.1 `[10 min]`

`sdkconfig.defaults.esp32s3:3` declară `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` și
`platformio.ini` `[env:esp32s3]` are `board_build.flash_size = 16MB`. Dar
`components/boards/partitions.csv` are titlul "8MB Flash Minimum" și se termină
exact la `0x800000`.

Ori ai 8 MB liberi nefolosiți, ori bootloaderul declară dublu față de realitate.
**Rezolvă asta cu `esptool.py flash_id` înainte să dimensionezi partițiile.**
Placa trebuie pusă în download mode (BOOT ținut apăsat la reset) — cere-i lui
Edward, și **amintește-i să elibereze BOOT și să dea RESET după**, altfel placa
rămâne blocată în download mode la fiecare pornire.

---

## Etapa 3 — `audio_buffer`: o singură sursă de adevăr

**Unde:** `main/audio/audio_buffer.c`.

Două probleme, aceeași cauză:

1. **`xSemaphoreTakeFromISR()` apelat din context de task** (linia 62), în
   interiorul unui `portENTER_CRITICAL`, ca să țină semaforul de numărare
   sincronizat manual cu `count`. Merge pe ESP-IDF, dar e o abatere de contract
   folosită ca să ocolească faptul că nu poți lua un semafor normal ținând un
   spinlock.
2. **`memmove` peste tot vectorul sortat, în secțiune critică** (liniile
   340-343): până la 1000 × `uint16_t` = 2 KB, cu întreruperile oprite, la
   fiecare consum. La blocuri de 352 cadre înseamnă ~136 de ori pe secundă,
   deci de ordinul a 0.5 MB/s de `memmove` sub spinlock.

**Varianta corectă:** `sorted[]` devine inel cu `head` și `count`, deci
`audio_buffer_take()` devine O(1). Semaforul de numărare dispare complet și e
înlocuit cu `xTaskNotify` către consumator — **există un singur consumator**,
taskul de randare, deci notificarea directă e suficientă și mai ieftină.
Odată cu semaforul dispar și sincronizarea manuală de la linia 62 și drenajul
din `audio_buffer_flush()` (linia ~224).

Inserția (`audio_buffer_put`, căutare binară la liniile ~57 și ~92) rămâne cum e:
în peste 99% din cazuri `pos == count`, deci `memmove`-ul de acolo aproape nu se
execută. **Nu o "optimiza" și pe aia** — ai schimba comportamentul pentru
pachetele dezordonate, care e exact ce a costat sesiuni de depanare.

**Testare:** modulul e izolat și are teste host. Scrie întâi un test care umple
bufferul peste capacitate, forțează calea de overflow, și verifică ordinea la
ieșire. Abia apoi rescrie. `tests/host/run.sh` trebuie să treacă înainte și după.

---

## Etapa 4 — calea de volum

**Unde:** `main/audio/audio_envelope.c:120-162`, `main/audio/audio_output.c:497`.

### 4.1 Fast path în envelope `[1 h]`

Pentru fiecare cadru se evaluează `audio_envelope_shape_q15()` (două înmulțiri
pe 64 de biți), o înmulțire pe 64 de biți și două împărțiri — **inclusiv când
envelope-ul e complet deschis și volumul e la țintă**, adică în peste 99% din
timp. Sunt 48000 de cadre pe secundă.

**Fix:** ramură rapidă când `state == ENVELOPE_OPEN && volume_q15 ==
volume_target_q15`. În acea ramură câștigul e constant, deci e o singură
înmulțire. Iar când `gain == 32768` (volum maxim), **bypass complet** — asta e
și un câștig de calitate: azi semnalul trece prin `(x * 32768) / 32768`, adică
printr-o trunchiere inutilă.

Testele host au deja verificări de monotonicitate; păstrează-le verzi.

### 4.2 Dither TPDF înainte de trunchiere `[1 h]`

PCM5102A **nu are volum hardware** — `CONFIG_DAC_CONTROLS_VOLUME` nu e setat —
deci toată atenuarea e software, la `audio_envelope.c:155-158`:

```c
int32_t l = ((int32_t)pcm[2 * i] * gain) / Q15_ONE;
pcm[2 * i] = (int16_t)l;
```

Rezultat re-trunchiat la 16 biți, fără dither, fără noise shaping (verificat:
nu există dither nicăieri în firmware). La -20 dB, volumul obișnuit seara,
pierzi peste trei biți din 16.

**Fix ieftin și sigur:** rotunjire în loc de trunchiere, plus dither TPDF
(diferența a două variabile aleatoare uniforme pe ±0.5 LSB) înainte de conversia
la `int16_t`. Un generator xorshift32 e destul. Atenție: **nu adăuga dither pe
calea de bypass de la 4.1** (gain unitar trebuie să rămână bit-perfect) și nu-l
adăuga când `gain == 0`.

**Verificare auditivă:** cere-i lui Edward o piesă liniștită, la volum mic,
noaptea. Întreabă-l dacă fondul e mai curat. Dacă nu aude diferența, e în regulă
— schimbarea e corectă oricum și costă nimic.

### 4.3 I2S pe 32 de biți — **AMÂNAT, nu-l face în această rundă**

`main/audio/audio_output.c:497` folosește `I2S_DATA_BIT_WIDTH_16BIT`. PCM5102A
acceptă 32 de biți nativ, iar un pipeline pe `int32` cu produs Q31 păstrat
întreg elimină complet pierderea de precizie, fără dither.

Dar atinge exact contoarele pe care se sprijină măsurarea pipeline-ului:
`output_queued_frames()` (`audio_output.c:151`, `:210`) împarte la
`2 * sizeof(int16_t)` în mai multe locuri, la fel `dma_write()` (`:205`) și
`MAX_RESAMPLE_FRAMES`. Alinierea sub-milisecundă de la seek depinde de ele.

**Fă-l abia după ce Etapele 1-6 sunt pe placă și stabile, singur, cu o sesiune
de măsurat `Acquired: ... err=` înainte și după.** Ținta: `|err| < 1 sample`.

---

## Etapa 5 — sparge `audio_timing_read()`

**Unde:** `main/audio/audio_timing.c:478` până la ~1047. **570 de linii,
o singură funcție.**

Face, în ordine: poarta de start, așteptarea lock-ului PTP, drenajul mărginit,
flush-ul deferred, respingerea insulelor de start, detecția de goluri, achiziția
exactă, regimul de tracking, servo-ul de poziție, raportul periodic, copierea
PCM cu shrink/stretch, și actualizarea continuității.

Eticheta `play_frame:` (linia 885) e ținta a **patru** `goto`-uri din contexte
diferite: liniile 639, 747, 773, 783. Cel de la 639 vine de după un
`audio_buffer_flush()`, adică sare peste toate verificările de timing cu un slot
deja scos din pool.

**Fix, pur mecanic, fără nicio schimbare de comportament:** trei funcții cu
contract explicit.

- `drain_and_select_frame()` → întoarce un cadru, sau motivul pentru care nu
- `schedule_frame()` → întoarce `ACQUIRE_SILENCE(n)` / `TRIM(n)` / `PLAY` /
  `PLAY_UNSCHEDULED` / `DROP`
- `emit_frame()` → copiere plus servo

Cele patru `goto play_frame` devin `return PLAY_UNSCHEDULED`.

**Regulă strictă:** această etapă **nu are voie să schimbe niciun comportament**.
Rulează `tests/host/run.sh` înainte și după; ieșirea buclei de timing cu PCM și
ceas fake trebuie să fie identică bit cu bit. Dacă nu e, ai schimbat ceva.

Etapa 5 e prerechizit pentru Etapa 6. Nu sări peste ea.

---

## Etapa 6 — mașina de stare seek/flush, pe conceptul de segment

**Cea mai mare datorie tehnică din repo, și cea mai riscantă etapă.**

Azi, "senderul a mutat capul de citire" e implementat prin **opt** mecanisme
independente care se suprapun:

1. `buffered_generation` — contor de generație pe pachete comprimate
   (`main/audio/audio_receiver_internal.h:56`)
2. `discard_all_until_anchor` — poartă oarbă (`:111`)
3. `arm_gate_on_next_anchor` — flag ca poarta să se armeze la următoarea ancoră (`:105`)
4. `discard_before_rtp` / `discard_above_rtp` — fereastră RTP (`:97-100`)
5. `live_flush_pending` + `live_flush_last_ts` — filtru de continuitate pe
   decodor (`:115-118`, logica la `main/audio/audio_stream_buffered.c:214-234`)
6. `deferred_flush_pending` cu `flush_from_ts`/`flush_until_ts` — interval sărit
   în motorul de timing (`main/audio/audio_timing.c:608-672`)
7. `paused_rtp` — snapshot folosit ca referință pentru detecția de seek (`:129`)
8. Path A / Path B / Phase 2 în `audio_receiver_set_anchor_time()`
   (`main/audio/audio_receiver.c:254-405`) — ~150 de linii de euristici cu
   praguri de 5 s și 10 s

Plus două căi separate de recuperare: "Deferred flush cancelled"
(`audio_timing.c:622-639`, care sare cu `goto play_frame` în mijlocul buclei) și
"No anchor 10 s after the flush" (`audio_stream_buffered.c:177-189`).

Citește comentariile din zona asta: fiecare descrie un bug reparat cu un plasture
peste plasturele anterior. "Without this...", "which would cause a second flush
and double the startup delay", "causing 6+ seconds of silence".

### Varianta corectă: un singur concept, **segmentul**

Un segment = `(id monoton, ancoră opțională, interval RTP valid, skip_range
opțional)`.

- Orice `FLUSH`, orice `FLUSHBUFFERED` (imediat sau deferred) și orice
  `SETRATEANCHORTIME` care descrie o poziție incompatibilă cu segmentul curent
  **creează un segment nou**, cu id incrementat. Un singur loc, un singur contor.
- Cititorul TCP ștampilează fiecare slot cu id-ul segmentului curent — asta face
  deja, sub numele de `generation`.
- **Chunk-urile PCM moștenesc id-ul segmentului.** Asta lipsește azi și e cauza
  rădăcină: după decodare informația de segment se pierde, și trebuie
  reconstruită din timestamp-uri — de aici toate porțile RTP și toate euristicile
  cu praguri.
- Motorul de timing redă doar segmentul curent și aruncă restul cu o singură
  comparație de `int32`. **Dispar mecanismele 2, 3, 4 și 5.**
- Un segment fără ancoră după N ms se redă nescheduled și își notează asta —
  o singură cale de recuperare în loc de trei.
- Flush-ul deferred rămâne, dar ca proprietate a segmentului (`skip_range`), nu
  ca stat global în `audio_timing_t`.

### Cum se face fără să spargi tot

**Nu porni fără harness.** Ordinea obligatorie:

1. Scrie în `tests/host/` un test per comportament actual, opt la număr, care
   reproduce fiecare din cele opt mecanisme prin secvențe de intrare (pachete cu
   timestamp, flush-uri, ancore) și fixează ieșirea așteptată. **Testele astea
   trebuie să treacă pe codul de azi, nemodificat.** Dacă unul nu trece, ai
   înțeles greșit comportamentul — oprește-te și clarifică.
2. Abia apoi rescrie sub ele.
3. Pe hardware, cere-i lui Edward exact secvențele astea, în ordine, una câte una:
   - play, pauză, play (reia din același loc, fără tăietură)
   - play, next în timpul redării (skip live — ancoră păstrată, reluare sub 1 s)
   - play, pauză, next (seek flush — ancoră nouă în 300-500 ms)
   - seek în interiorul piesei, de zece ori
   - lăsat să treacă natural la finalul piesei, cu crossfade activat în Apple
     Music (**flush deferred adevărat — ăsta n-a fost niciodată testat pe
     hardware**, e singurul din cele opt care n-are confirmare)

### Ce trebuie să știi despre iPhone înainte să atingi codul ăsta

Citește memoria `airplay-flush-semantics` din
`~/.claude/projects/-Users-edward-airplay-fable/memory/`. Pe scurt, dar nu
sări peste fișier:

- **`FLUSHBUFFERED` imediat, în timpul redării** = skip de piesă. Telefonul
  păstrează ancora și trimite noua piesă pe o axă RTP nouă, ancorând 1-5 s mai
  târziu, cu timpul ancorei **în trecut**.
- **`FLUSHBUFFERED` imediat, după `rate=0`** = seek. Urmează un
  `SETRATEANCHORTIME`.
- **`FLUSHBUFFERED` deferred (`flushFrom`/`flushUntil`)** = sari peste un
  interval pe aceeași axă. Atenție: `from` poate fi **peste** rtp-ul de start al
  piesei noi, deci regula naivă "aruncă `[from, until)` și așteaptă un cadru
  ≥ `until`" se blochează la infinit.
- **Senderul scrie toată rafala post-flush (tot avansul lui de ~20 s) în socketul
  de date ÎNAINTE să trimită `SETRATEANCHORTIME`, iar firul lui RTSP se blochează
  în acea scriere.** Deci **nu strangula niciodată socketul de date cât aștepți
  o ancoră**. Dacă cititorul nu mai ia de pe TCP, nu vine nicio ancoră, nu vine
  niciun request RTSP, și telefonul dărâmă sesiunea. Asta a costat o sesiune
  întreagă de depanare.
- Un socket de date liniștit, cu audio în buffer, e normal. **Nu închide
  niciodată socketul de date cât sesiunea RTSP e vie.**

---

## Ce NU se face în această rundă

- Orice feature: EQ software, copertă, MQTT, deșteptător, vizualizator, generator
  de ton, control de redare din WebUI, pagini web înglobate în firmware,
  autentificare pe endpoint-uri. Toate sunt discutate și aprobate ca direcție,
  dar **vin după**. Dacă termini Etapele 1-6, întreabă-l pe Edward ce urmează.
- I2S pe 32 de biți (4.3) și `DELAY_REQ` în PTP. Ambele sunt corecte tehnic —
  `main/network/ptp_clock.c:157-164` recunoaște în comentariu că offsetul conține
  un bias de întârziere de cale, iar `PIPELINE_LATENCY_US` (`audio_timing.c:21`,
  1500 µs) e o constantă reglată după ureche care compensează exact acel bias.
  Dar amândouă ating contoarele de aliniere. După ce restul e stabil, câte una,
  cu măsurători între ele.
- Ștergerea backend-urilor fosile `main/audio/audio_output_usb.c` și
  `audio_output_spdif.c`. Sunt copii bit-rotate ale buclei de randare de dinainte
  de rescrierea 5.1, nu se compilează pe placa asta, și au nume înșelător
  (`audio_output_usb.c` e "ESP32 ca microfon USB", complet diferit de sursa USB
  speaker din `main/usb/usb_audio_source.c`). **Propune-i lui Edward să le
  ștergi din fork, dar nu o face fără acordul lui** — e codul upstream.

---

## Rezumatul ordinii

| Etapă | Ce | Efort | Risc |
|---|---|---|---|
| 1.1 | Fix OTA eșuat care omoară AirPlay | 15 min | zero |
| 1.2 | Buffere de randare în DRAM intern | 45 min | mic |
| 1.3 | I-cache 32 KB, D-cache 64 KB | 10 min | mic |
| 1.4 | Elimină dubla formatare a logurilor | 10 min | zero |
| 1.5 | `usb_pull`: buffer 4 KB → 1.4 KB | 10 min | zero |
| 1.6 | `max_open_sockets` 3 → 5 | 10 min | mic |
| 1.7 | CPU% în `/api/tasks` | 15 min | mic |
| 2.1 | Coredump în flash + `/api/coredump` | 2-3 h | mic |
| 2.2 | Jurnal care supraviețuiește panicii (RTC_NOINIT) | 2 h | mic |
| 2.3 | Task watchdog cu panic | 30 min | mediu |
| 2.4 | Lămurit 8 vs 16 MB flash | 10 min | zero |
| 3 | `audio_buffer`: inel + `xTaskNotify` | 3 h | mic |
| 4.1 | Fast path în envelope | 1 h | mic |
| 4.2 | Dither TPDF | 1 h | mic |
| 5 | Spargerea lui `audio_timing_read()` | 4 h | mediu |
| 6 | Mașina de stare pe segmente | 2-3 zile | mare |

Etapa 1 într-o singură sesiune, un OTA la final, apoi cere-i lui Edward
cincisprezece minute de ascultat înainte să treci la 2.
