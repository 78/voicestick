import AppKit
import Foundation

final class VoiceStickCoordinator {
    private enum PendingPasteState {
        case idle
        case waitingToPaste(id: UUID, text: String)
        case paused(id: UUID, target: InputInjector.PasteTarget?, overlay: OverlayController, deviceID: String?)

        var isIdle: Bool {
            if case .idle = self {
                return true
            }
            return false
        }
    }

    private enum MainInputState {
        case ready
        case recording(sessionID: UInt32, peripheralID: UUID, startedAt: Date)
        case finalizing(sessionID: UInt32, peripheralID: UUID, startedAt: Date?)
        case pendingConfirmation(peripheralID: UUID)
        case pausedConfirmation(peripheralID: UUID?)
        case error(peripheralID: UUID?)

        var sessionID: UInt32? {
            switch self {
            case .recording(let sessionID, _, _), .finalizing(let sessionID, _, _):
                return sessionID
            case .ready, .pendingConfirmation, .pausedConfirmation, .error:
                return nil
            }
        }

        var peripheralID: UUID? {
            switch self {
            case .recording(_, let peripheralID, _),
                 .finalizing(_, let peripheralID, _),
                 .pendingConfirmation(let peripheralID):
                return peripheralID
            case .pausedConfirmation(let peripheralID), .error(let peripheralID):
                return peripheralID
            case .ready:
                return nil
            }
        }

        var startedAt: Date? {
            switch self {
            case .recording(_, _, let startedAt):
                return startedAt
            case .finalizing(_, _, let startedAt):
                return startedAt
            case .ready, .pendingConfirmation, .pausedConfirmation, .error:
                return nil
            }
        }

        var isRecording: Bool {
            if case .recording = self {
                return true
            }
            return false
        }

        var isFinalizing: Bool {
            if case .finalizing = self {
                return true
            }
            return false
        }

        var isBusy: Bool {
            if case .ready = self {
                return false
            }
            return true
        }
    }

    private final class SubtitleCycle {
        let peripheralID: UUID
        let deviceID: String?
        let sessionID: UInt32
        let startedAt: Date
        let oggMuxer = OggOpusMuxer(sampleRate: 16_000, channels: 1)
        let debugAudioRecorder: DebugAudioRecorder
        var asr: any ASRClient
        var receivedAudioFrames = 0
        var bufferedOggChunks: [Data] = []
        var asrStarted = false
        var sentFinalAudioChunk = false
        var finishedFinalText = false
        var waitingForAudioEnd = false
        var audioEndTimeoutTimer: Timer?

        init(peripheralID: UUID, deviceID: String?, sessionID: UInt32, config: AppConfig) {
            self.peripheralID = peripheralID
            self.deviceID = deviceID
            self.sessionID = sessionID
            self.startedAt = Date()
            self.asr = VoiceStickCoordinator.makeASRClient(config: config)
            self.debugAudioRecorder = DebugAudioRecorder(
                enabled: config.debugAudioCache,
                directory: config.debugAudioDirectory
            )
        }

        var duration: TimeInterval {
            Date().timeIntervalSince(startedAt)
        }

        deinit {
            audioEndTimeoutTimer?.invalidate()
        }
    }

    private struct SubtitleCycleKey: Hashable {
        let peripheralID: UUID
        let sessionID: UInt32
    }

    private var config: AppConfig
    private let statusController: StatusController
    private let ble: BleCentral
    private var asr: any ASRClient
    private var textClient: LLMTextClient
    private let subtitleController = SubtitleController()
    private let oggMuxer = OggOpusMuxer(sampleRate: 16_000, channels: 1)
    private let inputInjector = InputInjector()
    private let firmwareManifestClient = FirmwareManifestClient()
    private var debugAudioRecorder: DebugAudioRecorder
    private let minimumRecordingDuration: TimeInterval = 0.5
    private let audioEndTimeout: TimeInterval = 1.0
    private let firmwareManifestCacheDuration: TimeInterval = 24 * 60 * 60
    private let secondaryButtonClicks = SecondaryButtonClicks()
    private var buttonPresses: [UUID: [String: UInt64]] = [:]
    private var nextUtteranceID: UInt32 = 1
    private var activeCaptureUtterance: [UUID: UInt32] = [:]
    private var shakePeripheralID: UUID?
    private var shakeDetector = ShakeDetector()
    private var shakeReadyAt: TimeInterval = 0
    private var shakeRenewTimer: Timer?

    private var mainInputState = MainInputState.ready {
        didSet {
            if mainInputState.isBusy {
                secondaryButtonClicks.cancelAll()
            } else {
                scheduleFirmwareUpdatePrompt()
            }
        }
    }
    private var receivedAudioFrames = 0
    private var bufferedOggChunks: [Data] = []
    private var asrStarted = false
    private var sentFinalAudioChunk = false
    private var pastedFinalText = false
    private var pendingTextTransformID: UUID?
    private var waitingForAudioEnd = false
    private var audioEndTimeoutTimer: Timer?
    private var pendingPasteState = PendingPasteState.idle {
        didSet {
            if case .paused(_, _, _, let deviceID) = oldValue {
                statusController.hideOverlay(deviceID: deviceID)
                isSendingConfirmation = false
            }
        }
    }
    private var isSendingConfirmation = false
    private var countdownPasteTarget: InputInjector.PasteTarget?
    private var lastRecoverableText: String?
    private var lastRecoverablePeripheralID: UUID?
    private var pairedDeviceIDs: [String]
    private var firmwareInfoByDeviceID: [String: DeviceFirmwareInfo] = [:]
    private var firmwareReportingPeripherals: [String: UUID] = [:]
    private typealias FirmwareFeedCompletion = (Result<FirmwareManifest, Error>) -> Void
    private struct FirmwareFeed {
        var manifest: FirmwareManifest?
        var lastChecked: Date?
        var isChecking = false
        var completions: [FirmwareFeedCompletion] = []
        var errorMessage: String?
    }
    private var firmwareFeeds: [String: FirmwareFeed] = [:]
    private var firmwareManifestRefreshTimer: Timer?
    private static let firmwareReminderHistoryKey = "firmwareUpdateReminderHistory"
    private var firmwareUpdatePromptScheduled = false
    private var isShowingFirmwareUpdatePrompt = false
    private var shownFirmwareRequirements: Set<UUID> = []
    private var errorRecoveryToken = 0
    private var isShowingASRError = false
    private var subtitleCycles: [SubtitleCycleKey: SubtitleCycle] = [:] {
        didSet {
            if subtitleCycles.isEmpty {
                scheduleFirmwareUpdatePrompt()
            } else {
                secondaryButtonClicks.cancelAll()
            }
        }
    }
    private var activeSubtitleSessions: [UUID: UInt32] = [:]
    var isFirmwareUpdateWindowOpen = false {
        didSet {
            if !isFirmwareUpdateWindowOpen { scheduleFirmwareUpdatePrompt() }
        }
    }
    var isFirmwareRequirementWindowOpen = false {
        didSet {
            if !isFirmwareRequirementWindowOpen { scheduleFirmwareUpdatePrompt() }
        }
    }
    var onFirmwareUpdatePrompt: ((String, String, String) -> Void)?
    var onFirmwareRequirement: ((String) -> Void)?
    var onFirmwareAvailabilityChanged: (() -> Void)?
    var onToggleTranslationMode: ((String) -> Void)?

    init(config: AppConfig, statusController: StatusController) {
        self.config = config
        self.statusController = statusController
        self.pairedDeviceIDs = config.pairedDeviceIDs
        self.ble = BleCentral(pairedDeviceIDs: config.pairedDeviceIDs)
        self.asr = Self.makeASRClient(config: config)
        self.textClient = LLMTextClient(config: config)
        self.debugAudioRecorder = DebugAudioRecorder(
            enabled: config.debugAudioCache,
            directory: config.debugAudioDirectory
        )
    }

    private lazy var motionWebBridge = MotionWebBridge(ble: ble)

    private func isVoiceBusy() -> Bool {
        mainInputState.isBusy || !subtitleCycles.isEmpty || isFirmwareUpdateWindowOpen
    }

