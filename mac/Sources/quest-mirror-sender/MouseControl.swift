import AppKit
import ApplicationServices
import CoreGraphics
import MirrorProtocol

/// Which mouse gesture the held modifiers select. Only ⇧⌃⌥⌘ count; caps lock, fn and
/// the numeric-pad flag are ignored.
enum MouseGestureMode: Equatable {
    /// Mouse and scroll go to apps as usual.
    case inactive
    /// Exactly ⌃⌥⌘: mouse movement orbits, scroll changes the distance.
    case orbitAndDistance
    /// ⌃⌥⌘⇧: scroll changes the size (mouse movement does nothing).
    case size

    static let gestureModifiers: NSEvent.ModifierFlags = [.control, .option, .command]

    static func from(_ flags: NSEvent.ModifierFlags) -> MouseGestureMode {
        let relevant = flags.intersection([.shift, .control, .option, .command])
        if relevant == gestureModifiers { return .orbitAndDistance }
        if relevant == gestureModifiers.union(.shift) { return .size }
        return .inactive
    }
}

/// Turns mouse and scroll deltas into ORBIT / DISTANCE_FINE / SIZE_FINE commands.
/// Deltas accumulate between flushes (one per tick, ≤ 60 Hz); fractions below one
/// wire unit carry over to the next flush, so slow movements are not lost.
struct MouseGestureAccumulator {
    /// A wheel notch (one line) is a quarter DISTANCE/SIZE step.
    static let unitsPerWheelLine = 25.0
    /// A 200-point trackpad swipe is one DISTANCE/SIZE step.
    static let unitsPerTrackpadPoint = 0.5

    let degreesPerPoint: Double
    let scrollSensitivity: Double
    private var yaw = 0.0 // 1/100 degree
    private var pitch = 0.0
    private var distance = 0.0 // DISTANCE_FINE units
    private var size = 0.0

    init(degreesPerPoint: Double, scrollSensitivity: Double) {
        self.degreesPerPoint = degreesPerPoint
        self.scrollSensitivity = scrollSensitivity
    }

    /// Mouse movement in points as reported by the event (`dy` > 0 = down).
    mutating func addMouseDelta(dx: Double, dy: Double, mode: MouseGestureMode) {
        guard mode == .orbitAndDistance else { return }
        yaw += dx * degreesPerPoint * 100
        pitch -= dy * degreesPerPoint * 100
    }

    /// One scroll event. `deltaY` is `scrollingDeltaY` (lines for wheels, points for
    /// precise devices); `invertedFromDevice` is the "natural scrolling" flag, so the
    /// physical gesture "up / away from you" always zooms in: closer or bigger.
    mutating func addScroll(deltaY: Double, precise: Bool, invertedFromDevice: Bool, mode: MouseGestureMode) {
        let away = invertedFromDevice ? -deltaY : deltaY
        let units = away * (precise ? Self.unitsPerTrackpadPoint : Self.unitsPerWheelLine) * scrollSensitivity
        switch mode {
        case .orbitAndDistance: distance -= units
        case .size: size += units
        case .inactive: break
        }
    }

    /// Commands for everything accumulated since the last flush (whole units only).
    mutating func flush() -> [ControlCommand] {
        var commands: [ControlCommand] = []
        let yawUnits = Self.take(&yaw)
        let pitchUnits = Self.take(&pitch)
        if yawUnits != 0 || pitchUnits != 0 {
            commands.append(.orbit(yawCentidegrees: yawUnits, pitchCentidegrees: pitchUnits))
        }
        let distanceUnits = Self.take(&distance)
        if distanceUnits != 0 { commands.append(.distanceFine(distanceUnits)) }
        let sizeUnits = Self.take(&size)
        if sizeUnits != 0 { commands.append(.sizeFine(sizeUnits)) }
        return commands
    }

    /// Drops sub-unit leftovers (when the modifiers are released).
    mutating func discardRemainders() {
        yaw = 0
        pitch = 0
        distance = 0
        size = 0
    }

    /// Whole units of `value` (toward zero, clamped to Int16); the rest stays in `value`.
    private static func take(_ value: inout Double) -> Int16 {
        let whole = min(max(value.rounded(.towardZero), Double(Int16.min)), Double(Int16.max))
        value -= whole
        return Int16(whole)
    }
}

// MARK: - Cursor freeze

/// Non-zero while this process holds the cursor disassociated from the mouse. Read by
/// the atexit and fatal-signal handlers, which must restore it on every exit path.
private var cursorFreezeActive: sig_atomic_t = 0

