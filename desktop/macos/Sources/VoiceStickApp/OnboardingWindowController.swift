import AppKit
import ApplicationServices
import CoreBluetooth

private struct OnboardingDevice {
    let identifier: UUID
    var name: String
    var deviceID: String
    var rssi: Int
}

final class OnboardingWindowController: NSWindowController, NSWindowDelegate, CBCentralManagerDelegate, NSTableViewDataSource, NSTableViewDelegate {
    private enum Step: Int, CaseIterable {
        case device
        case provider
        case accessibility
        case finish

        var title: String {
            switch self {
            case .device:
                return "Pair Device"
            case .provider:
                return "ASR Key"
            case .accessibility:
                return "Accessibility"
            case .finish:
                return "Ready"
            }
        }
    }

    private let stepList = NSStackView()
    private let contentStack = NSStackView()
    private let titleLabel = NSTextField(labelWithString: "")
    private let detailLabel = NSTextField(wrappingLabelWithString: "")
    private let backButton = NSButton(title: "Back", target: nil, action: nil)
    private let nextButton = NSButton(title: "Continue", target: nil, action: nil)
    private let statusLabel = NSTextField(labelWithString: "")

    private let tableView = NSTableView()
    private let scanStatusLabel = NSTextField(labelWithString: "Scanning")
    private let providerPopup = NSPopUpButton()
    private let apiKeyField = NSTextField()
    private let applyTrialAPIKeyButton = NSButton(title: "Apply Trial", target: nil, action: nil)
    private let resourcePopup = NSPopUpButton()
    private let accessibilityStatusLabel = NSTextField(labelWithString: "")
    private let accessibilitySettingsButton = NSButton(title: "Open Accessibility Settings", target: nil, action: nil)

    private var central: CBCentralManager?
    private var devices: [OnboardingDevice] = []
    private var currentStep: Step = .device
    private var currentDisplayedProvider: ASRProvider
    private var config: AppConfig
    private var didComplete = false
    private var didConfigureAPIKeyControlConstraints = false
    private let onComplete: (AppConfig) -> Void

