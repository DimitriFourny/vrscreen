import AppKit
import MirrorProtocol
import ServiceManagement

/// The menu-bar icon and its menu: problems (with fixes), status, the screen sliders, the
/// settings (Video ▸, Controls ▸), the headset on USB (Headset ▸), placement, launch at login,
/// logs, about and quit. Every setting goes through the `SettingsModel`. Main thread only.
final class StatusMenu: NSObject, NSMenuDelegate {
    /// Slider drags are sent at most this often (the final value always goes out).
    static let sendInterval: TimeInterval = 1.0 / 30
    /// Travel mode is re-read this often while the menu is open.
    static let travelModeRefreshInterval: TimeInterval = 3

    struct Callbacks {
        /// RECENTER / RESET.
        var onCommand: (ControlCommand) -> Void
        /// A display was picked in Video ▸ Display (it also replaces --display).
        var onDisplayChosen: (UInt32) -> Void
        /// A fix chosen from the problem list.
        var onProblemAction: (Problem.Action) -> Void
        /// The menu is about to open: re-run the cheap checks.
        var onMenuWillOpen: () -> Void
        var onShowLogs: () -> Void
        /// Must shut the sender down.
        var onQuit: () -> Void
    }

    private let model: SettingsModel
    private let usb: UsbHeadsetLink?
    /// False with --no-audio: the audio items are shown disabled.
    private let audioAvailable: Bool
    private let callbacks: Callbacks

    private var statusItem: NSStatusItem?
    private let menu = NSMenu()
    /// Problem rows at the top of the menu (rebuilt by `setProblems`).
    private var problemItems: [NSMenuItem] = []
    private var problems: [Problem] = []
    private let statusLine = NSMenuItem(title: StatusMenu.statusText(forClientAddress: nil), action: nil, keyEquivalent: "")
    /// "Battery: 72 % — charging" under the status line; hidden while no headset is connected.
    private let batteryLine = NSMenuItem(title: StatusMenu.batteryText(nil), action: nil, keyEquivalent: "")
    private let travelModeLine = NSMenuItem(title: TravelModeState.notOnUSB.menuTitle, action: nil, keyEquivalent: "")
    private var curvatureRow: SliderRow?
    private var dimRow: SliderRow?
    private var throttle = ValueThrottle<SenderSettings>(interval: StatusMenu.sendInterval)
    /// Update checkmarks / enabled state from the settings and USB status.
    private var refreshers: [() -> Void] = []
    private let videoMenu = NSMenu()
    private var displayItems: [NSMenuItem] = []
    private var capturedDisplayID: UInt32?
    private var usbStatus: UsbStatus?
    private var loginItem: NSMenuItem?
    private var travelModeState: TravelModeState?
    private var travelModeQueryRunning = false
    private var travelModeTimer: DispatchSourceTimer?

    init(model: SettingsModel, usb: UsbHeadsetLink?, audioAvailable: Bool, callbacks: Callbacks) {
        self.model = model
        self.usb = usb
        self.audioAvailable = audioAvailable
        self.callbacks = callbacks
        usbStatus = usb == nil ? UsbStatus(link: .off) : nil
    }

    func install() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        statusItem = item
        updateIcon()

        menu.autoenablesItems = false
        for line in [statusLine, batteryLine, travelModeLine] {
            line.isEnabled = false
            menu.addItem(line)
        }
        batteryLine.isHidden = true
        menu.addItem(.separator())

        let settings = model.effective
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
        refreshers.append { [weak self] in
            guard let self, let curvatureRow = self.curvatureRow, let dimRow = self.dimRow else { return }
            let s = self.model.effective
            curvatureRow.setPercent(Double(s.curvature) / 10)
            dimRow.setPercent(Double(s.dim) / 10)
        }
        menu.addItem(.separator())

        menu.addItem(toggle("Hand gestures", \.handGestures,
                            toolTip: "Pinch and hold to move the screen, use two hands to resize it"))
        menu.addItem(hint("Pinch & hold to move, two hands to resize"))
        let audio = toggle("Audio in headset", \.audio,
                           toolTip: audioAvailable ? "Play the Mac's sound in the headset" : "Off: started with --no-audio",
                           enabled: { [audioAvailable] _ in audioAvailable })
        menu.addItem(audio)
        menu.addItem(toggle("Mute Mac while mirroring", \.muteMac,
                            toolTip: "Silence the Mac's speakers while its sound goes to the headset; restored afterwards",
                            enabled: { [audioAvailable] s in audioAvailable && s.audio }))
        menu.addItem(.separator())

