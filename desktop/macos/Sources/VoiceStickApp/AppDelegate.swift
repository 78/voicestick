import AppKit
import Sparkle

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusController: StatusController?
    private var coordinator: VoiceStickCoordinator?
    private var settingsWindowController: SettingsWindowController?
    private var pairDeviceWindowController: PairDeviceWindowController?
    private var onboardingWindowController: OnboardingWindowController?
    private var firmwareUpdateWindowController: FirmwareUpdateWindowController?
    private var updaterController: SPUStandardUpdaterController?
    private var dockIconWindowIDs = Set<ObjectIdentifier>()
    private var config = AppConfig.defaults

    func applicationDidFinishLaunching(_ notification: Notification) {
        configureMainMenu()
        configureApplicationIcon()
        let loadedConfig = AppConfig.load()
        if AppConfig.configExists {
            startApp(config: loadedConfig)
        } else {
            showOnboarding(config: loadedConfig)
        }
    }

    private func configureMainMenu() {
        let mainMenu = NSMenu()
        let appItem = NSMenuItem()
        mainMenu.addItem(appItem)

        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "Quit VoiceStick", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu

        let editItem = NSMenuItem()
        mainMenu.addItem(editItem)

        let editMenu = NSMenu(title: "Edit")
        editMenu.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
        editMenu.addItem(withTitle: "Redo", action: Selector(("redo:")), keyEquivalent: "Z")
        editMenu.addItem(NSMenuItem.separator())
        editMenu.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        editMenu.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        editMenu.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        editMenu.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        editItem.submenu = editMenu

        NSApp.mainMenu = mainMenu
    }

    private func startApp(config: AppConfig) {
        self.config = config
        let statusController = StatusController(
            pairedDeviceIDs: config.pairedDeviceIDs,
            deviceThemeColors: config.deviceThemeColors,
            deviceOverlayPositions: config.deviceOverlayPositions,
            interactionMode: config.interactionMode,
            autoEnter: config.autoEnter,
            defaultOutputProfile: config.defaultOutputProfile,
            deviceOutputProfiles: config.deviceOutputProfiles
        )
        let coordinator = VoiceStickCoordinator(config: config, statusController: statusController)

        self.statusController = statusController
        self.coordinator = coordinator
        statusController.setTranslationModeEnabled(Self.isTranslationEnabled(config))

        statusController.onQuit = { NSApp.terminate(nil) }
        statusController.onOpenSettings = { [weak self] in
            let controller = self?.settingsWindowController ?? SettingsWindowController()
            self?.settingsWindowController = controller
            controller.onConfigChanged = { [weak self] config in
                self?.config = config
                self?.statusController?.setPairedDeviceIDs(config.pairedDeviceIDs)
                self?.statusController?.setDeviceThemeColors(config.deviceThemeColors)
                self?.statusController?.setDeviceOverlayPositions(config.deviceOverlayPositions)
                self?.statusController?.setInputOptions(
                    interactionMode: config.interactionMode,
                    autoEnter: config.autoEnter
                )
                self?.statusController?.setDefaultOutputProfile(config.defaultOutputProfile)
                self?.statusController?.setDeviceOutputProfiles(config.deviceOutputProfiles)
                self?.statusController?.setTranslationModeEnabled(Self.isTranslationEnabled(config))
                self?.coordinator?.updateConfig(config)
            }
            self?.showDockIconWhileWindowVisible(controller)
            controller.show()
        }
        statusController.onPairDevice = { [weak self] in
            self?.showPairDeviceWindow()
        }
        statusController.onForgetDevice = { [weak self] deviceID in
            self?.forgetDevice(deviceID)
        }
        statusController.onUpdateFirmwareDevice = { [weak self] deviceID in
            self?.updateFirmwareFromLatest(for: deviceID)
        }
        coordinator.onFirmwareUpdatePrompt = { [weak self] deviceID, currentVersion, latestVersion, isBelowMinimum in
            self?.showFirmwareUpdatePrompt(
                deviceID: deviceID,
                currentVersion: currentVersion,
                latestVersion: latestVersion,
                isBelowMinimum: isBelowMinimum
            )
        }
        coordinator.onToggleTranslationMode = { [weak self] deviceID in
            self?.toggleTranslationMode(deviceID: deviceID)
        }
        statusController.onRestoreLastInput = { [weak self] in
            self?.coordinator?.restoreLastInputConfirmation() ?? false
        }
        statusController.onSetInteractionMode = { [weak self] mode in
            self?.updateInputOptions(interactionMode: mode, autoEnter: nil)
        }
        statusController.onSetAutoEnter = { [weak self] autoEnter in
            self?.updateInputOptions(interactionMode: nil, autoEnter: autoEnter)
        }
        statusController.onSetDefaultOutputProfile = { [weak self] profile in
            self?.updateDefaultOutputProfile(profile)
        }
        statusController.onSetDeviceOutputProfile = { [weak self] deviceID, profile in
            self?.updateDeviceOutputProfile(deviceID: deviceID, profile: profile)
        }
        statusController.onSetDeviceThemeColor = { [weak self] deviceID, color in
            self?.updateDeviceThemeColor(deviceID: deviceID, color: color)
        }
        statusController.onSetDeviceOverlayPosition = { [weak self] deviceID, position in
            self?.updateDeviceOverlayPosition(deviceID: deviceID, position: position)
        }
        if Self.hasSparklePublicKey {
            let updaterController = SPUStandardUpdaterController(
                startingUpdater: true,
                updaterDelegate: nil,
                userDriverDelegate: nil
            )
            self.updaterController = updaterController
            statusController.onCheckForUpdates = {
                updaterController.updater.checkForUpdates()
            }
        }
        statusController.setStatus(config.pairedDeviceIDs.isEmpty ? "Pair a VoiceStick" : "Ready")
        coordinator.start()
    }

    private func updateInputOptions(interactionMode: InteractionMode?, autoEnter: Bool?) {
        var config = self.config
        if let interactionMode {
            config.interactionMode = interactionMode
        }
        if let autoEnter {
            config.autoEnter = autoEnter
        }
        do {
            try config.save()
            self.config = config
            statusController?.setInputOptions(
                interactionMode: config.interactionMode,
                autoEnter: config.autoEnter
            )
            statusController?.setTranslationModeEnabled(Self.isTranslationEnabled(config))
            coordinator?.updateConfig(config)
        } catch {
            statusController?.setStatus("Input save failed")
        }
    }

    private func updateDeviceThemeColor(deviceID: String, color: OverlayThemeColor) {
        var config = self.config
        if color == .white {
            config.deviceThemeColors.removeValue(forKey: deviceID)
        } else {
            config.deviceThemeColors[deviceID] = color
        }
        do {
            try config.save()
            self.config = config
            statusController?.setDeviceThemeColors(config.deviceThemeColors)
            statusController?.setTranslationModeEnabled(Self.isTranslationEnabled(config))
        } catch {
            statusController?.setStatus("Theme save failed")
        }
    }

    private func updateDefaultOutputProfile(_ profile: OutputProfile) {
        var config = self.config
        config.defaultOutputProfile = profile
        do {
            try config.save()
            self.config = config
            statusController?.setDefaultOutputProfile(profile)
            statusController?.setTranslationModeEnabled(Self.isTranslationEnabled(config))
            coordinator?.updateConfig(config)
        } catch {
            statusController?.setStatus("Output save failed")
        }
    }

    private func updateDeviceOutputProfile(deviceID: String, profile: OutputProfile) {
        var config = self.config
        let storedProfile = OutputProfile(
            target: config.defaultOutputProfile.target,
            transform: profile.transform,
            translationTarget: profile.translationTarget
        )
        if storedProfile.transform == config.defaultOutputProfile.transform &&
            storedProfile.translationTarget == config.defaultOutputProfile.translationTarget {
            config.deviceOutputProfiles.removeValue(forKey: deviceID)
        } else {
            config.deviceOutputProfiles[deviceID] = storedProfile
        }
        do {
            try config.save()
            self.config = config
            statusController?.setDeviceOutputProfiles(config.deviceOutputProfiles)
            statusController?.setTranslationModeEnabled(Self.isTranslationEnabled(config))
            coordinator?.updateConfig(config)
        } catch {
            statusController?.setStatus("Output save failed")
        }
    }

    private func toggleTranslationMode(deviceID: String?) {
        if let deviceID {
            let currentProfile = config.outputProfile(for: deviceID)
            let toggledProfile = OutputProfile(
                target: currentProfile.target,
                transform: currentProfile.transform == .translate ? .original : .translate,
                translationTarget: currentProfile.transform == .translate ? currentProfile.translationTarget : "en"
            )
            updateDeviceOutputProfile(deviceID: deviceID, profile: toggledProfile)
            return
        }

        let currentProfile = config.defaultOutputProfile
        let toggledProfile = OutputProfile(
            target: currentProfile.target,
            transform: currentProfile.transform == .translate ? .original : .translate,
            translationTarget: currentProfile.transform == .translate ? currentProfile.translationTarget : "en"
        )
        updateDefaultOutputProfile(toggledProfile)
    }

    private func updateDeviceOverlayPosition(deviceID: String, position: OverlayPosition) {
        var config = self.config
        if position == .center {
            config.deviceOverlayPositions.removeValue(forKey: deviceID)
        } else {
            config.deviceOverlayPositions[deviceID] = position
        }
        do {
            try config.save()
            self.config = config
            statusController?.setDeviceOverlayPositions(config.deviceOverlayPositions)
        } catch {
            statusController?.setStatus("Position save failed")
        }
    }

    private static func isTranslationEnabled(_ config: AppConfig) -> Bool {
        if config.defaultOutputProfile.transform == .translate {
            return true
        }
        return config.deviceOutputProfiles.values.contains { $0.transform == .translate }
    }

    private func showOnboarding(config: AppConfig) {
        let controller = OnboardingWindowController(config: config) { [weak self] config in
            self?.onboardingWindowController = nil
            self?.startApp(config: config)
        }
        onboardingWindowController = controller
        showDockIconWhileWindowVisible(controller)
        controller.show()
    }

    private func configureApplicationIcon() {
        if let image = Self.applicationIconImage() {
            NSApp.applicationIconImage = image
            let imageView = NSImageView(frame: NSRect(x: 4, y: 4, width: 120, height: 120))
            imageView.image = image
            imageView.imageScaling = .scaleProportionallyUpOrDown
            let dockView = NSView(frame: NSRect(x: 0, y: 0, width: 128, height: 128))
            dockView.addSubview(imageView)
            NSApp.dockTile.contentView = dockView
            NSApp.dockTile.display()
        }
    }

    private static var hasSparklePublicKey: Bool {
        guard let publicKey = Bundle.main.object(forInfoDictionaryKey: "SUPublicEDKey") as? String else {
            return false
        }
        return !publicKey.isEmpty && !publicKey.hasPrefix("REPLACE_WITH")
    }

    private func showPairDeviceWindow() {
        var config = AppConfig.load()
        let controller = PairDeviceWindowController(existingDeviceIDs: config.pairedDeviceIDs) { [weak self] deviceID in
            if !config.pairedDeviceIDs.contains(deviceID) {
                config.pairedDeviceIDs.append(deviceID)
            }
            do {
                try config.save()
                self?.config = config
                self?.statusController?.setPairedDeviceIDs(config.pairedDeviceIDs)
                self?.statusController?.setDeviceThemeColors(config.deviceThemeColors)
                self?.statusController?.setDeviceOverlayPositions(config.deviceOverlayPositions)
                self?.coordinator?.updatePairedDeviceIDs(config.pairedDeviceIDs)
                self?.coordinator?.checkFirmwareAfterPairing(deviceID: deviceID)
            } catch {
                self?.statusController?.setStatus("Pair save failed")
            }
        }
        pairDeviceWindowController = controller
        showDockIconWhileWindowVisible(controller)
        controller.show()
    }

    private func updateFirmwareFromLatest(for deviceID: String) {
        let updateWindow = FirmwareUpdateWindowController(fileName: "VS-\(deviceID)")
        updateWindow.onCancel = { [weak self] in
            self?.coordinator?.cancelFirmwareUpdate()
        }
        firmwareUpdateWindowController = updateWindow
        showDockIconWhileWindowVisible(updateWindow)
        updateWindow.show()

        coordinator?.updateFirmwareFromLatest(for: deviceID, progress: { [weak self] progress in
            DispatchQueue.main.async {
                self?.firmwareUpdateWindowController?.update(progress: progress)
            }
        }, completion: { [weak self] result in
            DispatchQueue.main.async {
                self?.firmwareUpdateWindowController?.finish(result: result)
            }
        })
    }

    private func showFirmwareUpdatePrompt(deviceID: String,
                                          currentVersion: String,
                                          latestVersion: String,
                                          isBelowMinimum: Bool) {
        let alert = NSAlert()
        alert.messageText = isBelowMinimum ? "Firmware update recommended" : "Firmware update available"
        alert.informativeText = "VS-\(deviceID) is running firmware \(currentVersion). The latest firmware is \(latestVersion)."
        alert.addButton(withTitle: "Update Firmware")
        alert.addButton(withTitle: "Later")
        if alert.runModal() == .alertFirstButtonReturn {
            updateFirmwareFromLatest(for: deviceID)
        }
    }

    private func showDockIconWhileWindowVisible(_ windowController: NSWindowController) {
        configureApplicationIcon()
        NSApp.setActivationPolicy(.regular)
        configureApplicationIcon()
        guard let window = windowController.window else { return }
        dockIconWindowIDs.insert(ObjectIdentifier(window))

        NotificationCenter.default.addObserver(
            self,
            selector: #selector(windowWillCloseForDockIcon),
            name: NSWindow.willCloseNotification,
            object: window
        )
    }

    @objc private func windowWillCloseForDockIcon(_ notification: Notification) {
        if let window = notification.object as? NSWindow {
            dockIconWindowIDs.remove(ObjectIdentifier(window))
        }
        NotificationCenter.default.removeObserver(
            self,
            name: NSWindow.willCloseNotification,
            object: notification.object
        )
        hideDockIconIfNoWindowsAreVisible()
    }

    private func hideDockIconIfNoWindowsAreVisible() {
        DispatchQueue.main.async {
            if self.dockIconWindowIDs.isEmpty {
                NSApp.setActivationPolicy(.accessory)
            }
        }
    }

    private static func applicationIconImage() -> NSImage? {
        let fileManager = FileManager.default
        let cwd = URL(fileURLWithPath: fileManager.currentDirectoryPath, isDirectory: true)
        let executableDirectory = Bundle.main.executableURL?.deletingLastPathComponent()

        let candidateURLs = [
            Bundle.main.url(forResource: "AppIcon", withExtension: "icns"),
            Bundle.main.resourceURL?.appendingPathComponent("AppIcon.icns"),
            cwd.appendingPathComponent("Resources/AppIcon.icns"),
            cwd.appendingPathComponent("desktop/macos/Resources/AppIcon.icns"),
            executableDirectory?.appendingPathComponent("../../Resources/AppIcon.icns").standardizedFileURL,
            executableDirectory?.appendingPathComponent("../../../Resources/AppIcon.icns").standardizedFileURL,
            executableDirectory?.appendingPathComponent("../../../../Resources/AppIcon.icns").standardizedFileURL
        ]

        for url in candidateURLs.compactMap({ $0 }) where fileManager.fileExists(atPath: url.path) {
            if let image = NSImage(contentsOf: url) {
                return image
            }
        }
        return nil
    }

    private func forgetDevice(_ deviceID: String) {
        var config = AppConfig.load()
        config.pairedDeviceIDs.removeAll { $0 == deviceID }
        config.deviceThemeColors.removeValue(forKey: deviceID)
        config.deviceOverlayPositions.removeValue(forKey: deviceID)
        do {
            try config.save()
            self.config = config
            statusController?.setPairedDeviceIDs(config.pairedDeviceIDs)
            statusController?.setDeviceThemeColors(config.deviceThemeColors)
            statusController?.setDeviceOverlayPositions(config.deviceOverlayPositions)
            statusController?.setConnectedDevices([])
            coordinator?.updatePairedDeviceIDs(config.pairedDeviceIDs)
        } catch {
            statusController?.setStatus("Forget device failed")
        }
    }
}