    func start() {
        motionWebBridge.onDeviceReleased = { [weak self] id in
            self?.restoreVoiceMode(for: id)
        }
        motionWebBridge.onClientActivated = { [weak self] in
            self?.secondaryButtonClicks.cancelAll()
        }
        do {
            try motionWebBridge.start { [weak self] in
                self?.isVoiceBusy() ?? true
            }
        } catch {
            NSLog("Failed to start MotionWebBridge: %@", error.localizedDescription)
        }
        statusController.onOpenTennisStudio = { [weak self] in
            guard let self else { return }
            do {
                try self.motionWebBridge.start { [weak self] in
                    self?.isVoiceBusy() ?? true
                }
                self.motionWebBridge.openInBrowser()
            } catch {
                NSAlert(error: error).runModal()
            }
        }
        ble.onImuFrame = { [weak self] id, frame in
            self?.handleImuFrame(frame, from: id)
        }
        ble.onConnectionChange = { [weak self] connectedDevices in
            guard let self else { return }
            self.secondaryButtonClicks.removeDisconnected(isConnected: self.ble.isConnected)
            self.buttonPresses = self.buttonPresses.filter { self.ble.isConnected($0.key) }
            self.activeCaptureUtterance = self.activeCaptureUtterance.filter { self.ble.isConnected($0.key) }
            if let shakeID = self.shakePeripheralID, !self.ble.isConnected(shakeID) {
                self.stopShakeConfirmation()
            }
            self.statusController.setConnectedDevices(connectedDevices)
            self.firmwareReportingPeripherals = self.firmwareReportingPeripherals.filter {
                self.ble.isConnected($0.value)
            }
            self.shownFirmwareRequirements = self.shownFirmwareRequirements.filter(self.ble.isConnected)
            self.cancelActiveCycleIfDeviceDisconnected()
            self.refreshFirmwareAvailability()
            self.refreshConfirmationShortcuts()
            if case .pausedConfirmation = self.mainInputState {
                self.statusController.setStatus("Awaiting confirmation")
            } else {
                self.statusController.setStatus(self.pairedDeviceIDs.isEmpty ? "Pair a VoiceStick" : "Ready")
            }
        }

        ble.onDeviceInfo = { [weak self] peripheralID, info in
            guard let self else { return }
            NSLog("Connected VoiceStick v2 hardware=\(info.hardware) firmware=\(info.firmwareVersion)")
            self.updateDeviceFirmwareInfo(
                hardware: info.hardware,
                firmwareVersion: info.firmwareVersion,
                buttons: info.buttons,
                requiresUpgrade: !info.supportsProtocolV2,
                peripheralID: peripheralID
            )
            if self.activePeripheralID == peripheralID {
                self.refreshConfirmationShortcuts()
            }
        }

        ble.onLegacyDeviceInfo = { [weak self] peripheralID, legacy in
            guard let self else { return }
            NSLog("Connected legacy VoiceStick hardware=\(legacy.hardware ?? "unknown") firmware=\(legacy.firmwareVersion ?? "unknown")")
            self.updateDeviceFirmwareInfo(
                hardware: legacy.hardware,
                firmwareVersion: legacy.firmwareVersion,
                buttons: nil,
                requiresUpgrade: true,
                peripheralID: peripheralID
            )
        }

        ble.onDeviceReady = { [weak self] peripheralID, _ in
            guard let self else { return }
            if self.activePeripheralID == peripheralID, case .paused = self.pendingPasteState {
                self.sendDisplayState("pending_confirmation", to: peripheralID)
                self.startShakeConfirmation(for: peripheralID)
                return
            }
            self.restoreVoiceMode(for: peripheralID)
        }

        ble.onBatteryChanged = { [weak self] peripheralID, status in
            guard let self, self.ble.isConnected(peripheralID),
                  let deviceID = self.ble.deviceID(for: peripheralID) else { return }
            self.statusController.setBatteryStatus(status, for: deviceID)
        }

        ble.onButtonInput = { [weak self] peripheralID, button in
            self?.handleButton(button, peripheralID: peripheralID)
        }

        ble.onInputGap = { [weak self] peripheralID in
            self?.buttonPresses.removeValue(forKey: peripheralID)
            self?.secondaryButtonClicks.cancelAll()
        }

        ble.onAudioFrame = { [weak self] peripheralID, frame in
            self?.handleAudioFrame(frame, peripheralID: peripheralID)
        }

        ble.onStreamClosed = { [weak self] peripheralID, streamID, reason in
            guard let self else { return }
            NSLog("BLE stream closed id=\(streamID) reason=\(reason) dev=VS-\(self.deviceID(for: peripheralID) ?? "unknown")")
            if self.activeCaptureUtterance.removeValue(forKey: peripheralID) != nil {
                self.handlePrimaryButtonUp(peripheralID: peripheralID)
            }
            if self.shakePeripheralID == peripheralID && reason != "ota" {
                self.stopShakeConfirmation()
            }
        }

        configureASRCallbacks()
        ble.start()
        checkFirmwareUpdatesIfNeeded(force: false)
        startFirmwareManifestRefreshTimer()
    }

    deinit {
        audioEndTimeoutTimer?.invalidate()
        firmwareManifestRefreshTimer?.invalidate()
        shakeRenewTimer?.invalidate()
    }

    func updateConfig(_ config: AppConfig) {
        let wasRecognizing = asrStarted || mainInputState.isBusy || isWaitingForFinalText || !subtitleCycles.isEmpty
        if wasRecognizing {
            stopShakeConfirmation()
            asr.onPartial = nil
            asr.onSegment = nil
            asr.onFinal = nil
            asr.onError = nil
            asr.onUpgradeURL = nil
            asr.cancel()
            for cycle in subtitleCycles.values {
                cycle.asr.cancel()
                cycle.debugAudioRecorder.discard()
            }
            subtitleCycles.removeAll()
            activeSubtitleSessions.removeAll()
            activeCaptureUtterance.removeAll()
            mainInputState = .ready
            pendingPasteState = .idle
            debugAudioRecorder.discard()
            statusController.hideOverlay()
            subtitleController.hideAll()
            sendUIStateForActiveDevice("ready")
            finishRecognitionCycle()
        }

        let interactionChanged = self.config.interactionMode != config.interactionMode
        self.config = config
        if interactionChanged || wasRecognizing {
            for id in ble.connectedReadyPeripheralIDs() {
                restoreVoiceMode(for: id)
            }
        }
        debugAudioRecorder = DebugAudioRecorder(
            enabled: config.debugAudioCache,
            directory: config.debugAudioDirectory
        )
        asr = Self.makeASRClient(config: config)
        textClient = LLMTextClient(config: config)
        configureASRCallbacks()

        if pairedDeviceIDs != config.pairedDeviceIDs {
            updatePairedDeviceIDs(config.pairedDeviceIDs)
        } else if isShowingASRError {
            recoverFromASRError()
        } else if wasRecognizing {
            statusController.setStatus("Ready")
        }
    }

    static func makeASRClient(config: AppConfig) -> any ASRClient {
        switch config.asrProvider {
        case .voiceStickCloud:
            return ASRWebSocketClient(config: config)
        case .openAI:
            return OpenAIRealtimeASRClient(config: config)
        }
    }

    private func configureASRCallbacks() {
        asr.onPartial = { [weak self] text in
            guard let self, self.asrStarted, !self.pastedFinalText else { return }
            self.statusController.showPartial(text, deviceID: self.activeDeviceID)
            if self.shouldSendPartialToDevice() {
                self.sendUIStateForActiveDevice("thinking", text: text)
            }
        }

        asr.onSegment = { [weak self] segment in
            self?.handleDefiniteSegment(segment)
        }

        asr.onFinal = { [weak self] result in
            guard let self, self.asrStarted else { return }
            self.finishWithFinalText(result)
        }

        asr.onError = { [weak self] message, partialText in
            guard let self, self.asrStarted, !self.pastedFinalText else { return }
            self.finishWithASRError(message, partialText: partialText)
        }

        asr.onUpgradeURL = { [weak self] url, message in
            guard let self, self.isShowingASRError else { return }
            self.presentASRUpgradeAlert(url: url, message: message)
        }
    }

    private func configureSubtitleASRCallbacks(for cycle: SubtitleCycle) {
        let peripheralID = cycle.peripheralID
        let sessionID = cycle.sessionID
        cycle.asr.onPartial = { [weak self] text in
            guard
                let self,
                let cycle = self.subtitleCycle(peripheralID: peripheralID, sessionID: sessionID),
                self.canUpdateOverlayForSubtitleCycle(peripheralID: peripheralID, sessionID: sessionID)
            else { return }
            self.statusController.showPartial(text, deviceID: cycle.deviceID)
            if self.shouldSendSubtitlePartialToDevice(cycle) {
                self.sendDisplayState("thinking", text: text, to: peripheralID)
            }
        }
        cycle.asr.onSegment = { [weak self] segment in
            guard self?.isActiveSubtitleCycle(peripheralID: peripheralID, sessionID: sessionID) == true else {
                return
            }
            self?.handleSubtitleDefiniteSegment(segment, peripheralID: peripheralID)
        }
        cycle.asr.onFinal = { [weak self] result in
            self?.finishSubtitleCycleWithFinalText(
                peripheralID: peripheralID,
                sessionID: sessionID,
                result: result
            )
        }
        cycle.asr.onError = { [weak self] message, _ in
            self?.finishSubtitleCycleWithError(
                peripheralID: peripheralID,
                sessionID: sessionID,
                message: message
            )
        }
        cycle.asr.onUpgradeURL = { url, _ in
            NSWorkspace.shared.open(url)
        }
    }

    func updatePairedDeviceIDs(_ deviceIDs: [String]) {
        let wasEmpty = pairedDeviceIDs.isEmpty
        pairedDeviceIDs = deviceIDs
        statusController.setPairedDeviceIDs(deviceIDs)
        if deviceIDs.isEmpty != wasEmpty {
            statusController.setStatus(deviceIDs.isEmpty ? "Pair a VoiceStick" : "Ready")
        }
        ble.updatePairedDeviceIDs(deviceIDs)
    }

    func updateFirmware(from url: URL, for deviceID: String,
                        progress: @escaping (FirmwareUpdateProgress) -> Void,
                        completion: @escaping (Result<Void, Error>) -> Void) {
        do {
            let image = try Data(contentsOf: url)
            ble.updateFirmware(image: image, for: deviceID, progress: progress) { result in
                DispatchQueue.main.async {
                    completion(result)
                }
            }
        } catch {
            completion(.failure(error))
        }
    }

    func cancelFirmwareUpdate() {
        ble.cancelFirmwareUpdate()
    }

    func isDeviceConnected(_ deviceID: String) -> Bool {
        ble.isConnected(deviceID: deviceID)
    }

    func firmwareRequirement(for deviceID: String) -> DeviceFirmwareInfo? {
        guard let peripheralID = firmwareReportingPeripherals[deviceID], ble.isConnected(peripheralID),
              let info = firmwareInfoByDeviceID[deviceID], info.requiresInputFirmware else { return nil }
        return info
    }

    func supportsFirmwareUpdate(for deviceID: String) -> Bool {
        ble.supportsFirmwareUpdate(deviceID: deviceID)
    }

