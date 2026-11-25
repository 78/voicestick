import AppKit
import CoreBluetooth
import Foundation

struct PairingDevice {
    let peripheral: CBPeripheral
    let name: String
    let deviceID: String
    let rssi: Int
    let requiresAuthorization: Bool

    var identifier: UUID { peripheral.identifier }
}

final class BlePairingScanner: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    static let openingHint = "Hold the device’s cancel button to pair"

    var onDevice: ((PairingDevice) -> Void)?
    var onStatus: ((String) -> Void)?
    /// The device lost its bond with this Mac. macOS refuses to pair again until
    /// the user forgets the device in Bluetooth settings.
    var onPeerRemovedPairing: ((PairingDevice) -> Void)?

    private var central: CBCentralManager?
    private var pending: PairingDevice?
    private var lastAttempt: PairingDevice?
    private var onPair: ((String) -> Void)?
    private var timeout: DispatchWorkItem?
    private var isActive = false

    func start() {
        isActive = true
        if let central {
            scan(central)
        } else {
            central = CBCentralManager(delegate: self, queue: .main)
        }
    }

    func stop() {
        isActive = false
        central?.stopScan()
        lastAttempt = nil
        _ = endAttempt()
    }

    func pair(_ device: PairingDevice, onPair: @escaping (String) -> Void) {
        guard pending == nil, let central, central.state == .poweredOn else { return }
        guard device.requiresAuthorization else {
            onPair(device.deviceID)
            return
        }

        pending = device
        lastAttempt = device
        self.onPair = onPair
        onStatus?("Confirm the Bluetooth prompt if shown")
        central.stopScan()
        device.peripheral.delegate = self
        central.connect(device.peripheral)

        let timeout = DispatchWorkItem { [weak self] in self?.fail() }
        self.timeout = timeout
        DispatchQueue.main.asyncAfter(deadline: .now() + 45, execute: timeout)
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            scan(central)
        } else {
            _ = endAttempt()
            onStatus?("Bluetooth unavailable")
        }
    }

    private func scan(_ central: CBCentralManager) {
        guard isActive, central.state == .poweredOn, pending == nil else { return }
        onStatus?(Self.openingHint)
        central.scanForPeripherals(withServices: [
            CBUUID(string: BleProtocol.serviceUUID),
            CBUUID(string: BleProtocol.pairingAdvertisementUUID),
        ])
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard isActive, pending == nil else { return }
        let name = (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? peripheral.name ?? ""
        guard let deviceID = BleCentral.deviceID(from: name) else { return }
        let services = advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? []
        onDevice?(PairingDevice(
            peripheral: peripheral, name: name, deviceID: deviceID, rssi: RSSI.intValue,
            requiresAuthorization: services.contains(CBUUID(string: BleProtocol.pairingAdvertisementUUID))
        ))
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        guard pending?.peripheral === peripheral else { return }
        peripheral.discoverServices([CBUUID(string: BleProtocol.serviceUUID)])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        connectionClosed(peripheral, error: error)
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        connectionClosed(peripheral, error: error)
    }

    private func connectionClosed(_ peripheral: CBPeripheral, error: Error?) {
        // The authorization read can fail before the disconnect reports why.
        if (error as? CBError)?.code == .peerRemovedPairingInformation,
           let device = lastAttempt, device.peripheral === peripheral {
            lastAttempt = nil
            _ = endAttempt()
            if let central { scan(central) }
            onStatus?("Forget \(device.name) in Bluetooth settings")
            onPeerRemovedPairing?(device)
            return
        }
        if pending?.peripheral === peripheral { fail() }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard pending?.peripheral === peripheral else { return }
        guard error == nil, let service = peripheral.services?.first else { fail(); return }
        peripheral.discoverCharacteristics([CBUUID(string: BleProtocol.authorizedUUID)], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard pending?.peripheral === peripheral else { return }
        guard error == nil,
              let authorization = service.characteristics?.first(where: { $0.uuid.uuidString == BleProtocol.authorizedUUID }) else {
            fail()
            return
        }
        peripheral.readValue(for: authorization)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard let device = pending, device.peripheral === peripheral,
              characteristic.uuid.uuidString == BleProtocol.authorizedUUID else { return }
        guard error == nil, characteristic.value == Data([1]) else { fail(); return }
        lastAttempt = nil
        let completion = endAttempt()
        completion?(device.deviceID)
    }

    private func fail() {
        guard pending != nil else { return }
        _ = endAttempt()
        if let central { scan(central) }
        onStatus?("Hold the cancel button and try again")
    }

    private func endAttempt() -> ((String) -> Void)? {
        timeout?.cancel()
        timeout = nil
        let completion = onPair
        onPair = nil
        if let peripheral = pending?.peripheral {
            pending = nil
            peripheral.delegate = nil
            central?.cancelPeripheralConnection(peripheral)
        }
        return completion
    }
}

enum PeerRemovedPairingAlert {
    static func show(for device: PairingDevice, in window: NSWindow?) {
        guard window?.attachedSheet == nil else { return }
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = "Forget \(device.name) on this Mac first"
        alert.informativeText = "\(device.name) no longer has the pairing key saved on this Mac, so macOS will not connect to it. "
            + "In Bluetooth settings, forget \(device.name), then hold the device’s cancel button and pair again."
        alert.addButton(withTitle: "Open Bluetooth Settings")
        alert.addButton(withTitle: "Cancel")

        let handle: (NSApplication.ModalResponse) -> Void = { response in
            if response == .alertFirstButtonReturn { openBluetoothSettings() }
        }
        if let window {
            alert.beginSheetModal(for: window, completionHandler: handle)
        } else {
            NSApp.activate(ignoringOtherApps: true)
            handle(alert.runModal())
        }
    }

    private static func openBluetoothSettings() {
        let urls = [
            "x-apple.systempreferences:com.apple.BluetoothSettings",
            "x-apple.systempreferences:com.apple.preferences.Bluetooth"
        ]
        for text in urls {
            if let url = URL(string: text), NSWorkspace.shared.open(url) { return }
        }
    }
}
