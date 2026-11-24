import AppKit

final class StatusController {
    private enum AppStatus {
        case needsPairing
        case listening
        case processing
        case ready
        case error

        init(text: String) {
            let normalized = text.lowercased()
            if normalized.contains("pair") {
                self = .needsPairing
            } else if normalized.contains("listen") {
                self = .listening
            } else if normalized.contains("error") || normalized.contains("failed") {
                self = .error
            } else if normalized.contains("process") || normalized.contains("final") || normalized.contains("transcrib") {
                self = .processing
            } else if normalized.contains("ready") ||
                        normalized.contains("connect") ||
                        normalized.contains("scan") ||
                        normalized.contains("match") ||
                        normalized.contains("pause") ||
                        normalized.contains("no speech") {
                self = .ready
            } else {
                self = .processing
            }
        }

        func symbolName(hasConnectedDevices: Bool) -> String {
            switch self {
            case .needsPairing:
                return "dot.radiowaves.left.and.right"
            case .listening:
                return "mic.fill"
            case .processing:
                return "waveform"
            case .ready:
                if !hasConnectedDevices {
                    return "dot.radiowaves.left.and.right"
                }
                return "link.circle.fill"
            case .error:
                return "exclamationmark.triangle"
            }
        }

        var accessibilityDescription: String {
            switch self {
            case .needsPairing:
                return "Pair VoiceStick"
            case .listening:
                return "Listening"
            case .processing:
                return "Processing"
            case .ready:
                return "Ready"
            case .error:
                return "Error"
            }
        }
    }

    private static let batteryPercentagePreference = "showBatteryPercentageInMenuBar"
    private static let compactStatusItemWidth: CGFloat = 18
    private static let batteryStatusItemSidePadding: CGFloat = 5
    private let statusItem = NSStatusBar.system.statusItem(withLength: StatusController.compactStatusItemWidth)
    private let menu = NSMenu()
    private var overlays: [String: OverlayController] = [:]
    private var visibleOverlayKeys: Set<String> = []

    var onQuit: (() -> Void)?
    var onOpenSettings: (() -> Void)?
    var onPairDevice: (() -> Void)?
    var onForgetDevice: ((String) -> Void)?
    var onCheckFirmwareDevice: ((String) -> Void)?
    var onUpdateFirmwareDevice: ((String) -> Void)?
    var onSetDeviceThemeColor: ((String, OverlayThemeColor) -> Void)?
    var onSetDeviceOverlayPosition: ((String, OverlayPosition) -> Void)?
    var onRestoreLastInput: (() -> Bool)?
    var onSetInteractionMode: ((InteractionMode) -> Void)?
    var onSetAutoEnter: ((Bool) -> Void)?
    var onSetDefaultOutputProfile: ((OutputProfile) -> Void)?
    var onSetDeviceOutputProfile: ((String, OutputProfile) -> Void)?
    var onCheckForUpdates: (() -> Void)? {
        didSet { rebuildMenu() }
    }
    private var needsPairing: Bool
    private var appStatus: AppStatus = .ready
    private var hasRecoverableInput = false
    private var pairedDeviceIDs: [String]
    private var deviceThemeColors: [String: OverlayThemeColor]
    private var deviceOverlayPositions: [String: OverlayPosition]
    private var connectedDevices: [ConnectedVoiceStickDevice] = []
    private var batteryStatusByDeviceID: [String: DeviceBatteryStatus] = [:]
    private var showsBatteryPercentage: Bool {
        UserDefaults.standard.bool(forKey: Self.batteryPercentagePreference)
    }
    private var firmwareInfoByDeviceID: [String: DeviceFirmwareInfo] = [:]
    private var interactionMode: InteractionMode
    private var autoEnter: Bool
    private var defaultOutputProfile: OutputProfile
    private var deviceOutputProfiles: [String: OutputProfile]

    init(pairedDeviceIDs: [String] = [],
         deviceThemeColors: [String: OverlayThemeColor] = [:],
         deviceOverlayPositions: [String: OverlayPosition] = [:],
         interactionMode: InteractionMode = .holdToTalk,
         autoEnter: Bool = true,
         defaultOutputProfile: OutputProfile = .default,
         deviceOutputProfiles: [String: OutputProfile] = [:]) {
        self.pairedDeviceIDs = pairedDeviceIDs
        self.deviceThemeColors = deviceThemeColors
        self.deviceOverlayPositions = deviceOverlayPositions
        self.interactionMode = interactionMode
        self.autoEnter = autoEnter
        self.defaultOutputProfile = defaultOutputProfile
        self.deviceOutputProfiles = deviceOutputProfiles
        self.needsPairing = pairedDeviceIDs.isEmpty
        updateStatusButton()
        rebuildMenu()
    }

