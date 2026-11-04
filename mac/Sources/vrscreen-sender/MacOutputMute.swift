import CoreAudio
import Foundation

/// The output device this process muted, 0 if none. Read by the exit/crash safety net,
/// which must unmute it on every exit path.
nonisolated(unsafe) private var mutedDeviceID: AudioObjectID = 0

private func muteAddress() -> AudioObjectPropertyAddress {
    AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyMute, mScope: kAudioDevicePropertyScopeOutput,
                               mElement: kAudioObjectPropertyElementMain)
}

/// Unmutes the device this process muted, if any. Safe to call from a signal handler
/// (one CoreAudio property write, no allocation).
private func restoreMutedDevice() {
    let device = mutedDeviceID
    guard device != 0 else { return }
    mutedDeviceID = 0
    var address = muteAddress()
    var off: UInt32 = 0
    AudioObjectSetPropertyData(device, &address, 0, nil, UInt32(MemoryLayout<UInt32>.size), &off)
}

/// "Mute Mac while mirroring": mutes the Mac's default output device while audio goes to
/// the headset, and always restores it. ScreenCaptureKit captures system audio before the
/// output device's volume and mute (measured), so the headset keeps the sound.
///
/// Only a device that was not muted is muted, and only that device is unmuted again: if
/// the user had muted it already, it stays as it was. If the default output changes while
/// engaged, the old device is restored and the new one muted. Restored on `set(false)`
/// and, through `ExitSafetyNet`, on exit and crash signals. Call on one serial queue.
final class MacOutputMute {
    private let queue: DispatchQueue
    private var engaged = false
    private var listenerInstalled = false
    private var safetyNetRegistered = false

    init(queue: DispatchQueue) {
        self.queue = queue
    }

    /// Engages (mutes now) or releases (restores now).
    func set(_ wanted: Bool) {
        guard wanted != engaged else { return }
        engaged = wanted
        if wanted {
            installListener()
            muteDefaultDevice()
        } else {
            restore(reason: "restored")
        }
    }

    private func restore(reason: String) {
        let device = mutedDeviceID
        guard device != 0 else { return }
        restoreMutedDevice()
        log("audio: unmuted Mac output \"\(Self.name(of: device))\" (\(reason))")
    }

    private func muteDefaultDevice() {
        if !safetyNetRegistered {
            safetyNetRegistered = true
            ExitSafetyNet.register(restoreMutedDevice)
        }
        guard let device = Self.defaultOutputDevice() else {
            return log("audio: no default output device to mute")
        }
        let name = Self.name(of: device)
        var address = muteAddress()
        var settable: DarwinBoolean = false
        guard AudioObjectHasProperty(device, &address),
              AudioObjectIsPropertySettable(device, &address, &settable) == noErr, settable.boolValue else {
            return log("audio: Mac output \"\(name)\" cannot be muted; it keeps playing")
        }
        var current: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        guard AudioObjectGetPropertyData(device, &address, 0, nil, &size, &current) == noErr else {
            return log("audio: cannot read the mute state of \"\(name)\"; not muting it")
        }
        if current != 0 {
            return log("audio: Mac output \"\(name)\" is already muted; leaving it as it is")
        }
        var on: UInt32 = 1
        let status = AudioObjectSetPropertyData(device, &address, 0, nil, size, &on)
        guard status == noErr else {
            return log("audio: muting \"\(name)\" failed (OSStatus \(status))")
        }
        mutedDeviceID = device
        log("audio: muted Mac output \"\(name)\" while mirroring (restored when audio stops or the sender exits)")
    }

    /// Follows changes of the default output device while engaged.
    private func installListener() {
        guard !listenerInstalled else { return }
        listenerInstalled = true
        var address = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDefaultOutputDevice,
                                                 mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &address, queue) { [weak self] _, _ in
            guard let self, self.engaged, Self.defaultOutputDevice() != mutedDeviceID else { return }
            self.restore(reason: "default output changed")
            self.muteDefaultDevice()
        }
    }

    private static func defaultOutputDevice() -> AudioObjectID? {
        var address = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDefaultOutputDevice,
                                                 mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        var device = AudioObjectID(0)
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        let status = AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &device)
        return status == noErr && device != 0 ? device : nil
    }

    private static func name(of device: AudioObjectID) -> String {
        var address = AudioObjectPropertyAddress(mSelector: kAudioObjectPropertyName,
                                                 mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        var name: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(device, &address, 0, nil, &size, &name) == noErr,
              let value = name?.takeRetainedValue() else { return "device \(device)" }
        return value as String
    }
}
