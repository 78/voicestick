import AppKit
import CoreBluetooth
import Foundation
import MotionInput

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
        case otaWriteTooSmall
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
            case .otaWriteTooSmall:
                return "The Bluetooth connection cannot fit a firmware update packet."
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

    private struct PendingRPC {
        let timer: Timer
        let completion: (Result<([String: Any], Data), Error>) -> Void
    }

    private final class PeripheralSession {
        var streamTXSubscribed = false
        var controlTXSubscribed = false
        var handshakeStarted = false
        var initialized = false
        var info: DeviceInfoResult?
        var legacyInfo: LegacyDeviceInfo?
        var legacyReadySentAt: TimeInterval?

        var nextRpcID = 1
        var nextTxMessageID: UInt8 = 0
        var reassembler = ControlReassembler()
        var pendingRPCs: [Int: PendingRPC] = [:]

        var inputStreamID: UInt16?
        var lastInputSeq: UInt16?
        var buttonsMask: UInt32 = 0

        var audioInStreamID: UInt16?
        var audioInMode: InteractionMode?
        var audioInLocalDisplayFeedback = false
        var audioInInFlight = false

        var imuStreamID: UInt16?
        var imuIncludeGyro = false
        var imuRateHz = 0
        var imuInFlight = false

        var lastDisplayScene: String?
        var lastDisplayText: String?

        func cancelPendingRPCs() {
            for (_, pending) in pendingRPCs {
                pending.timer.invalidate()
                pending.completion(.failure(RpcError(code: "disconnected", message: "device disconnected")))
            }
            pendingRPCs.removeAll()
        }
    }

    private var pairedDeviceIDs: Set<String>
    private var central: CBCentralManager!
    private var peripherals: [UUID: CBPeripheral] = [:]
    private var discoveredDevices: [UUID: ConnectedVoiceStickDevice] = [:]
    private var connectedDevices: [UUID: ConnectedVoiceStickDevice] = [:]
    private var controlCharacteristics: [UUID: CBCharacteristic] = [:]
    private var pendingAuthorization: [UUID: [CBCharacteristic]] = [:]
    private var peerRemovedPairingIDs: Set<UUID> = []
    private var otaCharacteristics: [UUID: CBCharacteristic] = [:]
    private var sessions: [UUID: PeripheralSession] = [:]
    private var firmwareUpdateSession: FirmwareUpdateSession?
    private var pendingFirmwareStartWorkItem: DispatchWorkItem?
    private var isWorkspaceSleeping = false

    var onConnectionChange: (([ConnectedVoiceStickDevice]) -> Void)?
    var onDeviceInfo: ((UUID, DeviceInfoResult) -> Void)?
    var onLegacyDeviceInfo: ((UUID, LegacyDeviceInfo) -> Void)?
    var onDeviceReady: ((UUID, DeviceInfoResult) -> Void)?
    var onBatteryChanged: ((UUID, DeviceBatteryStatus) -> Void)?
    var onButtonInput: ((UUID, ButtonInput) -> Void)?
    var onInputGap: ((UUID) -> Void)?
    var onAudioFrame: ((UUID, AudioStreamFrame) -> Void)?
    var onImuFrame: ((UUID, ImuStreamFrame) -> Void)?
    var onStreamClosed: ((UUID, UInt16, String) -> Void)?

    var motionInputs: [MotionDevice] {
        sessions.compactMap { id, session in
            guard session.initialized, session.info?.streams?.imu?.supportsMotionGame == true,
                  let device = connectedDevices[id] else { return nil }
            return MotionDevice(id: id, name: device.name)
        }.sorted { ($0.name, $0.id.uuidString) < ($1.name, $1.id.uuidString) }
    }

    var probeDevices: Set<UUID> {
        Set(sessions.compactMap { id, session in
            guard session.initialized, session.info?.streams?.imu?.supportsSwingProbe == true,
                  connectedDevices[id] != nil else { return nil }
            return id
        })
    }

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
        let updatedIDs = Set(deviceIDs)
        pairedDeviceIDs = updatedIDs
        peerRemovedPairingIDs.removeAll()

        let removedPeripherals = peripherals.values.filter { peripheral in
            guard let deviceID = deviceID(for: peripheral.identifier) else { return true }
            return !updatedIDs.contains(deviceID)
        }
        let connectedCount = connectedDevices.count
        for peripheral in removedPeripherals {
            removePeripheral(peripheral)
            central?.cancelPeripheralConnection(peripheral)
        }
        if connectedDevices.count != connectedCount {
            onConnectionChange?(currentConnectedDevices)
        }

        central?.stopScan()
        scanIfReady()
    }

    // MARK: - Protocol v2 Control & Stream Operations

    func deviceInfo(for peripheralID: UUID) -> DeviceInfoResult? {
        sessions[peripheralID]?.info
    }

    func connectedReadyPeripheralIDs() -> [UUID] {
        sessions.compactMap { id, session in
            session.initialized && connectedDevices[id] != nil ? id : nil
        }
    }

    func supportsProtocolV2(_ peripheralID: UUID) -> Bool {
        sessions[peripheralID]?.initialized == true
    }

    func activeAudioInStreamID(for peripheralID: UUID) -> UInt16? {
        sessions[peripheralID]?.audioInStreamID
    }

    func activeImuStreamID(for peripheralID: UUID) -> UInt16? {
        sessions[peripheralID]?.imuStreamID
    }

    func sendRPC(
        method: String,
        params: [String: Any]? = nil,
        to peripheralID: UUID,
        timeout: TimeInterval = 3.0,
        completion: ((Result<([String: Any], Data), Error>) -> Void)? = nil
    ) {
        guard let session = sessions[peripheralID],
              let peripheral = peripherals[peripheralID],
              let control = controlCharacteristics[peripheralID] else {
            completion?(.failure(RpcError(code: "disconnected", message: "device unavailable")))
            return
        }

        let id = session.nextRpcID
        session.nextRpcID += 1
        var payload: [String: Any] = ["id": id, "method": method]
        if let params { payload["params"] = params }
        guard let jsonData = try? JSONSerialization.data(withJSONObject: payload) else {
            completion?(.failure(RpcError(code: "encode_failed", message: "invalid json")))
            return
        }

        let writeType: CBCharacteristicWriteType = control.properties.contains(.write) ? .withResponse : .withoutResponse
        let maxWrite = peripheral.maximumWriteValueLength(for: writeType)
        let frames = BleProtocol.encodeControlFrames(
            jsonData: jsonData,
            maximumWriteLength: maxWrite,
            messageID: &session.nextTxMessageID
        )
        guard !frames.isEmpty else {
            completion?(.failure(RpcError(code: "too_large", message: "control message too large")))
            return
        }

        if let completion {
            let timer = Timer.scheduledTimer(withTimeInterval: timeout, repeats: false) { [weak self, weak session] _ in
                guard let session, let pending = session.pendingRPCs.removeValue(forKey: id) else { return }
                let devID = self?.deviceID(for: peripheralID) ?? "unknown"
                NSLog("BLE RPC timeout method=\(method) id=\(id) dev=VS-\(devID)")
                pending.completion(.failure(RpcError(code: "timeout", message: "\(method) timed out")))
            }
            session.pendingRPCs[id] = PendingRPC(timer: timer, completion: completion)
        }

        for frame in frames {
            peripheral.writeValue(frame, for: control, type: writeType)
        }
    }

    func sendNotification(method: String, params: [String: Any]? = nil, to peripheralID: UUID) {
        guard let session = sessions[peripheralID],
              let peripheral = peripherals[peripheralID],
              let control = controlCharacteristics[peripheralID],
              peripheral.canSendWriteWithoutResponse else { return }

        var payload: [String: Any] = ["method": method]
        if let params { payload["params"] = params }
        guard let jsonData = try? JSONSerialization.data(withJSONObject: payload) else { return }

        let maxWrite = peripheral.maximumWriteValueLength(for: .withoutResponse)
        let frames = BleProtocol.encodeControlFrames(
            jsonData: jsonData,
            maximumWriteLength: maxWrite,
            messageID: &session.nextTxMessageID
        )
        for frame in frames {
            peripheral.writeValue(frame, for: control, type: .withoutResponse)
        }
    }

    func setDisplay(scene: String, text: String = "", to peripheralID: UUID? = nil) {
        let targets: [UUID] = peripheralID.map { [$0] } ?? Array(sessions.keys)
        for id in targets {
            guard let session = sessions[id], session.initialized, session.info?.display != nil else { continue }
            if session.lastDisplayScene == scene && session.lastDisplayText == text {
                continue
            }
            session.lastDisplayScene = scene
            session.lastDisplayText = text
            sendRPC(method: "display.set", params: ["scene": scene, "text": text], to: id) { [weak session] result in
                if case .failure = result,
                   session?.lastDisplayScene == scene,
                   session?.lastDisplayText == text {
                    session?.lastDisplayScene = nil
                    session?.lastDisplayText = nil
                }
            }
        }
    }

    func startAudioIn(mode: InteractionMode, localDisplayFeedback: Bool = true, to peripheralID: UUID) {
        guard let session = sessions[peripheralID], session.initialized else { return }
        if session.audioInStreamID != nil && session.audioInMode == mode {
            return
        }
        if session.audioInInFlight { return }
        if let existing = session.audioInStreamID {
            session.audioInInFlight = true
            session.audioInStreamID = nil
            session.audioInMode = nil
            sendRPC(method: "stream.stop", params: ["stream_id": Int(existing)], to: peripheralID) { [weak self] _ in
                guard let self, let session = self.sessions[peripheralID] else { return }
                session.audioInInFlight = false
                self.startAudioIn(mode: mode, localDisplayFeedback: localDisplayFeedback, to: peripheralID)
            }
            return
        }

        session.audioInInFlight = true
        let triggerMode = mode == .clickToTalk ? "button_toggle" : "button_hold"
        let params: [String: Any] = [
            "kind": "audio_in",
            "format": ["codec": "opus", "sample_rate": 16000, "channels": 1, "frame_ms": 60],
            "trigger": ["mode": triggerMode, "button": "primary"],
            "local_display_feedback": localDisplayFeedback
        ]
        sendRPC(method: "stream.start", params: params, to: peripheralID) { [weak self] result in
            guard let self, let session = self.sessions[peripheralID] else { return }
            session.audioInInFlight = false
            switch result {
            case .success(let (dict, _)):
                if let sid = dict["stream_id"] as? Int, sid > 0, sid <= Int(UInt16.max) {
                    session.audioInStreamID = UInt16(sid)
                    session.audioInMode = mode
                    session.audioInLocalDisplayFeedback = localDisplayFeedback
                }
            case .failure(let error):
                NSLog("BLE startAudioIn failed dev=VS-\(self.deviceID(for: peripheralID) ?? "unknown"): \(error.localizedDescription)")
            }
        }
    }

    func stopAudioIn(for peripheralID: UUID, completion: (() -> Void)? = nil) {
        guard let session = sessions[peripheralID] else {
            completion?()
            return
        }
        guard let existing = session.audioInStreamID else {
            completion?()
            return
        }
        session.audioInStreamID = nil
        session.audioInMode = nil
        sendRPC(method: "stream.stop", params: ["stream_id": Int(existing)], to: peripheralID) { _ in
            completion?()
        }
    }

    func startImu(
        includeGyro: Bool,
        rateHz: Int,
        leaseMs: Int = 3000,
        to peripheralID: UUID,
        completion: ((Bool) -> Void)? = nil
    ) {
        guard let session = sessions[peripheralID], session.initialized,
              let imuCaps = session.info?.streams?.imu,
              imuCaps.hasAccel, !includeGyro || imuCaps.hasGyro else {
            completion?(false)
            return
        }
        if session.imuStreamID != nil && session.imuIncludeGyro == includeGyro && session.imuRateHz == rateHz {
            renewImu(for: peripheralID)
            completion?(true)
            return
        }
        if session.imuInFlight {
            completion?(false)
            return
        }
        if let existing = session.imuStreamID {
            session.imuInFlight = true
            session.imuStreamID = nil
            sendRPC(method: "stream.stop", params: ["stream_id": Int(existing)], to: peripheralID) { [weak self] _ in
                guard let self, let session = self.sessions[peripheralID] else { completion?(false); return }
                session.imuInFlight = false
                self.startImu(includeGyro: includeGyro, rateHz: rateHz, leaseMs: leaseMs, to: peripheralID, completion: completion)
            }
            return
        }

        let actualRate = imuCaps.ratesHz.contains(rateHz) ? rateHz : (imuCaps.ratesHz.max() ?? 50)
        session.imuInFlight = true
        let channels = includeGyro ? ["accel", "gyro"] : ["accel"]
        let params: [String: Any] = [
            "kind": "imu",
            "format": ["channels": channels, "rate_hz": actualRate],
            "lease_ms": min(leaseMs, imuCaps.maxLeaseMs)
        ]
        sendRPC(method: "stream.start", params: params, to: peripheralID) { [weak self] result in
            guard let self, let session = self.sessions[peripheralID] else { completion?(false); return }
            session.imuInFlight = false
            switch result {
            case .success(let (dict, _)):
                if let sid = dict["stream_id"] as? Int, sid > 0, sid <= Int(UInt16.max) {
                    session.imuStreamID = UInt16(sid)
                    session.imuIncludeGyro = includeGyro
                    session.imuRateHz = actualRate
                    completion?(true)
                    return
                }
                completion?(false)
            case .failure(let error):
                NSLog("BLE startImu failed dev=VS-\(self.deviceID(for: peripheralID) ?? "unknown"): \(error.localizedDescription)")
                completion?(false)
            }
        }
    }

    func renewImu(for peripheralID: UUID) {
        guard let sid = sessions[peripheralID]?.imuStreamID else { return }
        sendNotification(method: "stream.renew", params: ["stream_id": Int(sid)], to: peripheralID)
    }

    func stopImu(for peripheralID: UUID) {
        guard let session = sessions[peripheralID], let sid = session.imuStreamID else { return }
        session.imuStreamID = nil
        session.imuIncludeGyro = false
        session.imuRateHz = 0
        sendRPC(method: "stream.stop", params: ["stream_id": Int(sid)], to: peripheralID)
    }

    // MARK: - Firmware OTA

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
        guard let chunkSize = BleProtocol.otaChunkSize(maximumWriteLength: maxWrite) else {
            completion(.failure(FirmwareUpdateError.otaWriteTooSmall))
            return
        }
        let transferID = UInt32.random(in: 1...UInt32.max)
        let isLegacy = sessions[peripheralID]?.info == nil
        if isLegacy {
            sendLegacyReadyIfNeeded(for: peripheralID)
        }
        NSLog("BLE OTA start dev=VS-\(deviceID) bytes=\(image.count) chunk=\(chunkSize) legacy=\(isLegacy)")
        firmwareUpdateSession = FirmwareUpdateSession(
            peripheralID: peripheralID,
            transferID: transferID,
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
        pendingFirmwareStartWorkItem?.cancel()
        pendingFirmwareStartWorkItem = nil
        if isLegacy, let sentAt = sessions[peripheralID]?.legacyReadySentAt {
            let settleDelay = max(0, 0.8 - (ProcessInfo.processInfo.systemUptime - sentAt))
            if settleDelay > 0.05 {
                let workItem = DispatchWorkItem { [weak self] in
                    guard let self, self.firmwareUpdateSession?.transferID == transferID else { return }
                    self.pendingFirmwareStartWorkItem = nil
                    self.sendNextFirmwareUpdateFrame()
                }
                pendingFirmwareStartWorkItem = workItem
                DispatchQueue.main.asyncAfter(deadline: .now() + settleDelay, execute: workItem)
                return
            }
        }
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

    func supportsFirmwareUpdate(deviceID: String) -> Bool {
        connectedDevices.contains { id, device in
            device.deviceID == deviceID && otaCharacteristics[id] != nil
        }
    }

    // MARK: - CBCentralManagerDelegate

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
        guard !peerRemovedPairingIDs.contains(peripheral.identifier) else { return }
        guard shouldConnect(localName: localName, peripheralName: peripheral.name) else { return }
        guard peripheral.state != .disconnecting else { return }
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
        guard peripherals[peripheral.identifier] != nil else {
            central.cancelPeripheralConnection(peripheral)
            return
        }
        peripheral.discoverServices([CBUUID(string: BleProtocol.serviceUUID)])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        handleConnectionClosed(peripheral, error: error)
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        handleConnectionClosed(peripheral, error: error)
    }

    private func handleConnectionClosed(_ peripheral: CBPeripheral, error: Error?) {
        let deviceID = deviceID(for: peripheral.identifier) ?? "unknown"
        if let error {
            NSLog("BLE VS-\(deviceID) disconnected error=\(error.localizedDescription)")
        }
        if (error as? CBError)?.code == .peerRemovedPairingInformation {
            NSLog("BLE VS-\(deviceID) removed its pairing with this Mac; forget it in Bluetooth settings and pair again")
            peerRemovedPairingIDs.insert(peripheral.identifier)
        }
        if peripherals[peripheral.identifier] != nil {
            removePeripheral(peripheral)
            onConnectionChange?(currentConnectedDevices)
        }
        guard !isWorkspaceSleeping else { return }
        central?.stopScan()
        scanIfReady()
    }

    // MARK: - CBPeripheralDelegate

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard error == nil,
              let service = peripheral.services?.first(where: { $0.uuid.uuidString == BleProtocol.serviceUUID }) else {
            central?.cancelPeripheralConnection(peripheral)
            return
        }
        peripheral.discoverCharacteristics(nil, for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard error == nil, let characteristics = service.characteristics else {
            central?.cancelPeripheralConnection(peripheral)
            return
        }
        if let authorization = characteristics.first(where: { $0.uuid.uuidString == BleProtocol.authorizedUUID }) {
            pendingAuthorization[peripheral.identifier] = characteristics
            peripheral.readValue(for: authorization)
        } else {
            activate(peripheral, characteristics: characteristics)
        }
    }

    private func activate(_ peripheral: CBPeripheral, characteristics: [CBCharacteristic]) {
        guard peripherals[peripheral.identifier] != nil else { return }
        connectedDevices[peripheral.identifier] = discoveredDevices[peripheral.identifier]
            ?? connectedDevice(localName: nil, peripheralName: peripheral.name)
        sessions[peripheral.identifier]?.cancelPendingRPCs()
        sessions[peripheral.identifier] = PeripheralSession()
        onConnectionChange?(currentConnectedDevices)

        characteristics.forEach { characteristic in
            switch characteristic.uuid.uuidString.uppercased() {
            case BleProtocol.streamTXUUID:
                peripheral.setNotifyValue(true, for: characteristic)
            case BleProtocol.controlTXUUID:
                peripheral.setNotifyValue(true, for: characteristic)
            case BleProtocol.controlRXUUID:
                controlCharacteristics[peripheral.identifier] = characteristic
            case BleProtocol.otaRXUUID:
                otaCharacteristics[peripheral.identifier] = characteristic
            case BleProtocol.otaStateUUID:
                peripheral.setNotifyValue(true, for: characteristic)
            default:
                break
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        let id = peripheral.identifier
        guard let session = sessions[id], connectedDevices[id] != nil else { return }
        if let error {
            NSLog("BLE subscription failed uuid=\(characteristic.uuid.uuidString): \(error.localizedDescription)")
            return
        }
        switch characteristic.uuid.uuidString.uppercased() {
        case BleProtocol.streamTXUUID:
            session.streamTXSubscribed = characteristic.isNotifying
        case BleProtocol.controlTXUUID:
            session.controlTXSubscribed = characteristic.isNotifying
        default:
            return
        }
        if session.streamTXSubscribed && session.controlTXSubscribed && !session.handshakeStarted {
            session.handshakeStarted = true
            beginHandshake(for: id)
        }
    }

    private func beginHandshake(for peripheralID: UUID) {
        sendRPC(method: "device.get_info", to: peripheralID, timeout: 1.5) { [weak self] result in
            guard let self, let session = self.sessions[peripheralID] else { return }
            switch result {
            case .success(let (_, rawData)):
                guard let info = BleProtocol.decodeDeviceInfoResult(from: rawData), info.supportsProtocolV2 else {
                    self.probeLegacyDeviceInfo(for: peripheralID)
                    return
                }
                session.info = info
                self.onDeviceInfo?(peripheralID, info)
                if let battery = info.battery {
                    self.onBatteryChanged?(peripheralID, battery)
                }
                self.sendRPC(method: "device.reset", to: peripheralID) { [weak self] resetResult in
                    guard let self, self.sessions[peripheralID] != nil else { return }
                    guard case .success = resetResult else {
                        NSLog("BLE device.reset failed dev=VS-\(self.deviceID(for: peripheralID) ?? "unknown")")
                        return
                    }
                    self.sendRPC(method: "stream.start", params: ["kind": "input"], to: peripheralID) { [weak self] inputResult in
                        guard let self, let session = self.sessions[peripheralID] else { return }
                        guard case .success(let (dict, _)) = inputResult,
                              let sid = dict["stream_id"] as? Int, sid > 0, sid <= Int(UInt16.max) else {
                            NSLog("BLE stream.start(input) failed dev=VS-\(self.deviceID(for: peripheralID) ?? "unknown")")
                            return
                        }
                        session.inputStreamID = UInt16(sid)
                        session.lastInputSeq = nil
                        session.buttonsMask = UInt32((dict["buttons_mask"] as? Int) ?? 0)
                        session.initialized = true
                        self.onDeviceReady?(peripheralID, info)
                    }
                }
            case .failure:
                // Fall back to legacy v1 get_device_info so 0.4.x firmware reports its version for OTA.
                self.probeLegacyDeviceInfo(for: peripheralID)
            }
        }
    }

    private func probeLegacyDeviceInfo(for peripheralID: UUID) {
        guard sessions[peripheralID]?.info == nil,
              let peripheral = peripherals[peripheralID],
              let control = controlCharacteristics[peripheralID] else { return }
        if sessions[peripheralID]?.legacyInfo == nil {
            peripheral.writeValue(BleProtocol.legacyDeviceInfoRequest, for: control, type: .withoutResponse)
        }
        sendLegacyReadyIfNeeded(for: peripheralID)
    }

    private func sendLegacyReadyIfNeeded(for peripheralID: UUID) {
        guard let session = sessions[peripheralID],
              session.info == nil,
              session.legacyReadySentAt == nil,
              let peripheral = peripherals[peripheralID],
              let control = controlCharacteristics[peripheralID] else { return }
        // Legacy 0.4.x firmware only raises the BLE supervision timeout to 6s
        // (via voice_ble_request_slow_interval) after receiving ui_state=ready.
        // Without it, synchronous flash erase in esp_ota_begin drops the link.
        peripheral.writeValue(BleProtocol.legacyUIStateReady, for: control, type: .withoutResponse)
        session.legacyReadySentAt = ProcessInfo.processInfo.systemUptime
        NSLog("BLE sent legacy ui_state=ready dev=VS-\(deviceID(for: peripheralID) ?? "unknown")")
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        let id = peripheral.identifier
        if characteristic.uuid.uuidString == BleProtocol.authorizedUUID {
            guard let characteristics = pendingAuthorization.removeValue(forKey: id) else { return }
            if error == nil, characteristic.value == Data([1]) {
                activate(peripheral, characteristics: characteristics)
            } else {
                central?.cancelPeripheralConnection(peripheral)
            }
            return
        }
        guard let data = characteristic.value, !data.isEmpty else { return }
        switch characteristic.uuid.uuidString.uppercased() {
        case BleProtocol.streamTXUUID:
            handleStreamData(data, from: id)
        case BleProtocol.controlTXUUID:
            handleControlData(data, from: id)
        case BleProtocol.otaStateUUID:
            if let event = BleProtocol.parseFirmwareOTAStateEvent(data) {
                handleFirmwareUpdateStateEvent(event)
            }
        default:
            break
        }
    }

    private func handleControlData(_ data: Data, from peripheralID: UUID) {
        guard let session = sessions[peripheralID] else { return }

        // Handle legacy v1 device_info (from 0.4.x firmware) to guide OTA upgrade.
        if data[0] == 1 {
            if let legacy = BleProtocol.parseLegacyDeviceInfo(data), session.info == nil {
                session.legacyInfo = legacy
                sendLegacyReadyIfNeeded(for: peripheralID)
                onLegacyDeviceInfo?(peripheralID, legacy)
            }
            return
        }

        guard let jsonData = session.reassembler.receive(data),
              let message = BleProtocol.parseControlMessage(jsonData) else { return }

        switch message {
        case .rpcResult(let id, let dict, let raw):
            if let pending = session.pendingRPCs.removeValue(forKey: id) {
                pending.timer.invalidate()
                pending.completion(.success((dict, raw)))
            }
        case .rpcError(let id, let rpcError):
            if let pending = session.pendingRPCs.removeValue(forKey: id) {
                pending.timer.invalidate()
                pending.completion(.failure(rpcError))
            }
        case .batteryChanged(let status):
            onBatteryChanged?(peripheralID, status)
        case .streamClosed(let streamID, let reason):
            if session.inputStreamID == streamID { session.inputStreamID = nil }
            if session.audioInStreamID == streamID {
                session.audioInStreamID = nil
                session.audioInMode = nil
            }
            if session.imuStreamID == streamID {
                session.imuStreamID = nil
                session.imuIncludeGyro = false
                session.imuRateHz = 0
            }
            onStreamClosed?(peripheralID, streamID, reason)
        case .unknownEvent:
            break
        }
    }

    private func handleStreamData(_ data: Data, from peripheralID: UUID) {
        guard let session = sessions[peripheralID], session.initialized, data.count >= 4,
              data[0] == BleProtocol.protocolVersion else { return }

        switch data[1] {
        case BleProtocol.typeNop:
            break
        case BleProtocol.typeAudioIn:
            guard let frame = BleProtocol.parseAudioStreamFrame(data),
                  session.audioInStreamID == frame.streamID || frame.isEnd else { return }
            if frame.isStart && session.audioInLocalDisplayFeedback {
                session.lastDisplayScene = "recording"
                session.lastDisplayText = ""
            }
            onAudioFrame?(peripheralID, frame)
        case BleProtocol.typeImu:
            guard let frame = BleProtocol.parseImuStreamFrame(data),
                  session.imuStreamID == frame.streamID else { return }
            onImuFrame?(peripheralID, frame)
        case BleProtocol.typeInput:
            guard let frame = BleProtocol.parseInputStreamFrame(data),
                  session.inputStreamID == frame.streamID,
                  let buttons = session.info?.buttons else { return }
            if let prevSeq = session.lastInputSeq, frame.seq != prevSeq &+ 1 {
                onInputGap?(peripheralID)
            }
            session.lastInputSeq = frame.seq
            let previousMask = session.buttonsMask
            session.buttonsMask = frame.buttonsMask
            let changed = previousMask ^ frame.buttonsMask
            for bitIndex in 0..<min(buttons.count, 32) {
                let bit = UInt32(1) << UInt32(bitIndex)
                guard changed & bit != 0 else { continue }
                let isDown = (frame.buttonsMask & bit) != 0
                let input = ButtonInput(
                    id: buttons[bitIndex],
                    action: isDown ? .down : .up,
                    timestampMs: UInt64(frame.timestampMs)
                )
                onButtonInput?(peripheralID, input)
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

    // MARK: - Helpers

    private func shouldConnect(localName: String?, peripheralName: String?) -> Bool {
        let advertisedName = localName ?? peripheralName ?? ""
        guard !pairedDeviceIDs.isEmpty, let deviceID = Self.deviceID(from: advertisedName) else {
            return false
        }
        return pairedDeviceIDs.contains(deviceID)
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
        }
    }

    private func handleFirmwareUpdateStateEvent(_ event: FirmwareOTAStateEvent) {
        guard let session = firmwareUpdateSession else { return }
        if let transferID = event.transferID, transferID != session.transferID { return }

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
        pendingFirmwareStartWorkItem?.cancel()
        pendingFirmwareStartWorkItem = nil
        guard let session = firmwareUpdateSession else { return }
        NSLog("BLE OTA failed dev=VS-\(deviceID(for: session.peripheralID) ?? "unknown"): \(error.localizedDescription)")
        if let peripheral = peripherals[session.peripheralID],
           let characteristic = otaCharacteristics[session.peripheralID] {
            let payload = BleProtocol.otaAbortPayload(transferID: session.transferID)
            peripheral.writeValue(payload, for: characteristic, type: .withoutResponse)
        }
        sessions[session.peripheralID]?.lastDisplayScene = nil
        sessions[session.peripheralID]?.lastDisplayText = nil
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
            central.scanForPeripherals(withServices: [
                CBUUID(string: BleProtocol.serviceUUID),
                CBUUID(string: BleProtocol.reconnectAdvertisementUUID),
            ])
        }
    }

    private func restoreConnectedPeripherals() {
        guard let central, central.state == .poweredOn, !isWorkspaceSleeping, !pairedDeviceIDs.isEmpty else { return }
        let serviceUUID = CBUUID(string: BleProtocol.serviceUUID)
        let restoredPeripherals = central.retrieveConnectedPeripherals(withServices: [serviceUUID])
        guard !restoredPeripherals.isEmpty else { return }

        for peripheral in restoredPeripherals {
            guard peripherals[peripheral.identifier] == nil, let device = knownDevice(for: peripheral) else {
                continue
            }
            discoveredDevices[peripheral.identifier] = device
            peripherals[peripheral.identifier] = peripheral
            peripheral.delegate = self
            central.connect(peripheral)
        }
    }

    private func knownDevice(for peripheral: CBPeripheral) -> ConnectedVoiceStickDevice? {
        if let device = discoveredDevices[peripheral.identifier]
            ?? connectedDevice(localName: nil, peripheralName: peripheral.name) {
            return pairedDeviceIDs.contains(device.deviceID) ? device : nil
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
        for session in sessions.values { session.cancelPendingRPCs() }
        sessions.removeAll()
        peripherals.removeAll()
        discoveredDevices.removeAll()
        connectedDevices.removeAll()
        controlCharacteristics.removeAll()
        pendingAuthorization.removeAll()
        otaCharacteristics.removeAll()
        onConnectionChange?([])
    }

    private func removePeripheral(_ peripheral: CBPeripheral) {
        sessions.removeValue(forKey: peripheral.identifier)?.cancelPendingRPCs()
        peripheral.delegate = nil
        peripherals.removeValue(forKey: peripheral.identifier)
        discoveredDevices.removeValue(forKey: peripheral.identifier)
        connectedDevices.removeValue(forKey: peripheral.identifier)
        controlCharacteristics.removeValue(forKey: peripheral.identifier)
        pendingAuthorization.removeValue(forKey: peripheral.identifier)
        otaCharacteristics.removeValue(forKey: peripheral.identifier)
        if firmwareUpdateSession?.peripheralID == peripheral.identifier {
            failFirmwareUpdate(FirmwareUpdateError.noConnectedDevice)
        }
    }

    @objc private func workspaceWillSleep() {
        isWorkspaceSleeping = true
        central?.stopScan()
        setDisplay(scene: "idle", text: "")
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