    func setPairedDeviceIDs(_ deviceIDs: [String]) {
        pairedDeviceIDs = deviceIDs
        deviceThemeColors = deviceThemeColors.filter { deviceIDs.contains($0.key) }
        deviceOverlayPositions = deviceOverlayPositions.filter { deviceIDs.contains($0.key) }
        deviceOutputProfiles = deviceOutputProfiles.filter { deviceIDs.contains($0.key) }
        batteryStatusByDeviceID = batteryStatusByDeviceID.filter { deviceIDs.contains($0.key) }
        needsPairing = deviceIDs.isEmpty
        updateStatusButton()
        rebuildMenu()
    }

    func setDeviceThemeColors(_ colors: [String: OverlayThemeColor]) {
        deviceThemeColors = colors.filter { pairedDeviceIDs.contains($0.key) }
        rebuildMenu()
    }

    func setDeviceOverlayPositions(_ positions: [String: OverlayPosition]) {
        deviceOverlayPositions = positions.filter { pairedDeviceIDs.contains($0.key) }
        rebuildMenu()
    }

    func setConnectedDevices(_ devices: [ConnectedVoiceStickDevice]) {
        let sortedDevices = devices.sorted { $0.deviceID < $1.deviceID }
        guard connectedDevices.map(\.deviceID) != sortedDevices.map(\.deviceID) ||
                connectedDevices.map(\.name) != sortedDevices.map(\.name) else { return }
        connectedDevices = sortedDevices
        let connectedIDs = Set(sortedDevices.map(\.deviceID))
        batteryStatusByDeviceID = batteryStatusByDeviceID.filter { connectedIDs.contains($0.key) }
        updateStatusButton()
        rebuildMenu()
    }

    func setBatteryStatus(_ status: DeviceBatteryStatus, for deviceID: String) {
        guard batteryStatusByDeviceID[deviceID] != status else { return }
        batteryStatusByDeviceID[deviceID] = status
        updateStatusButton()
        rebuildMenu()
    }

    func setFirmwareInfo(_ infoByDeviceID: [String: DeviceFirmwareInfo]) {
        firmwareInfoByDeviceID = infoByDeviceID
        rebuildMenu()
    }

    func setHasRecoverableInput(_ hasRecoverableInput: Bool) {
        guard self.hasRecoverableInput != hasRecoverableInput else { return }
        self.hasRecoverableInput = hasRecoverableInput
        rebuildMenu()
    }

    func setInputOptions(interactionMode: InteractionMode, autoEnter: Bool) {
        guard self.interactionMode != interactionMode || self.autoEnter != autoEnter else { return }
        self.interactionMode = interactionMode
        self.autoEnter = autoEnter
        rebuildMenu()
    }

    func setDefaultOutputProfile(_ profile: OutputProfile) {
        guard defaultOutputProfile != profile else { return }
        defaultOutputProfile = profile
        rebuildMenu()
    }

    func setDeviceOutputProfiles(_ profiles: [String: OutputProfile]) {
        deviceOutputProfiles = profiles.filter { pairedDeviceIDs.contains($0.key) }
        rebuildMenu()
    }

    private func rebuildMenu() {
        menu.removeAllItems()
        if hasRecoverableInput {
            menu.addItem(makeMenuItem(
                title: "Restore Last Input",
                symbolName: "arrow.uturn.backward",
                action: #selector(restoreLastInput)
            ))
            menu.addItem(NSMenuItem.separator())
        }

        addDeviceItems()

        let batteryPercentageItem = makeMenuItem(
            title: "Show Battery Percentage in Menu Bar",
            symbolName: "battery.100",
            action: #selector(toggleBatteryPercentage)
        )
        batteryPercentageItem.state = showsBatteryPercentage ? .on : .off
        menu.addItem(batteryPercentageItem)

        addInputItems()

        menu.addItem(makeMenuItem(
            title: "Pair Device...",
            symbolName: "dot.radiowaves.left.and.right",
            action: #selector(pairDevice)
        ))

        menu.addItem(makeMenuItem(
            title: "Settings...",
            symbolName: "gearshape",
            action: #selector(openSettings),
            keyEquivalent: ","
        ))

        menu.addItem(NSMenuItem.separator())

        menu.addItem(makeMenuItem(
            title: "Website",
            symbolName: "safari",
            action: #selector(openWebsite)
        ))

        if onCheckForUpdates != nil {
            menu.addItem(makeMenuItem(
                title: "Check for App Updates...",
                symbolName: "arrow.triangle.2.circlepath",
                action: #selector(checkForUpdates)
            ))
        }

        menu.addItem(makeMenuItem(
            title: "Quit",
            symbolName: "power",
            action: #selector(quitApp),
            keyEquivalent: "q"
        ))

        statusItem.menu = menu
    }

