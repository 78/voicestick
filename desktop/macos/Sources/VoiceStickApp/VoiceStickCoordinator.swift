import AppKit
import Foundation

final class VoiceStickCoordinator {
    private enum PendingPasteState {
        case idle
        case waitingToPaste(text: String)
        case paused(text: String)

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
        case pausedConfirmation(peripheralID: UUID)
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
                 .pendingConfirmation(let peripheralID),
                 .pausedConfirmation(let peripheralID):
                return peripheralID
            case .error(let peripheralID):
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
            self.asr = VolcengineASRClient(config: config)
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

    private var mainInputState = MainInputState.ready
    private var receivedAudioFrames = 0
    private var bufferedOggChunks: [Data] = []
    private var asrStarted = false
    private var sentFinalAudioChunk = false
    private var pastedFinalText = false
    private var waitingForAudioEnd = false
    private var audioEndTimeoutTimer: Timer?
    private var pendingPasteState = PendingPasteState.idle
    private var lastRecoverableText: String?
    private var lastRecoverablePeripheralID: UUID?
    private var pairedDeviceIDs: [String]
    private var firmwareInfoByDeviceID: [String: DeviceFirmwareInfo] = [:]
    private var latestFirmwareManifest: FirmwareManifest?
    private var lastFirmwareManifestCheckAt: Date?
    private var firmwareManifestCheckInFlight = false
    private var firmwareManifestRefreshTimer: Timer?
    private var pendingFirmwareUpdatePromptDeviceIDs: Set<String> = []
    private var errorRecoveryToken = 0
    private var isShowingASRError = false
    private var subtitleCycles: [SubtitleCycleKey: SubtitleCycle] = [:]
    private var activeSubtitleSessions: [UUID: UInt32] = [:]
    var onFirmwareUpdatePrompt: ((String, String, String, Bool) -> Void)?

    init(config: AppConfig, statusController: StatusController) {
        self.config = config
        self.statusController = statusController
        self.pairedDeviceIDs = config.pairedDeviceIDs
        self.ble = BleCentral(pairedDeviceIDs: config.pairedDeviceIDs)
        self.asr = VolcengineASRClient(config: config)
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
            self.cancelActiveCycleIfDeviceDisconnected()
            self.refreshFirmwareAvailability()
            if !connectedDevices.isEmpty {
                self.statusController.setStatus("Ready")
                self.ble.sendInteractionMode(self.config.interactionMode)
            } else {
                self.statusController.setStatus(self.pairedDeviceIDs.isEmpty ? "Pair a VoiceStick" : "Ready")
            }
        }

        ble.onStateEvent = { [weak self] peripheralID, event in
            self?.handleStateEvent(event, peripheralID: peripheralID)
        }

        ble.onAudioFrame = { [weak self] peripheralID, frame in
            self?.handleAudioFrame(frame, peripheralID: peripheralID)
        }

        configureASRCallbacks()
        ble.start()
        checkFirmwareUpdatesIfNeeded(force: false, showErrors: false)
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
        asr = VolcengineASRClient(config: config)
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
            DispatchQueue.main.async {
                guard let self else { return }
                self.statusController.showPartial(text, deviceID: self.activeDeviceID)
                if self.shouldSendPartialToDevice() {
                    self.sendUIStateForActiveDevice("thinking", text: text)
                }
            }
        }

        asr.onSegment = { [weak self] segment in
            DispatchQueue.main.async {
                self?.handleDefiniteSegment(segment)
            }
        }

        asr.onFinal = { [weak self] text in
            DispatchQueue.main.async {
                self?.finishWithFinalText(text)
            }
        }

        asr.onError = { [weak self] message in
            DispatchQueue.main.async {
                self?.finishWithASRError(message)
            }
        }