        menu.addItem(submenu("Video", videoMenu))
        buildVideoMenu()
        let controls = NSMenu()
        menu.addItem(submenu("Controls", controls))
        buildControlsMenu(controls)
        let headset = NSMenu()
        menu.addItem(submenu("Headset", headset))
        buildHeadsetMenu(headset)
        menu.addItem(.separator())

        menu.addItem(button("Re-center") { [weak self] in self?.callbacks.onCommand(.recenter) })
        menu.addItem(button("Reset placement") { [weak self] in self?.callbacks.onCommand(.reset) })
        menu.addItem(.separator())

        let login = button("Launch at login") { [weak self] in self?.toggleLaunchAtLogin() }
        menu.addItem(login)
        loginItem = login
        menu.addItem(.separator())
        menu.addItem(button("Show Logs…") { [weak self] in self?.callbacks.onShowLogs() })
        menu.addItem(button("About VRScreen") { StatusMenu.showAbout() })
        menu.addItem(button("Quit VRScreen") { [weak self] in
            self?.model.flushSave()
            self?.callbacks.onQuit()
        })

        menu.delegate = self
        item.menu = menu
        refresh()
        log("menu: status item installed (\(model.effective.summary); settings in \(model.store.url.path))")
        refreshTravelMode()
    }

    /// Opens the menu (the app was opened again from Finder).
    func open() {
        statusItem?.button?.performClick(nil)
    }

    // MARK: State from main

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

    func setUsb(_ status: UsbStatus) {
        let wasConnected = usbStatus?.isConnected ?? false
        usbStatus = status
        refresh()
        if status.isConnected != wasConnected { refreshTravelMode() }
    }

    func setCapturedDisplay(_ id: UInt32?) {
        capturedDisplayID = id
    }

    /// Rebuilds the problem rows at the top and switches the icon to its warning variant
    /// while there are any.
    func setProblems(_ new: [Problem]) {
        guard new != problems else { return }
        problems = new
        for item in problemItems { menu.removeItem(item) }
        problemItems = []
        var index = 0
        func insert(_ item: NSMenuItem) {
            menu.insertItem(item, at: index)
            problemItems.append(item)
            index += 1
        }
        for problem in new {
            insert(problemItem(problem))
            for action in problem.actions {
                let item = button(action.title) { [weak self] in self?.callbacks.onProblemAction(action) }
                item.indentationLevel = 1
                insert(item)
            }
        }
        if !new.isEmpty { insert(.separator()) }
        updateIcon()
    }

    // MARK: Text

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

    // MARK: Building blocks

    private func button(_ title: String, _ handler: @escaping () -> Void) -> NSMenuItem {
        let action = MenuAction(handler)
        let item = NSMenuItem(title: title, action: #selector(MenuAction.run), keyEquivalent: "")
        item.target = action
        item.representedObject = action // NSMenuItem.target is weak
        return item
    }

    /// A checkbox bound to a Bool setting.
    private func toggle(_ title: String, _ key: WritableKeyPath<SenderSettings, Bool>, toolTip: String? = nil,
                        enabled: ((SenderSettings) -> Bool)? = nil) -> NSMenuItem {
        let item = button(title) { [weak self] in
            guard let self else { return }
            self.model.set(key, !self.model.effective[keyPath: key])
            self.refresh()
        }
        item.toolTip = toolTip
        refreshers.append { [weak self, weak item] in
            guard let self, let item else { return }
            let s = self.model.effective
            let isEnabled = enabled?(s) ?? true
            item.isEnabled = isEnabled
            item.state = isEnabled && s[keyPath: key] ? .on : .off
        }
        return item
    }

    /// A header and one radio item per preset; a disabled "Custom" row shows a value set on
    /// the command line (or by hand in settings.json) that matches no preset.
    private func choices<T: Equatable>(_ menu: NSMenu, header: String, _ key: WritableKeyPath<SenderSettings, T>,
                                       _ options: [(label: String, value: T)], custom: @escaping (T) -> String) {
        if menu.numberOfItems > 0 { menu.addItem(.separator()) }
        menu.addItem(.sectionHeader(title: header))
        var rows: [(NSMenuItem, T)] = []
        for option in options {
            let item = button(option.label) { [weak self] in
                self?.model.set(key, option.value)
                self?.refresh()
            }
            menu.addItem(item)
            rows.append((item, option.value))
        }
        let customItem = NSMenuItem(title: "", action: nil, keyEquivalent: "")
        customItem.isEnabled = false
        customItem.state = .on
        menu.addItem(customItem)
        refreshers.append { [weak self] in
            guard let self else { return }
            let current = self.model.effective[keyPath: key]
            for (item, value) in rows { item.state = value == current ? .on : .off }
            customItem.isHidden = rows.contains { $0.1 == current }
            customItem.title = custom(current) + (self.model.isOverridden(key) ? " (command line)" : "")
        }
    }

    private func submenu(_ title: String, _ submenu: NSMenu) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        submenu.autoenablesItems = false
        submenu.delegate = self
        item.submenu = submenu
        return item
    }

    private func hint(_ text: String) -> NSMenuItem {
        let item = NSMenuItem(title: "", action: nil, keyEquivalent: "")
        item.attributedTitle = NSAttributedString(
            string: text,
            attributes: [.font: NSFont.menuFont(ofSize: NSFont.smallSystemFontSize),
                         .foregroundColor: NSColor.secondaryLabelColor])
        item.isEnabled = false
        item.indentationLevel = 1
        return item
    }

    /// Bold title, detail below in small secondary text, a warning symbol in front.
    private func problemItem(_ problem: Problem) -> NSMenuItem {
        let actions = problem.actions
        let item = button(problem.title) { [weak self] in
            if actions.count == 1 { self?.callbacks.onProblemAction(actions[0]) }
        }
        let title = NSMutableAttributedString(
            string: problem.title,
            attributes: [.font: NSFont.menuFont(ofSize: 0).bold, .foregroundColor: NSColor.labelColor])
        if let detail = problem.detail {
            title.append(NSAttributedString(
                string: "\n" + Self.wrap(detail, width: 58),
                attributes: [.font: NSFont.menuFont(ofSize: NSFont.smallSystemFontSize),
                             .foregroundColor: NSColor.secondaryLabelColor]))
        }
        item.attributedTitle = title
        let symbol = problem.severity == .error ? "exclamationmark.octagon.fill" : "exclamationmark.triangle.fill"
        let color: NSColor = problem.severity == .error ? .systemRed : .systemOrange
        item.image = NSImage(systemSymbolName: symbol, accessibilityDescription: problem.severity == .error ? "Error" : "Warning")?
            .withSymbolConfiguration(.init(paletteColors: [.white, color]))
        return item
    }

    /// Breaks `text` into lines of at most `width` characters at spaces.
    static func wrap(_ text: String, width: Int) -> String {
        var lines: [String] = []
        var line = ""
        for word in text.split(separator: " ") {
            if !line.isEmpty && line.count + 1 + word.count > width {
                lines.append(line)
                line = ""
            }
            line += (line.isEmpty ? "" : " ") + word
        }
        if !line.isEmpty { lines.append(line) }
        return lines.joined(separator: "\n")
    }

    private func updateIcon() {
        guard let button = statusItem?.button else { return }
        let warning = !problems.isEmpty
        if let image = Self.icon(warning: warning) {
            button.image = image
            button.title = ""
        } else {
            button.image = nil
            button.title = warning ? "VR!" : "VR"
        }
        button.toolTip = warning
            ? "VRScreen — \(problems.count) problem\(problems.count == 1 ? "" : "s"): \(problems[0].title)"
            : "VRScreen"
    }

    /// The Vision Pro symbol; the warning variant has a "!" badge cut into its lower right.
    /// A template image, so it follows the menu bar's appearance.
    static func icon(warning: Bool) -> NSImage? {
        guard let base = NSImage(systemSymbolName: "visionpro", accessibilityDescription: "VRScreen")
            ?? NSImage(systemSymbolName: "eyeglasses", accessibilityDescription: "VRScreen") else { return nil }
        guard warning else {
            base.isTemplate = true
            return base
        }
        let badge: CGFloat = 9
        let size = NSSize(width: base.size.width + 4, height: max(base.size.height, badge + 2))
        let image = NSImage(size: size, flipped: false) { rect in
            base.draw(in: NSRect(x: 0, y: (rect.height - base.size.height) / 2, width: base.size.width, height: base.size.height))
            guard let context = NSGraphicsContext.current else { return false }
            let circle = NSRect(x: rect.maxX - badge, y: 0, width: badge, height: badge)
            // A clear ring around the badge separates it from the symbol.
            context.compositingOperation = .clear
            NSBezierPath(ovalIn: circle.insetBy(dx: -1.5, dy: -1.5)).fill()
            context.compositingOperation = .sourceOver
            NSColor.black.setFill()
            NSBezierPath(ovalIn: circle).fill()
            context.compositingOperation = .clear
            NSBezierPath(roundedRect: NSRect(x: circle.midX - 0.8, y: circle.minY + 3.6, width: 1.6, height: 3.8),
                         xRadius: 0.8, yRadius: 0.8).fill()
            NSBezierPath(ovalIn: NSRect(x: circle.midX - 0.85, y: circle.minY + 1.3, width: 1.7, height: 1.7)).fill()
            context.compositingOperation = .sourceOver
            return true
        }
        image.isTemplate = true
        image.accessibilityDescription = "VRScreen (problems)"
        return image
    }

    // MARK: Submenus

    private func buildVideoMenu() {
        choices(videoMenu, header: "Resolution", \.maxWidth,
                SenderSettings.maxWidthPresets.map { width in
                    (label: "Up to \(width) px wide" + (width == 3840 ? " (4K)" : width == 1920 ? " (1080p)" : ""), value: width)
                },
                custom: { "Up to \($0) px wide" })
        choices(videoMenu, header: "Frame rate", \.fps, SenderSettings.fpsPresets.map { (label: "\($0) fps", value: $0) },
                custom: { "\($0) fps" })
        choices(videoMenu, header: "Codec", \.codec, [(label: "HEVC (H.265)", value: .hevc), (label: "H.264", value: .h264)],
                custom: { $0.rawValue })
        choices(videoMenu, header: "Bitrate", \.bitrateMbps,
                SenderSettings.bitratePresets.map { (label: "\(SenderSettings.format(mbps: $0)) Mbit/s", value: $0) },
                custom: { "\(SenderSettings.format(mbps: $0)) Mbit/s" })
        // The Display section is rebuilt when the menu opens (displays come and go).
    }

    /// The Display choices, only with more than one display.
    private func rebuildDisplayItems() {
        for item in displayItems { videoMenu.removeItem(item) }
        displayItems = []
        let screens = NSScreen.screens.compactMap { screen -> (id: UInt32, name: String)? in
            guard let number = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber else { return nil }
            return (number.uint32Value, screen.localizedName)
        }
        guard screens.count > 1 else { return }
        let mainID = CGMainDisplayID()
        let chosen = capturedDisplayID ?? model.effective.displayID ?? mainID
        displayItems.append(.separator())
        displayItems.append(.sectionHeader(title: "Display"))
        for screen in screens {
            let item = button(screen.name + (screen.id == mainID ? " (main)" : "")) { [weak self] in
                self?.capturedDisplayID = nil
                self?.callbacks.onDisplayChosen(screen.id)
            }
            item.state = screen.id == chosen ? .on : .off
            displayItems.append(item)
        }
        for item in displayItems { videoMenu.addItem(item) }
    }

    private func buildControlsMenu(_ controls: NSMenu) {
        controls.addItem(toggle("Keyboard shortcuts (⌃⌥⌘ + arrows, R, 0)", \.hotkeys,
                                toolTip: "Slide, resize and re-center the screen from any app"))
        controls.addItem(toggle("Mouse orbit & scroll zoom (⌃⌥⌘)", \.mouseGestures,
                                toolTip: "Hold ⌃⌥⌘ and move the mouse to orbit the screen, scroll to bring it closer"))
        choices(controls, header: "Mouse sensitivity", \.mouseSensitivity,
                SenderSettings.mouseSensitivityPresets.map { (label: $0.label, value: $0.value) },
                custom: { String(format: "Custom: %.3g° per point", $0) })
        choices(controls, header: "Scroll sensitivity", \.scrollSensitivity,
                SenderSettings.scrollSensitivityPresets.map { (label: $0.label, value: $0.value) },
                custom: { String(format: "Custom: ×%.3g", $0) })
    }

    private func buildHeadsetMenu(_ headset: NSMenu) {
        let linkLine = NSMenuItem(title: "", action: nil, keyEquivalent: "")
        linkLine.isEnabled = false
        headset.addItem(linkLine)
        let appLine = NSMenuItem(title: "", action: nil, keyEquivalent: "")
        appLine.isEnabled = false
        headset.addItem(appLine)
        let progressLine = hint("")
        headset.addItem(progressLine)
        let adbHint = hint("Install Android platform-tools for USB:")
        headset.addItem(adbHint)
        let copyCommand = button("Copy “\(Adb.installCommand)”") { StatusMenu.copyAdbInstallCommand() }
        copyCommand.indentationLevel = 1
        headset.addItem(copyCommand)
        let download = button("Download Platform-Tools…") { NSWorkspace.shared.open(Adb.downloadPage) }
        download.indentationLevel = 1
        headset.addItem(download)
        headset.addItem(.separator())
        let install = button("Install headset app") { [weak self] in self?.usb?.installApp() }
        headset.addItem(install)
        let launch = button("Launch headset app") { [weak self] in self?.usb?.launchApp() }
        headset.addItem(launch)
        let quickSettings = button("Open Quick Settings (Travel mode)…") { [weak self] in self?.openQuickSettings() }
        quickSettings.toolTip = "Travel mode can only be switched in the headset's Quick Settings"
        headset.addItem(quickSettings)
        let restartAdb = button("Restart adb") { [weak self] in self?.usb?.restartAdb() }
        headset.addItem(restartAdb)
        headset.addItem(.separator())
        headset.addItem(toggle("Open headset app when connected", \.launchHeadsetApp,
                               toolTip: "Start VRScreen in the headset when it is plugged in over USB",
                               enabled: { [weak self] _ in self?.usb != nil }))

        refreshers.append { [weak self] in
            guard let self else { return }
            let status = self.usbStatus ?? UsbStatus(link: .noHeadset)
            let connected = status.isConnected
            linkLine.title = status.linkText
            appLine.title = status.appText ?? ""
            appLine.isHidden = status.appText == nil
            let progress = status.installing ? "Installing the headset app…" : status.installResult
            progressLine.attributedTitle = NSAttributedString(
                string: progress ?? "", attributes: [.font: NSFont.menuFont(ofSize: NSFont.smallSystemFontSize),
                                                     .foregroundColor: NSColor.secondaryLabelColor])
            progressLine.isHidden = progress == nil
            let adbMissing = status.link == .adbMissing
            adbHint.isHidden = !adbMissing
            copyCommand.isHidden = !adbMissing
            download.isHidden = !adbMissing
            if let offer = HeadsetAppAction.offer(installed: status.app, bundled: status.bundledVersion) {
                install.title = offer.menuTitle
                install.isHidden = false
            } else {
                install.isHidden = status.bundledVersion == nil || !connected
                install.title = "Install headset app"
            }
            install.isEnabled = connected && !status.installing && status.bundledVersion != nil
            launch.isEnabled = connected && status.app != .notInstalled && !status.installing
            quickSettings.isEnabled = connected
            if case .adbBroken = status.link { restartAdb.isHidden = false } else { restartAdb.isHidden = !status.serverUnstable }
        }
    }

    static func copyAdbInstallCommand() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(Adb.installCommand, forType: .string)
        log("menu: copied “\(Adb.installCommand)” to the clipboard")
    }

    static func showAbout() {
        NSApp.activate(ignoringOtherApps: true)
        let credits = NSAttributedString(
            string: "Mac screen mirroring for Meta Quest.\ngithub.com/DimitriFourny/vrscreen · MIT License",
            attributes: [.font: NSFont.systemFont(ofSize: NSFont.smallSystemFontSize),
                         .foregroundColor: NSColor.secondaryLabelColor])
        NSApp.orderFrontStandardAboutPanel(options: [
            .applicationName: "VRScreen",
            .applicationVersion: AppInfo.version,
            .version: "",
            .credits: credits,
        ])
    }

    // MARK: Refresh

    private func refresh() {
        for refresher in refreshers { refresher() }
        refreshLoginItem()
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        if menu === videoMenu { rebuildDisplayItems() }
        refresh()
    }

    func menuWillOpen(_ menu: NSMenu) {
        guard menu === self.menu else { return }
        callbacks.onMenuWillOpen()
        refreshTravelMode()
        let timer = DispatchSource.makeTimerSource(queue: .main) // GCD main runs during menu tracking
        timer.schedule(deadline: .now() + Self.travelModeRefreshInterval, repeating: Self.travelModeRefreshInterval)
        timer.setEventHandler { [weak self] in self?.refreshTravelMode() }
        timer.resume()
        travelModeTimer = timer
    }

    func menuDidClose(_ menu: NSMenu) {
        guard menu === self.menu else { return }
        travelModeTimer?.cancel()
        travelModeTimer = nil
    }

    // MARK: Sliders

    @objc private func sliderMoved(_ sender: NSSlider) {
        guard let curvatureRow, let dimRow else { return }
        curvatureRow.showValue()
        dimRow.showValue()
        model.set(\.curvature, SenderSettings.permille(fromPercent: curvatureRow.slider.doubleValue), publish: false)
        model.set(\.dim, SenderSettings.permille(fromPercent: dimRow.slider.doubleValue), publish: false)
        let now = ProcessInfo.processInfo.systemUptime
        switch throttle.submit(model.effective, now: now) {
        case .send:
            model.publish()
        case .schedule(let at):
            // The main queue keeps running while the menu tracks the mouse.
            DispatchQueue.main.asyncAfter(deadline: .now() + max(0, at - now)) { [weak self] in
                guard let self, self.throttle.fire(now: ProcessInfo.processInfo.systemUptime) != nil else { return }
                self.model.publish()
            }
        case .pending:
            break
        }
    }

    // MARK: Launch at login (VRScreen.app only)

    private func refreshLoginItem() {
        guard let loginItem else { return }
        guard AppInfo.isAppBundle else {
            loginItem.isEnabled = false
            loginItem.state = .off
            loginItem.toolTip = "Only for VRScreen.app"
            return
        }
        loginItem.isEnabled = true
        switch SMAppService.mainApp.status {
        case .enabled:
            loginItem.state = .on
            loginItem.title = "Launch at login"
        case .requiresApproval:
            loginItem.state = .mixed
            loginItem.title = "Launch at login (approve in System Settings…)"
        default:
            loginItem.state = .off
            loginItem.title = "Launch at login"
        }
    }

    private func toggleLaunchAtLogin() {
        let service = SMAppService.mainApp
        do {
            switch service.status {
            case .enabled:
                try service.unregister()
                log("menu: launch at login off")
            case .requiresApproval:
                SMAppService.openSystemSettingsLoginItems()
            default:
                try service.register()
                log("menu: launch at login on (\(service.status == .requiresApproval ? "needs approval in System Settings › General › Login Items" : "registered"))")
                if service.status == .requiresApproval { SMAppService.openSystemSettingsLoginItems() }
            }
        } catch {
            log("menu: launch at login: \(error)")
        }
        refreshLoginItem()
    }

    // MARK: Travel mode (USB only)

    /// Reads the state in the background (adb, ≤ 3 s) and updates the menu when it's back.
    private func refreshTravelMode() {
        guard let usb else { return apply(.usbOff) }
        if usbStatus?.link == .adbMissing { return apply(.adbMissing) }
        guard let headset = usb.headset else { return apply(.notOnUSB) }
        guard !travelModeQueryRunning else { return }
        travelModeQueryRunning = true
        DispatchQueue.global().async { [weak self] in
            let state = headset.readTravelMode()
            DispatchQueue.main.async {
                self?.travelModeQueryRunning = false
                self?.apply(state)
            }
        }
    }

    private func apply(_ state: TravelModeState) {
        travelModeLine.title = state.menuTitle
        if state != travelModeState { log("menu: \(state.menuTitle)") }
        travelModeState = state
    }

    private func openQuickSettings() {
        guard let headset = usb?.headset else { return }
        DispatchQueue.global().async {
            switch headset.openQuickSettings() {
            case .success: log("menu: opened Quick Settings in the headset (tap Travel mode there)")
            case .failure(let error): log("menu: could not open Quick Settings: \(error)")
            }
        }
        // The user flips the tile in the headset; pick the new state up shortly after.
        for delay in [3.0, 6.0, 10.0] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in self?.refreshTravelMode() }
        }
    }
}

/// Target for closure-based menu items.
private final class MenuAction: NSObject {
    let handler: () -> Void

    init(_ handler: @escaping () -> Void) {
        self.handler = handler
    }

    @objc func run() { handler() }
}

private extension NSFont {
    var bold: NSFont { NSFontManager.shared.convert(self, toHaveTrait: .boldFontMask) }
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

    /// Follows a change made elsewhere (settings reload); a no-op while dragging to the same value.
    func setPercent(_ percent: Double) {
        guard abs(slider.doubleValue - percent) >= 0.05 else { return }
        slider.doubleValue = percent
        showValue()
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