    private func addInputItems() {
        addOutputItems()

        let afterPasteItem = makeMenuItem(
            title: "Press Return After Paste",
            symbolName: "return",
            action: #selector(toggleAutoEnter)
        )
        afterPasteItem.state = autoEnter ? .on : .off
        menu.addItem(afterPasteItem)

        let interactionItem = makeMenuItem(
            title: "Interaction",
            symbolName: "hand.tap",
            action: nil
        )
        let interactionSubmenu = NSMenu()
        let holdItem = makeMenuItem(
            title: InteractionMode.holdToTalk.displayName,
            symbolName: "hand.tap",
            action: #selector(selectInteractionMode)
        )
        holdItem.representedObject = InteractionMode.holdToTalk.rawValue
        holdItem.state = interactionMode == .holdToTalk ? .on : .off
        interactionSubmenu.addItem(holdItem)

        let clickItem = makeMenuItem(
            title: InteractionMode.clickToTalk.displayName,
            symbolName: "cursorarrow.click",
            action: #selector(selectInteractionMode)
        )
        clickItem.representedObject = InteractionMode.clickToTalk.rawValue
        clickItem.state = interactionMode == .clickToTalk ? .on : .off
        interactionSubmenu.addItem(clickItem)

        interactionItem.submenu = interactionSubmenu
        menu.addItem(interactionItem)
        menu.addItem(NSMenuItem.separator())
    }

    private func addOutputItems() {
        let outputItem = makeMenuItem(
            title: "Output",
            symbolName: "text.bubble",
            action: nil
        )
        let outputSubmenu = NSMenu()
        for target in OutputTarget.allCases {
            let item = NSMenuItem(
                title: target.displayName,
                action: #selector(selectOutputTarget),
                keyEquivalent: ""
            )
            item.target = self
            item.representedObject = target.rawValue
            item.state = defaultOutputProfile.target == target ? .on : .off
            outputSubmenu.addItem(item)
        }
        outputItem.submenu = outputSubmenu
        menu.addItem(outputItem)
    }

    private func addDeviceItems() {
        guard !pairedDeviceIDs.isEmpty else { return }

        let connectedByID = Dictionary(uniqueKeysWithValues: connectedDevices.map { ($0.deviceID, $0) })
        for deviceID in pairedDeviceIDs.sorted() {
            let connectedDevice = connectedByID[deviceID]
            var title = connectedDevice?.name ?? "VS-\(deviceID)"
            if connectedDevice != nil, let battery = batteryStatusByDeviceID[deviceID] {
                title += " · \(battery.percent)%"
                if battery.charging {
                    title += " · Charging"
                } else if battery.usbPowered {
                    title += " · USB Powered"
                }
            }
            let deviceItem = makeMenuItem(
                title: title,
                symbolName: connectedDevice == nil ? "link.circle" : "link.circle.fill",
                action: nil
            )
            let submenu = NSMenu()
            if connectedDevice != nil {
                let stateItem = NSMenuItem(title: "Connected", action: nil, keyEquivalent: "")
                stateItem.isEnabled = false
                stateItem.image = Self.symbolImage(named: "checkmark.circle", accessibilityDescription: stateItem.title)
                submenu.addItem(stateItem)
                submenu.addItem(NSMenuItem.separator())
            }

            addThemeColorItems(to: submenu, deviceID: deviceID)
            addOverlayPositionItems(to: submenu, deviceID: deviceID)
            addDeviceTextItems(to: submenu, deviceID: deviceID)
            submenu.addItem(NSMenuItem.separator())

            addFirmwareItems(to: submenu, deviceID: deviceID, isConnected: connectedDevice != nil)

            let forgetItem = makeMenuItem(
                title: "Forget This Device",
                symbolName: "xmark.circle",
                action: #selector(forgetConnectedDevice)
            )
            forgetItem.representedObject = deviceID
            submenu.addItem(forgetItem)
            deviceItem.submenu = submenu
            menu.addItem(deviceItem)
        }

        menu.addItem(NSMenuItem.separator())
    }

