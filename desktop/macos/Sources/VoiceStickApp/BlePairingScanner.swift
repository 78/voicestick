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

    private var central: CBCentralManager?
    private var pending: PairingDevice?
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
        _ = endAttempt()
    }

    func pair(_ device: PairingDevice, onPair: @escaping (String) -> Void) {
        guard pending == nil, let central, central.state == .poweredOn else { return }
        guard device.requiresAuthorization else {
            onPair(device.deviceID)
            return
        }

        pending = device
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
        if pending?.peripheral === peripheral { fail() }
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
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
