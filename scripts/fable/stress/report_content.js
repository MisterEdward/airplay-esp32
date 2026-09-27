/* Prose and chips for the report; D is defined by the page script. */
const n = v => v.toLocaleString('ro-RO');
const s2 = ms => (ms / 1000).toLocaleString('ro-RO', {minimumFractionDigits: 2, maximumFractionDigits: 2}) + ' s';

function LEAD_TEXT(K, total, hours) {
  return `În ${n(total)} de acțiuni trimise din Music pe Mac, de la ritm normal până la 20 de comenzi pe secundă, boxa nu s-a resetat niciodată, n-a pierdut sincronizarea PTP și și-a revenit după fiecare furtună în sub o secundă. Singura cădere a venit în anduranță: Mac-ul a închis sesiunea în mijlocul unei piese, iar Music a continuat să arate „playing” fără sunet. ` +
    `La latență e la egalitate cu un receiver AirPlay 2 comercial: seek în ${s2(K.seek.esp.med)} față de ${s2(K.seek.tv.med)} pe TV, iar la next, prev și pause e mai rapidă. ` +
    `Rămâne în urmă la două lucruri pe care TV-ul le face bine: un <b>pop audibil la play după pauză</b> și <b>pornirea de sesiune</b> (${s2(K.sstart.esp.med)} față de ${s2(K.sstart.tv.med)}).`;
}

const OBS = {};
(function () {
  const K = Object.fromEntries(D.compare.map(c => [c.key, c]));
  const chip = (c, t) => `<span class="chip ${c}">${t}</span>`;
  OBS.seek = chip('good', '0 cadre pierdute');
  OBS.next = chip('muted', `jurnal: ${s2(K.next.esp_journal.acq.med)}`);
  OBS.prev = chip('muted', `jurnal: ${s2(K.prev.esp_journal.acq.med)}`);
  OBS.play2 = chip('bad', `pop ${K.play2.esp_pops}/${K.play2.esp.n_total}`);
  OBS.play60 = chip('bad', `pop ${K.play60.esp_pops}/${K.play60.esp.n_total}`);
  OBS.sstart = chip('warn', 'crypto 1,6 s');
  OBS.seekafternext = chip('good', 'mai rapid ca TV');
  OBS.seekload = chip('warn', `p90 ${s2(K.seekload.esp.p90)}`);
  OBS.rc = chip('bad', 'TV: blocat 59 s');
})();

const BD_NOTE = 'La seek, Mac-ul trimite pauză + flush în ~100 ms, apoi are nevoie de ~0,5 s ca să pregătească audio de la noua poziție și să trimită ancora, pe care o pune ~250 ms în viitor. La play după pauză, ancora vine imediat, dar e pusă ~0,7 s în viitor. La start de sesiune, cea mai mare parte e conectarea, din care 1,6 s sunt calcule de pairing pe ESP32.';