    private func addThemeColorItems(to submenu: NSMenu, deviceID: String) {
        let currentColor = deviceThemeColors[deviceID] ?? .white
        let themeItem = makeMenuItem(
            title: "Theme Color",
            symbolName: "paintpalette",
            action: nil
        )
        let themeSubmenu = NSMenu()
        for color in OverlayThemeColor.allCases {
            let colorItem = NSMenuItem(
                title: color.displayName,
                action: #selector(selectDeviceThemeColor),
                keyEquivalent: ""
            )
            colorItem.target = self
            colorItem.representedObject = "\(deviceID):\(color.rawValue)"
            colorItem.state = currentColor == color ? .on : .off
            themeSubmenu.addItem(colorItem)
        }
        themeItem.submenu = themeSubmenu
        submenu.addItem(themeItem)
    }

    private func addOverlayPositionItems(to submenu: NSMenu, deviceID: String) {
        let currentPosition = deviceOverlayPositions[deviceID] ?? .center
        let positionItem = makeMenuItem(
            title: "Overlay Position",
            symbolName: "rectangle.inset.filled",
            action: nil
        )
        let positionSubmenu = NSMenu()
        for position in OverlayPosition.allCases {
            let item = NSMenuItem(
                title: position.displayName,
                action: #selector(selectDeviceOverlayPosition),
                keyEquivalent: ""
            )
            item.target = self
            item.representedObject = "\(deviceID):\(position.rawValue)"
            item.state = currentPosition == position ? .on : .off
            positionSubmenu.addItem(item)
        }
        positionItem.submenu = positionSubmenu
        submenu.addItem(positionItem)
    }

    private func addDeviceTextItems(to submenu: NSMenu, deviceID: String) {
        let profile = outputProfile(for: deviceID)
        let textItem = makeMenuItem(
            title: "Translation",
            symbolName: "textformat",
            action: nil
        )
        let textSubmenu = NSMenu()
        let originalItem = NSMenuItem(
            title: "Original",
            action: #selector(selectDeviceTextMode),
            keyEquivalent: ""
        )
        originalItem.target = self
        originalItem.representedObject = "\(deviceID):original:"
        originalItem.state = profile.transform == .original ? .on : .off
        textSubmenu.addItem(originalItem)
        textSubmenu.addItem(NSMenuItem.separator())

        for language in Self.translationTargets {
            let item = NSMenuItem(
                title: "Translate to \(language.name)",
                action: #selector(selectDeviceTextMode),
                keyEquivalent: ""
            )
            item.target = self
            item.representedObject = "\(deviceID):translate:\(language.code)"
            item.state = profile.transform == .translate && profile.translationTarget == language.code ? .on : .off
            textSubmenu.addItem(item)
        }
        textItem.submenu = textSubmenu
        submenu.addItem(textItem)
    }

    private func addFirmwareItems(to submenu: NSMenu, deviceID: String, isConnected: Bool) {
        let info = firmwareInfoByDeviceID[deviceID]
        let currentTitle = info?.currentVersion.map { "Firmware \($0)" } ?? "Firmware Unknown"
        let currentItem = NSMenuItem(title: currentTitle, action: nil, keyEquivalent: "")
        currentItem.isEnabled = false
        currentItem.image = Self.symbolImage(named: "info.circle", accessibilityDescription: currentTitle)
        submenu.addItem(currentItem)

        if info?.isChecking == true {
            let checkingItem = NSMenuItem(title: "Checking for Firmware Updates…", action: nil, keyEquivalent: "")
            checkingItem.isEnabled = false
            checkingItem.image = Self.symbolImage(named: "arrow.triangle.2.circlepath", accessibilityDescription: "Checking")
            submenu.addItem(checkingItem)
            return
        }

        if info?.errorMessage == nil, info?.updateAvailable == true, let latestVersion = info?.latestVersion {
            let updateItem = makeMenuItem(
                title: "Update to \(latestVersion)...",
                symbolName: "square.and.arrow.down",
                action: #selector(updateFirmwareForDevice)
            )
            updateItem.representedObject = deviceID
            updateItem.isEnabled = isConnected
            submenu.addItem(updateItem)
            return
        }

        let checkItem = makeMenuItem(
            title: "Check for Firmware Updates…",
            symbolName: "arrow.triangle.2.circlepath",
            action: #selector(checkFirmwareForDevice)
        )
        checkItem.representedObject = deviceID
        submenu.addItem(checkItem)

        if let errorMessage = info?.errorMessage {
            let errorItem = NSMenuItem(title: "Update Check Failed", action: nil, keyEquivalent: "")
            errorItem.toolTip = errorMessage
            errorItem.isEnabled = false
            errorItem.image = Self.symbolImage(named: "exclamationmark.triangle", accessibilityDescription: "Update Check Failed")
            submenu.addItem(errorItem)
        }
    }

