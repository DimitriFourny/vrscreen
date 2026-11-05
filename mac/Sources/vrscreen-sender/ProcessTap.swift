import AudioToolbox
import CoreAudio
import Foundation
import MirrorProtocol

/// What the sender needs from a system-audio tap (lets `main` hold one without an
/// availability check). Any thread.
protocol SystemAudioTap: AnyObject {
    func setRunning(_ on: Bool)
    func setMuted(_ on: Bool)
    func invalidate()
}

/// System audio through a Core Audio process tap (macOS 14.2+): a private, global stereo
/// tap of every process except this one, read through a private aggregate device that
/// contains only the tap (no output device: with one, the tap's input is delayed by that
/// device's latency, 160 ms with AirPods). Buffers of `bufferFrames` reach the pipeline
/// ~0.1 ms after their last sample, versus 15-50 ms for ScreenCaptureKit audio.
///
/// Needs the "System Audio Recording" permission of the app that started the sender (the
/// terminal); macOS asks once, on the first start. "Mute Mac while mirroring" uses the
/// tap's own mute behaviour (`mutedWhenTapped`): nothing to restore, macOS undoes it when
/// the tap goes away, also on a crash.
@available(macOS 14.2, *)
final class ProcessTapCapture: SystemAudioTap {
    /// 5 ms at 48 kHz: two buffers per 10 ms chunk. Any size is accepted (measured 15-960).
    static let bufferFrames: UInt32 = 240

    enum Failure: Error, CustomStringConvertible {
        case permissionDenied
        case coreAudio(String, OSStatus)
        case unsupportedFormat(String)

        var description: String {
            switch self {
            case .permissionDenied: "System Audio Recording permission denied for the app running the sender"
            case .coreAudio(let what, let status): "\(what) failed (OSStatus \(status))"
            case .unsupportedFormat(let format): "tap format \(format) (needs 48 kHz float32)"
            }
        }
    }

    private let pipeline: AudioPipeline
    private let tapDescription: CATapDescription
    private var tapID = AudioObjectID(kAudioObjectUnknown)
    private var aggregateID = AudioObjectID(kAudioObjectUnknown)
    private var ioProcID: AudioDeviceIOProcID?
    private var format = AudioStreamBasicDescription()
    private var running = false
    private var muted = false
    /// Serializes start/stop/mute (a first start can block on the permission prompt).
    private let control = DispatchQueue(label: "vrscreen.tap.control")

    /// Creates the tap and its aggregate device; capture starts with `setRunning(true)`.
    init(pipeline: AudioPipeline) throws(Failure) {
        self.pipeline = pipeline
        if SystemAudioPermission.preflight() == .denied { throw .permissionDenied }
        let ownProcess = Self.ownProcessObject()
        tapDescription = CATapDescription(stereoGlobalTapButExcludeProcesses: ownProcess.map { [$0] } ?? [])
        tapDescription.name = "VRScreen system audio"
        tapDescription.isPrivate = true
        tapDescription.muteBehavior = .unmuted
        do {
            try create()
        } catch {
            destroy()
            throw error
        }
    }