    func checkFirmwareUpdatesNow(for deviceID: String,
                                 completion: @escaping (Result<(current: String, latest: String, required: Bool), Error>) -> Void) {
        guard let info = firmwareInfoByDeviceID[deviceID], let current = info.currentVersion,
              let hardware = info.hardware, let url = AppConfig.firmwareManifestURLs[hardware] else {
            completion(.failure(FirmwareManifestClient.FirmwareManifestError.deviceInfoUnavailable))
            return
        }
        let reportingPeripheral = firmwareReportingPeripherals[deviceID]
        checkFirmwareFeed(for: hardware, at: url, force: true) { [weak self] result in
            completion(result.flatMap { manifest in
                if info.requiresInputFirmware && !FirmwareVersion.meetsMinimum(
                    AppConfig.minimumCompatibleFirmwareVersion, current: manifest.version
                ) {
                    return .failure(FirmwareManifestClient.FirmwareManifestError.compatibleFirmwareUnavailable)
                }
                if let reportingPeripheral, FirmwareVersion.isVersion(current, olderThan: manifest.version) {
                    _ = self?.recordFirmwareReminder(for: reportingPeripheral, version: manifest.version)
                }
                return .success((current, manifest.version, info.requiresInputFirmware))
            })
            self?.scheduleFirmwareUpdatePrompt()
        }
        refreshFirmwareAvailability()
    }

    func checkFirmwareAfterPairing() {
        checkFirmwareUpdatesIfNeeded(force: true)
    }

    func updateFirmwareFromLatest(for deviceID: String,
                                  progress: @escaping (FirmwareUpdateProgress) -> Void,
                                  completion: @escaping (Result<Void, Error>) -> Void) {
        guard let info = firmwareInfoByDeviceID[deviceID], let hardware = info.hardware,
              let manifest = firmwareFeeds[hardware]?.manifest, info.updateAvailable else {
            completion(.failure(FirmwareManifestClient.FirmwareManifestError.invalidResponse))
            return
        }

        firmwareManifestClient.downloadOTA(from: manifest) { [weak self] result in
            DispatchQueue.main.async {
                guard let self else { return }
                switch result {
                case .success(let image):
                    guard self.firmwareInfoByDeviceID[deviceID]?.hardware == manifest.hardware else {
                        completion(.failure(FirmwareManifestClient.FirmwareManifestError.invalidResponse))
                        return
                    }
                    self.ble.updateFirmware(image: image, for: deviceID, progress: progress) { result in
                        DispatchQueue.main.async {
                            completion(result)
                        }
                    }
                case .failure(let error):
                    completion(.failure(error))
                }
            }
        }
    }

    private var idleDisplayHint: String {
        config.interactionMode == .clickToTalk ? "Click to Talk" : "Hold to Talk"
    }

    private func sendDisplayState(_ state: String, text: String = "", to peripheralID: UUID?) {
        switch state {
        case "ready":
            ble.setDisplay(scene: "idle", text: idleDisplayHint, to: peripheralID)
        case "recording":
            ble.setDisplay(scene: "recording", text: "", to: peripheralID)
        case "thinking":
            ble.setDisplay(scene: "busy", text: text, to: peripheralID)
        case "pending_confirmation":
            ble.setDisplay(scene: "busy", text: "Confirm or cancel", to: peripheralID)
        case "error":
            ble.setDisplay(scene: "error", text: text, to: peripheralID)
        default:
            break
        }
    }

    private func restoreVoiceMode(for peripheralID: UUID) {
        guard ble.supportsProtocolV2(peripheralID),
              !motionWebBridge.ownsDevice(peripheralID) else { return }
        if activePeripheralID == peripheralID, !pendingPasteState.isIdle {
            return
        }
        ble.startAudioIn(mode: config.interactionMode, localDisplayFeedback: true, to: peripheralID)
        sendDisplayState("ready", to: peripheralID)
    }

    private func startShakeConfirmation(for peripheralID: UUID) {
        stopShakeConfirmation()
        guard ble.deviceInfo(for: peripheralID)?.streams?.imu?.hasAccel == true else { return }
        shakePeripheralID = peripheralID
        shakeDetector.reset()
        shakeReadyAt = ProcessInfo.processInfo.systemUptime + 0.15
        ble.startImu(includeGyro: false, rateHz: 50, leaseMs: 3000, to: peripheralID)
        shakeRenewTimer = Timer.scheduledTimer(withTimeInterval: 0.8, repeats: true) { [weak self] _ in
            guard let self, let id = self.shakePeripheralID else { return }
            self.ble.renewImu(for: id)
        }
    }

    private func stopShakeConfirmation() {
        shakeRenewTimer?.invalidate()
        shakeRenewTimer = nil
        if let id = shakePeripheralID {
            shakePeripheralID = nil
            ble.stopImu(for: id)
        }
        shakeDetector.reset()
    }

    private func handleImuFrame(_ frame: ImuStreamFrame, from peripheralID: UUID) {
        if shakePeripheralID == peripheralID,
           case .paused = pendingPasteState,
           !isSendingConfirmation,
           frame.samples.count >= 3,
           ProcessInfo.processInfo.systemUptime >= shakeReadyAt,
           let accelScale = ble.deviceInfo(for: peripheralID)?.streams?.imu?.accelLsbPerG {
            let triggered = shakeDetector.sample(
                ax: frame.samples[0], ay: frame.samples[1], az: frame.samples[2],
                accelLsbPerG: accelScale, timestampUs: Int64(frame.timestampMs) * 1000
            )
            if triggered {
                NSLog("Host shake detector cancelled paused confirmation")
                dismissPendingConfirmation()
                return
            }
        }
        motionWebBridge.receive(frame, from: peripheralID)
    }

    private func handleButton(_ button: ButtonInput, peripheralID: UUID) {
        let heldFor: UInt64?
        if button.action == .down {
            buttonPresses[peripheralID, default: [:]][button.id] = button.timestampMs
            heldFor = nil
        } else if let pressedAt = buttonPresses[peripheralID]?.removeValue(forKey: button.id) {
            let delta = UInt32(truncatingIfNeeded: button.timestampMs) &- UInt32(truncatingIfNeeded: pressedAt)
            heldFor = UInt64(delta)
        } else {
            heldFor = nil
        }

        let webOwnsButton = motionWebBridge.handleButton(button, from: peripheralID)
        if button.id == "secondary", let heldFor, heldFor < 3_000 {
            motionWebBridge.identify(peripheralID)
        }
        if webOwnsButton { return }

        switch (button.id, button.action) {
        case ("primary", .down):
            if !handleFrontButtonDuringPendingPaste(peripheralID: peripheralID),
               mainInputState.isFinalizing, activePeripheralID == peripheralID {
                sendDisplayState("thinking", to: peripheralID)
            }
        case ("secondary", _):
            guard !motionWebBridge.hasActiveClient else { return }
            handleSecondaryButton(button, heldFor: heldFor, peripheralID: peripheralID)
        default:
            break
        }
    }

    private func canDoubleClick(_ peripheralID: UUID) -> Bool {
        pendingPasteState.isIdle && !isVoiceBusy() && !motionWebBridge.hasActiveClient &&
            ble.supportsProtocolV2(peripheralID)
    }

    private func handleSecondaryButton(_ button: ButtonInput, heldFor: UInt64?, peripheralID: UUID) {
        if button.action == .down {
            secondaryButtonClicks.press(from: peripheralID, at: button.timestampMs,
                                        canDoubleClick: canDoubleClick(peripheralID))
            return
        }
        guard let heldFor, let deviceID = deviceID(for: peripheralID) else { return }
        secondaryButtonClicks.release(
            from: peripheralID, at: button.timestampMs, heldFor: heldFor,
            canDoubleClick: { [weak self] in self?.canDoubleClick(peripheralID) == true },
            onSingleClick: { [weak self] in self?.performSecondarySingleClick(peripheralID: peripheralID) },
            onDoubleClick: { onToggleTranslationMode?(deviceID) }
        )
    }

    private func performSecondarySingleClick(peripheralID: UUID) {
        if activeSubtitleSessions[peripheralID] != nil {
            cancelSubtitleCycle(peripheralID: peripheralID, reason: "secondary_cancel")
            return
        }
        if subtitleCycles.values.contains(where: { $0.peripheralID == peripheralID }) {
            cancelSubtitleCycles(peripheralID: peripheralID, reason: "secondary_cancel")
            return
        }
        cancelPendingPaste(peripheralID: peripheralID)
    }

    private func handlePrimaryButtonDown(sessionID: UInt32?, peripheralID: UUID) {
        if config.defaultOutputProfile.target == .subtitle {
            handleSubtitlePrimaryButtonDown(sessionID: sessionID, peripheralID: peripheralID)
            return
        }
        if handleFrontButtonDuringPendingPaste(peripheralID: peripheralID) {
            return
        }
        if mainInputState.isBusy {
            if activePeripheralID != peripheralID {
                sendDisplayState("ready", to: peripheralID)
            } else if mainInputState.isFinalizing || isWaitingForFinalText {
                sendDisplayState("thinking", to: peripheralID)
            }
            NSLog("Ignoring primary button while main input is busy")
            return
        }
        guard let sessionID, sessionID != 0 else {
            NSLog("Ignoring primary button down with missing/zero session; sending ready")
            sendDisplayState("ready", to: peripheralID)
            return
        }

        mainInputState = .recording(sessionID: sessionID, peripheralID: peripheralID, startedAt: Date())
        receivedAudioFrames = 0
        bufferedOggChunks.removeAll(keepingCapacity: true)
        asrStarted = false
        sentFinalAudioChunk = false
        pastedFinalText = false
        pendingPasteState = .idle
        isShowingASRError = false
        oggMuxer.reset()
        debugAudioRecorder.start(deviceID: deviceID(for: peripheralID), sessionID: sessionID)
        statusController.showListening(deviceID: deviceID(for: peripheralID))
        sendUIStateForActiveDevice("recording")
    }