    init(config: AppConfig, onComplete: @escaping (AppConfig) -> Void) {
        self.config = config
        self.currentDisplayedProvider = config.asrProvider
        self.onComplete = onComplete

        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 680, height: 470),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false
        )
        window.title = "Set Up VoiceStick"
        window.isReleasedWhenClosed = false
        super.init(window: window)
        window.delegate = self
        accessibilitySettingsButton.target = self
        accessibilitySettingsButton.action = #selector(requestAccessibilityPermission)
        applyTrialAPIKeyButton.target = self
        applyTrialAPIKeyButton.action = #selector(applyTrialAPIKey)
        buildContent()
        loadConfigIntoFields()
        renderStep()
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(applicationDidBecomeActive),
            name: NSApplication.didBecomeActiveNotification,
            object: nil
        )
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(apiKeyFieldDidChange),
            name: NSControl.textDidChangeNotification,
            object: apiKeyField
        )
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    func show() {
        showWindow(nil)
        window?.center()
        NSApp.activate(ignoringOtherApps: true)
        central = CBCentralManager(delegate: self, queue: .main)
    }

    func windowWillClose(_ notification: Notification) {
        central?.stopScan()
        if !didComplete {
            NSApp.terminate(nil)
        }
    }

    private func buildContent() {
        guard let contentView = window?.contentView else { return }

        let root = NSStackView()
        root.orientation = .horizontal
        root.spacing = 0
        root.translatesAutoresizingMaskIntoConstraints = false
        contentView.addSubview(root)

        stepList.orientation = .vertical
        stepList.alignment = .leading
        stepList.spacing = 8
        stepList.edgeInsets = NSEdgeInsets(top: 24, left: 20, bottom: 24, right: 20)
        stepList.wantsLayer = true
        stepList.layer?.backgroundColor = NSColor.controlBackgroundColor.cgColor
        stepList.widthAnchor.constraint(equalToConstant: 170).isActive = true
        root.addArrangedSubview(stepList)

        let main = NSStackView()
        main.orientation = .vertical
        main.alignment = .leading
        main.spacing = 16
        main.translatesAutoresizingMaskIntoConstraints = false
        root.addArrangedSubview(main)

        titleLabel.font = .systemFont(ofSize: 22, weight: .semibold)
        detailLabel.textColor = .secondaryLabelColor
        detailLabel.maximumNumberOfLines = 2
        main.addArrangedSubview(titleLabel)
        main.addArrangedSubview(detailLabel)

        contentStack.orientation = .vertical
        contentStack.alignment = .leading
        contentStack.spacing = 12
        main.addArrangedSubview(contentStack)
        contentStack.widthAnchor.constraint(equalTo: main.widthAnchor).isActive = true

        let footer = NSStackView()
        footer.orientation = .horizontal
        footer.alignment = .centerY
        footer.spacing = 10
        let spacer = NSView()
        spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
        statusLabel.textColor = .secondaryLabelColor
        backButton.target = self
        backButton.action = #selector(goBack)
        nextButton.target = self
        nextButton.action = #selector(goNext)
        nextButton.keyEquivalent = "\r"
        footer.addArrangedSubview(statusLabel)
        footer.addArrangedSubview(spacer)
        footer.addArrangedSubview(backButton)
        footer.addArrangedSubview(nextButton)
        main.addArrangedSubview(footer)
        footer.widthAnchor.constraint(equalTo: main.widthAnchor).isActive = true

        NSLayoutConstraint.activate([
            root.leadingAnchor.constraint(equalTo: contentView.leadingAnchor),
            root.trailingAnchor.constraint(equalTo: contentView.trailingAnchor),
            root.topAnchor.constraint(equalTo: contentView.topAnchor),
            root.bottomAnchor.constraint(equalTo: contentView.bottomAnchor),
            main.topAnchor.constraint(equalTo: contentView.topAnchor, constant: 26),
            main.bottomAnchor.constraint(equalTo: contentView.bottomAnchor, constant: -24),
            main.trailingAnchor.constraint(equalTo: contentView.trailingAnchor, constant: -28)
        ])
    }

    private func loadConfigIntoFields() {
        providerPopup.addItems(withTitles: [
            ASRProvider.voiceStickCloud.displayName,
            ASRProvider.volcengine.displayName
        ])
        providerPopup.target = self
        providerPopup.action = #selector(providerSelectionChanged)
        providerPopup.selectItem(withTitle: config.asrProvider.displayName)

        resourcePopup.addItems(withTitles: AppConfig.supportedResourceIDs)
        resourcePopup.selectItem(withTitle: config.resourceID)
        apiKeyField.stringValue = apiKey(for: config.asrProvider)
    }

    private func renderStep() {
        renderStepList()
        contentStack.arrangedSubviews.forEach { view in
            contentStack.removeArrangedSubview(view)
            view.removeFromSuperview()
        }

        switch currentStep {
        case .device:
            titleLabel.stringValue = "Pair your VoiceStick"
            detailLabel.stringValue = "Choose a nearby VS-XXXX device. VoiceStick needs a paired device before the app can listen."
            contentStack.addArrangedSubview(deviceView())
        case .provider:
            titleLabel.stringValue = "Choose your speech provider"
            detailLabel.stringValue = "Pick the ASR provider and enter the key or endpoint settings it needs."
            contentStack.addArrangedSubview(providerView())
        case .accessibility:
            titleLabel.stringValue = "Allow text insertion"
            detailLabel.stringValue = "VoiceStick pastes recognized text at your cursor, so macOS Accessibility permission is required."
            contentStack.addArrangedSubview(accessibilityView())
            updateAccessibilityStatus()
        case .finish:
            titleLabel.stringValue = "VoiceStick is ready"
            detailLabel.stringValue = "The device and ASR settings are configured. Finish setup to start scanning and connecting."
            contentStack.addArrangedSubview(finishView())
        }

        backButton.isEnabled = currentStep.rawValue > 0
        nextButton.title = currentStep == .finish ? "Finish" : "Continue"
        updateNextButton()
    }

    private func renderStepList() {
        stepList.arrangedSubviews.forEach { view in
            stepList.removeArrangedSubview(view)
            view.removeFromSuperview()
        }

        for step in Step.allCases {
            let label = NSTextField(labelWithString: "\(step.rawValue + 1). \(step.title)")
            label.font = .systemFont(ofSize: 13, weight: step == currentStep ? .semibold : .regular)
            label.textColor = step.rawValue <= currentStep.rawValue ? .labelColor : .secondaryLabelColor
            stepList.addArrangedSubview(label)
        }
    }

    private func deviceView() -> NSView {
        let stack = NSStackView()
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 10

        let scrollView = NSScrollView()
        scrollView.hasVerticalScroller = true
        scrollView.documentView = tableView
        scrollView.borderType = .bezelBorder

        if tableView.tableColumns.isEmpty {
            tableView.addTableColumn(column(id: "name", title: "Device", width: 210))
            tableView.addTableColumn(column(id: "id", title: "ID", width: 90))
            tableView.addTableColumn(column(id: "rssi", title: "RSSI", width: 70))
            tableView.delegate = self
            tableView.dataSource = self
            tableView.target = self
            tableView.doubleAction = #selector(pairSelectedDevice)
        }

        stack.addArrangedSubview(scrollView)
        stack.addArrangedSubview(scanStatusLabel)
        scrollView.widthAnchor.constraint(equalToConstant: 440).isActive = true
        scrollView.heightAnchor.constraint(equalToConstant: 220).isActive = true
        return stack
    }

    private func providerView() -> NSView {
        let stack = NSStackView()
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 12
        stack.addArrangedSubview(row(label: "Provider", control: providerPopup))
        stack.addArrangedSubview(row(label: "API Key", control: apiKeyControl()))
        if selectedProvider() == .volcengine {
            stack.addArrangedSubview(row(label: "Resource ID", control: resourcePopup))
        }
        updateApplyTrialButton()
        return stack
    }

    private func apiKeyControl() -> NSStackView {
        let stack = NSStackView()
        stack.orientation = .horizontal
        stack.alignment = .centerY
        stack.spacing = 8
        if !didConfigureAPIKeyControlConstraints {
            apiKeyField.widthAnchor.constraint(greaterThanOrEqualToConstant: 190).isActive = true
            applyTrialAPIKeyButton.widthAnchor.constraint(equalToConstant: 102).isActive = true
            didConfigureAPIKeyControlConstraints = true
        }
        stack.addArrangedSubview(apiKeyField)
        stack.addArrangedSubview(applyTrialAPIKeyButton)
        return stack
    }

    private func accessibilityView() -> NSView {
        let stack = NSStackView()
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 12
        stack.addArrangedSubview(accessibilityStatusLabel)
        stack.addArrangedSubview(accessibilitySettingsButton)
        return stack
    }

    private func finishView() -> NSView {
        let stack = NSStackView()
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 8
        stack.addArrangedSubview(summaryLine("Device", value: config.pairedDeviceIDs.first.map { "VS-\($0)" } ?? "Not paired"))
        stack.addArrangedSubview(summaryLine("Provider", value: selectedProvider().displayName))
        stack.addArrangedSubview(summaryLine("Accessibility", value: AXIsProcessTrusted() ? "Allowed" : "Not allowed yet"))
        return stack
    }

    private func summaryLine(_ title: String, value: String) -> NSTextField {
        let label = NSTextField(labelWithString: "\(title): \(value)")
        label.font = .systemFont(ofSize: 13)
        return label
    }

    private func column(id: String, title: String, width: CGFloat) -> NSTableColumn {
        let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id))
        column.title = title
        column.width = width
        return column
    }

    private func row(label: String, control: NSView) -> NSStackView {
        let row = NSStackView()
        row.orientation = .horizontal
        row.alignment = .centerY
        row.spacing = 12
        let labelView = NSTextField(labelWithString: label)
        labelView.alignment = .right
        labelView.textColor = .secondaryLabelColor
        labelView.widthAnchor.constraint(equalToConstant: 100).isActive = true
        control.widthAnchor.constraint(equalToConstant: 300).isActive = true
        row.addArrangedSubview(labelView)
        row.addArrangedSubview(control)
        return row
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        guard central.state == .poweredOn else {
            scanStatusLabel.stringValue = "Bluetooth unavailable"
            return
        }
        scanStatusLabel.stringValue = "Scanning"
        central.scanForPeripherals(withServices: [CBUUID(string: BleProtocol.serviceUUID)])
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let selectedIdentifier = selectedDeviceIdentifier
        let name = (advertisementData[CBAdvertisementDataLocalNameKey] as? String)
            ?? peripheral.name
            ?? ""
        guard let deviceID = BleCentral.deviceID(from: name) else { return }

        let device = OnboardingDevice(
            identifier: peripheral.identifier,
            name: name,
            deviceID: deviceID,
            rssi: RSSI.intValue
        )

        if let index = devices.firstIndex(where: { $0.identifier == peripheral.identifier }) {
            devices[index] = device
        } else {
            devices.append(device)
        }
        tableView.reloadData()
        restoreSelection(selectedIdentifier)
        scanStatusLabel.stringValue = devices.isEmpty ? "Scanning" : "\(devices.count) found"
    }

    func numberOfRows(in tableView: NSTableView) -> Int {
        devices.count
    }

    func tableViewSelectionDidChange(_ notification: Notification) {
        selectCurrentDevice()
    }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        guard row < devices.count, let tableColumn else { return nil }
        let device = devices[row]
        let value: String
        switch tableColumn.identifier.rawValue {
        case "name":
            value = device.name
        case "id":
            value = device.deviceID
        case "rssi":
            value = "\(device.rssi)"
        default:
            value = ""
        }
        return NSTextField(labelWithString: value)
    }

    @objc private func pairSelectedDevice() {
        guard selectCurrentDevice() else { return }
        goNext()
    }

    @discardableResult
    private func selectCurrentDevice() -> Bool {
        let row = tableView.selectedRow
        guard row >= 0, row < devices.count else {
            scanStatusLabel.stringValue = "Select a device"
            updateNextButton()
            return false
        }
        let deviceID = devices[row].deviceID
        config.pairedDeviceIDs = [deviceID]
        scanStatusLabel.stringValue = "Selected VS-\(deviceID)"
        updateNextButton()
        return true
    }

    @objc private func providerSelectionChanged() {
        saveDisplayedProviderFields()
        currentDisplayedProvider = selectedProvider()
        config.asrProvider = currentDisplayedProvider
        apiKeyField.stringValue = apiKey(for: currentDisplayedProvider)
        renderStep()
    }

    @objc private func apiKeyFieldDidChange() {
        updateApplyTrialButton()
    }

    @objc private func applyTrialAPIKey() {
        saveDisplayedProviderFields()
        guard currentDisplayedProvider == .voiceStickCloud else { return }

        applyTrialAPIKeyButton.isEnabled = false
        statusLabel.stringValue = "Applying trial API key..."
        VoiceStickCloudAPI.applyTrialAPIKey(
            cloudURL: config.voiceStickCloudURL,
            deviceID: config.pairedDeviceIDs.first
        ) { [weak self] result in
            DispatchQueue.main.async {
                guard let self else { return }
                self.applyTrialAPIKeyButton.isEnabled = true
                switch result {
                case .success(.apiKey(let apiKey)):
                    self.config.voiceStickAPIKey = apiKey
                    self.apiKeyField.stringValue = apiKey
                    self.statusLabel.stringValue = "Trial API key applied."
                    self.updateApplyTrialButton()
                    self.updateNextButton()
                case .success(.url(let url)):
                    self.statusLabel.stringValue = "Opened trial application page."
                    NSWorkspace.shared.open(url)
                case .failure(let error):
                    self.statusLabel.stringValue = "Apply failed: \(error.localizedDescription)"
                    self.updateApplyTrialButton()
                }
            }
        }
    }

    @objc private func requestAccessibilityPermission() {
        openAccessibilitySettings()
        updateAccessibilityStatus()
    }

    @objc private func applicationDidBecomeActive() {
        guard currentStep == .accessibility else { return }
        updateAccessibilityStatus()
    }

    private func openAccessibilitySettings() {
        let appPaths = [
            "/System/Applications/System Settings.app",
            "/System/Applications/System Preferences.app"
        ]
        for path in appPaths {
            let url = URL(fileURLWithPath: path)
            guard FileManager.default.fileExists(atPath: url.path) else { continue }
            NSWorkspace.shared.openApplication(at: url, configuration: NSWorkspace.OpenConfiguration()) { [weak self] _, error in
                DispatchQueue.main.async {
                    if error == nil {
                        self?.statusLabel.stringValue = "Opened System Settings."
                        self?.openAccessibilityPaneURL()
                    } else {
                        self?.openAccessibilityPaneURL()
                    }
                }
            }
            return
        }

        openAccessibilityPaneURL()
    }

    private func openAccessibilityPaneURL() {
        let urls = [
            "x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?Privacy_Accessibility",
            "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility",
            "x-apple.systempreferences:com.apple.preference.universalaccess"
        ]

        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
            for text in urls {
                guard let url = URL(string: text) else { continue }
                if NSWorkspace.shared.open(url) {
                    self.statusLabel.stringValue = "Opened System Settings."
                    return
                }
            }
            self.statusLabel.stringValue = "Open System Settings, then go to Privacy & Security > Accessibility."
        }
    }

    @objc private func updateAccessibilityStatus() {
        let isTrusted = AXIsProcessTrusted()
        accessibilityStatusLabel.stringValue = isTrusted
            ? "Accessibility permission is allowed."
            : "Accessibility permission is not allowed yet."
        accessibilitySettingsButton.isHidden = isTrusted
        if isTrusted {
            statusLabel.stringValue = ""
        }
        updateNextButton()
    }

    @objc private func goBack() {
        guard let step = Step(rawValue: currentStep.rawValue - 1) else { return }
        saveDisplayedProviderFields()
        currentStep = step
        renderStep()
    }

    @objc private func goNext() {
        statusLabel.stringValue = ""
        saveDisplayedProviderFields()

        if currentStep == .finish {
            do {
                try config.save()
                didComplete = true
                central?.stopScan()
                close()
                onComplete(config)
            } catch {
                statusLabel.stringValue = "Save failed: \(error.localizedDescription)"
            }
            return
        }

        guard validateCurrentStep() else { return }
        guard let step = Step(rawValue: currentStep.rawValue + 1) else { return }
        currentStep = step
        renderStep()
    }

    private func validateCurrentStep() -> Bool {
        switch currentStep {
        case .device:
            if config.pairedDeviceIDs.isEmpty {
                statusLabel.stringValue = "Select a VoiceStick device first."
                return false
            }
        case .provider:
            if activeAPIKey().isEmpty {
                statusLabel.stringValue = "Enter the API key for \(selectedProvider().displayName)."
                return false
            }
            if selectedProvider() == .voiceStickCloud,
               URL(string: config.voiceStickCloudURL.trimmingCharacters(in: .whitespacesAndNewlines)) == nil {
                statusLabel.stringValue = "Enter a valid Cloud URL."
                return false
            }
        case .accessibility:
            if !AXIsProcessTrusted() {
                statusLabel.stringValue = "Allow Accessibility permission before continuing."
                updateNextButton()
                return false
            }
        case .finish:
            break
        }
        return true
    }

    private func updateNextButton() {
        switch currentStep {
        case .device:
            nextButton.isEnabled = !config.pairedDeviceIDs.isEmpty
        case .accessibility:
            nextButton.isEnabled = AXIsProcessTrusted()
        default:
            nextButton.isEnabled = true
        }
    }

    private var selectedDeviceIdentifier: UUID? {
        let row = tableView.selectedRow
        guard row >= 0, row < devices.count else { return nil }
        return devices[row].identifier
    }

    private func restoreSelection(_ identifier: UUID?) {
        guard let identifier,
              let row = devices.firstIndex(where: { $0.identifier == identifier }) else {
            return
        }
        tableView.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
    }

    private func selectedProvider() -> ASRProvider {
        switch providerPopup.titleOfSelectedItem {
        case ASRProvider.voiceStickCloud.displayName:
            return .voiceStickCloud
        case ASRProvider.volcengine.displayName:
            return .volcengine
        default:
            return config.asrProvider
        }
    }

    private func apiKey(for provider: ASRProvider) -> String {
        switch provider {
        case .voiceStickCloud:
            return config.voiceStickAPIKey
        case .volcengine:
            return config.volcengineAPIKey
        }
    }

    private func activeAPIKey() -> String {
        switch selectedProvider() {
        case .voiceStickCloud:
            return config.voiceStickAPIKey.trimmingCharacters(in: .whitespacesAndNewlines)
        case .volcengine:
            return config.volcengineAPIKey.trimmingCharacters(in: .whitespacesAndNewlines)
        }
    }

    private func saveDisplayedProviderFields() {
        let key = apiKeyField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        switch currentDisplayedProvider {
        case .voiceStickCloud:
            config.voiceStickAPIKey = key
        case .volcengine:
            config.volcengineAPIKey = key
        }
        config.asrProvider = selectedProvider()
        config.resourceID = resourcePopup.titleOfSelectedItem ?? config.resourceID
    }

    private func updateApplyTrialButton() {
        let isCloud = selectedProvider() == .voiceStickCloud
        let isEmpty = apiKeyField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        applyTrialAPIKeyButton.isHidden = !(isCloud && isEmpty)
    }
}