        asr.onUpgradeURL = { url in
            DispatchQueue.main.async {
                let alert = NSAlert()
                alert.messageText = "ASR quota reached"
                alert.informativeText = "Your VoiceStick Cloud quota has been used. Upgrade your account to continue."
                alert.addButton(withTitle: "Upgrade")
                alert.addButton(withTitle: "Cancel")
                if alert.runModal() == .alertFirstButtonReturn {
                    NSWorkspace.shared.open(url)
                }
            }
        }
    }

    private func configureSubtitleASRCallbacks(for cycle: SubtitleCycle) {
        let peripheralID = cycle.peripheralID
        let sessionID = cycle.sessionID
        cycle.asr.onPartial = { [weak self] text in
            DispatchQueue.main.async {
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
        }
        cycle.asr.onSegment = { [weak self] segment in
            DispatchQueue.main.async {
                guard self?.isActiveSubtitleCycle(peripheralID: peripheralID, sessionID: sessionID) == true else {
                    return
                }
                self?.handleSubtitleDefiniteSegment(segment, peripheralID: peripheralID)
            }
        }
        cycle.asr.onFinal = { [weak self] text in
            DispatchQueue.main.async {
                self?.finishSubtitleCycleWithFinalText(
                    peripheralID: peripheralID,
                    sessionID: sessionID,
                    text: text
                )
            }
        }
        cycle.asr.onError = { [weak self] message in
            DispatchQueue.main.async {
                self?.finishSubtitleCycleWithError(
                    peripheralID: peripheralID,
                    sessionID: sessionID,
                    message: message
                )
            }
        }
        cycle.asr.onUpgradeURL = { url in
            DispatchQueue.main.async {
                NSWorkspace.shared.open(url)
            }
        }
    }

    func updatePairedDeviceIDs(_ deviceIDs: [String]) {
        pairedDeviceIDs = deviceIDs
        statusController.setPairedDeviceIDs(deviceIDs)
        statusController.setConnectedDevices([])
        statusController.setStatus(deviceIDs.isEmpty ? "Pair a VoiceStick" : "Ready")
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

    func checkFirmwareUpdatesNow() {
        checkFirmwareUpdatesIfNeeded(force: true, showErrors: true)
    }

    func checkFirmwareAfterPairing(deviceID: String) {
        pendingFirmwareUpdatePromptDeviceIDs.insert(deviceID)
        checkFirmwareUpdatesIfNeeded(force: false, showErrors: false)
        refreshFirmwareAvailability()
    }

    func updateFirmwareFromLatest(for deviceID: String,
                                  progress: @escaping (FirmwareUpdateProgress) -> Void,
                                  completion: @escaping (Result<Void, Error>) -> Void) {
        guard let manifest = latestFirmwareManifest else {
            completion(.failure(FirmwareManifestClient.FirmwareManifestError.invalidResponse))
            return
        }

        firmwareManifestClient.downloadOTA(from: manifest) { [weak self] result in
            DispatchQueue.main.async {
                guard let self else { return }
                switch result {
                case .success(let image):
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
        case "button_down":
            handleButtonDown(event, peripheralID: peripheralID)
        case "button_up":
            handleButtonUp(event, peripheralID: peripheralID)
        case "button_click":
            handleButtonClick(event, peripheralID: peripheralID)
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
                    finishWithASRError("Failed to start ASR")
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
                        message: "Failed to start ASR"
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
                message: "Failed to start ASR"
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
            finishWithASRError("Failed to start ASR")
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
        lastRecoverableText = text
        lastRecoverablePeripheralID = activePeripheralID
        statusController.setHasRecoverableInput(true)
        pendingPasteState = .waitingToPaste(text: text)
        if let peripheralID = activePeripheralID {
            mainInputState = .pendingConfirmation(peripheralID: peripheralID)
        }
        statusController.showFinal(text, deviceID: activeDeviceID) { [weak self] in
            self?.commitPendingPaste(text: text)
        }
        sendUIStateForActiveDevice("pending_confirmation", text: text)
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

    private func finishWithASRError(_ message: String) {
        NSLog("ASR error: \(message)")
        cancelAudioEndTimeout()
        asr.cancel()
        pendingPasteState = .idle
        debugAudioRecorder.discard()
        isShowingASRError = true
        errorRecoveryToken += 1
        let token = errorRecoveryToken
        let errorPeripheralID = activePeripheralID
        mainInputState = .error(peripheralID: errorPeripheralID)
        finishRecognitionCycle()
        sendUIStateForActiveDevice("error", text: message)
        statusController.showError(message, deviceID: activeDeviceID) { [weak self] in
            guard let self, self.errorRecoveryToken == token else { return }
            self.recoverFromASRError(hideOverlay: false)
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

    private func commitPendingPaste(text: String) {
        guard pendingPasteText == text else {
            return
        }

        completePendingPaste(text: text)
    }

    private func completePendingPaste(text: String) {
        let shouldPressEnter = config.autoEnter
        pendingPasteState = .idle
        finishRecognitionCycle()
        statusController.setStatus("Ready")
        sendUIStateForActiveDevice("ready")
        mainInputState = .ready
        inputInjector.paste(text: text, pressEnter: shouldPressEnter)
    }

    private var isWaitingForFinalText: Bool {
        mainInputState.isFinalizing &&
            sentFinalAudioChunk &&
            asrStarted &&
            pendingPasteState.isIdle &&
            !pastedFinalText
    }

    private var pendingPasteText: String? {
        switch pendingPasteState {
        case .idle:
            return nil
        case .waitingToPaste(let text), .paused(let text):
            return text
        }
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

        pendingPasteState = .paused(text: text)
        if let peripheralID {
            mainInputState = .pausedConfirmation(peripheralID: peripheralID)
        }
        statusController.showPausedFinal(text, deviceID: activeDeviceID)
        sendUIStateForActiveDevice("pending_confirmation", text: text)
        return true
    }

    private func handleFrontButtonDuringPendingPaste(peripheralID: UUID) -> Bool {
        switch pendingPasteState {
        case .idle:
            return false
        case .waitingToPaste(let text):
            guard activePeripheralID == peripheralID else { return true }
            pendingPasteState = .paused(text: text)
            mainInputState = .pausedConfirmation(peripheralID: peripheralID)
            statusController.showPausedFinal(text, deviceID: deviceID(for: peripheralID))
            sendUIStateForActiveDevice("pending_confirmation", text: text)
            return true
        case .paused(let text):
            guard activePeripheralID == peripheralID else { return true }
            statusController.hideOverlay { [weak self] in
                self?.commitPendingPaste(text: text)
            }
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
        if pendingPasteText == nil {
            _ = restoreLastInputConfirmation(peripheralID: peripheralID)
            return
        }
        guard activePeripheralID == peripheralID else { return }
        pendingPasteState = .idle
        finishRecognitionCycle()
        statusController.hideOverlay()
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
    }

    private func updateDeviceFirmwareInfo(event: StateEvent, peripheralID: UUID) {
        guard let deviceID = ble.deviceID(for: peripheralID) else { return }
        var info = firmwareInfoByDeviceID[deviceID] ?? DeviceFirmwareInfo()
        if let hardware = event.hardware {
            info.hardware = hardware
        }
        if let firmwareVersion = event.firmwareVersion {
            info.currentVersion = firmwareVersion
        }
        info.errorMessage = nil
        firmwareInfoByDeviceID[deviceID] = info
        refreshFirmwareAvailability()
    }

    private func startFirmwareManifestRefreshTimer() {
        firmwareManifestRefreshTimer?.invalidate()
        firmwareManifestRefreshTimer = Timer.scheduledTimer(withTimeInterval: 60 * 60, repeats: true) { [weak self] _ in
            self?.checkFirmwareUpdatesIfNeeded(force: false, showErrors: false)
        }
    }

    private func checkFirmwareUpdatesIfNeeded(force: Bool, showErrors: Bool) {
        if firmwareManifestCheckInFlight {
            return
        }
        if !force,
           let lastFirmwareManifestCheckAt,
           Date().timeIntervalSince(lastFirmwareManifestCheckAt) < firmwareManifestCacheDuration {
            refreshFirmwareAvailability()
            return
        }

        firmwareManifestCheckInFlight = true
        setFirmwareChecking(true)
        firmwareManifestClient.fetchManifest { [weak self] result in
            DispatchQueue.main.async {
                guard let self else { return }
                self.firmwareManifestCheckInFlight = false
                self.setFirmwareChecking(false)
                switch result {
                case .success(let manifest):
                    NSLog("Firmware manifest version=\(manifest.version) hardware=\(manifest.hardware)")
                    self.lastFirmwareManifestCheckAt = Date()
                    self.latestFirmwareManifest = manifest
                    self.clearFirmwareErrors()
                    self.refreshFirmwareAvailability()
                case .failure(let error):
                    NSLog("Firmware manifest check failed: \(error.localizedDescription)")
                    if showErrors {
                        self.setFirmwareError(error.localizedDescription)
                    } else {
                        self.refreshFirmwareAvailability()
                    }
                }
            }
        }
    }

    private func refreshFirmwareAvailability() {
        for (deviceID, var info) in firmwareInfoByDeviceID {
            info.latestVersion = nil
            info.updateAvailable = false
            guard let manifest = latestFirmwareManifest else {
                firmwareInfoByDeviceID[deviceID] = info
                continue
            }
            guard let hardware = info.hardware else {
                firmwareInfoByDeviceID[deviceID] = info
                continue
            }
            guard let currentVersion = info.currentVersion else {
                firmwareInfoByDeviceID[deviceID] = info
                continue
            }
            guard hardware == manifest.hardware else {
                NSLog("Firmware availability VS-\(deviceID) hardware=\(hardware) current=\(currentVersion) latest=\(manifest.version) update=false reason=hardware_mismatch manifest_hardware=\(manifest.hardware)")
                firmwareInfoByDeviceID[deviceID] = info
                continue
            }
            info.latestVersion = manifest.version
            info.updateAvailable = FirmwareVersion.isVersion(currentVersion, olderThan: manifest.version)
            firmwareInfoByDeviceID[deviceID] = info
            NSLog("Firmware availability VS-\(deviceID) hardware=\(hardware) current=\(currentVersion) latest=\(manifest.version) update=\(info.updateAvailable)")
            maybeShowFirmwareUpdatePromptAfterPairing(deviceID: deviceID, info: info)
        }
        statusController.setFirmwareInfo(firmwareInfoByDeviceID)
    }

    private func maybeShowFirmwareUpdatePromptAfterPairing(deviceID: String, info: DeviceFirmwareInfo) {
        guard
            pendingFirmwareUpdatePromptDeviceIDs.contains(deviceID),
            let currentVersion = info.currentVersion,
            let latestVersion = info.latestVersion
        else {
            return
        }
        guard info.updateAvailable else {
            pendingFirmwareUpdatePromptDeviceIDs.remove(deviceID)
            return
        }
        pendingFirmwareUpdatePromptDeviceIDs.remove(deviceID)
        let isBelowMinimum = FirmwareVersion.isVersion(
            currentVersion,
            olderThan: AppConfig.minimumCompatibleFirmwareVersion
        )
        DispatchQueue.main.async { [onFirmwareUpdatePrompt] in
            onFirmwareUpdatePrompt?(deviceID, currentVersion, latestVersion, isBelowMinimum)
        }
    }

    private func setFirmwareChecking(_ isChecking: Bool) {
        for deviceID in pairedDeviceIDs {
            var info = firmwareInfoByDeviceID[deviceID] ?? DeviceFirmwareInfo()
            info.isChecking = isChecking
            if isChecking {
                info.errorMessage = nil
            }
            firmwareInfoByDeviceID[deviceID] = info
        }
        statusController.setFirmwareInfo(firmwareInfoByDeviceID)
    }

    private func clearFirmwareErrors() {
        for (deviceID, var info) in firmwareInfoByDeviceID {
            info.errorMessage = nil
            firmwareInfoByDeviceID[deviceID] = info
        }
    }

    private func setFirmwareError(_ message: String) {
        for deviceID in pairedDeviceIDs {
            var info = firmwareInfoByDeviceID[deviceID] ?? DeviceFirmwareInfo()
            info.errorMessage = message
            firmwareInfoByDeviceID[deviceID] = info
        }
        statusController.setFirmwareInfo(firmwareInfoByDeviceID)
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