    private func handlePrimaryButtonUp(peripheralID: UUID) {
        if activeSubtitleSessions[peripheralID] != nil {
            handleSubtitlePrimaryButtonUp(peripheralID: peripheralID)
            return
        }
        guard case .recording(_, let recordingPeripheralID, _) = mainInputState,
              recordingPeripheralID == peripheralID
        else {
            return
        }
        let recordingDuration = currentRecordingDuration
        let isValidRecording = recordingDuration >= minimumRecordingDuration
        if !isValidRecording {
            cancelShortRecording()
        } else {
            beginWaitingForAudioEnd(reason: "button_up")
        }
    }

    private func handleAudioFrame(_ frame: AudioStreamFrame, peripheralID: UUID) {
        if frame.isStart {
            let utteranceID = nextUtteranceID
            nextUtteranceID &+= 1
            if nextUtteranceID == 0 { nextUtteranceID = 1 }
            activeCaptureUtterance[peripheralID] = utteranceID
            handlePrimaryButtonDown(sessionID: utteranceID, peripheralID: peripheralID)
            if frame.payload.isEmpty && !frame.isEnd {
                return
            }
        }

        guard let utteranceID = activeCaptureUtterance[peripheralID] else { return }
        if frame.isEnd {
            activeCaptureUtterance.removeValue(forKey: peripheralID)
        }

        if subtitleCycle(peripheralID: peripheralID, sessionID: utteranceID) != nil {
            handleSubtitleAudioFrame(frame, sessionID: utteranceID, peripheralID: peripheralID)
            return
        }
        guard utteranceID == activeSessionID, activePeripheralID == peripheralID else { return }
        guard !sentFinalAudioChunk || (frame.isEnd && frame.payload.isEmpty) else { return }

        if frame.isEnd && frame.payload.isEmpty {
            cancelAudioEndTimeout()
            sendFinalOggChunkIfNeeded(recordingDuration: currentRecordingDuration)
            return
        }

        guard !frame.payload.isEmpty else { return }
        receivedAudioFrames += 1
        let oggChunk = oggMuxer.append(opusPayload: frame.payload, isLast: frame.isEnd)
        debugAudioRecorder.append(oggChunk)
        sendOrBufferOggChunk(
            oggChunk,
            isLast: frame.isEnd,
            canStartASR: currentRecordingDuration >= minimumRecordingDuration
        )
        guard activeSessionID == utteranceID else { return }
        if frame.isEnd {
            cancelAudioEndTimeout()
            let recordingDuration = currentRecordingDuration
            sentFinalAudioChunk = true
            if !asrStarted && recordingDuration < minimumRecordingDuration {
                cancelShortRecording()
            } else if asrStarted {
                enterFinalizingState(reason: "audio_end")
                debugAudioRecorder.finish()
            } else {
                enterFinalizingState(reason: "audio_end")
                debugAudioRecorder.finish()
                if !startASRAndFlushBufferedChunks(lastChunkIsFinal: true) {
                    finishWithASRError(asr.lastStartError ?? "Failed to start ASR")
                    return
                }
            }
        }
    }

    private func beginWaitingForAudioEnd(reason: String) {
        guard !waitingForAudioEnd else { return }
        waitingForAudioEnd = true
        NSLog("Waiting for audio END frame reason=\(reason)")
        enterFinalizingState(reason: reason)
        scheduleAudioEndTimeout()
    }

    private func enterFinalizingState(reason: String) {
        guard case .recording(let sessionID, let peripheralID, let startedAt) = mainInputState else {
            return
        }
        NSLog("Main input finalizing reason=\(reason)")
        mainInputState = .finalizing(sessionID: sessionID, peripheralID: peripheralID, startedAt: startedAt)
        statusController.setStatus("Processing")
        sendUIStateForActiveDevice("thinking")
    }

    private func scheduleAudioEndTimeout() {
        audioEndTimeoutTimer?.invalidate()
        let sessionID = activeSessionID
        let peripheralID = activePeripheralID
        audioEndTimeoutTimer = Timer.scheduledTimer(withTimeInterval: audioEndTimeout, repeats: false) { [weak self] _ in
            guard let self else { return }
            guard self.waitingForAudioEnd,
                  self.activeSessionID == sessionID,
                  self.activePeripheralID == peripheralID
            else { return }
            NSLog("Audio END timeout; finalizing buffered audio")
            self.sendFinalOggChunkIfNeeded(recordingDuration: self.currentRecordingDuration)
        }
    }

    private func cancelAudioEndTimeout() {
        waitingForAudioEnd = false
        audioEndTimeoutTimer?.invalidate()
        audioEndTimeoutTimer = nil
    }

    private func handleSubtitlePrimaryButtonDown(sessionID: UInt32?, peripheralID: UUID) {
        guard let sessionID, sessionID != 0 else {
            NSLog("Ignoring subtitle button_down with missing/zero session dev=VS-\(deviceID(for: peripheralID) ?? "unknown"); sending ready")
            sendDisplayState("ready", to: peripheralID)
            return
        }
        let deviceID = deviceID(for: peripheralID)
        NSLog("Subtitle button_down dev=VS-\(deviceID ?? "unknown") session=\(sessionID) active=\(activeSubtitleSessions[peripheralID].map(String.init) ?? "nil") existing=\(subtitleCycle(peripheralID: peripheralID, sessionID: sessionID) != nil)")
        if activeSubtitleSessions[peripheralID] == sessionID ||
            subtitleCycle(peripheralID: peripheralID, sessionID: sessionID) != nil {
            NSLog("Subtitle button_down ignored dev=VS-\(deviceID ?? "unknown") session=\(sessionID)")
            return
        }
        if let previousSessionID = activeSubtitleSessions[peripheralID] {
            NSLog("Subtitle button_down preempt active dev=VS-\(deviceID ?? "unknown") previous=\(previousSessionID) next=\(sessionID)")
            clearActiveSubtitleSession(peripheralID: peripheralID, sessionID: previousSessionID)
        }
        let cycle = SubtitleCycle(
            peripheralID: peripheralID,
            deviceID: deviceID,
            sessionID: sessionID,
            config: config
        )
        configureSubtitleASRCallbacks(for: cycle)
        subtitleCycles[SubtitleCycleKey(peripheralID: peripheralID, sessionID: sessionID)] = cycle
        activeSubtitleSessions[peripheralID] = sessionID
        cycle.debugAudioRecorder.start(deviceID: deviceID, sessionID: sessionID)
        NSLog("Subtitle cycle start dev=VS-\(deviceID ?? "unknown") session=\(sessionID)")
        statusController.showListening(deviceID: deviceID)
        sendDisplayState("recording", to: peripheralID)
    }

    private func handleSubtitlePrimaryButtonUp(peripheralID: UUID) {
        guard let cycle = activeSubtitleCycle(peripheralID: peripheralID) else {
            NSLog("Subtitle button_up ignored no active cycle dev=VS-\(deviceID(for: peripheralID) ?? "unknown")")
            return
        }
        let sessionID = cycle.sessionID
        NSLog("Subtitle button_up dev=VS-\(cycle.deviceID ?? "unknown") session=\(sessionID) frames=\(cycle.receivedAudioFrames) duration=\(String(format: "%.3f", cycle.duration))")
        if cycle.duration < minimumRecordingDuration {
            cancelSubtitleCycle(peripheralID: peripheralID, reason: "short_recording")
        } else if cycle.receivedAudioFrames == 0 {
            finishSubtitleCycleWithError(
                peripheralID: peripheralID,
                sessionID: sessionID,
                message: "No audio frames from device"
            )
        } else {
            beginWaitingForSubtitleAudioEnd(cycle, reason: "button_up")
            finishSubtitleAudioInput(cycle)
        }
    }

    private func handleSubtitleAudioFrame(_ frame: AudioStreamFrame, sessionID: UInt32, peripheralID: UUID) {
        guard let cycle = subtitleCycle(peripheralID: peripheralID, sessionID: sessionID) else { return }
        if frame.isEnd && frame.payload.isEmpty {
            cancelSubtitleAudioEndTimeout(cycle)
            sendSubtitleFinalOggChunkIfNeeded(peripheralID: peripheralID, sessionID: sessionID)
            return
        }
        guard !frame.payload.isEmpty else { return }
        cycle.receivedAudioFrames += 1
        let oggChunk = cycle.oggMuxer.append(opusPayload: frame.payload, isLast: frame.isEnd)
        cycle.debugAudioRecorder.append(oggChunk)
        sendOrBufferSubtitleOggChunk(
            oggChunk,
            isLast: frame.isEnd,
            canStartASR: cycle.duration >= minimumRecordingDuration,
            cycle: cycle
        )
        if frame.isEnd {
            cancelSubtitleAudioEndTimeout(cycle)
            cycle.sentFinalAudioChunk = true
            if !cycle.asrStarted && cycle.duration < minimumRecordingDuration {
                cancelSubtitleCycle(peripheralID: peripheralID, reason: "short_recording")
            } else if cycle.asrStarted {
                cycle.debugAudioRecorder.finish()
                finishSubtitleAudioInput(cycle)
            } else {
                cycle.debugAudioRecorder.finish()
                if !startSubtitleASRAndFlushBufferedChunks(cycle, lastChunkIsFinal: true) {
                    finishSubtitleCycleWithError(
                        peripheralID: peripheralID,
                        sessionID: cycle.sessionID,
                        message: cycle.asr.lastStartError ?? "Failed to start ASR"
                    )
                    return
                }
                finishSubtitleAudioInput(cycle)
            }
        }
    }

