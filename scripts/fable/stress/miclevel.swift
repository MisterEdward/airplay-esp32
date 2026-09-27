// Mic level logger with hardware timestamps.
// Prints "epoch_ms rms_db band_db hf_db peak_db hfpeak_db" per 10 ms block
// (hf = >2.5 kHz: a pop is broadband, a fade-in is not) (band = 100-800 Hz via
// a 2nd-order band-pass pair), epoch from AVAudioTime.hostTime.
import AVFoundation
import Foundation

setvbuf(stdout, nil, _IOLBF, 0)
let engine = AVAudioEngine()
let input = engine.inputNode
let fmt = input.outputFormat(forBus: 0)
let sr = fmt.sampleRate
let blk = Int(sr / 100)
var tb = mach_timebase_info_data_t(); mach_timebase_info(&tb)
// host time -> epoch: sample both clocks now
let hostNow = mach_absolute_time()
let epochNow = Date().timeIntervalSince1970 * 1000
func hostToEpoch(_ h: UInt64) -> Double {
  let dns = (Double(h) - Double(hostNow)) * Double(tb.numer) / Double(tb.denom)
  return epochNow + dns / 1e6
}
// biquads: HP 100 Hz then LP 800 Hz (RBJ)
struct BQ { var b0=0.0,b1=0.0,b2=0.0,a1=0.0,a2=0.0,z1=0.0,z2=0.0
  mutating func run(_ x: Double) -> Double { let y = b0*x + z1; z1 = b1*x - a1*y + z2; z2 = b2*x - a2*y; return y } }
func mk(_ hp: Bool, _ f: Double) -> BQ {
  let w = 2 * Double.pi * f / sr, q = 0.707, al = sin(w) / (2 * q), c = cos(w), a0 = 1 + al
  var b = BQ()
  if hp { b.b0 = (1 + c) / 2 / a0; b.b1 = -(1 + c) / a0; b.b2 = b.b0 }
  else { b.b0 = (1 - c) / 2 / a0; b.b1 = (1 - c) / a0; b.b2 = b.b0 }
  b.a1 = -2 * c / a0; b.a2 = (1 - al) / a0; return b
}
var hp = mk(true, 100), lp = mk(false, 800), hp2 = mk(true, 100), lp2 = mk(false, 800)
var hf1 = mk(true, 2500), hf2 = mk(true, 2500)
var acch = 0.0, pk = 0.0, pkh = 0.0
var acc = 0.0, accb = 0.0, cnt = 0
var blockStart: Double = 0
var lastEnd: Double = -1
input.installTap(onBus: 0, bufferSize: 1024, format: fmt) { buf, when in
  let n = Int(buf.frameLength)
  guard let ch = buf.floatChannelData?[0] else { return }
  let t0 = hostToEpoch(when.hostTime)
  if lastEnd > 0 && abs(t0 - lastEnd) > 5 { FileHandle.standardError.write("gap \(t0 - lastEnd) ms\n".data(using: .utf8)!) }
  lastEnd = t0 + Double(n) / sr * 1000
  for i in 0..<n {
    if cnt == 0 { blockStart = t0 + Double(i) / sr * 1000 }
    let x = Double(ch[i])
    let y = lp2.run(lp.run(hp2.run(hp.run(x))))
    let h = hf2.run(hf1.run(x))
    acc += x * x; accb += y * y; acch += h * h; cnt += 1
    pk = max(pk, abs(x)); pkh = max(pkh, abs(h))
    if cnt == blk {
      let r = 10 * log10(acc / Double(blk) + 1e-12), b = 10 * log10(accb / Double(blk) + 1e-12)
      let hr = 10 * log10(acch / Double(blk) + 1e-12)
      print(String(format: "%.1f %.1f %.1f %.1f %.1f %.1f", blockStart, r, b, hr,
                   20 * log10(pk + 1e-9), 20 * log10(pkh + 1e-9)))
      acc = 0; accb = 0; acch = 0; pk = 0; pkh = 0; cnt = 0
    }
  }
}
try engine.start()
signal(SIGTERM) { _ in exit(0) }
RunLoop.main.run()