function STAGES_FN() {
  const K = Object.fromEntries(D.compare.map(c => [c.key, c]));
  const G = D.group;
  const errG = Math.max(...Object.values(G).map(g => g.err_max_us || 0));
  const out = [
    {eyebrow: 'Etapa 1 · ~17 min per dispozitiv', title: 'Realist', chip: ['warn', 'trece, cu pop'], items: [
      '20 de seek-uri la 10 s, 10 next, 10 prev, 10 pauze de 2 s și 5 de 60 s, 5 opriri și porniri de sesiune, 10 schimbări de volum.',
      `Seek ${s2(K.seek.esp.med)} față de ${s2(K.seek.tv.med)} pe TV. Next, prev și pause sunt mai rapide decât pe TV: ${s2(K.next.esp.med)}, ${s2(K.prev.esp.med)} și ${s2(K.pause.esp.med)}, față de ${s2(K.next.tv.med)}, ${s2(K.prev.tv.med)} și ${s2(K.pause.tv.med)}.`,
      `Fiecare pornire: <code>domain=ptp</code>, |err| ≤ ${K.seek.esp_journal.err_max_us} µs față de ancoră, <code>dropped=0</code>. Reia exact de la versul ales.`,
      `Pop audibil la ${K.play2.esp_pops + K.play60.esp_pops} din ${K.play2.esp.n_total + K.play60.esp.n_total} play-uri după pauză. TV: 0.`,
      `Start de sesiune în ${s2(K.sstart.esp.med)}, față de ${s2(K.sstart.tv.med)} pe TV.`]},
    {eyebrow: 'Etapa 2 · ~5 min', title: 'Utilizator agitat', chip: ['good', 'trece'], items: [
      `Seek la 3, 2 și 1 s: ${s2(K.seek3.esp.med)}, ${s2(K.seek2.esp.med)} și ${s2(K.seek1.esp.med)} mediană, la egalitate cu TV-ul (${s2(K.seek3.tv.med)}, ${s2(K.seek2.tv.med)} și ${s2(K.seek1.tv.med)}).`,
      `10 next în 5 s, pause/play la fiecare secundă (20 de cicluri), volum schimbat la 0,3 s. Seek la 0,3 s după next: ${s2(K.seekafternext.esp.med)}, față de ${s2(K.seekafternext.tv.med)} pe TV.`,
      '0 gaps, 0 underrun-uri DMA, heap constant, nicio sesiune pierdută.']},
    {eyebrow: 'Etapa 3 · ~9 min', title: 'Nerealist', chip: ['good', 'nu pică'], items: [
      '172 de seek-uri în 60 s (la 200–500 ms), 601 next/prev în 30 s, 226 de comenzi haotice la 200 ms, 30 de reconectări la 2 s, load HTTP la 20 Hz + upload + descărcări de jurnal, apoi toate deodată timp de 60 s.',
      'ESP32: 0 resetări, 0 erori la sender, heap intern 135,2 KB constant în toate cele 30 de sesiuni, niciun task scăpat.',
      'După haos: sunet la 0,88 s, iar seek-urile de control la 0,92 s și 0,91 s, cu err sub 20 µs.',
      'TV-ul a cedat primul: o reconectare a blocat Music 59,5 s și s-a terminat cu eroare.',
      `Singura degradare pe ESP32: sub load HTTP, seek-ul ajunge la p90 ${s2(K.seekload.esp.p90)}, pentru că sender-ul trimite ancora mai târziu. Interfața web nu mai răspunde sub load maxim.`,
      'La reconectări la 2 s, prima pornire iese la −1,6…−7,7 ms față de ancoră (PTP abia blocat). Inaudibil singur, contează în multiroom.']},
    {eyebrow: 'Grup · ~5 min', title: 'ESP32 + TV în același grup', chip: ['good', 'sincron'], items: [
      `10 seek-uri (${s2(G.seek.mic.med)} mediană), 5 pause/play, 5 next, 30 s de seek la 0,3–0,5 s, 5 seek-uri de control.`,
      `Toate pornirile ESP32: <code>domain=ptp</code>, |err| ≤ ${errG} µs, <code>dropped=0</code>, inclusiv după furtuna de seek-uri.`,
      `Pop la play și în grup: ${G['play-short'].pops} din 5.`]},
  ];
  if (D.soak) {
    const S = D.soak;
    out.push({eyebrow: 'Etapa 4 · 90 min pe fir, 27.09 seara', title: 'Anduranță', chip: ['good', 'stabil'], items: SOAK_ITEMS(S)});
  }
  return out;
}