private func restoreCursorAssociation() {
    if cursorFreezeActive != 0 {
        CGAssociateMouseAndMouseCursorPosition(1)
        cursorFreezeActive = 0
    }
}

private func fatalSignalHandler(_ signalNumber: Int32) {
    restoreCursorAssociation()
    signal(signalNumber, SIG_DFL)
    raise(signalNumber)
}

/// Freezes the Mac cursor while orbiting (the mouse keeps producing deltas), using
/// `CGAssociateMouseAndMouseCursorPosition`. `freeze()` on the main thread only;
/// `release()` from any thread (shutdown calls it from the signal queue).
enum CursorFreeze {
    private static var safetyNetsInstalled = false
    private static var unavailable = false

    static func freeze() {
        guard cursorFreezeActive == 0, !unavailable else { return }
        installSafetyNets()
        let error = CGAssociateMouseAndMouseCursorPosition(0)
        if error == .success {
            cursorFreezeActive = 1
        } else {
            unavailable = true
            log("mouse: cannot freeze the cursor (CGError \(error.rawValue)); it keeps moving while orbiting")
        }
    }

    static func release() {
        restoreCursorAssociation()
    }

    /// atexit covers exit() (Ctrl-C, SIGTERM, SIGHUP shut down through exit); the
    /// handlers cover crashes. SIGKILL cannot be caught.
    private static func installSafetyNets() {
        guard !safetyNetsInstalled else { return }
        safetyNetsInstalled = true
        atexit { restoreCursorAssociation() }
        for signalNumber in [SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE, SIGTRAP] {
            signal(signalNumber, fatalSignalHandler)
        }
    }
}

// MARK: - Controller

/// ⌃⌥⌘ + mouse move → ORBIT, ⌃⌥⌘ + scroll → DISTANCE_FINE, ⌃⌥⌘⇧ + scroll → SIZE_FINE.
///
/// Without Accessibility it observes with a global NSEvent monitor (no permission
/// needed for mouse events; events still reach apps) and freezes the cursor while
/// ⌃⌥⌘ is held. If the process is already trusted for Accessibility (never prompted)
/// it uses an active event tap that swallows those events instead. Modifiers are
/// polled from the session state, which needs no permission either. Main thread only.
final class MouseControl {
    static let tickInterval: DispatchTimeInterval = .nanoseconds(1_000_000_000 / 60)

    private var accumulator: MouseGestureAccumulator
    private let onCommands: ([ControlCommand]) -> Void
    private var mode = MouseGestureMode.inactive
    private var monitor: Any?
    private var tap: CFMachPort?
    private var tapSource: CFRunLoopSource?
    private var timer: DispatchSourceTimer?
    private var stats = Stats()
    private var lastStatsLog = Date()

    private struct Stats {
        var orbitMessages = 0, yaw = 0, pitch = 0
        var distanceMessages = 0, distance = 0
        var sizeMessages = 0, size = 0
    }

    /// `onCommands` runs on the main thread with each coalesced batch.
    init(degreesPerPoint: Double, scrollSensitivity: Double, onCommands: @escaping ([ControlCommand]) -> Void) {
        accumulator = MouseGestureAccumulator(degreesPerPoint: degreesPerPoint, scrollSensitivity: scrollSensitivity)
        self.onCommands = onCommands
    }

    func start() {
        if AXIsProcessTrusted(), startEventTap() {
            log("mouse: event tap, events swallowed while ⌃⌥⌘ is held (Accessibility already granted); cursor frozen while orbiting")
        } else {
            monitor = NSEvent.addGlobalMonitorForEvents(matching: [.mouseMoved, .scrollWheel]) { [weak self] event in
                _ = self?.handle(event)
            }
            if monitor == nil {
                log("mouse: could not install a global event monitor; ⌃⌥⌘ + mouse/scroll disabled")
                return
            }
            log("mouse: observe-only (scroll passes through to apps); cursor frozen while orbiting")
        }
        let timer = DispatchSource.makeTimerSource(queue: .main)
        timer.schedule(deadline: .now(), repeating: Self.tickInterval, leeway: .milliseconds(2))
        timer.setEventHandler { [weak self] in self?.tick() }
        timer.resume()
        self.timer = timer
    }

