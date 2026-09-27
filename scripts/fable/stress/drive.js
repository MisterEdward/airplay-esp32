// JXA driver for Music: reads one command per line on stdin, prints
// "<t0_ms> <t1_ms> <cmd> <result>" with epoch ms taken right around the
// Apple Event.  Commands: seek N | next | prev | pause | play | vol N |
// dvol DEVICE=N | only DEVICE | pos | state | quit
ObjC.import('Foundation');
ObjC.import('stdlib');
const M = Application('Music');
const out = $.NSFileHandle.fileHandleWithStandardOutput;
function say(s) { out.writeData($(s + '\n').dataUsingEncoding($.NSUTF8StringEncoding)); }
const stdin = $.NSFileHandle.fileHandleWithStandardInput;
let buf = '';
function dev(name) { return M.airplayDevices.byName(name); }
function run(line) {
  const [cmd, ...rest] = String(line).trim().split(' ');
  const arg = rest.join(' ');
  let r = '';
  const t0 = Date.now();
  try {
    switch (cmd) {
      case 'seek': M.playerPosition = parseFloat(arg); break;
      case 'next': M.nextTrack(); break;
      case 'prev': M.previousTrack(); break;
      case 'pause': M.pause(); break;
      case 'play': M.play(); break;
      case 'vol': M.soundVolume = parseInt(arg); break;
      case 'dvol': { const [n, v] = arg.split('='); dev(n).soundVolume = parseInt(v); break; }
      case 'only': {  // select exactly this device (by name)
        dev(arg).selected = true;
        for (const d of M.airplayDevices()) if (d.name() !== arg && d.selected()) d.selected = false;
        break; }
      case 'start': {  // start I: play track I of playlist PL (context = playlist)
        const pl = M.playlists.byName('Replay All Time');
        M.play(pl.tracks[parseInt(arg)]); break; }
      case 'group': {  // group A|B: select exactly these devices
        const names = arg.split('|');
        for (const n of names) dev(n).selected = true;
        for (const d of M.airplayDevices()) if (!names.includes(d.name()) && d.selected()) d.selected = false;
        break; }
      case 'pos': r = String(M.playerPosition()); break;
      case 'state': r = M.playerState() + '|' + M.currentTrack.name() + '|' + M.playerPosition(); break;
      case 'quit': say(`${t0} ${Date.now()} quit`); $.exit(0);
      default: r = 'unknown';
    }
  } catch (e) { r = 'ERR ' + e; }
  const t1 = Date.now();
  say(`${t0} ${t1} ${cmd} ${arg.replace(/ /g,'_')} ${r.replace(/ /g,'_')}`);
}
while (true) {
  const d = stdin.availableData;
  if (Number(d.length) == 0) break;
  buf += ObjC.unwrap($.NSString.alloc.initWithDataEncoding(d, $.NSUTF8StringEncoding));
  let i;
  while ((i = buf.indexOf('\n')) >= 0) { run(buf.slice(0, i)); buf = buf.slice(i + 1); }
}
