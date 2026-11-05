import AppKit
import MirrorProtocol

/// The menu-bar icon: connection status, curvature and dimming sliders, hand gestures,
/// audio, re-center, reset, travel mode (USB only) and quit. Main thread only.
final class StatusMenu: NSObject, NSMenuDelegate {
    /// Slider drags are sent at most this often (the final value always goes out).
    static let sendInterval: TimeInterval = 1.0 / 30
    /// Settings are written to disk this long after the last change.
    static let saveDelay: TimeInterval = 0.5
    /// Travel mode is re-read this often while the menu is open.
    static let travelModeRefreshInterval: TimeInterval = 3

    /// Where the travel-mode state comes from.
    enum TravelModeSource {
        case adb(HeadsetAdb)
        /// Fixed state: `.notOnUSB` (no serial, e.g. Wi-Fi only) or `.adbMissing`.
        case unavailable(TravelModeState)
    }

    private let store: DisplaySettingsStore
    private var settings: DisplaySettings
    private let onSettingsChange: (DisplaySettings) -> Void
    private let onCommand: (ControlCommand) -> Void
    private let onQuit: () -> Void
    private let travelModeSource: TravelModeSource

    private var statusItem: NSStatusItem?
    private let statusLine = NSMenuItem(title: StatusMenu.statusText(forClientAddress: nil), action: nil, keyEquivalent: "")
    /// "Battery: 72 % — charging" under the status line; hidden while no headset is connected.
    private let batteryLine = NSMenuItem(title: StatusMenu.batteryText(nil), action: nil, keyEquivalent: "")
    private var curvatureRow: SliderRow?
    private var dimRow: SliderRow?
    private var handGesturesItem: NSMenuItem?
    private var audioItem: NSMenuItem?
    private var muteMacItem: NSMenuItem?
    /// False with --no-audio: the audio items are shown disabled.
    private let audioAvailable: Bool
    private var throttle = ValueThrottle<DisplaySettings>(interval: StatusMenu.sendInterval)
    private var pendingSave: DispatchWorkItem?
    private let travelModeLine = NSMenuItem(title: TravelModeState.notOnUSB.menuTitle, action: nil, keyEquivalent: "")
    private var quickSettingsItem: NSMenuItem?
    private var travelModeState: TravelModeState?
    private var travelModeQueryRunning = false
    private var travelModeTimer: DispatchSourceTimer?

    /// `onSettingsChange` gets every (throttled) settings change; `onCommand` gets
    /// RECENTER / RESET; `onQuit` must shut the sender down.
    init(store: DisplaySettingsStore, settings: DisplaySettings, travelModeSource: TravelModeSource,
         audioAvailable: Bool,
         onSettingsChange: @escaping (DisplaySettings) -> Void,
         onCommand: @escaping (ControlCommand) -> Void,
         onQuit: @escaping () -> Void) {
        self.store = store
        self.settings = settings
        self.travelModeSource = travelModeSource
        self.audioAvailable = audioAvailable
        self.onSettingsChange = onSettingsChange
        self.onCommand = onCommand
        self.onQuit = onQuit
    }

    func install() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let image = NSImage(systemSymbolName: "visionpro", accessibilityDescription: "VRScreen")
            ?? NSImage(systemSymbolName: "eyeglasses", accessibilityDescription: "VRScreen") {
            image.isTemplate = true
            item.button?.image = image
        } else {
            item.button?.title = "QM"
        }
        item.button?.toolTip = "VRScreen"

