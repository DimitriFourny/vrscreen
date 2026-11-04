import Carbon.HIToolbox
import Foundation
import MirrorProtocol
import Testing
@testable import quest_mirror_sender

private let base = UInt32(controlKey | optionKey | cmdKey)
private let shifted = base | UInt32(shiftKey)

@Test func shortcutTableMapsPhysicalKeysToCommands() {
    let expected: [(Int, UInt32, ControlCommand, Bool)] = [
        (kVK_LeftArrow, base, .move(dx: -1, dy: 0), true),
        (kVK_RightArrow, base, .move(dx: 1, dy: 0), true),
        (kVK_UpArrow, base, .move(dx: 0, dy: 1), true),
        (kVK_DownArrow, base, .move(dx: 0, dy: -1), true),
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
    let right = UInt32(HotkeyBinding.all.firstIndex { $0.command == .move(dx: 1, dy: 0) }!)
    let recenter = UInt32(HotkeyBinding.all.firstIndex { $0.command == .recenter }!)

    #expect(controller.handle(hotkeyEvent(kEventHotKeyPressed, index: right)) == noErr)
    #expect(controller.handle(hotkeyEvent(kEventHotKeyPressed, index: right)) == noErr) // held: system repeat
    #expect(controller.handle(hotkeyEvent(kEventHotKeyReleased, index: right)) == noErr)
    #expect(controller.handle(hotkeyEvent(kEventHotKeyPressed, index: recenter)) == noErr)
    #expect(controller.handle(hotkeyEvent(kEventHotKeyPressed, index: recenter)) == noErr)
    #expect(controller.handle(hotkeyEvent(kEventHotKeyReleased, index: recenter)) == noErr)
    #expect(fired.map(\.0) == [.move(dx: 1, dy: 0), .recenter])
    #expect(fired.allSatisfy { !$0.1 })

    // Events that are not ours are passed on.
    var foreign = EventHotKeyID(signature: 0x41424344, id: 0)
    let event = hotkeyEvent(kEventHotKeyPressed, index: 0)
    SetEventParameter(event, EventParamName(kEventParamDirectObject), EventParamType(typeEventHotKeyID),
                      MemoryLayout<EventHotKeyID>.size, &foreign)
    #expect(controller.handle(event) == OSStatus(eventNotHandledErr))
}

@Test func debugControlSpecParsing() throws {
    #expect(try SenderOptions.parseControlSpec("move:1,0; move:0,-1;distance:2;size:-1;recenter;reset")
        == [.move(dx: 1, dy: 0), .move(dx: 0, dy: -1), .distance(steps: 2), .size(steps: -1), .recenter, .reset])
    for bad in ["move:1", "size:200", "spin", "recenter:1", "distance:x"] {
        #expect(throws: OptionError.self) { try SenderOptions.parseControlSpec(bad) }
    }
}