    private func beginWaitingForSubtitleAudioEnd(_ cycle: SubtitleCycle, reason: String) {
        guard !cycle.waitingForAudioEnd else { return }
        cycle.waitingForAudioEnd = true
        NSLog("Waiting for subtitle audio END frame VS-\(cycle.deviceID ?? "unknown") reason=\(reason)")
        if config.interactionMode != .holdToTalk {
            statusController.setStatus("Processing")
            sendDisplayState("thinking", to: cycle.peripheralID)
        }
        scheduleSubtitleAudioEndTimeout(cycle)
    }

    private func scheduleSubtitleAudioEndTimeout(_ cycle: SubtitleCycle) {
        cycle.audioEndTimeoutTimer?.invalidate()
        let peripheralID = cycle.peripheralID
        let sessionID = cycle.sessionID
        cycle.audioEndTimeoutTimer = Timer.scheduledTimer(withTimeInterval: audioEndTimeout, repeats: false) { [weak self] _ in
            guard let self,
                  let cycle = self.subtitleCycle(peripheralID: peripheralID, sessionID: sessionID),
                  cycle.waitingForAudioEnd
            else { return }
            NSLog("Subtitle audio END timeout VS-\(cycle.deviceID ?? "unknown"); finalizing buffered audio")
            self.sendSubtitleFinalOggChunkIfNeeded(peripheralID: peripheralID, sessionID: sessionID)
        }
    }

    private func cancelSubtitleAudioEndTimeout(_ cycle: SubtitleCycle) {
        cycle.waitingForAudioEnd = false
        cycle.audioEndTimeoutTimer?.invalidate()
        cycle.audioEndTimeoutTimer = nil
    }

    private func sendSubtitleFinalOggChunkIfNeeded(peripheralID: UUID, sessionID: UInt32) {
        guard let cycle = subtitleCycle(peripheralID: peripheralID, sessionID: sessionID),
              !cycle.sentFinalAudioChunk
        else { return }
        cycle.sentFinalAudioChunk = true
        NSLog("Subtitle audio final dev=VS-\(cycle.deviceID ?? "unknown") session=\(sessionID) frames=\(cycle.receivedAudioFrames) asr_started=\(cycle.asrStarted)")
        cancelSubtitleAudioEndTimeout(cycle)
        if !cycle.asrStarted && cycle.duration < minimumRecordingDuration {
            cancelSubtitleCycle(peripheralID: peripheralID, reason: "short_recording")
            return
        }
        if cycle.receivedAudioFrames == 0 {
            finishSubtitleCycleWithError(
                peripheralID: peripheralID,
                sessionID: sessionID,
                message: "No audio frames from device"
            )
            return
        }
        let finalChunk = cycle.oggMuxer.finish()
        cycle.debugAudioRecorder.append(finalChunk)
        cycle.debugAudioRecorder.finish()
        sendOrBufferSubtitleOggChunk(finalChunk, isLast: true, canStartASR: true, cycle: cycle)
        if let updatedCycle = subtitleCycle(peripheralID: peripheralID, sessionID: sessionID) {
            finishSubtitleAudioInput(updatedCycle)
        }
    }

    private func finishSubtitleAudioInput(_ cycle: SubtitleCycle) {
        if config.interactionMode == .holdToTalk {
            NSLog("Subtitle audio input finished dev=VS-\(cycle.deviceID ?? "unknown") session=\(cycle.sessionID) -> device ready")
            clearActiveSubtitleSession(peripheralID: cycle.peripheralID, sessionID: cycle.sessionID)
            sendDisplayState("ready", to: cycle.peripheralID)
        } else {
            statusController.setStatus("Processing")
            sendDisplayState("thinking", to: cycle.peripheralID)
        }
    }

    private func sendOrBufferSubtitleOggChunk(_ chunk: Data, isLast: Bool, canStartASR: Bool, cycle: SubtitleCycle) {
        if cycle.asrStarted {
            cycle.asr.sendOggOpusChunk(chunk, isLast: isLast)
            return
        }
        cycle.bufferedOggChunks.append(chunk)
        guard canStartASR else { return }
        if !startSubtitleASRAndFlushBufferedChunks(cycle, lastChunkIsFinal: isLast) {
            finishSubtitleCycleWithError(
                peripheralID: cycle.peripheralID,
                sessionID: cycle.sessionID,
                message: cycle.asr.lastStartError ?? "Failed to start ASR"
            )
        }
    }

    private func startSubtitleASRAndFlushBufferedChunks(_ cycle: SubtitleCycle, lastChunkIsFinal: Bool) -> Bool {
        guard !cycle.asrStarted else { return true }
        let profile = outputProfile(for: cycle.deviceID)
        let useDefiniteSegments = shouldUseDefiniteSegments(for: profile)
        guard cycle.asr.start(options: ASRSessionOptions(
            hotwords: config.asrHotwords,
            resultType: useDefiniteSegments ? .single : .full,
            showUtterances: useDefiniteSegments,
            cloudTextProfile: cloudTextProfile(for: profile)
        )) else {
            cycle.bufferedOggChunks.removeAll(keepingCapacity: true)
            return false
        }
        cycle.asrStarted = true
        for bufferedChunk in cycle.bufferedOggChunks.dropLast() {
            cycle.asr.sendOggOpusChunk(bufferedChunk, isLast: false)
        }
        if let lastChunk = cycle.bufferedOggChunks.last {
            cycle.asr.sendOggOpusChunk(lastChunk, isLast: lastChunkIsFinal)
        }
        cycle.bufferedOggChunks.removeAll(keepingCapacity: true)
        return true
    }

    private func sendFinalOggChunkIfNeeded(recordingDuration: TimeInterval) {
        guard !sentFinalAudioChunk else { return }
        sentFinalAudioChunk = true
        cancelAudioEndTimeout()
        if !asrStarted && recordingDuration < minimumRecordingDuration {
            cancelShortRecording()
            return
        }
        if receivedAudioFrames == 0 {
            finishWithASRError("No audio frames from device")
            return
        }
        enterFinalizingState(reason: "final_audio_sent")

        let finalChunk = oggMuxer.finish()
        debugAudioRecorder.append(finalChunk)
        debugAudioRecorder.finish()
        sendOrBufferOggChunk(finalChunk, isLast: true, canStartASR: true)
    }

    private func shouldSendPartialToDevice() -> Bool {
        sentFinalAudioChunk
    }

    private var currentRecordingDuration: TimeInterval {
        guard let activeSessionStartedAt else { return 0 }
        return Date().timeIntervalSince(activeSessionStartedAt)
    }

    private func sendOrBufferOggChunk(_ chunk: Data, isLast: Bool, canStartASR: Bool) {
        if asrStarted {
            asr.sendOggOpusChunk(chunk, isLast: isLast)
            return
        }

        bufferedOggChunks.append(chunk)
        guard canStartASR else { return }
        if !startASRAndFlushBufferedChunks(lastChunkIsFinal: isLast) {
            finishWithASRError(asr.lastStartError ?? "Failed to start ASR")
        }
    }

    private func startASRAndFlushBufferedChunks(lastChunkIsFinal: Bool) -> Bool {
        guard !asrStarted else { return true }
        let profile = outputProfile(for: activeDeviceID)
        let useDefiniteSegments = shouldUseDefiniteSegments(for: profile)
        guard asr.start(options: ASRSessionOptions(
            hotwords: config.asrHotwords,
            resultType: useDefiniteSegments ? .single : .full,
            showUtterances: useDefiniteSegments,
            cloudTextProfile: cloudTextProfile(for: profile)
        )) else {
            bufferedOggChunks.removeAll(keepingCapacity: true)
            return false
        }

        asrStarted = true
        for bufferedChunk in bufferedOggChunks.dropLast() {
            asr.sendOggOpusChunk(bufferedChunk, isLast: false)
        }
        if let lastChunk = bufferedOggChunks.last {
            asr.sendOggOpusChunk(lastChunk, isLast: lastChunkIsFinal)
        }
        bufferedOggChunks.removeAll(keepingCapacity: true)
        return true
    }

    private func cancelShortRecording() {
        NSLog("Ignoring short recording under \(minimumRecordingDuration)s")
        cancelAudioEndTimeout()
        bufferedOggChunks.removeAll(keepingCapacity: true)
        asr.cancel()
        asrStarted = false
        sentFinalAudioChunk = false
        pastedFinalText = false
        debugAudioRecorder.discard()
        statusController.setStatus("Ready")
        statusController.hideOverlay()
        sendUIStateForActiveDevice("ready")
        mainInputState = .ready
    }

    private func finishWithFinalText(_ result: ASRFinalResult) {
        guard !pastedFinalText else { return }
        let text = result.text
        var profile = outputProfile(for: activeDeviceID)
        if result.handledByCloud { profile.transform = .original }
        if profile.target == .subtitle {
            pastedFinalText = true
            pendingPasteState = .idle
            let deviceID = activeDeviceID
            if !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                showSubtitleText(text, profile: profile, deviceID: deviceID)
            }
            finishRecognitionCycle()
            statusController.hideOverlay()
            statusController.setStatus("Ready")
            sendUIStateForActiveDevice("ready")
            mainInputState = .ready
            return
        }
        if text.isEmpty {
            pastedFinalText = true
            pendingPasteState = .idle
            finishRecognitionCycle()
            statusController.hideOverlay()
            statusController.setStatus("Ready")
            sendUIStateForActiveDevice("ready")
            mainInputState = .ready
            return
        }