function SOAK_STATS(S) {
  const po = S.playout || [];
  const errs = po.map(p => Math.abs(p[1]));
  const sorted = [...errs].sort((a, b) => a - b);
  const pct = q => sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(q * sorted.length))] : null;
  const gaps = po.length ? Math.max(...po.map(p => p[4])) : 0;
  const under = po.length ? Math.max(...po.map(p => p[5])) : 0;
  const local = po.filter(p => p[6] !== 'ptp').length;
  const heaps = (S.heap || []).map(h => h[1]);
  const blocks = (S.heap || []).map(h => h[2]);
  const ups = (S.snaps || []).map(s => s[1]).filter(v => v != null);
  let resets = 0; for (let i = 1; i < ups.length; i++) if (ups[i] < ups[i - 1]) resets++;
  const steps = (S.snaps || []).map(s => s[4]).filter(v => v != null);
  return {n: po.length, p50: pct(0.5), p99: pct(0.99), max: sorted[sorted.length - 1], gaps, under, local,
    heapMin: heaps.length ? Math.min(...heaps) : null, heapMax: heaps.length ? Math.max(...heaps) : null,
    blockMin: blocks.length ? Math.min(...blocks) : null, resets, steps: steps.length ? Math.max(...steps) : 0};
}
function SOAK_CHIP(S) {
  const t = SOAK_STATS(S);
  return (t.resets || t.gaps || t.under || t.local) ? ['warn', 'cu observații'] : ['good', 'stabil'];
}
function SOAK_ITEMS(S) {
  const t = SOAK_STATS(S);
  const sk = S.seek;
  return [
    `${sk.n_total} de seek-uri, câte unul pe minut, plus câte o furtună la fiecare 15 minute (20 de seek-uri la 0,4 s, 6 next, pause/play). Sunet la ${s2(sk.med)} mediană.`,
    `Music a închis sesiunea de ${(S.disconnects || []).length} ori (timer-ul de inactivitate). Paznicul din harness a reselectat boxa de fiecare dată în ~45 s. Zonele roșii din grafic sunt aceste pauze.`,
    'Rulări anterioare: 26.09 (45 min, oprită pentru TV) și 27.09 dimineața (87 min, cu PTP pierdut după căderea WiFi); cifrele de aici sunt din rularea de seară, măsurată pe fir.',
    `Eroare de sincronizare pe ${n(t.n)} de rânduri Playout: mediana ${n(t.p50)} µs, p99 ${n(t.p99)} µs, max ${n(t.max)} µs. <code>gaps=${t.gaps}</code>, <code>under=${t.under}</code>, rânduri în afara PTP: ${t.local}.`,
    `Resetări: ${t.resets}. Pași de timescale PTP: ${t.steps}.`];
}

function SOAK_RENDER(root, S) {
  const title = document.getElementById('soak-title');
  if (!S) { title.textContent = 'Anduranță: în lucru'; root.innerHTML = '<p class="intro">Rularea de 90 de minute nu s-a terminat când a fost generată pagina.</p>'; return; }
  const t = SOAK_STATS(S);
  title.textContent = '90 de minute pe fir: stabil, pe PTP tot timpul';
  const po = S.playout || [];
  const W = 900, H = 220, L = 54, R = 12, T = 12, B = 30;
  const t0 = po.length ? po[0][0] : 0, t1 = po.length ? po[po.length - 1][0] : 1;
  const x = u => L + (u - t0) / Math.max(1, t1 - t0) * (W - L - R);
  const lim = 3000;
  const y = e => T + (1 - (Math.max(-lim, Math.min(lim, e)) + lim) / (2 * lim)) * (H - T - B);
  let svg = `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="eroare de sincronizare în timp">`;
  for (const g of [-3000, -2000, -1000, 0, 1000, 2000, 3000]) {
    svg += `<line x1="${L}" x2="${W - R}" y1="${y(g)}" y2="${y(g)}" stroke="var(--grid)" stroke-width="${g === 0 ? 1.4 : 0.8}"/>`;
    svg += `<text x="${L - 6}" y="${y(g) + 3.5}" font-size="10.5" text-anchor="end" fill="var(--muted)" font-family="IBM Plex Mono, monospace">${g / 1000} ms</text>`;
  }
  const mins = (t1 - t0) / 60000;
  for (let m = 0; m <= mins; m += 15) {
    const xx = x(t0 + m * 60000);
    svg += `<line x1="${xx}" x2="${xx}" y1="${T}" y2="${H - B}" stroke="var(--grid)" stroke-width="0.8"/>`;
    svg += `<text x="${xx}" y="${H - B + 16}" font-size="10.5" text-anchor="middle" fill="var(--muted)" font-family="IBM Plex Mono, monospace">${m} min</text>`;
  }
  let path = '';
  po.forEach((p, i) => {
    const gap = i && p[0] - po[i - 1][0] > 5000;
    path += (i && !gap ? 'L' : 'M') + x(p[0]).toFixed(1) + ' ' + y(p[1]).toFixed(1);
  });
  // Music's idle disconnects: from the TEARDOWN to the next playout line
  for (const td of (S.teardowns || [])) {
    if (td < t0 || td > t1) continue;
    const next = po.find(p => p[0] > td);
    const a = x(td), b = x(next ? next[0] : td + 50000);
    svg += `<rect x="${a}" y="${T}" width="${Math.max(2, b - a)}" height="${H - T - B}" fill="var(--bad-soft)"/>`;
    svg += `<text x="${a + 3}" y="${T + 12}" font-size="10" fill="var(--bad)" font-family="IBM Plex Sans Condensed, sans-serif">Music</text>`;
  }
  svg += `<path d="${path}" fill="none" stroke="var(--esp)" stroke-width="1.2" stroke-linejoin="round"/>`;
  svg += '</svg>';
  const kv = `<dl class="kv">
    <dt>Rânduri Playout</dt><dd>${n(t.n)}</dd>
    <dt>|err| mediană / p99 / max</dt><dd>${n(t.p50)} / ${n(t.p99)} / ${n(t.max)} µs</dd>
    <dt>gaps / under / non-PTP</dt><dd>${t.gaps} / ${t.under} / ${t.local}</dd>
    <dt>Heap intern la reconectare</dt><dd>${t.heapMin != null ? n(t.heapMin) + '–' + n(t.heapMax) + ' B în ' + (S.heap || []).length + ' sesiuni' : '—'}</dd>
    <dt>Resetări / pași PTP</dt><dd>${t.resets} / ${t.steps}</dd>
    <dt>Seek (n=${S.seek.n_total}, pe fir)</dt><dd>mediană ${s2(S.seek.med)}, p90 ${s2(S.seek.p90)}</dd></dl>`;
  root.innerHTML = `<p class="intro">Eroarea de sincronizare raportată de boxă o dată pe secundă (<code>Playout: err</code>). Linia de zero este programul exact din ancora Mac-ului. Zonele roșii sunt pauzele în care Music a deconectat boxa.</p>${svg}${kv}`;
}

