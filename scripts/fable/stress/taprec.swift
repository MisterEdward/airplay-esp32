// Records the speaker's USB capture input ("Bedroom Speakers Audio")
// losslessly: OUT.pcm = raw s16le stereo 48 kHz, OUT.idx = one line per
// buffer "first_frame_index epoch_ms" (hardware host time).
// usage: taprec OUT_PREFIX [device-name-substring]
import AVFoundation
import AudioToolbox
import CoreAudio
import Foundation

setvbuf(stdout, nil, _IOLBF, 0)
let args = CommandLine.arguments
let prefix = args[1]
let want = args.count > 2 ? args[2] : "Bedroom Speakers"

func deviceID(named sub: String) -> AudioDeviceID? {
  var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices,
    mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
  var size: UInt32 = 0
  AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size)
  var ids = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
  AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &ids)
  for id in ids {
    var n = AudioObjectPropertyAddress(mSelector: kAudioObjectPropertyName,
      mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var name: CFString = "" as CFString
    var s = UInt32(MemoryLayout<CFString>.size)
    AudioObjectGetPropertyData(id, &n, 0, nil, &s, &name)
    var ia = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreams,
      mScope: kAudioObjectPropertyScopeInput, mElement: kAudioObjectPropertyElementMain)
    var isz: UInt32 = 0
    AudioObjectGetPropertyDataSize(id, &ia, 0, nil, &isz)
    if (name as String).contains(sub) && isz > 0 { return id }
  }
  return nil
}

guard let dev = deviceID(named: want) else { print("device not found"); exit(1) }
let engine = AVAudioEngine()
var d = dev
let au = engine.inputNode.audioUnit!
let st = AudioUnitSetProperty(au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &d, UInt32(MemoryLayout<AudioDeviceID>.size))
if st != noErr { print("set device failed \(st)"); exit(1) }
let fmt = engine.inputNode.outputFormat(forBus: 0)
print("device \(dev) format \(fmt)")
var tb = mach_timebase_info_data_t(); mach_timebase_info(&tb)
let hostNow = mach_absolute_time(); let epochNow = Date().timeIntervalSince1970 * 1000
func ep(_ h: UInt64) -> Double { epochNow + (Double(h) - Double(hostNow)) * Double(tb.numer) / Double(tb.denom) / 1e6 }
FileManager.default.createFile(atPath: prefix + ".pcm", contents: nil)
FileManager.default.createFile(atPath: prefix + ".idx", contents: nil)
let pcm = FileHandle(forWritingAtPath: prefix + ".pcm")!
let idx = FileHandle(forWritingAtPath: prefix + ".idx")!
var frameIndex: Int64 = 0
engine.inputNode.installTap(onBus: 0, bufferSize: 2048, format: fmt) { buf, when in
  let n = Int(buf.frameLength)
  guard let ch = buf.floatChannelData else { return }
  let nc = Int(fmt.channelCount)
  var out = [Int16](repeating: 0, count: n * 2)
  for i in 0..<n {
    for c in 0..<2 {
      let v = ch[min(c, nc - 1)][i]
      out[i * 2 + c] = Int16(max(-32768, min(32767, (v * 32768).rounded())))
    }
  }
  out.withUnsafeBufferPointer { pcm.write(Data(buffer: $0)) }
  idx.write("\(frameIndex) \(String(format: "%.2f", ep(when.hostTime)))\n".data(using: .utf8)!)
  frameIndex += Int64(n)
}
try engine.start()
signal(SIGTERM) { _ in exit(0) }
signal(SIGINT) { _ in exit(0) }
RunLoop.main.run()