        let menu = NSMenu()
        menu.autoenablesItems = false
        statusLine.isEnabled = false
        menu.addItem(statusLine)
        batteryLine.isEnabled = false
        batteryLine.isHidden = true
        menu.addItem(batteryLine)
        menu.addItem(.separator())
        let curvature = SliderRow(title: "Curved screen", percent: Double(settings.curvature) / 10,
                                  target: self, action: #selector(sliderMoved(_:)))
        let dim = SliderRow(title: "Dim surroundings", percent: Double(settings.dim) / 10,
                            target: self, action: #selector(sliderMoved(_:)))
        for row in [curvature, dim] {
            let menuItem = NSMenuItem()
            menuItem.view = row
            menu.addItem(menuItem)
        }
        curvatureRow = curvature
        dimRow = dim
        menu.addItem(.separator())
        let hands = actionItem("Hand gestures", #selector(toggleHandGestures))
        hands.state = settings.handGestures ? .on : .off
        hands.toolTip = "Pinch and hold to move the screen, use two hands to resize it"
        menu.addItem(hands)
        handGesturesItem = hands
        let hint = NSMenuItem(title: "", action: nil, keyEquivalent: "")
        hint.attributedTitle = NSAttributedString(
            string: "Pinch & hold to move, two hands to resize",
            attributes: [.font: NSFont.menuFont(ofSize: NSFont.smallSystemFontSize),
                         .foregroundColor: NSColor.secondaryLabelColor])
        hint.isEnabled = false
        hint.indentationLevel = 1
        menu.addItem(hint)
        menu.addItem(.separator())
        let audio = actionItem("Audio in headset", #selector(toggleAudio))
        audio.toolTip = audioAvailable
            ? "Play the Mac's sound in the headset"
            : "Off: the sender was started with --no-audio"
        menu.addItem(audio)
        audioItem = audio
        let muteMac = actionItem("Mute Mac while mirroring", #selector(toggleMuteMac))
        muteMac.toolTip = "Silence the Mac's speakers while its sound goes to the headset; restored afterwards"
        menu.addItem(muteMac)
        muteMacItem = muteMac
        showAudioState()
        menu.addItem(.separator())
        menu.addItem(actionItem("Re-center", #selector(recenter)))
        menu.addItem(actionItem("Reset placement", #selector(resetPlacement)))
        menu.addItem(.separator())
        travelModeLine.isEnabled = false
        menu.addItem(travelModeLine)
        let quickSettings = actionItem("Open Quick Settings in headset…", #selector(openQuickSettings))
        quickSettings.isEnabled = false
        menu.addItem(quickSettings)
        quickSettingsItem = quickSettings
        menu.addItem(.separator())
        menu.addItem(actionItem("Quit VRScreen", #selector(quit)))
        menu.delegate = self
        item.menu = menu
        statusItem = item
        log("menu: status item installed (curvature \(settings.curvature / 10) %, dim \(settings.dim / 10) %, "
            + "audio \(audioAvailable ? (settings.audio ? "on" : "off") : "off (--no-audio)"), "
            + "mute Mac \(settings.muteMac ? "on" : "off"), settings in \(store.url.path))")
        refreshTravelMode()
    }

    /// Updates the status line (also while the menu is open). A new or no connection
    /// forgets the battery state: "Battery: —" until the headset reports it, hidden while
    /// no headset is connected.
    func setClient(address: String?) {
        statusLine.title = Self.statusText(forClientAddress: address)
        batteryLine.title = Self.batteryText(nil)
        batteryLine.isHidden = address == nil
    }

    /// Shows the battery state reported by the connected headset.
    func setBattery(_ status: HeadsetStatus) {
        batteryLine.title = Self.batteryText(status)
    }

    /// "Headset: USB (127.0.0.1)" for the adb tunnel, "Headset: Wi-Fi <ip>" otherwise.
    static func statusText(forClientAddress address: String?) -> String {
        guard let address else { return "No headset connected" }
        let host = address.lastIndex(of: ":").map { String(address[..<$0]) } ?? address
        return host == "127.0.0.1" ? "Headset: USB (127.0.0.1)" : "Headset: Wi-Fi \(host)"
    }

    /// "Battery: 72 %", "Battery: 72 % — charging", "Battery: 72 % — plugged in" (powered but
    /// not charging, e.g. full), "Battery: —" (unknown or not reported yet).
    static func batteryText(_ status: HeadsetStatus?) -> String {
        guard let status, let percent = status.battery else { return "Battery: —" }
        if status.isCharging { return "Battery: \(percent) % — charging" }
        if status.isPluggedIn { return "Battery: \(percent) % — plugged in" }
        return "Battery: \(percent) %"
    }

    /// Log form: "headset battery 72 % charging", "headset battery unknown".
    static func batteryLogText(_ status: HeadsetStatus) -> String {
        var text = "headset battery " + (status.battery.map { "\($0) %" } ?? "unknown")
        if status.isCharging { text += " charging" } else if status.isPluggedIn { text += " plugged in" }
        return text
    }

    /// Writes a pending settings change now (before quitting).
    func flushSave() {
        guard let pendingSave else { return }
        pendingSave.cancel()
        self.pendingSave = nil
        save()
    }

    // MARK: Actions

    private func actionItem(_ title: String, _ action: Selector) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: action, keyEquivalent: "")
        item.target = self
        return item
    }

    @objc private func sliderMoved(_ sender: NSSlider) {
        guard let curvatureRow, let dimRow else { return }
        settings.curvature = DisplaySettings.permille(fromPercent: curvatureRow.slider.doubleValue)
        settings.dim = DisplaySettings.permille(fromPercent: dimRow.slider.doubleValue)
        curvatureRow.showValue()
        dimRow.showValue()
        let now = ProcessInfo.processInfo.systemUptime
        switch throttle.submit(settings, now: now) {
        case .send(let value):
            onSettingsChange(value)
        case .schedule(let at):
            // The main queue keeps running while the menu tracks the mouse.
            DispatchQueue.main.asyncAfter(deadline: .now() + max(0, at - now)) { [weak self] in
                // Send the current settings (a checkbox may have changed since the drag).
                guard let self, self.throttle.fire(now: ProcessInfo.processInfo.systemUptime) != nil else { return }
                self.onSettingsChange(self.settings)
            }
        case .pending:
            break
        }
        scheduleSave()
    }

    @objc private func toggleHandGestures() {
        settings.handGestures.toggle()
        handGesturesItem?.state = settings.handGestures ? .on : .off
        onSettingsChange(settings) // a single click: no throttling
        scheduleSave()
    }

    @objc private func toggleAudio() {
        settings.audio.toggle()
        applyAudioChange()
    }

    @objc private func toggleMuteMac() {
        settings.muteMac.toggle()
        applyAudioChange()
    }

    private func applyAudioChange() {
        showAudioState()
        onSettingsChange(settings) // a single click: no throttling
        scheduleSave()
    }

    private func showAudioState() {
        audioItem?.state = audioAvailable && settings.audio ? .on : .off
        audioItem?.isEnabled = audioAvailable
        muteMacItem?.state = settings.muteMac ? .on : .off
        muteMacItem?.isEnabled = audioAvailable && settings.audio
    }

    @objc private func recenter() { onCommand(.recenter) }
    @objc private func resetPlacement() { onCommand(.reset) }

    @objc private func quit() {
        flushSave()
        onQuit()
    }

    // MARK: Travel mode (USB only)

    func menuWillOpen(_ menu: NSMenu) {
        refreshTravelMode()
        let timer = DispatchSource.makeTimerSource(queue: .main) // GCD main runs during menu tracking
        timer.schedule(deadline: .now() + Self.travelModeRefreshInterval, repeating: Self.travelModeRefreshInterval)
        timer.setEventHandler { [weak self] in self?.refreshTravelMode() }
        timer.resume()
        travelModeTimer = timer
    }

    func menuDidClose(_ menu: NSMenu) {
        travelModeTimer?.cancel()
        travelModeTimer = nil
    }

    /// Reads the state in the background (adb, ≤ 3 s) and updates the menu when it's back.
    private func refreshTravelMode() {
        switch travelModeSource {
        case .unavailable(let state):
            apply(state)
        case .adb(let adb):
            guard !travelModeQueryRunning else { return }
            travelModeQueryRunning = true
            DispatchQueue.global().async { [weak self] in
                adb.readTravelMode { state in
                    DispatchQueue.main.async {
                        self?.travelModeQueryRunning = false
                        self?.apply(state)
                    }
                }
            }
        }
    }

    private func apply(_ state: TravelModeState) {
        travelModeLine.title = state.menuTitle
        quickSettingsItem?.isEnabled = state.headsetReachable
        if state != travelModeState { log("menu: \(state.menuTitle)") }
        travelModeState = state
    }

    @objc private func openQuickSettings() {
        guard case .adb(let adb) = travelModeSource else { return }
        DispatchQueue.global().async {
            adb.openQuickSettings { result in
                switch result {
                case .success: log("menu: opened Quick Settings in the headset (tap Travel mode there)")
                case .failure(let error): log("menu: could not open Quick Settings: \(error)")
                }
            }
        }
        // The user flips the tile in the headset; pick the new state up shortly after.
        for delay in [3.0, 6.0, 10.0] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in self?.refreshTravelMode() }
        }
    }

    private func scheduleSave() {
        pendingSave?.cancel()
        let work = DispatchWorkItem { [weak self] in
            self?.pendingSave = nil
            self?.save()
        }
        pendingSave = work
        DispatchQueue.main.asyncAfter(deadline: .now() + Self.saveDelay, execute: work)
    }

    private func save() {
        do {
            try store.save(settings)
            log("menu: curvature \(settings.curvature / 10) %, dim \(settings.dim / 10) %, "
                + "hand gestures \(settings.handGestures ? "on" : "off"), audio \(settings.audio ? "on" : "off"), "
                + "mute Mac \(settings.muteMac ? "on" : "off") (saved)")
        } catch {
            log("menu: could not save settings to \(store.url.path): \(error)")
        }
    }
}

/// A menu row: title, current percentage, and a continuous 0–100 % slider.
private final class SliderRow: NSView {
    let slider: NSSlider
    private let valueLabel = NSTextField(labelWithString: "")

    init(title: String, percent: Double, target: AnyObject, action: Selector) {
        slider = NSSlider()
        // Menus are never key windows, so the stock cell leaves the track unfilled except on the
        // slider being dragged; this cell always draws the filled part.
        slider.cell = FilledSliderCell()
        slider.minValue = 0
        slider.maxValue = 100
        slider.doubleValue = percent
        slider.target = target
        slider.action = action
        super.init(frame: NSRect(x: 0, y: 0, width: 260, height: 46))
        slider.isContinuous = true
        let titleLabel = NSTextField(labelWithString: title)
        for label in [titleLabel, valueLabel] {
            label.font = NSFont.menuFont(ofSize: 0)
            label.translatesAutoresizingMaskIntoConstraints = false
            addSubview(label)
        }
        valueLabel.textColor = .secondaryLabelColor
        valueLabel.alignment = .right
        slider.translatesAutoresizingMaskIntoConstraints = false
        addSubview(slider)
        NSLayoutConstraint.activate([
            titleLabel.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 14),
            titleLabel.topAnchor.constraint(equalTo: topAnchor, constant: 4),
            valueLabel.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -14),
            valueLabel.firstBaselineAnchor.constraint(equalTo: titleLabel.firstBaselineAnchor),
            slider.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 14),
            slider.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -14),
            slider.topAnchor.constraint(equalTo: titleLabel.bottomAnchor, constant: 2),
        ])
        showValue()
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    func showValue() {
        valueLabel.stringValue = "\(Int(slider.doubleValue.rounded())) %"
    }
}

/// Slider track with the part left of the knob always drawn in the accent colour.
private final class FilledSliderCell: NSSliderCell {
    override func drawBar(inside rect: NSRect, flipped: Bool) {
        let height: CGFloat = 4
        let bar = NSRect(x: rect.minX, y: rect.midY - height / 2, width: rect.width, height: height)
        NSColor.tertiaryLabelColor.setFill()
        NSBezierPath(roundedRect: bar, xRadius: height / 2, yRadius: height / 2).fill()
        var filled = bar
        filled.size.width = max(0, knobRect(flipped: flipped).midX - bar.minX)
        NSColor.controlAccentColor.setFill()
        NSBezierPath(roundedRect: filled, xRadius: height / 2, yRadius: height / 2).fill()
    }
}