    func setStatus(_ text: String) {
        DispatchQueue.main.async {
            self.appStatus = AppStatus(text: text)
            self.updateStatusButton()
        }
    }

    func showListening(deviceID: String? = nil) {
        setStatus("Listening")
        let overlay = overlay(for: deviceID)
        markOverlayVisible(for: deviceID)
        applyOverlayStyle(for: deviceID, overlay: overlay)
        overlay.showListening(text: "")
    }

    func showPartial(_ text: String, deviceID: String? = nil) {
        setStatus(text.isEmpty ? "Listening" : text)
        let overlay = overlay(for: deviceID)
        markOverlayVisible(for: deviceID)
        applyOverlayStyle(for: deviceID, overlay: overlay)
        overlay.showListening(text: text)
    }

    func showFinal(_ text: String, deviceID: String? = nil, onHidden: (() -> Void)? = nil) {
        setStatus(text.isEmpty ? "No speech" : "Ready")
        let overlay = overlay(for: deviceID)
        markOverlayVisible(for: deviceID)
        applyOverlayStyle(for: deviceID, overlay: overlay)
        overlay.showFinal(text: text, onHidden: { [weak self] in
            self?.markOverlayHidden(for: deviceID)
            onHidden?()
        })
    }

    func showPausedFinal(_ text: String, deviceID: String? = nil) {
        let overlay = overlay(for: deviceID)
        markOverlayVisible(for: deviceID)
        applyOverlayStyle(for: deviceID, overlay: overlay)
        overlay.showPaused(text: text)
    }

    func showError(_ text: String, deviceID: String? = nil, onHidden: (() -> Void)? = nil) {
        setStatus("ASR error: \(text)")
        let overlay = overlay(for: deviceID)
        markOverlayVisible(for: deviceID)
        applyOverlayStyle(for: deviceID, overlay: overlay)
        overlay.showError(text, onHidden: { [weak self] in
            self?.markOverlayHidden(for: deviceID)
            onHidden?()
        })
    }

    func hideOverlay(onHidden: (() -> Void)? = nil) {
        let overlayList = overlays.values
        visibleOverlayKeys.removeAll()
        var remaining = overlayList.count
        guard remaining > 0 else {
            onHidden?()
            return
        }
        for overlay in overlayList {
            overlay.hide {
                remaining -= 1
                if remaining == 0 {
                    onHidden?()
                }
            }
        }
    }

    func hideOverlay(deviceID: String?, onHidden: (() -> Void)? = nil) {
        let key = overlayKey(for: deviceID)
        visibleOverlayKeys.remove(key)
        updateOverlayStackIndices()
        guard let overlay = overlays[key] else {
            onHidden?()
            return
        }
        overlay.hide(onHidden: onHidden)
    }

    private func updateStatusButton() {
        guard let button = statusItem.button else { return }
        var title = ""
        var description = "VoiceStick: \(appStatus.accessibilityDescription)"
        if showsBatteryPercentage, connectedDevices.count == 1, let device = connectedDevices.first,
           let battery = batteryStatusByDeviceID[device.deviceID] {
            title = "\(battery.percent)%"
            description += ", battery \(title)"
        }

        button.image = Self.symbolImage(
            named: appStatus.symbolName(hasConnectedDevices: !connectedDevices.isEmpty),
            accessibilityDescription: appStatus.accessibilityDescription,
            pointSize: 16
        )
        button.title = title
        button.imagePosition = title.isEmpty ? .imageOnly : .imageLeading
        if title.isEmpty {
            statusItem.length = Self.compactStatusItemWidth
        } else {
            let contentWidth = button.intrinsicContentSize.width
            statusItem.length = contentWidth > 0
                ? ceil(contentWidth) + 2 * Self.batteryStatusItemSidePadding
                : NSStatusItem.variableLength
        }
        button.toolTip = description
        button.setAccessibilityLabel(description)
    }