    private func create() throws(Failure) {
        var status = AudioHardwareCreateProcessTap(tapDescription, &tapID)
        guard status == noErr else { throw .coreAudio("AudioHardwareCreateProcessTap", status) }
        status = Self.get(tapID, kAudioTapPropertyFormat, &format)
        guard status == noErr else { throw .coreAudio("reading the tap format", status) }
        guard format.mFormatID == kAudioFormatLinearPCM, format.mSampleRate == Double(AudioPipeline.config.sampleRate),
              format.mFormatFlags & kAudioFormatFlagIsFloat != 0, format.mBitsPerChannel == 32 else {
            throw .unsupportedFormat("\(format.mSampleRate) Hz, \(format.mBitsPerChannel) bit, flags 0x\(String(format.mFormatFlags, radix: 16))")
        }
        let description: [String: Any] = [
            kAudioAggregateDeviceNameKey: "VRScreen system audio",
            kAudioAggregateDeviceUIDKey: "vrscreen-tap-" + UUID().uuidString,
            kAudioAggregateDeviceIsPrivateKey: true,
            kAudioAggregateDeviceIsStackedKey: false,
            kAudioAggregateDeviceTapAutoStartKey: true,
            kAudioAggregateDeviceTapListKey: [[kAudioSubTapUIDKey: tapDescription.uuid.uuidString,
                                               kAudioSubTapDriftCompensationKey: true]],
        ]
        status = AudioHardwareCreateAggregateDevice(description as CFDictionary, &aggregateID)
        guard status == noErr else { throw .coreAudio("AudioHardwareCreateAggregateDevice", status) }
        var frames = Self.bufferFrames
        var address = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyBufferFrameSize,
                                                 mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        status = AudioObjectSetPropertyData(aggregateID, &address, 0, nil, UInt32(MemoryLayout<UInt32>.size), &frames)
        if status != noErr { log("audio: process tap keeps its default buffer size (OSStatus \(status))") }
        let format = format
        let pipeline = pipeline
        status = AudioDeviceCreateIOProcIDWithBlock(&ioProcID, aggregateID, pipeline.queue) { _, input, inputTime, _, _ in
            let list = UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: input))
            guard let first = list.first, format.mBytesPerFrame > 0,
                  inputTime.pointee.mFlags.contains(.hostTimeValid) else { return }
            let frames = Int(first.mDataByteSize / format.mBytesPerFrame)
            let micros = AudioConvertHostTimeToNanos(inputTime.pointee.mHostTime) / 1000
            pipeline.handle(list, format: format, frameCount: frames, captureMicros: micros, source: .tap)
        }
        guard status == noErr else { throw .coreAudio("AudioDeviceCreateIOProcIDWithBlock", status) }
    }

    /// Starts or stops reading the tap (audio on/off). The first start can show the
    /// permission prompt and blocks until it is answered, so this runs on `control`.
    func setRunning(_ on: Bool) {
        control.async { [self] in
            guard on != running, let ioProcID else { return }
            let status = on ? AudioDeviceStart(aggregateID, ioProcID) : AudioDeviceStop(aggregateID, ioProcID)
            if status == noErr {
                running = on
                log("audio: process tap \(on ? "started (\(Self.bufferFrames)-frame buffers)" : "stopped")")
            } else {
                log("audio: \(on ? "starting" : "stopping") the process tap failed (OSStatus \(status))")
            }
        }
    }

    /// "Mute Mac while mirroring": the tapped audio is muted on the Mac's outputs while the
    /// tap is read (`mutedWhenTapped`), and plays normally again when this is turned off,
    /// capture stops, or the process exits.
    func setMuted(_ on: Bool) {
        control.async { [self] in
            guard on != muted, tapID != kAudioObjectUnknown else { return }
            tapDescription.muteBehavior = on ? .mutedWhenTapped : .unmuted
            var address = AudioObjectPropertyAddress(mSelector: kAudioTapPropertyDescription,
                                                     mScope: kAudioObjectPropertyScopeGlobal,
                                                     mElement: kAudioObjectPropertyElementMain)
            var value = tapDescription
            let status = withUnsafeMutablePointer(to: &value) {
                AudioObjectSetPropertyData(tapID, &address, 0, nil, UInt32(MemoryLayout<CATapDescription>.size), $0)
            }
            if status == noErr {
                muted = on
                log("audio: Mac output \(on ? "muted while mirroring (process tap)" : "unmuted")")
            } else {
                log("audio: \(on ? "muting" : "unmuting") through the process tap failed (OSStatus \(status))")
            }
        }
    }

    /// Stops capture and destroys the aggregate device and the tap (waits for pending calls).
    func invalidate() {
        control.sync { destroy() }
    }

    private func destroy() {
        if let ioProcID, aggregateID != kAudioObjectUnknown {
            if running { AudioDeviceStop(aggregateID, ioProcID) }
            AudioDeviceDestroyIOProcID(aggregateID, ioProcID)
        }
        ioProcID = nil
        running = false
        if aggregateID != kAudioObjectUnknown { AudioHardwareDestroyAggregateDevice(aggregateID) }
        if tapID != kAudioObjectUnknown { AudioHardwareDestroyProcessTap(tapID) }
        aggregateID = AudioObjectID(kAudioObjectUnknown)
        tapID = AudioObjectID(kAudioObjectUnknown)
    }

    /// This process's Core Audio object, if it has one, so the tap leaves it out.
    private static func ownProcessObject() -> AudioObjectID? {
        var pid = getpid()
        var object = AudioObjectID(kAudioObjectUnknown)
        var address = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyTranslatePIDToProcessObject,
                                                 mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        let status = AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address,
                                                UInt32(MemoryLayout<pid_t>.size), &pid, &size, &object)
        return status == noErr && object != kAudioObjectUnknown ? object : nil
    }

    private static func get<T>(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector, _ value: inout T) -> OSStatus {
        var address = AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        var size = UInt32(MemoryLayout<T>.size)
        return withUnsafeMutablePointer(to: &value) { AudioObjectGetPropertyData(object, &address, 0, nil, &size, $0) }
    }
}