const BUGS = [
  {title: 'Pop la play după o pauză simplă', sev: ['good', 'reparat 27.09'],
    what: `La fiecare reluare după o pauză fără seek, boxa scoate un click scurt la ~65 ms după ce primește play, cu ~0,7 s înainte de muzică. Pe ESP32 apare la ${D.pops.esp.pops} din ${D.pops.esp.plays} de play-uri, pe TV la ${D.pops.tv.pops} din ${D.pops.tv.plays}; l-ai confirmat și tu pe telefon. După seek sau skip nu apare. A apărut după fix-ul de snapshot la pauză (7f713a4): până atunci, reluarea declanșa din greșeală un flush care ascundea problema.`,
    ev: 'rate=1.0 -> PLAY (was_paused=1)   First early frame early=680 ms\npop pe microfon: +8 ms, +8 ms, −2 ms față de linia RTSP (test cu telefonul)\nseek/skip (flush, start=quick): 0 pop-uri',
    fix: 'Lanțul de redare e read → resample → envelope. Flush-ul resetează resampler-ul, pauza nu. Primul bloc de liniște programată de la reluare iese din resampler cu ultimele eșantioane de muzică în istoria filtrului și nu trece prin envelope. Soluția: <code>audio_resample_reset()</code> când se termină fade-ul de pauză (<code>audio_output.c:434</code>), sau zerouri forțate pentru blocurile non-media după resampler.'},
  {title: 'Sesiune închisă de Mac, Music rămâne pe „playing” fără sunet', sev: ['muted', 'cauza: Music (timer de inactivitate)'],
    what: 'În anduranță, după 39 de minute de sesiune, Mac-ul a încetat să mai trimită audio (buffer-ul boxei a scăzut de la 900 la 772 de cadre în ~2 s), apoi a închis conexiunea și a trimis TEARDOWN complet în mijlocul piesei. Boxa nu avusese nicio eroare: ultimul seek pornise cu err=−14 µs. Music a rămas selectat pe Bedroom Speakers și a arătat „playing” ~10 minute, fără să reîncerce conexiunea; ai observat tu liniștea. Boxa a rămas tot timpul accesibilă (port 7000 deschis, anunț mDNS corect).',
    ev: 'I (20704159) Playout: … buffered=873     I (20705129) Playout: … buffered=772\nI (20705479) audio_buf: Buffered audio connection closed by peer\nI (20705519) rtsp_handlers: sid=61 TEARDOWN: has_streams=0\n… nicio conexiune nouă până la reselectarea manuală (21281259, sid=63)',
    fix: 'Găsit pe 27.09 în log-ul Mac-ului: Music execută <code>kDisconnectDevices</code> „due idle timeout”, la ~73 s după un seek făcut prin AppleScript, deși redarea continuă. Nu ține de boxă, așa că nu se repară în firmware. În harness, un paznic reselectează boxa; în uz normal, un seek din interfață probabil nu declanșează cursa.'},
  {title: 'Legătura WiFi pe 2,4 GHz cade periodic', sev: ['bad', 'liniște; mediu sau hardware'],
    what: 'Pe 27.09, între 10:40 și 11:08, ping-ul spre boxă a urcat la ~85 ms cu ~20 % pierderi, deși RSSI-ul a rămas la −60, iar Mac-ul (pe 5 GHz) vedea routerul normal. Boxa primea ~10 pachete audio pe secundă din ~43 necesare și s-a făcut liniște. De când e alimentată din Mac, ping-ul stă la ~7 ms, fără pierderi.',
    ev: 'ping 10:40–11:08: median 83–144 ms, pierderi 10–24 % pe fiecare fereastră de 2 minute\nMac Send-Q spre boxă: ~130 KB blocați; router: 22 ms',
    fix: 'Întâi verificarea alimentării (a revenit la normal pe USB-ul Mac-ului) și a canalului 2,4 GHz. În firmware se poate doar atenua: la o legătură proastă prelungită, reconectare WiFi sau alt nod mesh.'},
  {title: 'PTP pierdut după o cădere WiFi', sev: ['warn', 'reparat, în test'],
    what: 'După căderea WiFi, boxa n-a mai primit niciun pachet PTP (<code>sync_count 0</code>) în sesiunile următoare, până la restart. Fiecare seek aștepta 1,5 s și pornea pe ceasul local: ~2,1 s în loc de ~0,8 s, cu eroare de sincronizare ~4,5 ms.',
    ev: 'W audio_time: PTP not locked to 3c06307f6ae40008 after 1502 ms … local timeline latched  (×71)',
    fix: 'Commit 2ea4409: când o sesiune așteaptă PTP și nu vine nimic 5 s, boxa reintră în grupul multicast 224.0.1.129 (IGMP nou). Contor <code>ptp.rejoins</code> în /api/status.'},
  {title: 'Pornirea sesiunii durează 3,7 s', sev: ['good', 'reparat 27.09: 1,73 s'],
    what: 'De la selectarea boxei în Music până la sunet trec 3,7 s, față de 2,2 s pe TV. Din cele ~2,9 s dintre conectare și sunet, 1,6 s sunt primele două POST-uri de pairing, calculate pe ESP32.',
    ev: 'W rtsp_handlers: sid=6 #2 POST took 733 ms\nW rtsp_handlers: sid=6 #3 POST took 882 ms   (în fiecare sesiune, 98 de apariții)',
    fix: 'Profilare pe criptografia de pair-verify (X25519, Ed25519, ChaCha20-Poly1305). Pe S3 aceste operații ar trebui să dureze zeci de ms, nu sute: fie folosesc implementări software lente, fie rulează pe un task cu prioritate mică sub load audio.'},
  {title: 'Grandmaster PTP schimbat în timpul redării', sev: ['bad', 'desincronizare'],
    what: 'Apărut o dată azi, în sesiunea ta de pe Mac, înainte de test: sursa PTP urmărită a început să anunțe alt grandmaster în timp ce cânta. Filtrul s-a re-blocat pe noul offset, dar ancora veche a rămas, așa că 16 s mai târziu boxa a găsit ancora la 12 s în viitor și a pornit nesincronizată, aruncând 4048 de cadre.',
    ev: 'W ptp_clock: Tracked source changed grandmaster: re-locking\nW audio_buf: Data socket quiet 7991 ms while playing\nW audio_time: Anchor 12242 ms in the future — implausible, playing unscheduled\nI audio_time: Acquired: … dropped=4048 … re-acquire (no anchor)',
    fix: 'La schimbarea de grandmaster în mijlocul redării, ancora trebuie translatată cu saltul de offset măsurat, la fel ca la pasul de timescale deja reparat, sau ignorată până vine ancora nouă. Fără reset orb al filtrului.'},
  {title: 'Prima pornire după reconectări rapide iese decalată cu până la 7,7 ms', sev: ['warn', 'multiroom'],
    what: 'Când sesiunea pornește la 1–2 s după conectare, PTP-ul e blocat pe doar 6–7 eșantioane, cu o deviație de ~13 ms. Prima pornire iese atunci la −1,6…−7,7 ms față de ancoră și aruncă 19–31 de cadre. Singur nu se aude; lângă altă boxă ar fi un ecou scurt până corectează servo-ul.',
    ev: 'I ptp_clock: LOCKED: … dev=13763380ns samples=7\nI audio_time: Acquired: err=-7671 us … trimmed=338 dropped=19',
    fix: 'Blocarea PTP pentru ancoră să ceară o deviație sub ~1 ms, sau mai multe eșantioane, înainte să fie folosită.'},
  {title: 'Fiecare deconectare blochează 1 s', sev: ['muted', 'latență'],
    what: 'La TEARDOWN, serverul RTSP așteaptă 1 s task-ul de evenimente, care nu iese la timp. În furtuna de reconectări a apărut de 37 de ori, iar sesiunea următoare pornește cu 1 s mai târziu. Nu există leak: rămân 24 de task-uri.',
    ev: 'W rtsp_handlers: Event port task did not exit within timeout   (×37)',
    fix: '<code>shutdown()</code> pe socket-ul de evenimente înainte de așteptare, sau așteptarea mutată în afara task-ului RTSP.'},
  {title: 'Eroare de decodare AAC la fiecare seek', sev: ['muted', 'cosmetic'],
    what: 'Pachetul de 36 de octeți de la <code>untilSeq</code> ajunge în decoder, care dă eroare și se resetează. Nu se aude, dar umple jurnalul și costă un reset de decoder la fiecare seek.',
    ev: 'E ESP_AAC_DEC: Failed to decode aac frame, error:20.\nW audio_dec: AAC decode error -1 — resetting decoder',
    fix: 'Pachetele de sub ~40 de octeți de la granița flush-ului să nu mai fie trimise la decoder.'},
  {title: 'Interfața web nu mai răspunde sub load maxim', sev: ['muted', 'nerealist'],
    what: 'Cu /api/status la 20 Hz, descărcări de jurnal și upload în paralel, snapshot-ul de verificare n-a mai primit răspuns. Audio a continuat normal.', ev: '', fix: ''},
  {dim: true, title: 'Nereprodus: crackle în golul de seek', sev: ['muted', 'probabil zgomot din cameră'],
    what: 'În etapa 1, microfonul a prins scurte rafale în liniștea de la 10 din 20 de seek-uri, cât ai fost în cameră. După aceea: 0 din 40 de seek-uri (cu LED-ul aprins și stins), 0 în testul cu trafic WiFi forțat, 0 în 280 s de pauză. Nu l-am trecut ca bug.', ev: '', fix: ''},
];

