import AppKit

final class PairDeviceWindowController: NSWindowController, NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate {
    private let tableView = NSTableView()
    private let statusLabel = NSTextField(labelWithString: "Scanning")
    private let existingDeviceIDs: Set<String>
    private let onPair: (String) -> Void
    private let scanner = BlePairingScanner()
    private var devices: [PairingDevice] = []

    init(existingDeviceIDs: [String], onPair: @escaping (String) -> Void) {
        self.existingDeviceIDs = Set(existingDeviceIDs)
        self.onPair = onPair

        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 420, height: 280),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false
        )
        window.title = "Pair VoiceStick"
        window.isReleasedWhenClosed = false
        super.init(window: window)
        window.delegate = self
        scanner.onDevice = { [weak self] in self?.updateDevice($0) }
        scanner.onStatus = { [weak self] in self?.statusLabel.stringValue = $0 }
        buildContent()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func show() {
        showWindow(nil)
        window?.center()
        NSApp.activate(ignoringOtherApps: true)
        scanner.start()
    }

    func windowWillClose(_ notification: Notification) {
        scanner.stop()
    }

    private func buildContent() {
        guard let contentView = window?.contentView else { return }

        let stack = NSStackView()
        stack.orientation = .vertical
        stack.spacing = 12
        stack.translatesAutoresizingMaskIntoConstraints = false
        contentView.addSubview(stack)

        let scrollView = NSScrollView()
        scrollView.hasVerticalScroller = true
        scrollView.documentView = tableView

        tableView.addTableColumn(column(id: "name", title: "Device", width: 170))
        tableView.addTableColumn(column(id: "id", title: "ID", width: 90))
        tableView.addTableColumn(column(id: "rssi", title: "RSSI", width: 70))
        tableView.delegate = self
        tableView.dataSource = self
        tableView.target = self
        tableView.doubleAction = #selector(pairSelectedDevice)

        let buttonRow = NSStackView()
        buttonRow.orientation = .horizontal
        buttonRow.alignment = .centerY
        buttonRow.spacing = 8

        let pairButton = NSButton(title: "Pair", target: self, action: #selector(pairSelectedDevice))
        let cancelButton = NSButton(title: "Cancel", target: self, action: #selector(cancel))
        buttonRow.addArrangedSubview(statusLabel)
        buttonRow.addArrangedSubview(NSView())
        buttonRow.addArrangedSubview(pairButton)
        buttonRow.addArrangedSubview(cancelButton)

        stack.addArrangedSubview(scrollView)
        stack.addArrangedSubview(buttonRow)

        scrollView.heightAnchor.constraint(equalToConstant: 200).isActive = true
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: contentView.leadingAnchor, constant: 16),
            stack.trailingAnchor.constraint(equalTo: contentView.trailingAnchor, constant: -16),
            stack.topAnchor.constraint(equalTo: contentView.topAnchor, constant: 16),
            stack.bottomAnchor.constraint(equalTo: contentView.bottomAnchor, constant: -16)
        ])
    }

    private func column(id: String, title: String, width: CGFloat) -> NSTableColumn {
        let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id))
        column.title = title
        column.width = width
        return column
    }

    private func updateDevice(_ device: PairingDevice) {
        let selectedIdentifier = selectedDeviceIdentifier
        if let index = devices.firstIndex(where: { $0.identifier == device.identifier }) {
            devices[index] = device
        } else {
            devices.append(device)
        }
        tableView.reloadData()
        restoreSelection(selectedIdentifier)
        statusLabel.stringValue = "\(devices.count) found"
    }

    func numberOfRows(in tableView: NSTableView) -> Int {
        devices.count
    }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        guard row < devices.count, let tableColumn else { return nil }
        let device = devices[row]
        let value: String
        switch tableColumn.identifier.rawValue {
        case "name":
            value = existingDeviceIDs.contains(device.deviceID) ? "\(device.name) (paired)" : device.name
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
        let row = tableView.selectedRow
        guard row >= 0, row < devices.count else {
            statusLabel.stringValue = "Select a device"
            return
        }
        scanner.pair(devices[row]) { [weak self] deviceID in
            self?.onPair(deviceID)
            self?.close()
        }
    }

    @objc private func cancel() {
        close()
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
}