/// Read-only check of the "System Audio Recording" (kTCCServiceAudioCapture) permission
/// through TCC's preflight call: it never prompts and never changes anything. Unknown if
/// the call is unavailable.
enum SystemAudioPermission {
    case granted, denied, unknown

    static func preflight() -> SystemAudioPermission {
        typealias Preflight = @convention(c) (CFString, CFDictionary?) -> Int
        guard let handle = dlopen("/System/Library/PrivateFrameworks/TCC.framework/Versions/A/TCC", RTLD_NOW),
              let symbol = dlsym(handle, "TCCAccessPreflight") else { return .unknown }
        switch unsafeBitCast(symbol, to: Preflight.self)("kTCCServiceAudioCapture" as CFString, nil) {
        case 0: return .granted
        case 1: return .denied
        default: return .unknown
        }
    }
}

/// The default output device's presentation latency (device + safety offset + stream
/// latency): how long after capture the Mac plays what a tap or ScreenCaptureKit captured.
/// Calls `onChange` on `queue` now and whenever the default output device changes.
final class OutputLatency {
    struct Value: Equatable {
        var micros: UInt64
        var deviceName: String
    }

    private let queue: DispatchQueue
    private let onChange: (Value) -> Void
    private var current: Value?

    init(queue: DispatchQueue, onChange: @escaping (Value) -> Void) {
        self.queue = queue
        self.onChange = onChange
        var address = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDefaultOutputDevice,
                                                 mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &address, queue) { [weak self] _, _ in
            self?.update()
        }
        queue.async { self.update() }
    }

    /// Frames of latency at `sampleRate` → µs.
    static func micros(latencyFrames: UInt32, safetyOffsetFrames: UInt32, streamLatencyFrames: UInt32,
                       sampleRate: Double) -> UInt64 {
        guard sampleRate > 0 else { return 0 }
        let frames = Double(latencyFrames) + Double(safetyOffsetFrames) + Double(streamLatencyFrames)
        return UInt64((frames * 1e6 / sampleRate).rounded())
    }

    private func update() {
        guard let value = Self.read(), value != current else { return }
        current = value
        onChange(value)
    }

    private static func read() -> Value? {
        var device = AudioObjectID(kAudioObjectUnknown)
        guard get(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyDefaultOutputDevice,
                  kAudioObjectPropertyScopeGlobal, &device) == noErr, device != kAudioObjectUnknown else { return nil }
        var latency: UInt32 = 0, safety: UInt32 = 0, rate: Float64 = 0
        _ = get(device, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, &latency)
        _ = get(device, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, &safety)
        _ = get(device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &rate)
        var streamLatency: UInt32 = 0
        var address = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreams, mScope: kAudioObjectPropertyScopeOutput,
                                                 mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        if AudioObjectGetPropertyDataSize(device, &address, 0, nil, &size) == noErr, size >= 4 {
            var streams = [AudioStreamID](repeating: 0, count: Int(size) / 4)
            if AudioObjectGetPropertyData(device, &address, 0, nil, &size, &streams) == noErr {
                _ = get(streams[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, &streamLatency)
            }
        }
        var name: Unmanaged<CFString>?
        _ = get(device, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, &name)
        return Value(micros: micros(latencyFrames: latency, safetyOffsetFrames: safety, streamLatencyFrames: streamLatency,
                                    sampleRate: rate),
                     deviceName: name?.takeRetainedValue() as String? ?? "device \(device)")
    }

    private static func get<T>(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector,
                               _ scope: AudioObjectPropertyScope, _ value: inout T) -> OSStatus {
        var address = AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: kAudioObjectPropertyElementMain)
        var size = UInt32(MemoryLayout<T>.size)
        return withUnsafeMutablePointer(to: &value) { AudioObjectGetPropertyData(object, &address, 0, nil, &size, $0) }
    }
}
