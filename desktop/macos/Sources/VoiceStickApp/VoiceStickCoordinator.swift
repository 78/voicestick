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
            self.asr = ASRWebSocketClient(config: config)
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
    private var translator: LLMTranslationClient
    private let subtitleController = SubtitleController()
    private let oggMuxer = OggOpusMuxer(sampleRate: 16_000, channels: 1)
    private let inputInjector = InputInjector()
    private let firmwareManifestClient = FirmwareManifestClient()
    private var debugAudioRecorder: DebugAudioRecorder
    private let minimumRecordingDuration: TimeInterval = 0.5
    private let audioEndTimeout: TimeInterval = 1.0
    private let firmwareManifestCacheDuration: TimeInterval = 24 * 60 * 60

    private var mainInputState = MainInputState.ready {
        didSet {
            if !mainInputState.isBusy { scheduleFirmwareUpdatePrompt() }
        }
    }
    private var receivedAudioFrames = 0
    private var bufferedOggChunks: [Data] = []
    private var asrStarted = false
    private var sentFinalAudioChunk = false
    private var pastedFinalText = false
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
    private var errorRecoveryToken = 0
    private var isShowingASRError = false
    private var subtitleCycles: [SubtitleCycleKey: SubtitleCycle] = [:] {
        didSet {
            if subtitleCycles.isEmpty { scheduleFirmwareUpdatePrompt() }
        }
    }
    private var activeSubtitleSessions: [UUID: UInt32] = [:]
    var isFirmwareUpdateWindowOpen = false {
        didSet {
            if !isFirmwareUpdateWindowOpen { scheduleFirmwareUpdatePrompt() }
        }
    }
    var onFirmwareUpdatePrompt: ((String, String, String, Bool) -> Void)?

    init(config: AppConfig, statusController: StatusController) {
        self.config = config
        self.statusController = statusController
        self.pairedDeviceIDs = config.pairedDeviceIDs
        self.ble = BleCentral(pairedDeviceIDs: config.pairedDeviceIDs)
        self.asr = ASRWebSocketClient(config: config)
        self.translator = LLMTranslationClient(config: config)
        self.debugAudioRecorder = DebugAudioRecorder(
            enabled: config.debugAudioCache,
            directory: config.debugAudioDirectory
        )
    }

    func start() {
        ble.onConnectionChange = { [weak self] connectedDevices in
            guard let self else { return }
            self.statusController.setConnectedDevices(connectedDevices)
            self.firmwareReportingPeripherals = self.firmwareReportingPeripherals.filter {
                self.ble.isConnected($0.value)
            }
            self.cancelActiveCycleIfDeviceDisconnected()
            self.refreshFirmwareAvailability()
            self.refreshConfirmationShortcuts()
            if case .pausedConfirmation = self.mainInputState {
                self.statusController.setStatus("Awaiting confirmation")
            } else {
                self.statusController.setStatus(self.pairedDeviceIDs.isEmpty ? "Pair a VoiceStick" : "Ready")
            }
            if !connectedDevices.isEmpty {
                self.ble.sendInteractionMode(self.config.interactionMode)
            }
        }

        ble.onControlReady = { [weak self] peripheralID in
            guard let self, self.activePeripheralID == peripheralID,
                  case .paused(let id, _, _, _) = self.pendingPasteState
            else { return }
            self.ble.sendUIState("pending_confirmation", confirmationID: id, to: peripheralID)
        }

        ble.onStateEvent = { [weak self] peripheralID, event in
            self?.handleStateEvent(event, peripheralID: peripheralID)
        }

        ble.onAudioFrame = { [weak self] peripheralID, frame in
            self?.handleAudioFrame(frame, peripheralID: peripheralID)
        }

        configureASRCallbacks()
        ble.start()
        checkFirmwareUpdatesIfNeeded(force: false)
        startFirmwareManifestRefreshTimer()
    }

    deinit {
        audioEndTimeoutTimer?.invalidate()
        firmwareManifestRefreshTimer?.invalidate()
    }

    func updateConfig(_ config: AppConfig) {
        let wasRecognizing = asrStarted || mainInputState.isBusy || isWaitingForFinalText || !subtitleCycles.isEmpty
        if wasRecognizing {
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
            mainInputState = .ready
            pendingPasteState = .idle
            debugAudioRecorder.discard()
            statusController.hideOverlay()
            subtitleController.hideAll()
            sendUIStateForActiveDevice("ready")
            finishRecognitionCycle()
        }

        self.config = config
        ble.sendInteractionMode(config.interactionMode)
        debugAudioRecorder = DebugAudioRecorder(
            enabled: config.debugAudioCache,
            directory: config.debugAudioDirectory
        )
        asr = ASRWebSocketClient(config: config)
        translator = LLMTranslationClient(config: config)
        configureASRCallbacks()

        if pairedDeviceIDs != config.pairedDeviceIDs {
            updatePairedDeviceIDs(config.pairedDeviceIDs)
        } else if isShowingASRError {
            recoverFromASRError()
        } else if wasRecognizing {
            statusController.setStatus("Ready")
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

        asr.onFinal = { [weak self] text in
            guard let self, self.asrStarted else { return }
            self.finishWithFinalText(text)
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
                self.ble.sendUIState("thinking", text: text, to: peripheralID)
            }
        }
        cycle.asr.onSegment = { [weak self] segment in
            guard self?.isActiveSubtitleCycle(peripheralID: peripheralID, sessionID: sessionID) == true else {
                return
            }
            self?.handleSubtitleDefiniteSegment(segment, peripheralID: peripheralID)
        }
        cycle.asr.onFinal = { [weak self] text in
            self?.finishSubtitleCycleWithFinalText(
                peripheralID: peripheralID,
                sessionID: sessionID,
                text: text
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

    func checkFirmwareUpdatesNow(for deviceID: String,
                                 completion: @escaping (Result<(current: String, latest: String), Error>) -> Void) {
        guard let info = firmwareInfoByDeviceID[deviceID], let current = info.currentVersion,
              let hardware = info.hardware, let url = AppConfig.firmwareManifestURLs[hardware] else {
            completion(.failure(FirmwareManifestClient.FirmwareManifestError.deviceInfoUnavailable))
            return
        }
        let reportingPeripheral = firmwareReportingPeripherals[deviceID]
        checkFirmwareFeed(for: hardware, at: url, force: true) { [weak self] result in
            if case .success(let manifest) = result, let reportingPeripheral,
               FirmwareVersion.isVersion(current, olderThan: manifest.version) {
                _ = self?.recordFirmwareReminder(for: reportingPeripheral, version: manifest.version)
            }
            completion(result.map { (current, $0.version) })
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

    private func handleStateEvent(_ event: StateEvent, peripheralID: UUID) {
        switch event.event {
        case "device_info":
            if let hardware = event.hardware, let firmwareVersion = event.firmwareVersion {
                NSLog("Connected VoiceStick hardware=\(hardware) firmware=\(firmwareVersion)")
            }
            updateDeviceFirmwareInfo(event: event, peripheralID: peripheralID)
            if activePeripheralID == peripheralID {
                refreshConfirmationShortcuts()
            }
        case "battery_status":
            guard ble.isConnected(peripheralID), let deviceID = ble.deviceID(for: peripheralID),
                  let status = event.batteryStatus else { return }
            statusController.setBatteryStatus(status, for: deviceID)
        case "button_down":
            handleButtonDown(event, peripheralID: peripheralID)
        case "button_up":
            handleButtonUp(event, peripheralID: peripheralID)
        case "button_click":
            handleButtonClick(event, peripheralID: peripheralID)
        case "confirmation_shake":
            guard activePeripheralID == peripheralID, !isSendingConfirmation,
                  case .paused(let id, _, _, _) = pendingPasteState, event.confirmationID == id
            else { return }
            NSLog("Shake cancelled paused confirmation")
            dismissPendingConfirmation()
        default:
            break
        }
    }

    private func handleButtonDown(_ event: StateEvent, peripheralID: UUID) {
        NSLog("Button down button=\(event.button ?? "nil") dev=VS-\(deviceID(for: peripheralID) ?? "unknown") session=\(event.sessionID.map(String.init) ?? "nil")")
        switch event.button {
        case "primary":
            handlePrimaryButtonDown(sessionID: event.sessionID, peripheralID: peripheralID)
        case "secondary":
            break
        default:
            break
        }
    }

    private func handleButtonUp(_ event: StateEvent, peripheralID: UUID) {
        NSLog("Button up button=\(event.button ?? "nil") dev=VS-\(deviceID(for: peripheralID) ?? "unknown") session=\(event.sessionID.map(String.init) ?? "nil") duration_ms=\(event.durationMs.map(String.init) ?? "nil")")
        switch event.button {
        case "primary":
            handlePrimaryButtonUp(peripheralID: peripheralID)
        case "secondary":
            handleSecondaryButtonClick(peripheralID: peripheralID)
        default:
            break
        }
    }

    private func handleButtonClick(_ event: StateEvent, peripheralID: UUID) {
        NSLog("Button click button=\(event.button ?? "nil") dev=VS-\(deviceID(for: peripheralID) ?? "unknown") session=\(event.sessionID.map(String.init) ?? "nil") duration_ms=\(event.durationMs.map(String.init) ?? "nil")")
        switch event.button {
        case "primary":
            if handleFrontButtonDuringPendingPaste(peripheralID: peripheralID) {
                return
            }
            guard config.interactionMode == .clickToTalk else {
                ble.sendUIState("ready", to: peripheralID)
                return
            }
            if case .recording(_, let recordingPeripheralID, _) = mainInputState,
               recordingPeripheralID == peripheralID {
                handlePrimaryButtonUp(peripheralID: peripheralID)
            } else if case .finalizing(_, let finalizingPeripheralID, _) = mainInputState,
                      finalizingPeripheralID == peripheralID {
                NSLog("Ignoring primary button click while recording is finalizing")
                ble.sendUIState("thinking", to: peripheralID)
            } else {
                handlePrimaryButtonDown(sessionID: event.sessionID, peripheralID: peripheralID)
            }
        case "secondary":
            handleSecondaryButtonClick(peripheralID: peripheralID)
        default:
            break
        }
    }

    private func handleSecondaryButtonClick(peripheralID: UUID) {
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
                ble.sendUIState("ready", to: peripheralID)
            } else if mainInputState.isFinalizing || isWaitingForFinalText {
                ble.sendUIState("thinking", to: peripheralID)
            }
            NSLog("Ignoring primary button while main input is busy")
            return
        }
        guard let sessionID, sessionID != 0 else {
            NSLog("Ignoring primary button down with missing/zero session; sending ready")
            ble.sendUIState("ready", to: peripheralID)
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

    private func handleAudioFrame(_ frame: AudioFrame, peripheralID: UUID) {
        if subtitleCycle(peripheralID: peripheralID, sessionID: frame.sessionID) != nil {
            handleSubtitleAudioFrame(frame, peripheralID: peripheralID)
            return
        }
        guard frame.sessionID == activeSessionID, activePeripheralID == peripheralID else { return }
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
        guard activeSessionID == frame.sessionID else { return }
        if frame.isEnd {
            cancelAudioEndTimeout()
            let recordingDuration = currentRecordingDuration
            sentFinalAudioChunk = true
            enterFinalizingState(reason: "audio_end")
            if !asrStarted && recordingDuration < minimumRecordingDuration {
                cancelShortRecording()
            } else if asrStarted {
                debugAudioRecorder.finish()
                statusController.setStatus("Processing")
                sendUIStateForActiveDevice("thinking")
            } else {
                debugAudioRecorder.finish()
                if !startASRAndFlushBufferedChunks(lastChunkIsFinal: true) {
                    finishWithASRError(asr.lastStartError ?? "Failed to start ASR")
                    return
                }
                statusController.setStatus("Processing")
                sendUIStateForActiveDevice("thinking")
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
            ble.sendUIState("ready", to: peripheralID)
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
        ble.sendUIState("recording", to: peripheralID)
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

    private func handleSubtitleAudioFrame(_ frame: AudioFrame, peripheralID: UUID) {
        guard let cycle = subtitleCycle(peripheralID: peripheralID, sessionID: frame.sessionID) else { return }
        if frame.isEnd && frame.payload.isEmpty {
            cancelSubtitleAudioEndTimeout(cycle)
            sendSubtitleFinalOggChunkIfNeeded(peripheralID: peripheralID, sessionID: frame.sessionID)
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
            ble.sendUIState("thinking", to: cycle.peripheralID)
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
        let finalChunk = cycle.oggMuxer.finish()
        cycle.debugAudioRecorder.append(finalChunk)
        cycle.debugAudioRecorder.finish()
        sendOrBufferSubtitleOggChunk(finalChunk, isLast: true, canStartASR: true, cycle: cycle)
    }

    private func finishSubtitleAudioInput(_ cycle: SubtitleCycle) {
        if config.interactionMode == .holdToTalk {
            NSLog("Subtitle audio input finished dev=VS-\(cycle.deviceID ?? "unknown") session=\(cycle.sessionID) -> device ready")
            clearActiveSubtitleSession(peripheralID: cycle.peripheralID, sessionID: cycle.sessionID)
            ble.sendUIState("ready", to: cycle.peripheralID)
        } else {
            statusController.setStatus("Processing")
            ble.sendUIState("thinking", to: cycle.peripheralID)
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
        let useDefiniteSegments = shouldUseDefiniteSegments(for: outputProfile(for: cycle.deviceID))
        guard cycle.asr.start(options: ASRSessionOptions(
            hotwords: config.asrHotwords,
            resultType: useDefiniteSegments ? .single : .full,
            showUtterances: useDefiniteSegments
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
        enterFinalizingState(reason: "final_audio_sent")
        if !asrStarted && recordingDuration < minimumRecordingDuration {
            cancelShortRecording()
            return
        }
        if receivedAudioFrames == 0 {
            finishWithASRError("No audio frames from device")
            return
        }

        let finalChunk = oggMuxer.finish()
        debugAudioRecorder.append(finalChunk)
        debugAudioRecorder.finish()
        sendOrBufferOggChunk(finalChunk, isLast: true, canStartASR: true)
        guard asrStarted else { return }
        statusController.setStatus("Processing")
        sendUIStateForActiveDevice("thinking")
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
            showUtterances: useDefiniteSegments
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

    private func finishWithFinalText(_ text: String) {
        guard !pastedFinalText else { return }
        let profile = outputProfile(for: activeDeviceID)
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

        if profile.transform == .translate {
            pastedFinalText = true
            statusController.setStatus("Translating")
            transformText(text, profile: profile, deviceID: activeDeviceID) { [weak self] result in
                guard let self else { return }
                switch result {
                case .success(let translatedText):
                    self.enterPendingConfirmation(text: translatedText)
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
            ble.sendUIState("pending_confirmation", confirmationID: id, to: peripheralID)
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
        showSubtitleText(segment.text, profile: profile, deviceID: cycle.deviceID)
    }

    private func finishSubtitleCycleWithFinalText(peripheralID: UUID, sessionID: UInt32, text: String) {
        guard let cycle = subtitleCycle(peripheralID: peripheralID, sessionID: sessionID),
              !cycle.finishedFinalText
        else { return }
        cycle.finishedFinalText = true
        NSLog("Subtitle final text dev=VS-\(cycle.deviceID ?? "unknown") session=\(sessionID) text_len=\(text.count)")
        let profile = outputProfile(for: cycle.deviceID)
        if !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            showSubtitleText(text, profile: profile, deviceID: cycle.deviceID) { [weak self] didShowSubtitle in
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
                self?.ble.sendUIState("ready", to: peripheralID)
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
        ble.sendUIState("ready", to: peripheralID)
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
            ble.sendUIState("ready", to: peripheralID)
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
        ble.sendUIState("ready", to: peripheralID)
    }

    private func showSubtitleText(
        _ text: String,
        profile: OutputProfile,
        deviceID: String?,
        completion: ((Bool) -> Void)? = nil
    ) {
        guard let deviceID else {
            completion?(false)
            return
        }
        transformText(text, profile: profile, deviceID: deviceID) { [weak self] result in
            guard let self else { return }
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
        deviceID: String?,
        completion: @escaping (Result<String, Error>) -> Void
    ) {
        guard profile.transform == .translate else {
            completion(.success(text))
            return
        }
        translator.translate(
            text,
            targetLanguage: profile.translationTarget,
            hotwords: config.asrHotwords
        ) { [weak self] result in
            DispatchQueue.main.async {
                guard self != nil else { return }
                completion(result)
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
                    self.ble.sendUIState("pending_confirmation", confirmationID: id, to: peripheralID)
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
        rememberInput(text)
        pendingPasteState = .idle
        finishRecognitionCycle()
        statusController.setStatus("Ready")
        sendUIStateForActiveDevice("ready")
        mainInputState = .ready
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
        if isWaitingForFinalText {
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
        if case .waitingToPaste = pendingPasteState {
            statusController.hideOverlay(deviceID: activeDeviceID)
        }
        pendingPasteState = .idle
        finishRecognitionCycle()
        statusController.setStatus("Ready")
        sendUIStateForActiveDevice("ready")
        mainInputState = .ready
    }

    private func cancelRecognitionInProgress() {
        cancelAudioEndTimeout()
        asr.cancel()
        pendingPasteState = .idle
        finishRecognitionCycle()
        statusController.hideOverlay()
        statusController.setStatus("Ready")
        sendUIStateForActiveDevice("ready")
        mainInputState = .ready
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
        asrStarted = false
        sentFinalAudioChunk = false
        pastedFinalText = false
        bufferedOggChunks.removeAll(keepingCapacity: true)
        countdownPasteTarget = nil
    }

    private func updateDeviceFirmwareInfo(event: StateEvent, peripheralID: UUID) {
        guard let deviceID = ble.deviceID(for: peripheralID) else { return }
        var info = firmwareInfoByDeviceID[deviceID] ?? DeviceFirmwareInfo()
        if event.hardware != nil, event.firmwareVersion != nil, ble.isConnected(peripheralID) {
            firmwareReportingPeripherals[deviceID] = peripheralID
        }
        if let hardware = event.hardware {
            info.hardware = hardware
        }
        if let buttons = event.buttons {
            info.buttons = buttons
        }
        if let firmwareVersion = event.firmwareVersion {
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
                    switch error {
                    case FirmwareManifestClient.FirmwareManifestError.appUpdateRequired:
                        feed.manifest = nil
                        feed.lastChecked = nil
                        feed.errorMessage = error.localizedDescription
                    default:
                        if !completions.isEmpty {
                            feed.errorMessage = error.localizedDescription
                        }
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
            info.updateAvailable = FirmwareVersion.isVersion(currentVersion, olderThan: manifest.version)
            firmwareInfoByDeviceID[deviceID] = info
            NSLog("Firmware availability VS-\(deviceID) hardware=\(manifest.hardware) current=\(currentVersion) latest=\(manifest.version) update=\(info.updateAvailable)")
        }
        statusController.setFirmwareInfo(firmwareInfoByDeviceID)
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
              !isShowingFirmwareUpdatePrompt, NSApp.modalWindow == nil,
              let onFirmwareUpdatePrompt else { return }

        for deviceID in pairedDeviceIDs {
            guard let info = firmwareInfoByDeviceID[deviceID], info.updateAvailable,
                  !info.isChecking, info.errorMessage == nil,
                  let currentVersion = info.currentVersion, let latestVersion = info.latestVersion,
                  let peripheralID = firmwareReportingPeripherals[deviceID], ble.isConnected(peripheralID),
                  recordFirmwareReminder(for: peripheralID, version: latestVersion) else { continue }

            let isBelowMinimum = FirmwareVersion.isVersion(
                currentVersion,
                olderThan: AppConfig.minimumCompatibleFirmwareVersion
            )
            isShowingFirmwareUpdatePrompt = true
            onFirmwareUpdatePrompt(deviceID, currentVersion, latestVersion, isBelowMinimum)
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
        ble.sendUIState(state, text: text, to: activePeripheralID)
    }

    private var activeDeviceID: String? {
        activePeripheralID.flatMap { ble.deviceID(for: $0) }
    }

    private func outputProfile(for deviceID: String?) -> OutputProfile {
        config.outputProfile(for: deviceID)
    }

    private func shouldUseDefiniteSegments(for profile: OutputProfile) -> Bool {
        profile.target == .subtitle && config.interactionMode == .clickToTalk
    }

    private func deviceID(for peripheralID: UUID) -> String? {
        ble.deviceID(for: peripheralID)
    }
}
