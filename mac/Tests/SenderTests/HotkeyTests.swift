import Carbon.HIToolbox
import Foundation
import MirrorProtocol
import Testing
@testable import quest_mirror_sender

private let base = UInt32(controlKey | optionKey | cmdKey)
private let shifted = base | UInt32(shiftKey)

@Test func shortcutTableMapsPhysicalKeysToCommands() {
    let expected: [(Int, UInt32, ControlCommand, Bool)] = [
        (kVK_LeftArrow, base, .translate(dx: -1, dy: 0), true),
        (kVK_RightArrow, base, .translate(dx: 1, dy: 0), true),
        (kVK_UpArrow, base, .translate(dx: 0, dy: 1), true),
        (kVK_DownArrow, base, .translate(dx: 0, dy: -1), true),
        (kVK_UpArrow, shifted, .distance(steps: 1), true),
        (kVK_DownArrow, shifted, .distance(steps: -1), true),
        (kVK_RightArrow, shifted, .size(steps: 1), true),
        (kVK_LeftArrow, shifted, .size(steps: -1), true),
        (kVK_ANSI_R, base, .recenter, false),
        (kVK_ANSI_0, base, .reset, false),
    ]
    let actual = HotkeyBinding.all.map { (Int($0.keyCode), $0.modifiers, $0.command, $0.repeats) }
    #expect(actual.count == expected.count)
    for (a, e) in zip(actual, expected) {
        #expect(a.0 == e.0 && a.1 == e.1 && a.2 == e.2 && a.3 == e.3)
    }
    // No two shortcuts share a key + modifier combination.
    #expect(Set(HotkeyBinding.all.map { "\($0.keyCode)/\($0.modifiers)" }).count == HotkeyBinding.all.count)
}

private func hotkeyEvent(_ kind: Int, index: UInt32) -> EventRef {
    var event: EventRef?
    CreateEvent(nil, OSType(kEventClassKeyboard), UInt32(kind), 0, EventAttributes(kEventAttributeNone), &event)
    var id = EventHotKeyID(signature: HotkeyController.signature, id: index)
    SetEventParameter(event, EventParamName(kEventParamDirectObject), EventParamType(typeEventHotKeyID),
                      MemoryLayout<EventHotKeyID>.size, &id)
    return event!
}

@Test func pressFiresOnceAndSystemRepeatsAreIgnored() {
    var fired: [(ControlCommand, Bool)] = []
    let controller = HotkeyController { binding, isRepeat in fired.append((binding.command, isRepeat)) }
    let right = UInt32(HotkeyBinding.all.firstIndex { $0.command == .translate(dx: 1, dy: 0) }!)
    let recenter = UInt32(HotkeyBinding.all.firstIndex { $0.command == .recenter }!)

    #expect(controller.handle(hotkeyEvent(kEventHotKeyPressed, index: right)) == noErr)
    #expect(controller.handle(hotkeyEvent(kEventHotKeyPressed, index: right)) == noErr) // held: system repeat
    #expect(controller.handle(hotkeyEvent(kEventHotKeyReleased, index: right)) == noErr)
    #expect(controller.handle(hotkeyEvent(kEventHotKeyPressed, index: recenter)) == noErr)
    #expect(controller.handle(hotkeyEvent(kEventHotKeyPressed, index: recenter)) == noErr)
    #expect(controller.handle(hotkeyEvent(kEventHotKeyReleased, index: recenter)) == noErr)
    #expect(fired.map(\.0) == [.translate(dx: 1, dy: 0), .recenter])
    #expect(fired.allSatisfy { !$0.1 })

    // Events that are not ours are passed on.
    var foreign = EventHotKeyID(signature: 0x41424344, id: 0)
    let event = hotkeyEvent(kEventHotKeyPressed, index: 0)
    SetEventParameter(event, EventParamName(kEventParamDirectObject), EventParamType(typeEventHotKeyID),
                      MemoryLayout<EventHotKeyID>.size, &foreign)
    #expect(controller.handle(event) == OSStatus(eventNotHandledErr))
}

@Test func debugControlSpecParsing() throws {
    let commands: [ControlCommand] = [
        .translate(dx: 1, dy: 0), .orbit(yawCentidegrees: 500, pitchCentidegrees: -250), .distance(steps: 2),
        .distanceFine(-25), .size(steps: -1), .sizeFine(30), .recenter, .reset, .setCurvature(1000), .setDim(0),
    ]
    #expect(try SenderOptions.parseControlSpec(
        "translate:1,0; orbit:500,-250;distance:2;distance_fine:-25;size:-1;size_fine:30;recenter;reset;curvature:1000;dim:0")
        == commands.map { .send($0) })
    #expect(try SenderOptions.parseControlSpec("orbit:50,0*2;wait:1500;dim:500")
        == [.send(.orbit(yawCentidegrees: 50, pitchCentidegrees: 0)), .send(.orbit(yawCentidegrees: 50, pitchCentidegrees: 0)),
            .wait(milliseconds: 1500), .send(.setDim(500))])
    for bad in ["move:1,0", "translate:1", "size:200", "orbit:40000,0", "spin", "recenter:1", "distance:x",
                "reset*0", "reset*x", "curvature:1001", "dim:-1", "wait", "wait:-5"] {
        #expect(throws: OptionError.self) { try SenderOptions.parseControlSpec(bad) }
    }
}
