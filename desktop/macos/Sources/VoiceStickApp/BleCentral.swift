import AppKit
import CoreBluetooth
import Foundation

struct ConnectedVoiceStickDevice {
    let name: String
    let deviceID: String
}

struct FirmwareUpdateProgress {
    let writtenBytes: Int
    let totalBytes: Int
    let isDeviceConfirmed: Bool

    var fraction: Double {
        guard totalBytes > 0 else { return 0 }
        return Double(writtenBytes) / Double(totalBytes)
    }
}

final class BleCentral: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    enum FirmwareUpdateError: LocalizedError {
        case noConnectedDevice
        case otaCharacteristicUnavailable
        case imageTooLarge
        case transferAlreadyActive
        case firmwareUpdateCancelled
        case peripheralWriteFailed(String)
        case deviceError(String)

        var errorDescription: String? {
            switch self {
            case .noConnectedDevice:
                return "No VoiceStick is connected."
            case .otaCharacteristicUnavailable:
                return "The connected firmware does not expose BLE OTA."
            case .imageTooLarge:
                return "Firmware image is larger than the OTA partition."
            case .transferAlreadyActive:
                return "A firmware update is already running."
            case .firmwareUpdateCancelled:
                return "Firmware update cancelled."
            case .peripheralWriteFailed(let message):
                return "BLE write failed: \(message)"
            case .deviceError(let code):
                return "Device rejected OTA: \(code)"
            }
        }
    }

    private struct FirmwareUpdateSession {
        let peripheralID: UUID
        let transferID: UInt32
        let image: Data
        let chunkSize: Int
        var began = false
        var offset = 0
        var lastQueuedProgressOffset = 0
        var ended = false
        let progress: (FirmwareUpdateProgress) -> Void
        let completion: (Result<Void, Error>) -> Void
    }

    private var pairedDeviceIDs: Set<String>
    private var central: CBCentralManager!
    private var peripherals: [UUID: CBPeripheral] = [:]
    private var discoveredDevices: [UUID: ConnectedVoiceStickDevice] = [:]
    private var connectedDevices: [UUID: ConnectedVoiceStickDevice] = [:]
    private var controlCharacteristics: [UUID: CBCharacteristic] = [:]
    private var otaCharacteristics: [UUID: CBCharacteristic] = [:]
    private var firmwareUpdateSession: FirmwareUpdateSession?
    private var interactionMode: InteractionMode = .holdToTalk
    private var isWorkspaceSleeping = false

    var onConnectionChange: (([ConnectedVoiceStickDevice]) -> Void)?
    var onAudioFrame: ((UUID, AudioFrame) -> Void)?
    var onStateEvent: ((UUID, StateEvent) -> Void)?

    init(pairedDeviceIDs: [String]) {
        self.pairedDeviceIDs = Set(pairedDeviceIDs)
        super.init()
    }

    deinit {
        NSWorkspace.shared.notificationCenter.removeObserver(self)
    }

    func start() {
        central = CBCentralManager(delegate: self, queue: .main)
        NSWorkspace.shared.notificationCenter.addObserver(
            self,
            selector: #selector(workspaceWillSleep),
            name: NSWorkspace.willSleepNotification,
            object: nil
        )
        NSWorkspace.shared.notificationCenter.addObserver(
            self,
            selector: #selector(workspaceDidWake),
            name: NSWorkspace.didWakeNotification,
            object: nil
        )
    }

    func updatePairedDeviceIDs(_ deviceIDs: [String]) {
        pairedDeviceIDs = Set(deviceIDs)
        for peripheral in peripherals.values {
            central.cancelPeripheralConnection(peripheral)
        }
        peripherals.removeAll()
        discoveredDevices.removeAll()
        connectedDevices.removeAll()
        controlCharacteristics.removeAll()
        otaCharacteristics.removeAll()
        failFirmwareUpdate(FirmwareUpdateError.noConnectedDevice)
        onConnectionChange?([])
        scanIfReady()
    }

    func sendUIState(_ state: String, text: String = "", to peripheralID: UUID? = nil) {
        let data = BleProtocol.uiStatePayload(state: state, text: text)
        if let peripheralID {
            if let characteristic = controlCharacteristics[peripheralID] {
                if let peripheral = peripherals[peripheralID] {
                    let deviceID = connectedDevices[peripheralID]?.deviceID ?? "unknown"
                    NSLog("BLE send ui_state state=\(state) dev=VS-\(deviceID) text_len=\(text.count)")
                    peripheral.writeValue(data, for: characteristic, type: .withoutResponse)
                } else {
                    NSLog("BLE send ui_state skipped missing peripheral state=\(state) id=\(peripheralID) text_len=\(text.count)")
                }
            } else {
                NSLog("BLE send ui_state skipped missing characteristic state=\(state) id=\(peripheralID) text_len=\(text.count)")
            }
            return
        }

        NSLog("BLE send ui_state broadcast state=\(state) targets=\(controlCharacteristics.count) text_len=\(text.count)")
        for (id, characteristic) in controlCharacteristics {
            if let peripheral = peripherals[id] {
                let deviceID = connectedDevices[id]?.deviceID ?? "unknown"
                NSLog("BLE send ui_state state=\(state) dev=VS-\(deviceID) text_len=\(text.count)")
                peripheral.writeValue(data, for: characteristic, type: .withoutResponse)
            } else {
                NSLog("BLE send ui_state skipped missing peripheral state=\(state) id=\(id) text_len=\(text.count)")
            }
        }
    }

    func sendInteractionMode(_ mode: InteractionMode, to peripheralID: UUID? = nil) {
        interactionMode = mode
        let data = BleProtocol.interactionModePayload(mode)
        if let peripheralID {
            if let characteristic = controlCharacteristics[peripheralID] {
                peripherals[peripheralID]?.writeValue(data, for: characteristic, type: .withoutResponse)
            }
            return
        }

        for (id, characteristic) in controlCharacteristics {
            peripherals[id]?.writeValue(data, for: characteristic, type: .withoutResponse)
        }
    }


    func updateFirmware(image: Data, for deviceID: String,
                        progress: @escaping (FirmwareUpdateProgress) -> Void,
                        completion: @escaping (Result<Void, Error>) -> Void) {
        guard firmwareUpdateSession == nil else {
            completion(.failure(FirmwareUpdateError.transferAlreadyActive))
            return
        }
        guard let peripheralID = connectedDevices.first(where: { $0.value.deviceID == deviceID })?.key,
              let peripheral = peripherals[peripheralID] else {
            completion(.failure(FirmwareUpdateError.noConnectedDevice))
            return
        }
        guard otaCharacteristics[peripheralID] != nil else {
            completion(.failure(FirmwareUpdateError.otaCharacteristicUnavailable))
            return
        }
        guard image.count <= 3 * 1024 * 1024 else {
            completion(.failure(FirmwareUpdateError.imageTooLarge))
            return
        }

        let maxWrite = peripheral.maximumWriteValueLength(for: .withoutResponse)
        let chunkSize = max(20, min(maxWrite - 12, 244))
        firmwareUpdateSession = FirmwareUpdateSession(
            peripheralID: peripheralID,
            transferID: UInt32.random(in: 1...UInt32.max),
            image: image,
            chunkSize: chunkSize,
            progress: progress,
            completion: completion
        )
        progress(FirmwareUpdateProgress(
            writtenBytes: 0,
            totalBytes: image.count,
            isDeviceConfirmed: true
        ))
        sendNextFirmwareUpdateFrame()
    }

    func cancelFirmwareUpdate() {
        failFirmwareUpdate(FirmwareUpdateError.firmwareUpdateCancelled)
    }

    func deviceID(for peripheralID: UUID) -> String? {
        connectedDevices[peripheralID]?.deviceID ?? discoveredDevices[peripheralID]?.deviceID
    }

    func isConnected(_ peripheralID: UUID) -> Bool {
        connectedDevices[peripheralID] != nil
    }

    func isConnected(deviceID: String) -> Bool {
        connectedDevices.values.contains { $0.deviceID == deviceID }
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            if !isWorkspaceSleeping {
                restoreConnectedPeripherals()
            }
            scanIfReady()
        case .unknown, .resetting, .unsupported, .unauthorized, .poweredOff:
            clearConnectionState()
        @unknown default:
            clearConnectionState()
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let localName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        guard !isWorkspaceSleeping else { return }
        guard shouldConnect(localName: localName, peripheralName: peripheral.name) else { return }
        if let existingPeripheral = peripherals[peripheral.identifier] {
            if existingPeripheral.state == .disconnected {
                removePeripheral(existingPeripheral)
            } else {
                return
            }
        }
        if let device = connectedDevice(localName: localName, peripheralName: peripheral.name) {
            discoveredDevices[peripheral.identifier] = device
        }
        peripherals[peripheral.identifier] = peripheral
        peripheral.delegate = self
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        connectedDevices[peripheral.identifier] = discoveredDevices[peripheral.identifier]
            ?? connectedDevice(localName: nil, peripheralName: peripheral.name)
        onConnectionChange?(currentConnectedDevices)
        peripheral.discoverServices([CBUUID(string: BleProtocol.serviceUUID)])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        removePeripheral(peripheral)
        onConnectionChange?(currentConnectedDevices)
        guard !isWorkspaceSleeping else { return }
        scanIfReady()
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        removePeripheral(peripheral)
        onConnectionChange?(currentConnectedDevices)
        guard !isWorkspaceSleeping else { return }
        scanIfReady()
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        peripheral.services?.forEach {
            peripheral.discoverCharacteristics(nil, for: $0)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        service.characteristics?.forEach { characteristic in
            switch characteristic.uuid.uuidString.uppercased() {
            case BleProtocol.audioUUID:
                peripheral.setNotifyValue(true, for: characteristic)
            case BleProtocol.stateUUID:
                peripheral.setNotifyValue(true, for: characteristic)
            case BleProtocol.controlUUID:
                controlCharacteristics[peripheral.identifier] = characteristic
                sendUIState("ready", to: peripheral.identifier)
                sendInteractionMode(interactionMode, to: peripheral.identifier)
            case BleProtocol.otaRXUUID:
                otaCharacteristics[peripheral.identifier] = characteristic
            case BleProtocol.otaStateUUID:
                peripheral.setNotifyValue(true, for: characteristic)
            default:
                break
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard let data = characteristic.value else { return }
        switch characteristic.uuid.uuidString.uppercased() {
        case BleProtocol.audioUUID:
            if let frame = BleProtocol.parseAudioFrame(data) {
                onAudioFrame?(peripheral.identifier, frame)
            }
        case BleProtocol.stateUUID:
            if let event = BleProtocol.parseStateEvent(data) {
                onStateEvent?(peripheral.identifier, event)
            }
        case BleProtocol.otaStateUUID:
            if let event = BleProtocol.parseFirmwareOTAStateEvent(data) {
                handleFirmwareUpdateStateEvent(event)
            }
        default:
            break
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid.uuidString.uppercased() == BleProtocol.otaRXUUID,
              firmwareUpdateSession?.peripheralID == peripheral.identifier else {
            return
        }
        if let error {
            failFirmwareUpdate(FirmwareUpdateError.peripheralWriteFailed(error.localizedDescription))
            return
        }
        sendNextFirmwareUpdateFrame()
    }

    func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        guard firmwareUpdateSession?.peripheralID == peripheral.identifier else {
            return
        }
        sendNextFirmwareUpdateFrame()
    }

    private func shouldConnect(localName: String?, peripheralName: String?) -> Bool {
        let advertisedName = localName ?? peripheralName ?? ""
        if !pairedDeviceIDs.isEmpty {
            guard let deviceID = Self.deviceID(from: advertisedName) else { return false }
            return pairedDeviceIDs.contains(deviceID)
        }

        return false
    }

    private func connectedDevice(localName: String?, peripheralName: String?) -> ConnectedVoiceStickDevice? {
        let advertisedName = localName ?? peripheralName ?? ""
        guard let deviceID = Self.deviceID(from: advertisedName) else { return nil }
        return ConnectedVoiceStickDevice(
            name: advertisedName.isEmpty ? "VS-\(deviceID)" : advertisedName,
            deviceID: deviceID
        )
    }

    private var currentConnectedDevices: [ConnectedVoiceStickDevice] {
        connectedDevices.values.sorted { $0.name < $1.name }
    }

    private func sendNextFirmwareUpdateFrame() {
        guard var session = firmwareUpdateSession,
              let peripheral = peripherals[session.peripheralID],
              let characteristic = otaCharacteristics[session.peripheralID] else {
            failFirmwareUpdate(FirmwareUpdateError.otaCharacteristicUnavailable)
            return
        }

        if !session.began {
            let payload = BleProtocol.otaBeginPayload(
                imageSize: UInt32(session.image.count),
                transferID: session.transferID
            )
            session.began = true
            firmwareUpdateSession = session
            peripheral.writeValue(payload, for: characteristic, type: .withResponse)
        } else if session.offset < session.image.count {
            while session.offset < session.image.count && peripheral.canSendWriteWithoutResponse {
                let end = min(session.offset + session.chunkSize, session.image.count)
                let chunk = session.image.subdata(in: session.offset..<end)
                let payload = BleProtocol.otaDataPayload(
                    transferID: session.transferID,
                    offset: UInt32(session.offset),
                    chunk: chunk
                )
                peripheral.writeValue(payload, for: characteristic, type: .withoutResponse)
                session.offset = end
                if session.offset - session.lastQueuedProgressOffset >= 64 * 1024 ||
                    session.offset == session.image.count {
                    session.lastQueuedProgressOffset = session.offset
                    session.progress(FirmwareUpdateProgress(
                        writtenBytes: session.offset,
                        totalBytes: session.image.count,
                        isDeviceConfirmed: false
                    ))
                }
            }
            firmwareUpdateSession = session
            if session.offset == session.image.count {
                sendNextFirmwareUpdateFrame()
            }
        } else if !session.ended {
            let payload = BleProtocol.otaEndPayload(
                transferID: session.transferID,
                imageSize: UInt32(session.image.count)
            )
            session.ended = true
            firmwareUpdateSession = session
            peripheral.writeValue(payload, for: characteristic, type: .withResponse)
        } else {
            return
        }
    }

    private func handleFirmwareUpdateStateEvent(_ event: FirmwareOTAStateEvent) {
        guard let session = firmwareUpdateSession else { return }
        if let transferID = event.transferID, transferID != session.transferID {
            return
        }

        switch event.event {
        case "progress":
            if let written = event.written, let size = event.size, size > 0 {
                session.progress(FirmwareUpdateProgress(
                    writtenBytes: Int(written),
                    totalBytes: Int(size),
                    isDeviceConfirmed: true
                ))
            }
        case "done":
            firmwareUpdateSession = nil
            session.progress(FirmwareUpdateProgress(
                writtenBytes: session.image.count,
                totalBytes: session.image.count,
                isDeviceConfirmed: true
            ))
            session.completion(.success(()))
        case "error":
            failFirmwareUpdate(FirmwareUpdateError.deviceError(event.code ?? "unknown"))
        default:
            break
        }
    }

    private func failFirmwareUpdate(_ error: Error) {
        guard let session = firmwareUpdateSession else { return }
        if let peripheral = peripherals[session.peripheralID],
           let characteristic = otaCharacteristics[session.peripheralID] {
            let payload = BleProtocol.otaAbortPayload(transferID: session.transferID)
            peripheral.writeValue(payload, for: characteristic, type: .withoutResponse)
        }
        firmwareUpdateSession = nil
        session.completion(.failure(error))
    }

    private func scanIfReady() {
        guard let central, central.state == .poweredOn else { return }
        guard !isWorkspaceSleeping else {
            central.stopScan()
            return
        }
        if pairedDeviceIDs.isEmpty {
            central.stopScan()
        } else {
            central.scanForPeripherals(withServices: [CBUUID(string: BleProtocol.serviceUUID)])
        }
    }

    private func restoreConnectedPeripherals() {
        guard let central, central.state == .poweredOn, !isWorkspaceSleeping, !pairedDeviceIDs.isEmpty else { return }
        let serviceUUID = CBUUID(string: BleProtocol.serviceUUID)
        let restoredPeripherals = central.retrieveConnectedPeripherals(withServices: [serviceUUID])
        guard !restoredPeripherals.isEmpty else { return }

        var didRestore = false
        for peripheral in restoredPeripherals {
            guard let device = knownDevice(for: peripheral) else {
                continue
            }
            discoveredDevices[peripheral.identifier] = device
            connectedDevices[peripheral.identifier] = device
            peripherals[peripheral.identifier] = peripheral
            peripheral.delegate = self
            peripheral.discoverServices([serviceUUID])
            didRestore = true
        }
        if didRestore {
            onConnectionChange?(currentConnectedDevices)
        }
    }

    private func knownDevice(for peripheral: CBPeripheral) -> ConnectedVoiceStickDevice? {
        if let device = discoveredDevices[peripheral.identifier] {
            return device
        }
        if let device = connectedDevice(localName: nil, peripheralName: peripheral.name) {
            return device
        }
        guard pairedDeviceIDs.count == 1, let deviceID = pairedDeviceIDs.first else {
            return nil
        }
        return ConnectedVoiceStickDevice(
            name: peripheral.name ?? "VS-\(deviceID)",
            deviceID: deviceID
        )
    }

    private func clearConnectionState() {
        central?.stopScan()
        if firmwareUpdateSession != nil {
            failFirmwareUpdate(FirmwareUpdateError.noConnectedDevice)
        }
        peripherals.removeAll()
        discoveredDevices.removeAll()
        connectedDevices.removeAll()
        controlCharacteristics.removeAll()
        otaCharacteristics.removeAll()
        onConnectionChange?([])
    }

    private func removePeripheral(_ peripheral: CBPeripheral) {
        peripherals.removeValue(forKey: peripheral.identifier)
        discoveredDevices.removeValue(forKey: peripheral.identifier)
        connectedDevices.removeValue(forKey: peripheral.identifier)
        controlCharacteristics.removeValue(forKey: peripheral.identifier)
        otaCharacteristics.removeValue(forKey: peripheral.identifier)
        if firmwareUpdateSession?.peripheralID == peripheral.identifier {
            failFirmwareUpdate(FirmwareUpdateError.noConnectedDevice)
        }
    }

    @objc private func workspaceWillSleep() {
        isWorkspaceSleeping = true
        central?.stopScan()
        sendUIState("ready")
        for peripheral in peripherals.values where peripheral.state != .disconnected {
            central?.cancelPeripheralConnection(peripheral)
        }
        clearConnectionState()
    }

    @objc private func workspaceDidWake() {
        isWorkspaceSleeping = false
        restoreConnectedPeripherals()
        scanIfReady()
    }

    static func deviceID(from name: String) -> String? {
        let upper = name.uppercased()
        guard upper.hasPrefix("VS-") else { return nil }
        let id = String(upper.dropFirst(3).prefix(4))
        guard id.count == 4, id.allSatisfy(\.isHexDigit) else { return nil }
        return id
    }
}