const UPDATE = [
  ['Pop la play după pauză', ['good', 'reparat'], 'Resampler-ul se resetează la final de pauză, iar blocurile fără muzică ies ca zerouri. Pe microfon: vârful după play a scăzut de la 27–38 dB la 2–6 dB, 0 pop-uri din 8. Commit 36eaced, pe <code>main</code>.'],
  ['Pornirea sesiunii', ['good', 'mai rapidă decât TV'], 'SRP-ul din pairing: cheie secretă de 256 de biți, acceleratorul hardware folosit corect și chei pregătite dinainte. M1 durează acum 0,07 ms (de la 730 ms), M3 141 ms (de la 880 ms). De la click la primul sample: <b>1,73 s</b> (înainte 3,61 s); cu fade-in, ~1,85 s la ureche, față de 2,23 s pe TV. Pe boxă, încă nepublicat: aștept testul tău de pe iPhone.'],
  ['WiFi după OTA', ['warn', 'atenuat'], 'După două OTA-uri boxa n-a mai revenit în rețea până la power-cycle. Acum se deconectează curat înainte de orice restart, iar un watchdog o repornește după 3 minute fără IP (a doua oară cu deep-sleep). La OTA-ul următor a revenit singură. Pe boxă, nepublicat.'],
  ['Închiderea sesiunii („sesiunea fantomă”)', ['muted', 'cauza: Music, nu boxa'], 'Log-ul intern al Mac-ului a arătat că <b>aplicația Music</b> deconectează singură ieșirea AirPlay: <code>auto disconnecting from selected devices due idle timeout</code>. Un timer de inactivitate armat la un seek făcut prin AppleScript nu e oprit la reluare și expiră ~73 s mai târziu. Pe partea AirPlay, seek-ul fatal arată identic cu unul normal, cu boxa activă și datele curgând. Apoi Music cade pe difuzoarele Mac-ului.'],
  ['PTP pierdut după o cădere WiFi', ['warn', 'reparat, în test'], 'După căderea de legătură de dimineață, boxa n-a mai primit niciun pachet PTP până la restart; restartul l-a readus, deci problema era la boxă (abonamentul multicast pierdut). Fix: dacă o sesiune așteaptă PTP și nu vine nimic 5 s, boxa reintră în grupul multicast (commit 2ea4409, pe boxă din 20:53).'],
  ['Captură audio pe fir (unealtă nouă)', ['good', 'funcționează'], 'Boxa apare pe Mac și ca intrare USB („Bedroom Speakers Audio”) cu exact eșantioanele trimise spre DAC; opțional, DAC-ul e mut. Înregistrare fără pierderi, fără zgomot de cameră, fără sunet în casă. Firmware de test, oprit implicit.'],
  ['Anduranță 90 min pe fir (seara)', ['good', 'stabil'], 'Niciun reset, niciun underrun, PTP tot timpul. Seek: mediană <b>0,80 s</b>, p90 1,18 s. Sincronizare: |err| median 157 µs, p99 1 ms. Heap intern stabil. Singurele întreruperi au fost cele 4 deconectări făcute de Music. Anduranța de noapte (4 ore) rulează acum.'],
  ['Legătura WiFi pe 2,4 GHz', ['warn', 'hardware sau mediu'], '10:40–11:08: ping ~85 ms și ~20 % pierderi spre boxă, cu RSSI stabil la −60, în timp ce Mac-ul (pe 5 GHz) vedea routerul normal. Boxa a rămas fără date și s-a făcut liniște. Suspecți: interferență pe canalul 1 sau alimentarea din PS5.'],
  ['Music blocat pe Mac', ['muted', 'nu e firmware-ul'], 'Music rula de 11 zile: arăta „playing”, dar nu trimitea date nici boxei, nici TV-ului. După repornirea aplicației a mers normal. Tot pe Mac, o piesă din cloud a rămas blocată la 0:00.'],
];
const UPDATE_NOTE = 'Pe scurt: problemele rămase în boxă sunt legătura WiFi pe 2,4 GHz (mediu sau hardware) și PTP-ul după căderi (reparat, în test). Sesiunile fantomă vin din Music. Boxa e acum alimentată din Mac, iar căderile WiFi n-au mai apărut (ping ~7 ms).';

const RAW = [
  ['Director', '<span class="path">/private/tmp/claude-501/…/scratchpad/stress/</span>'],
  ['Per rulare', '<span class="path">runs/&lt;id&gt;/actions.tsv, results.tsv, mic.env, journal-*.log, snaps.jsonl, clock.tsv</span>'],
  ['Harness', '<span class="path">bin/run.py, plans.py, drive.js, miclevel.swift, analyze.py, stats.py</span>'],
  ['Audio', 'nu s-a păstrat nicio înregistrare, doar nivelul la 10 ms'],
];
const STAGES = STAGES_FN();
