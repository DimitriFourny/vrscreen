import AppKit
import MirrorProtocol
import Testing
@testable import vrscreen_sender

@Test func gestureModeRequiresExactModifiers() {
    #expect(MouseGestureMode.from([.control, .option, .command]) == .orbitAndDistance)
    #expect(MouseGestureMode.from([.control, .option, .command, .shift]) == .size)
    // Caps lock, fn and numeric-pad flags do not matter.
    #expect(MouseGestureMode.from([.control, .option, .command, .capsLock, .function, .numericPad]) == .orbitAndDistance)
    for flags: NSEvent.ModifierFlags in [[], [.control, .option], [.command], [.control, .command, .shift], [.option, .command]] {
        #expect(MouseGestureMode.from(flags) == .inactive)
    }
}

@Test func mouseDeltasBecomeOrbitWithCarriedFractions() {
    var a = MouseGestureAccumulator(degreesPerPoint: 0.1, scrollSensitivity: 1)
    a.addMouseDelta(dx: 3, dy: -2, mode: .orbitAndDistance) // right 3 pt, up 2 pt (dy < 0 = up)
    #expect(a.flush() == [.orbit(yawCentidegrees: 30, pitchCentidegrees: 20)])
    #expect(a.flush() == [])
    // Slow movement: 0.04 pt per event is 0.4 centidegree; it adds up instead of vanishing.
    for _ in 0..<5 { a.addMouseDelta(dx: 0.04, dy: 0, mode: .orbitAndDistance) }
    #expect(a.flush() == [.orbit(yawCentidegrees: 2, pitchCentidegrees: 0)])
    // Mouse movement is ignored outside exact ⌃⌥⌘.
    a.addMouseDelta(dx: 100, dy: 100, mode: .size)
    a.addMouseDelta(dx: 100, dy: 100, mode: .inactive)
    #expect(a.flush() == [])
    // Sensitivity scales linearly; huge bursts clamp to Int16 and carry the rest.
    var fast = MouseGestureAccumulator(degreesPerPoint: 1, scrollSensitivity: 1)
    fast.addMouseDelta(dx: 400, dy: 0, mode: .orbitAndDistance) // 400° = 40000 centidegrees
    #expect(fast.flush() == [.orbit(yawCentidegrees: .max, pitchCentidegrees: 0)])
    #expect(fast.flush() == [.orbit(yawCentidegrees: Int16(40000 - 32767), pitchCentidegrees: 0)])
}

@Test func scrollFollowsThePhysicalGesture() {
    var a = MouseGestureAccumulator(degreesPerPoint: 0.1, scrollSensitivity: 1)
    // Wheel notch away from the user, traditional scrolling: deltaY +1 line = a quarter step closer.
    a.addScroll(deltaY: 1, precise: false, invertedFromDevice: false, mode: .orbitAndDistance)
    #expect(a.flush() == [.distanceFine(-25)])
    // Same physical notch with natural scrolling: the system reports -1, inverted = true.
    a.addScroll(deltaY: -1, precise: false, invertedFromDevice: true, mode: .orbitAndDistance)
    #expect(a.flush() == [.distanceFine(-25)])
    // Trackpad, two fingers pulled toward the user by 200 points (natural scrolling): one step farther.
    a.addScroll(deltaY: 200, precise: true, invertedFromDevice: true, mode: .orbitAndDistance)
    #expect(a.flush() == [.distanceFine(100)])
    // With ⇧ the same "away" gesture makes the screen bigger.
    a.addScroll(deltaY: 2, precise: false, invertedFromDevice: false, mode: .size)
    #expect(a.flush() == [.sizeFine(50)])
    a.addScroll(deltaY: 5, precise: false, invertedFromDevice: false, mode: .inactive)
    #expect(a.flush() == [])
}

@Test func scrollSensitivityAndCoalescing() {
    var a = MouseGestureAccumulator(degreesPerPoint: 0.1, scrollSensitivity: 2)
    // Many small trackpad events between two ticks become one message.
    for _ in 0..<10 { a.addScroll(deltaY: -3, precise: true, invertedFromDevice: false, mode: .orbitAndDistance) }
    a.addMouseDelta(dx: -1, dy: 1, mode: .orbitAndDistance)
    #expect(a.flush() == [.orbit(yawCentidegrees: -10, pitchCentidegrees: -10), .distanceFine(30)])
    // Sub-unit leftovers are dropped when the gesture ends.
    a.addScroll(deltaY: 0.3, precise: true, invertedFromDevice: false, mode: .size) // 0.3 units
    #expect(a.flush() == [])
    a.discardRemainders()
    a.addScroll(deltaY: 0.8, precise: true, invertedFromDevice: false, mode: .size)
    #expect(a.flush() == [])
}