    func stop() {
        timer?.cancel()
        timer = nil
        if let monitor { NSEvent.removeMonitor(monitor) }
        monitor = nil
        if let tap { CGEvent.tapEnable(tap: tap, enable: false) }
        if let tapSource { CFRunLoopRemoveSource(CFRunLoopGetMain(), tapSource, .commonModes) }
        tap = nil
        tapSource = nil
        CursorFreeze.release()
    }

    /// Feeds one event; returns true if it belongs to an active gesture (the event tap
    /// then swallows it).
    private func handle(_ event: NSEvent) -> Bool {
        let eventMode = MouseGestureMode.from(event.modifierFlags)
        guard eventMode != .inactive else { return false }
        switch event.type {
        case .mouseMoved:
            accumulator.addMouseDelta(dx: Double(event.deltaX), dy: Double(event.deltaY), mode: eventMode)
            return eventMode == .orbitAndDistance
        case .scrollWheel:
            // Momentum (inertia) after the fingers lift is ignored: zoom stops with the gesture.
            if event.momentumPhase.isEmpty {
                accumulator.addScroll(deltaY: Double(event.scrollingDeltaY), precise: event.hasPreciseScrollingDeltas,
                                      invertedFromDevice: event.isDirectionInvertedFromDevice, mode: eventMode)
            }
            return true
        default:
            return false
        }
    }

    private func tick() {
        let flags = NSEvent.ModifierFlags(rawValue: UInt(CGEventSource.flagsState(.combinedSessionState).rawValue))
        let newMode = MouseGestureMode.from(flags)
        if newMode == .orbitAndDistance { CursorFreeze.freeze() } else { CursorFreeze.release() }
        send(accumulator.flush())
        if newMode == .inactive && mode != .inactive { accumulator.discardRemainders() }
        mode = newMode
        logStatsIfDue()
    }

    private func send(_ commands: [ControlCommand]) {
        guard !commands.isEmpty else { return }
        for command in commands {
            switch command {
            case .orbit(let yaw, let pitch):
                stats.orbitMessages += 1; stats.yaw += Int(yaw); stats.pitch += Int(pitch)
            case .distanceFine(let v):
                stats.distanceMessages += 1; stats.distance += Int(v)
            case .sizeFine(let v):
                stats.sizeMessages += 1; stats.size += Int(v)
            default:
                break
            }
        }
        onCommands(commands)
    }

    /// One line per second of activity instead of one per 60 Hz message.
    private func logStatsIfDue() {
        guard Date().timeIntervalSince(lastStatsLog) >= 1 else { return }
        lastStatsLog = Date()
        var parts: [String] = []
        if stats.orbitMessages > 0 {
            parts.append(String(format: "ORBIT ×%d Σ yaw %+.2f° pitch %+.2f°", stats.orbitMessages,
                                Double(stats.yaw) / 100, Double(stats.pitch) / 100))
        }
        if stats.distanceMessages > 0 { parts.append("DISTANCE_FINE ×\(stats.distanceMessages) Σ \(stats.distance)") }
        if stats.sizeMessages > 0 { parts.append("SIZE_FINE ×\(stats.sizeMessages) Σ \(stats.size)") }
        if !parts.isEmpty { log("mouse: " + parts.joined(separator: " | ")) }
        stats = Stats()
    }

    // MARK: Event tap (only when Accessibility is already granted)

    private func startEventTap() -> Bool {
        let mask = (CGEventMask(1) << CGEventType.mouseMoved.rawValue) | (CGEventMask(1) << CGEventType.scrollWheel.rawValue)
        guard let tap = CGEvent.tapCreate(
            tap: .cgSessionEventTap, place: .headInsertEventTap, options: .defaultTap, eventsOfInterest: mask,
            callback: { _, type, event, userInfo in
                guard let userInfo else { return Unmanaged.passUnretained(event) }
                let control = Unmanaged<MouseControl>.fromOpaque(userInfo).takeUnretainedValue()
                return control.tapCallback(type: type, event: event)
            },
            userInfo: Unmanaged.passUnretained(self).toOpaque())
        else { return false }
        let source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0)
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
        CGEvent.tapEnable(tap: tap, enable: true)
        self.tap = tap
        tapSource = source
        return true
    }

    private func tapCallback(type: CGEventType, event: CGEvent) -> Unmanaged<CGEvent>? {
        if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
            if let tap { CGEvent.tapEnable(tap: tap, enable: true) }
            return Unmanaged.passUnretained(event)
        }
        guard let nsEvent = NSEvent(cgEvent: event), handle(nsEvent) else { return Unmanaged.passUnretained(event) }
        return nil // swallowed: the gesture is ours
    }
}