        if profile.transform != .original {
            pastedFinalText = true
            if profile.transform == .polish {
                statusController.showPartial(text, deviceID: activeDeviceID)
            } else {
                statusController.setStatus("Translating")
            }
            let transformID = UUID()
            pendingTextTransformID = transformID
            transformText(text, profile: profile) { [weak self] result in
                guard let self, self.pendingTextTransformID == transformID else { return }
                self.pendingTextTransformID = nil
                switch result {
                case .success(let outputText):
                    self.enterPendingConfirmation(text: outputText)
                case .failure(let error):
                    self.finishWithASRError(error.localizedDescription)
                }
            }
            return
        }

        enterPendingConfirmation(text: text)
    }

    private func enterPendingConfirmation(text: String) {
        pastedFinalText = true
        let mode = ConfirmationMode.preferred
        if mode == .immediate {
            statusController.hideOverlay(deviceID: activeDeviceID)
            completePendingPaste(text: text)
            return
        }

        rememberInput(text)
        if mode == .manual {
            enterPausedConfirmation(text: text, peripheralID: activePeripheralID)
            return
        }

        let id = UUID()
        countdownPasteTarget = inputInjector.capturePasteTarget()
        pendingPasteState = .waitingToPaste(id: id, text: text)
        if let peripheralID = activePeripheralID {
            mainInputState = .pendingConfirmation(peripheralID: peripheralID)
            ble.stopAudioIn(for: peripheralID)
        }
        let duration = TimeInterval(mode.rawValue) / 1000
        statusController.showFinal(text, duration: duration, deviceID: activeDeviceID) { [weak self] in
            guard let self, case .waitingToPaste(let pendingID, let pendingText) = self.pendingPasteState,
                  pendingID == id
            else { return }
            self.completePendingPaste(text: pendingText)
        }
        sendUIStateForActiveDevice("pending_confirmation", text: text)
    }

    private func enterPausedConfirmation(text: String, peripheralID: UUID?) {
        let target = countdownPasteTarget ?? inputInjector.capturePasteTarget()
        mainInputState = .pausedConfirmation(peripheralID: peripheralID)
        let deviceID = activeDeviceID
        statusController.setStatus("Awaiting confirmation")
        let overlay = statusController.showConfirmation(
            text,
            deviceID: deviceID,
            shortcuts: confirmationShortcuts(for: peripheralID),
            onSend: { [weak self] useCurrentField in
                self?.sendEditedConfirmation(useCurrentField: useCurrentField)
            },
            onCancel: { [weak self] in
                self?.dismissPendingConfirmation()
            }
        )
        let id = UUID()
        pendingPasteState = .paused(id: id, target: target, overlay: overlay, deviceID: deviceID)
        if let peripheralID {
            ble.stopAudioIn(for: peripheralID)
            sendDisplayState("pending_confirmation", to: peripheralID)
            startShakeConfirmation(for: peripheralID)
        }
    }

    private func confirmationShortcuts(for peripheralID: UUID?) -> ConfirmationShortcuts {
        guard let peripheralID, ble.isConnected(peripheralID),
              let deviceID = ble.deviceID(for: peripheralID),
              let info = firmwareInfoByDeviceID[deviceID]
        else { return .none }
        return ConfirmationShortcuts(hardware: info.hardware, buttons: info.buttons)
    }

    private func refreshConfirmationShortcuts() {
        guard case .paused(_, _, let overlay, _) = pendingPasteState else { return }
        overlay.updateConfirmationShortcuts(confirmationShortcuts(for: activePeripheralID))
    }

    private func handleDefiniteSegment(_ segment: ASRSegment) {
        guard segment.definite, let deviceID = activeDeviceID else { return }
        let profile = outputProfile(for: deviceID)
        guard shouldUseDefiniteSegments(for: profile) else { return }
        statusController.hideOverlay(deviceID: deviceID)
        showSubtitleText(segment.text, profile: profile, deviceID: deviceID)
    }

    private func handleSubtitleDefiniteSegment(_ segment: ASRSegment, peripheralID: UUID) {
        guard segment.definite, let cycle = activeSubtitleCycle(peripheralID: peripheralID) else { return }
        let profile = outputProfile(for: cycle.deviceID)
        guard shouldUseDefiniteSegments(for: profile) else { return }
        statusController.hideOverlay(deviceID: cycle.deviceID)
        showSubtitleText(segment.text, profile: profile, deviceID: cycle.deviceID, cycle: cycle)
    }

    private func finishSubtitleCycleWithFinalText(peripheralID: UUID, sessionID: UInt32, result: ASRFinalResult) {
        guard let cycle = subtitleCycle(peripheralID: peripheralID, sessionID: sessionID),
              !cycle.finishedFinalText
        else { return }
        cycle.finishedFinalText = true
        let text = result.text
        NSLog("Subtitle final text dev=VS-\(cycle.deviceID ?? "unknown") session=\(sessionID) text_len=\(text.count)")
        var profile = outputProfile(for: cycle.deviceID)
        if result.handledByCloud { profile.transform = .original }
        if !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            showSubtitleText(text, profile: profile, deviceID: cycle.deviceID, cycle: cycle) { [weak self] didShowSubtitle in
                guard let self else { return }
                self.finishSubtitleCycle(
                    peripheralID: peripheralID,
                    sessionID: sessionID,
                    hideOverlay: didShowSubtitle &&
                        self.shouldHideOverlayForFinishedSubtitleCycle(
                            peripheralID: peripheralID,
                            sessionID: sessionID
                        )
                )
            }
            return
        }
        finishSubtitleCycle(
            peripheralID: peripheralID,
            sessionID: sessionID,
            hideOverlay: shouldHideOverlayForFinishedSubtitleCycle(peripheralID: peripheralID, sessionID: sessionID)
        )
    }

    private func finishSubtitleCycleWithError(peripheralID: UUID, sessionID: UInt32, message: String) {
        guard let cycle = subtitleCycle(peripheralID: peripheralID, sessionID: sessionID) else { return }
        NSLog("ASR error VS-\(cycle.deviceID ?? "unknown"): \(message)")
        cycle.asr.cancel()
        cycle.debugAudioRecorder.discard()
        cancelSubtitleAudioEndTimeout(cycle)
        clearActiveSubtitleSession(peripheralID: peripheralID, sessionID: sessionID)
        if !hasActiveSubtitleSession(peripheralID: peripheralID) {
            statusController.showError(message, deviceID: cycle.deviceID) { [weak self] in
                self?.sendDisplayState("ready", to: peripheralID)
            }
        }
        subtitleCycles.removeValue(forKey: SubtitleCycleKey(peripheralID: peripheralID, sessionID: sessionID))
    }

    private func cancelSubtitleCycle(peripheralID: UUID, reason: String) {
        guard let cycle = activeSubtitleCycle(peripheralID: peripheralID) else { return }
        NSLog("Cancel subtitle cycle VS-\(cycle.deviceID ?? "unknown") reason=\(reason)")
        cycle.asr.cancel()
        cycle.debugAudioRecorder.discard()
        cancelSubtitleAudioEndTimeout(cycle)
        statusController.hideOverlay(deviceID: cycle.deviceID)
        sendDisplayState("ready", to: peripheralID)
        clearActiveSubtitleSession(peripheralID: peripheralID, sessionID: cycle.sessionID)
        subtitleCycles.removeValue(
            forKey: SubtitleCycleKey(peripheralID: peripheralID, sessionID: cycle.sessionID)
        )
    }

    private func finishSubtitleCycle(peripheralID: UUID, sessionID: UInt32, hideOverlay: Bool) {
        guard let cycle = subtitleCycle(peripheralID: peripheralID, sessionID: sessionID) else { return }
        NSLog("Subtitle cycle finish dev=VS-\(cycle.deviceID ?? "unknown") session=\(sessionID) hide_overlay=\(hideOverlay) active=\(activeSubtitleSessions[peripheralID].map(String.init) ?? "nil")")
        if hideOverlay {
            statusController.hideOverlay(deviceID: cycle.deviceID)
        }
        clearActiveSubtitleSession(peripheralID: peripheralID, sessionID: sessionID)
        if !hasActiveSubtitleSession(peripheralID: peripheralID) {
            statusController.setStatus("Ready")
            sendDisplayState("ready", to: peripheralID)
        }
        subtitleCycles.removeValue(forKey: SubtitleCycleKey(peripheralID: peripheralID, sessionID: sessionID))
    }

    private func shouldHideOverlayForFinishedSubtitleCycle(peripheralID: UUID, sessionID: UInt32) -> Bool {
        isActiveSubtitleCycle(peripheralID: peripheralID, sessionID: sessionID) ||
            !hasActiveSubtitleSession(peripheralID: peripheralID)
    }

    private func canUpdateOverlayForSubtitleCycle(peripheralID: UUID, sessionID: UInt32) -> Bool {
        activeSubtitleSessions[peripheralID].map { $0 == sessionID } ?? true
    }

    private func shouldSendSubtitlePartialToDevice(_ cycle: SubtitleCycle) -> Bool {
        guard isActiveSubtitleCycle(peripheralID: cycle.peripheralID, sessionID: cycle.sessionID) else {
            return false
        }
        return cycle.sentFinalAudioChunk
    }

    private func subtitleCycle(peripheralID: UUID, sessionID: UInt32) -> SubtitleCycle? {
        subtitleCycles[SubtitleCycleKey(peripheralID: peripheralID, sessionID: sessionID)]
    }

    private func activeSubtitleCycle(peripheralID: UUID) -> SubtitleCycle? {
        guard let sessionID = activeSubtitleSessions[peripheralID] else { return nil }
        return subtitleCycle(peripheralID: peripheralID, sessionID: sessionID)
    }

    private func isActiveSubtitleCycle(peripheralID: UUID, sessionID: UInt32) -> Bool {
        activeSubtitleSessions[peripheralID] == sessionID
    }

    private func hasActiveSubtitleSession(peripheralID: UUID) -> Bool {
        activeSubtitleSessions[peripheralID] != nil
    }

    private func clearActiveSubtitleSession(peripheralID: UUID, sessionID: UInt32) {
        if activeSubtitleSessions[peripheralID] == sessionID {
            activeSubtitleSessions.removeValue(forKey: peripheralID)
        }
    }

    private func cancelSubtitleCycles(peripheralID: UUID, reason: String) {
        NSLog("Cancel subtitle cycles \(peripheralID) reason=\(reason)")
        activeSubtitleSessions.removeValue(forKey: peripheralID)
        let keys = subtitleCycles.keys.filter { $0.peripheralID == peripheralID }
        for key in keys {
            guard let cycle = subtitleCycles[key] else { continue }
            cycle.asr.cancel()
            cycle.debugAudioRecorder.discard()
            cancelSubtitleAudioEndTimeout(cycle)
            subtitleCycles.removeValue(forKey: key)
        }
        statusController.hideOverlay(deviceID: deviceID(for: peripheralID))
        sendDisplayState("ready", to: peripheralID)
    }

    private func showSubtitleText(
        _ text: String,
        profile: OutputProfile,
        deviceID: String?,
        cycle: SubtitleCycle? = nil,
        completion: ((Bool) -> Void)? = nil
    ) {
        guard let deviceID else {
            completion?(false)
            return
        }
        transformText(text, profile: profile) { [weak self] result in
            guard let self else { return }
            if let cycle,
               self.subtitleCycle(peripheralID: cycle.peripheralID, sessionID: cycle.sessionID) !== cycle {
                return
            }
            switch result {
            case .success(let outputText):
                NSLog("Subtitle show text dev=VS-\(deviceID) text_len=\(outputText.count)")
                self.subtitleController.show(
                    text: outputText,
                    deviceID: deviceID,
                    color: self.config.themeColor(for: deviceID)
                )
                completion?(true)
            case .failure(let error):
                self.statusController.showError(error.localizedDescription, deviceID: deviceID)
                completion?(false)
            }
        }
    }

    private func transformText(
        _ text: String,
        profile: OutputProfile,
        completion: @escaping (Result<String, Error>) -> Void
    ) {
        guard profile.transform != .original else {
            completion(.success(text))
            return
        }
        textClient.transform(
            text,
            profile: profile,
            hotwords: config.asrHotwords
        ) { result in
            DispatchQueue.main.async {
                if case .failure(let error) = result, profile.transform == .polish {
                    NSLog("Text polishing failed: \(error.localizedDescription)")
                    completion(.success(text))
                } else {
                    completion(result)
                }
            }
        }
    }

    private func finishWithASRError(_ message: String, partialText: String = "") {
        NSLog("ASR error: \(message)")
        finishRecognitionCycle()
        asr.cancel()
        pendingPasteState = .idle
        debugAudioRecorder.discard()
        if let peripheralID = activePeripheralID, outputProfile(for: activeDeviceID).target == .focusedApp,
           !partialText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            enterPausedConfirmation(text: partialText, peripheralID: peripheralID)
            return
        }
        isShowingASRError = true
        errorRecoveryToken += 1
        let token = errorRecoveryToken
        let errorPeripheralID = activePeripheralID
        mainInputState = .error(peripheralID: errorPeripheralID)
        sendUIStateForActiveDevice("error", text: message)
        statusController.showError(message, deviceID: activeDeviceID) { [weak self] in
            guard let self, self.errorRecoveryToken == token else { return }
            self.recoverFromASRError(hideOverlay: false)
        }
    }

    private func presentASRUpgradeAlert(url: URL, message: String) {
        statusController.hideOverlay { [weak self] in
            guard let self else { return }
            self.recoverFromASRError(hideOverlay: false)
            NSApp.activate(ignoringOtherApps: true)

            let alert = NSAlert()
            alert.alertStyle = .warning
            alert.messageText = "VoiceStick Cloud needs attention"
            alert.informativeText = message
            alert.addButton(withTitle: "Open")
            alert.addButton(withTitle: "Cancel")
            if alert.runModal() == .alertFirstButtonReturn {
                NSWorkspace.shared.open(url)
            }
        }
    }

    private func recoverFromASRError(hideOverlay: Bool = true) {
        guard isShowingASRError else { return }
        errorRecoveryToken += 1
        isShowingASRError = false
        if hideOverlay {
            statusController.hideOverlay()
        }
        if pairedDeviceIDs.isEmpty {
            statusController.setStatus("Pair a VoiceStick")
        } else {
            statusController.setStatus("Ready")
            sendUIStateForActiveDevice("ready")
            mainInputState = .ready
        }
    }

    private func sendEditedConfirmation(useCurrentField: Bool = false) {
        guard case .paused(let id, let target, let overlay, _) = pendingPasteState, !isSendingConfirmation
        else { return }
        let selectedApplication = useCurrentField ? inputInjector.frontmostPasteApplication() : nil
        let needsFocusRestoration = overlay.confirmationTookKeyboardFocus
        guard let text = overlay.takeDraftForSending() else { return }
        isSendingConfirmation = true
        let complete: (InputInjector.PasteTarget?) -> Void = { [weak self] destination in
            guard let self, case .paused(let pendingID, _, let overlay, _) = self.pendingPasteState,
                  pendingID == id
            else { return }
            self.isSendingConfirmation = false
            guard let destination,
                  self.inputInjector.paste(
                    text: text, pressEnter: !useCurrentField && self.config.autoEnter, target: destination
                  )
            else {
                overlay.showConfirmationSendFailure()
                if let peripheralID = self.activePeripheralID {
                    self.sendDisplayState("pending_confirmation", to: peripheralID)
                    self.startShakeConfirmation(for: peripheralID)
                }
                return
            }
            self.finishPendingPaste(text: text)
        }
        if let selectedApplication {
            // Let the nonactivating panel relinquish keyboard focus before querying the editor.
            DispatchQueue.main.async { [weak self] in
                complete(self?.inputInjector.selectedPasteTarget(in: selectedApplication))
            }
        } else if !useCurrentField, !needsFocusRestoration {
            complete(target?.windowTarget)
        } else if !useCurrentField, let target {
            inputInjector.restoreFocus(to: target, completion: complete)
        } else {
            complete(nil)
        }
    }

    private func completePendingPaste(text: String) {
        let shouldPressEnter = config.autoEnter
        finishPendingPaste(text: text)
        inputInjector.paste(text: text, pressEnter: shouldPressEnter)
    }

    private func finishPendingPaste(text: String) {
        let peripheralID = activePeripheralID
        stopShakeConfirmation()
        rememberInput(text)
        pendingPasteState = .idle
        finishRecognitionCycle()
        statusController.setStatus("Ready")
        mainInputState = .ready
        if let peripheralID {
            restoreVoiceMode(for: peripheralID)
        }
    }

    private func rememberInput(_ text: String) {
        lastRecoverableText = text
        lastRecoverablePeripheralID = activePeripheralID
        statusController.setHasRecoverableInput(true)
    }

    private var isWaitingForFinalText: Bool {
        mainInputState.isFinalizing &&
            sentFinalAudioChunk &&
            asrStarted &&
            pendingPasteState.isIdle &&
            !pastedFinalText
    }

    func restoreLastInputConfirmation() -> Bool {
        restoreLastInputConfirmation(peripheralID: lastRecoverablePeripheralID)
    }

    private func restoreLastInputConfirmation(peripheralID: UUID?) -> Bool {
        guard
            pendingPasteState.isIdle,
            !mainInputState.isBusy,
            !isWaitingForFinalText,
            let text = lastRecoverableText,
            !text.isEmpty
        else {
            return false
        }

        enterPausedConfirmation(text: text, peripheralID: peripheralID)
        return true
    }

    private func handleFrontButtonDuringPendingPaste(peripheralID: UUID) -> Bool {
        switch pendingPasteState {
        case .idle:
            return false
        case .waitingToPaste(_, let text):
            guard activePeripheralID == peripheralID else { return true }
            enterPausedConfirmation(text: text, peripheralID: peripheralID)
            return true
        case .paused:
            guard activePeripheralID == peripheralID else { return true }
            sendEditedConfirmation()
            return true
        }
    }

    private func cancelPendingPaste(peripheralID: UUID) {
        if activeSessionID != nil {
            if activePeripheralID == peripheralID {
                cancelRecognitionInProgress()
            }
            return
        }
        if isWaitingForFinalText || pendingTextTransformID != nil {
            guard activePeripheralID == peripheralID else { return }
            cancelRecognitionInProgress()
            return
        }
        if pendingPasteState.isIdle {
            _ = restoreLastInputConfirmation(peripheralID: peripheralID)
            return
        }
        guard activePeripheralID == peripheralID else { return }
        dismissPendingConfirmation()
    }

    private func dismissPendingConfirmation() {
        guard !pendingPasteState.isIdle else { return }
        let peripheralID = activePeripheralID
        stopShakeConfirmation()
        if case .waitingToPaste = pendingPasteState {
            statusController.hideOverlay(deviceID: activeDeviceID)
        }
        pendingPasteState = .idle
        finishRecognitionCycle()
        statusController.setStatus("Ready")
        mainInputState = .ready
        if let peripheralID {
            restoreVoiceMode(for: peripheralID)
        }
    }

    private func cancelRecognitionInProgress() {
        let peripheralID = activePeripheralID
        stopShakeConfirmation()
        cancelAudioEndTimeout()
        asr.cancel()
        pendingPasteState = .idle
        finishRecognitionCycle()
        statusController.hideOverlay()
        statusController.setStatus("Ready")
        mainInputState = .ready
        if let peripheralID {
            restoreVoiceMode(for: peripheralID)
        }
    }

    private func cancelActiveCycleIfDeviceDisconnected() {
        let disconnectedSubtitleKeys = subtitleCycles.keys.filter { !ble.isConnected($0.peripheralID) }
        for key in disconnectedSubtitleKeys {
            guard let cycle = subtitleCycles[key] else { continue }
            cycle.asr.cancel()
            cycle.debugAudioRecorder.discard()
            statusController.hideOverlay(deviceID: cycle.deviceID)
            clearActiveSubtitleSession(peripheralID: key.peripheralID, sessionID: key.sessionID)
            subtitleCycles.removeValue(forKey: key)
        }
        guard let activePeripheralID, !ble.isConnected(activePeripheralID) else { return }
        if case .pausedConfirmation = mainInputState { return }
        if waitingForAudioEnd {
            sendFinalOggChunkIfNeeded(recordingDuration: currentRecordingDuration)
            return
        }
        asr.cancel()
        pendingPasteState = .idle
        mainInputState = .ready
        debugAudioRecorder.discard()
        finishRecognitionCycle()
        statusController.hideOverlay()
        subtitleController.hideAll()
    }

    private func finishRecognitionCycle() {
        cancelAudioEndTimeout()
        pendingTextTransformID = nil
        asrStarted = false
        sentFinalAudioChunk = false
        pastedFinalText = false
        bufferedOggChunks.removeAll(keepingCapacity: true)
        countdownPasteTarget = nil
    }

    private func updateDeviceFirmwareInfo(
        hardware: String?,
        firmwareVersion: String?,
        buttons: [String]?,
        requiresUpgrade: Bool,
        peripheralID: UUID
    ) {
        guard let deviceID = ble.deviceID(for: peripheralID) else { return }
        var info = firmwareInfoByDeviceID[deviceID] ?? DeviceFirmwareInfo()
        info.requiresInputFirmware = requiresUpgrade
        if hardware != nil, firmwareVersion != nil, ble.isConnected(peripheralID) {
            if firmwareReportingPeripherals[deviceID] != peripheralID || info.requiresInputFirmware {
                info.buttons = nil
            }
            firmwareReportingPeripherals[deviceID] = peripheralID
        }
        if let hardware {
            info.hardware = hardware
        }
        if let buttons {
            info.buttons = buttons
        }
        if let firmwareVersion {
            info.currentVersion = firmwareVersion
        }
        firmwareInfoByDeviceID[deviceID] = info
        if let hardware = info.hardware, let url = AppConfig.firmwareManifestURLs[hardware] {
            checkFirmwareFeed(for: hardware, at: url, force: false)
        }
        refreshFirmwareAvailability()
    }

    private func startFirmwareManifestRefreshTimer() {
        firmwareManifestRefreshTimer?.invalidate()
        firmwareManifestRefreshTimer = Timer.scheduledTimer(withTimeInterval: 60 * 60, repeats: true) { [weak self] _ in
            self?.checkFirmwareUpdatesIfNeeded(force: false)
        }
    }

    private func checkFirmwareUpdatesIfNeeded(force: Bool) {
        for (hardware, url) in AppConfig.firmwareManifestURLs {
            checkFirmwareFeed(for: hardware, at: url, force: force)
        }
        refreshFirmwareAvailability()
    }

    private func checkFirmwareFeed(for hardware: String, at url: URL, force: Bool,
                                   completion: FirmwareFeedCompletion? = nil) {
        var feed = firmwareFeeds[hardware] ?? FirmwareFeed()
        if let completion {
            feed.completions.append(completion)
        }
        if feed.isChecking {
            firmwareFeeds[hardware] = feed
            return
        }
        if !force, let lastChecked = feed.lastChecked,
           Date().timeIntervalSince(lastChecked) < firmwareManifestCacheDuration {
            return
        }
        feed.isChecking = true
        feed.errorMessage = nil
        firmwareFeeds[hardware] = feed
        firmwareManifestClient.fetchManifest(at: url, for: hardware) { [weak self] result in
            DispatchQueue.main.async {
                guard let self, var feed = self.firmwareFeeds[hardware] else { return }
                feed.isChecking = false
                let completions = feed.completions
                feed.completions.removeAll()
                switch result {
                case .success(let manifest):
                    feed.manifest = manifest
                    feed.lastChecked = Date()
                case .failure(let error):
                    NSLog("Firmware manifest check failed for \(hardware): \(error.localizedDescription)")
                    feed.errorMessage = error.localizedDescription
                    if case FirmwareManifestClient.FirmwareManifestError.appUpdateRequired = error {
                        feed.manifest = nil
                        feed.lastChecked = nil
                    }
                }
                self.firmwareFeeds[hardware] = feed
                self.refreshFirmwareAvailability()
                completions.forEach { $0(result) }
            }
        }
    }

    private func refreshFirmwareAvailability() {
        for (deviceID, var info) in firmwareInfoByDeviceID {
            let feed = info.hardware.flatMap { firmwareFeeds[$0] }
            info.isChecking = feed?.isChecking ?? false
            info.errorMessage = feed?.errorMessage
            info.latestVersion = nil
            info.updateAvailable = false
            guard let manifest = feed?.manifest, let currentVersion = info.currentVersion else {
                firmwareInfoByDeviceID[deviceID] = info
                continue
            }
            info.latestVersion = manifest.version
            info.updateAvailable = FirmwareVersion.isVersion(currentVersion, olderThan: manifest.version) &&
                (!info.requiresInputFirmware || FirmwareVersion.meetsMinimum(
                    AppConfig.minimumCompatibleFirmwareVersion, current: manifest.version
                ))
            firmwareInfoByDeviceID[deviceID] = info
            NSLog("Firmware availability VS-\(deviceID) hardware=\(manifest.hardware) current=\(currentVersion) latest=\(manifest.version) update=\(info.updateAvailable)")
        }
        statusController.setFirmwareInfo(firmwareInfoByDeviceID)
        onFirmwareAvailabilityChanged?()
        scheduleFirmwareUpdatePrompt()
    }

    private func scheduleFirmwareUpdatePrompt() {
        guard !firmwareUpdatePromptScheduled else { return }
        firmwareUpdatePromptScheduled = true
        // Connection and input callbacks may still be updating the device state.
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.firmwareUpdatePromptScheduled = false
            self.showFirmwareUpdatePromptIfReady()
        }
    }

    private func showFirmwareUpdatePromptIfReady() {
        guard !mainInputState.isBusy, subtitleCycles.isEmpty, !isFirmwareUpdateWindowOpen,
              !isFirmwareRequirementWindowOpen, !isShowingFirmwareUpdatePrompt, NSApp.modalWindow == nil else { return }

        if let onFirmwareRequirement {
            for deviceID in pairedDeviceIDs {
                guard firmwareRequirement(for: deviceID) != nil,
                      let peripheralID = firmwareReportingPeripherals[deviceID],
                      shownFirmwareRequirements.insert(peripheralID).inserted else { continue }
                onFirmwareRequirement(deviceID)
                return
            }
        }

        guard let onFirmwareUpdatePrompt else { return }

        for deviceID in pairedDeviceIDs {
            guard let info = firmwareInfoByDeviceID[deviceID], !info.requiresInputFirmware, info.updateAvailable,
                  !info.isChecking, info.errorMessage == nil,
                  let currentVersion = info.currentVersion, let latestVersion = info.latestVersion,
                  let peripheralID = firmwareReportingPeripherals[deviceID], ble.isConnected(peripheralID),
                  recordFirmwareReminder(for: peripheralID, version: latestVersion) else { continue }

            isShowingFirmwareUpdatePrompt = true
            onFirmwareUpdatePrompt(deviceID, currentVersion, latestVersion)
            isShowingFirmwareUpdatePrompt = false
            scheduleFirmwareUpdatePrompt()
            return
        }
    }

    @discardableResult
    private func recordFirmwareReminder(for peripheralID: UUID, version: String) -> Bool {
        let defaults = UserDefaults.standard
        let key = Self.firmwareReminderHistoryKey
        var history = defaults.dictionary(forKey: key) as? [String: [String]] ?? [:]
        let deviceKey = peripheralID.uuidString
        guard history[deviceKey]?.contains(version) != true else { return false }
        history[deviceKey, default: []].append(version)
        defaults.set(history, forKey: key)
        return true
    }

    private var activeSessionID: UInt32? {
        mainInputState.sessionID
    }

    private var activePeripheralID: UUID? {
        mainInputState.peripheralID
    }

    private var activeSessionStartedAt: Date? {
        mainInputState.startedAt
    }

    private func sendUIStateForActiveDevice(_ state: String, text: String = "") {
        sendDisplayState(state, text: text, to: activePeripheralID)
    }

    private var activeDeviceID: String? {
        activePeripheralID.flatMap { ble.deviceID(for: $0) }
    }

    private func outputProfile(for deviceID: String?) -> OutputProfile {
        config.outputProfile(for: deviceID)
    }

    private func shouldUseDefiniteSegments(for profile: OutputProfile) -> Bool {
        profile.target == .subtitle && profile.transform != .polish && cloudTextProfile(for: profile) == nil &&
            config.interactionMode == .clickToTalk
    }

    private func cloudTextProfile(for profile: OutputProfile) -> OutputProfile? {
        guard config.asrProvider == .voiceStickCloud, profile.transform != .original else { return nil }
        return profile
    }

    private func deviceID(for peripheralID: UUID) -> String? {
        ble.deviceID(for: peripheralID)
    }
}