    private func makeMenuItem(
        title: String,
        symbolName: String,
        action: Selector?,
        keyEquivalent: String = ""
    ) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: action, keyEquivalent: keyEquivalent)
        item.target = self
        item.image = Self.symbolImage(named: symbolName, accessibilityDescription: title)
        return item
    }

    private static func symbolImage(
        named name: String,
        accessibilityDescription: String,
        pointSize: CGFloat = 14
    ) -> NSImage? {
        let configuration = NSImage.SymbolConfiguration(pointSize: pointSize, weight: .regular)
        let image = NSImage(systemSymbolName: name, accessibilityDescription: accessibilityDescription)?
            .withSymbolConfiguration(configuration)
        image?.isTemplate = true
        image?.size = NSSize(width: 16, height: 16)
        return image
    }

    private static let translationTargets: [(code: String, name: String)] = [
        ("en", "English"),
        ("zh-Hans", "Chinese (Simplified)"),
        ("zh-Hant", "Chinese (Traditional)"),
        ("ja", "Japanese"),
        ("ko", "Korean"),
        ("ru", "Russian"),
        ("fr", "French"),
        ("de", "German"),
        ("es", "Spanish"),
        ("it", "Italian"),
        ("pt", "Portuguese"),
        ("nl", "Dutch"),
        ("sv", "Swedish"),
        ("pl", "Polish"),
        ("tr", "Turkish"),
        ("ar", "Arabic"),
        ("hi", "Hindi"),
        ("id", "Indonesian"),
        ("vi", "Vietnamese"),
        ("th", "Thai")
    ]

    private func themeColor(for deviceID: String?) -> OverlayThemeColor {
        guard let deviceID else { return .white }
        return deviceThemeColors[AppConfig.normalizedDeviceID(deviceID)] ?? .white
    }

    private func overlayPosition(for deviceID: String?) -> OverlayPosition {
        guard let deviceID else { return .center }
        return deviceOverlayPositions[AppConfig.normalizedDeviceID(deviceID)] ?? .center
    }

    private func overlayKey(for deviceID: String?) -> String {
        guard let deviceID else { return "__default__" }
        return AppConfig.normalizedDeviceID(deviceID)
    }

    private func overlay(for deviceID: String?) -> OverlayController {
        let key = overlayKey(for: deviceID)
        if let overlay = overlays[key] {
            return overlay
        }
        let overlay = OverlayController()
        overlays[key] = overlay
        return overlay
    }

    private func markOverlayVisible(for deviceID: String?) {
        visibleOverlayKeys.insert(overlayKey(for: deviceID))
        updateOverlayStackIndices()
    }

    private func markOverlayHidden(for deviceID: String?) {
        visibleOverlayKeys.remove(overlayKey(for: deviceID))
        updateOverlayStackIndices()
    }

    private func updateOverlayStackIndices() {
        let groupedKeys = Dictionary(grouping: visibleOverlayKeys) { key in
            overlayPosition(for: deviceID(forOverlayKey: key))
        }
        for keys in groupedKeys.values {
            for (index, key) in keys.sorted().enumerated() {
                overlays[key]?.setStackIndex(index)
            }
        }
    }

    private func deviceID(forOverlayKey key: String) -> String? {
        key == "__default__" ? nil : key
    }

    private func outputProfile(for deviceID: String) -> OutputProfile {
        guard let deviceProfile = deviceOutputProfiles[AppConfig.normalizedDeviceID(deviceID)] else {
            return defaultOutputProfile
        }
        return OutputProfile(
            target: defaultOutputProfile.target,
            transform: deviceProfile.transform,
            translationTarget: deviceProfile.translationTarget
        )
    }

    private func applyOverlayStyle(for deviceID: String?, overlay: OverlayController) {
        overlay.setThemeColor(themeColor(for: deviceID))
        overlay.setPosition(overlayPosition(for: deviceID))
    }

    @objc private func toggleBatteryPercentage() {
        UserDefaults.standard.set(!showsBatteryPercentage, forKey: Self.batteryPercentagePreference)
        updateStatusButton()
        rebuildMenu()
    }

    @objc private func openSettings() {
        onOpenSettings?()
    }

    @objc private func pairDevice() {
        onPairDevice?()
    }

    @objc private func forgetConnectedDevice(_ sender: NSMenuItem) {
        guard let deviceID = sender.representedObject as? String else { return }
        onForgetDevice?(deviceID)
    }

    @objc private func checkFirmwareForDevice(_ sender: NSMenuItem) {
        guard let deviceID = sender.representedObject as? String else { return }
        onCheckFirmwareDevice?(deviceID)
    }

    @objc private func updateFirmwareForDevice(_ sender: NSMenuItem) {
        guard let deviceID = sender.representedObject as? String else { return }
        onUpdateFirmwareDevice?(deviceID)
    }

    @objc private func selectDeviceThemeColor(_ sender: NSMenuItem) {
        guard let selection = sender.representedObject as? String else { return }
        let parts = selection.split(separator: ":", maxSplits: 1).map(String.init)
        guard parts.count == 2,
              let color = OverlayThemeColor(rawValue: parts[1]) else { return }
        let deviceID = AppConfig.normalizedDeviceID(parts[0])
        if color == .white {
            deviceThemeColors.removeValue(forKey: deviceID)
        } else {
            deviceThemeColors[deviceID] = color
        }
        rebuildMenu()
        onSetDeviceThemeColor?(deviceID, color)
    }

    @objc private func selectDeviceOverlayPosition(_ sender: NSMenuItem) {
        guard let selection = sender.representedObject as? String else { return }
        let parts = selection.split(separator: ":", maxSplits: 1).map(String.init)
        guard parts.count == 2,
              let position = OverlayPosition(rawValue: parts[1]) else { return }
        let deviceID = AppConfig.normalizedDeviceID(parts[0])
        if position == .center {
            deviceOverlayPositions.removeValue(forKey: deviceID)
        } else {
            deviceOverlayPositions[deviceID] = position
        }
        rebuildMenu()
        onSetDeviceOverlayPosition?(deviceID, position)
    }

    @objc private func selectDeviceTextMode(_ sender: NSMenuItem) {
        guard let selection = sender.representedObject as? String else { return }
        let parts = selection.split(separator: ":", maxSplits: 2, omittingEmptySubsequences: false).map(String.init)
        guard parts.count >= 2,
              let transform = TextTransform(rawValue: parts[1]) else { return }
        let deviceID = AppConfig.normalizedDeviceID(parts[0])
        var profile = outputProfile(for: deviceID)
        profile.transform = transform
        if transform == .translate, parts.count == 3, !parts[2].isEmpty {
            profile.translationTarget = parts[2]
        }
        setDeviceOutputProfile(deviceID: deviceID, profile: profile)
    }

    private func setDeviceOutputProfile(deviceID: String, profile: OutputProfile) {
        let storedProfile = OutputProfile(
            target: defaultOutputProfile.target,
            transform: profile.transform,
            translationTarget: profile.translationTarget
        )
        if storedProfile.transform == defaultOutputProfile.transform &&
            storedProfile.translationTarget == defaultOutputProfile.translationTarget {
            deviceOutputProfiles.removeValue(forKey: deviceID)
        } else {
            deviceOutputProfiles[deviceID] = storedProfile
        }
        rebuildMenu()
        onSetDeviceOutputProfile?(deviceID, storedProfile)
    }

    @objc private func restoreLastInput() {
        _ = onRestoreLastInput?()
    }

    @objc private func selectInteractionMode(_ sender: NSMenuItem) {
        guard
            let rawValue = sender.representedObject as? String,
            let mode = InteractionMode(rawValue: rawValue)
        else { return }
        interactionMode = mode
        rebuildMenu()
        onSetInteractionMode?(mode)
    }

    @objc private func toggleAutoEnter() {
        autoEnter.toggle()
        rebuildMenu()
        onSetAutoEnter?(autoEnter)
    }

    @objc private func selectOutputTarget(_ sender: NSMenuItem) {
        guard
            let rawValue = sender.representedObject as? String,
            let target = OutputTarget(rawValue: rawValue)
        else { return }
        defaultOutputProfile.target = target
        rebuildMenu()
        onSetDefaultOutputProfile?(defaultOutputProfile)
    }

    @objc private func checkForUpdates() {
        onCheckForUpdates?()
    }

    @objc private func openWebsite() {
        NSWorkspace.shared.open(AppConfig.websiteURL)
    }

    @objc private func quitApp() {
        onQuit?()
    }
}
