import Carbon.HIToolbox
import Foundation
import MirrorProtocol

/// A global shortcut and the CONTROL command it sends.
struct HotkeyBinding: Equatable {
    /// Physical key (`kVK_*`), independent of the keyboard layout.
    let keyCode: UInt32
    /// Carbon modifier mask (`controlKey | optionKey | cmdKey [| shiftKey]`).
    let modifiers: UInt32
    let label: String
    let command: ControlCommand
    /// Held keys repeat the command (after `HotkeyController.repeatDelay`).
    let repeats: Bool

    static let base = UInt32(controlKey | optionKey | cmdKey)
    static let shifted = base | UInt32(shiftKey)

    /// ⌃⌥⌘ + arrows slide the screen; adding ⇧ turns ↑/↓ into distance and →/← into
    /// size. Arrows, R and 0 sit in the same place on QWERTY and AZERTY, unlike
    /// `[ ] = -`, which AZERTY puts on other keys or behind dead keys. (⌃⌥⌘ + mouse
    /// orbits and ⌃⌥⌘ + scroll zooms: see MouseControl.)
    static let all: [HotkeyBinding] = [
        HotkeyBinding(keyCode: UInt32(kVK_LeftArrow), modifiers: base, label: "⌃⌥⌘←", command: .translate(dx: -1, dy: 0), repeats: true),
        HotkeyBinding(keyCode: UInt32(kVK_RightArrow), modifiers: base, label: "⌃⌥⌘→", command: .translate(dx: 1, dy: 0), repeats: true),
        HotkeyBinding(keyCode: UInt32(kVK_UpArrow), modifiers: base, label: "⌃⌥⌘↑", command: .translate(dx: 0, dy: 1), repeats: true),
        HotkeyBinding(keyCode: UInt32(kVK_DownArrow), modifiers: base, label: "⌃⌥⌘↓", command: .translate(dx: 0, dy: -1), repeats: true),
        HotkeyBinding(keyCode: UInt32(kVK_UpArrow), modifiers: shifted, label: "⌃⌥⌘⇧↑", command: .distance(steps: 1), repeats: true),
        HotkeyBinding(keyCode: UInt32(kVK_DownArrow), modifiers: shifted, label: "⌃⌥⌘⇧↓", command: .distance(steps: -1), repeats: true),
        HotkeyBinding(keyCode: UInt32(kVK_RightArrow), modifiers: shifted, label: "⌃⌥⌘⇧→", command: .size(steps: 1), repeats: true),
        HotkeyBinding(keyCode: UInt32(kVK_LeftArrow), modifiers: shifted, label: "⌃⌥⌘⇧←", command: .size(steps: -1), repeats: true),
        HotkeyBinding(keyCode: UInt32(kVK_ANSI_R), modifiers: base, label: "⌃⌥⌘R", command: .recenter, repeats: false),
        HotkeyBinding(keyCode: UInt32(kVK_ANSI_0), modifiers: base, label: "⌃⌥⌘0", command: .reset, repeats: false),
    ]
}

/// Registers `HotkeyBinding.all` with Carbon `RegisterEventHotKey` (global, needs no
/// Accessibility permission) and calls `onCommand` on the main thread for each press
/// and each auto-repeat. Requires a running main event loop (NSApplication).
final class HotkeyController {
    static let repeatDelay: DispatchTimeInterval = .milliseconds(300)
    static let repeatInterval: DispatchTimeInterval = .milliseconds(80)
    /// Hotkey IDs carry this signature ('VRSC') and the binding index.
    static let signature: OSType = 0x5652_5343

    private let bindings: [HotkeyBinding]
    private let onCommand: (HotkeyBinding, _ isRepeat: Bool) -> Void
    private var refs: [EventHotKeyRef] = []
    private var handler: EventHandlerRef?
    private var repeatTimer: DispatchSourceTimer?
    private var heldIndex: Int?

    init(bindings: [HotkeyBinding] = HotkeyBinding.all,
         onCommand: @escaping (HotkeyBinding, _ isRepeat: Bool) -> Void) {
        self.bindings = bindings
        self.onCommand = onCommand
    }

    /// Installs the handler and registers every binding; logs what failed.
    func register() {
        var specs = [
            EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed)),
            EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyReleased)),
        ]
        let status = InstallEventHandler(GetApplicationEventTarget(), { _, event, userData in
            guard let event, let userData else { return OSStatus(eventNotHandledErr) }
            let controller = Unmanaged<HotkeyController>.fromOpaque(userData).takeUnretainedValue()
            return controller.handle(event)
        }, specs.count, &specs, Unmanaged.passUnretained(self).toOpaque(), &handler)
        guard status == noErr else {
            log("hotkeys: InstallEventHandler failed (OSStatus \(status)); shortcuts disabled")
            return
        }
        var registered: [String] = []
        var failed: [String] = []
        for (index, binding) in bindings.enumerated() {
            var ref: EventHotKeyRef?
            let id = EventHotKeyID(signature: Self.signature, id: UInt32(index))
            // Exclusive: fails if another app already owns the combination (instead of
            // silently sharing it), and keeps a second sender from grabbing it too.
            let s = RegisterEventHotKey(binding.keyCode, binding.modifiers, id, GetApplicationEventTarget(),
                                        OptionBits(kEventHotKeyExclusive), &ref)
            if s == noErr, let ref {
                refs.append(ref)
                registered.append("\(binding.label) \(binding.command)")
            } else {
                failed.append("\(binding.label) (\(s == OSStatus(eventHotKeyExistsErr) ? "taken by another app" : "OSStatus \(s)"))")
            }
        }
        log("hotkeys: registered \(refs.count)/\(bindings.count)"
            + (registered.isEmpty ? "" : ": \(registered.joined(separator: ", "))"))
        if !failed.isEmpty {
            log("hotkeys: could not register \(failed.joined(separator: ", "))")
        }
    }

    func unregister() {
        stopRepeat()
        refs.forEach { UnregisterEventHotKey($0) }
        refs.removeAll()
        if let handler { RemoveEventHandler(handler) }
        handler = nil
    }

    /// Handles a hotkey pressed/released event (internal for tests).
    func handle(_ event: EventRef) -> OSStatus {
        var id = EventHotKeyID()
        let status = GetEventParameter(event, EventParamName(kEventParamDirectObject), EventParamType(typeEventHotKeyID),
                                       nil, MemoryLayout<EventHotKeyID>.size, nil, &id)
        guard status == noErr, id.signature == Self.signature, Int(id.id) < bindings.count else {
            return OSStatus(eventNotHandledErr)
        }
        let index = Int(id.id)
        switch Int(GetEventKind(event)) {
        case kEventHotKeyPressed:
            guard heldIndex != index else { break } // system key repeat; our timer drives repeats
            stopRepeat()
            heldIndex = index
            onCommand(bindings[index], false)
            if bindings[index].repeats { startRepeat(index) }
        case kEventHotKeyReleased:
            if heldIndex == index { stopRepeat() }
        default:
            return OSStatus(eventNotHandledErr)
        }
        return noErr
    }

    private func startRepeat(_ index: Int) {
        let timer = DispatchSource.makeTimerSource(queue: .main)
        timer.schedule(deadline: .now() + Self.repeatDelay, repeating: Self.repeatInterval)
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            self.onCommand(self.bindings[index], true)
        }
        timer.resume()
        repeatTimer = timer
    }

    private func stopRepeat() {
        repeatTimer?.cancel()
        repeatTimer = nil
        heldIndex = nil
    }
}
