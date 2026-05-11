import AppKit
import Foundation
import TOMLKit

enum ASRProvider: String {
    case voiceStickCloud = "voicestick_cloud"
    case volcengine

    var displayName: String {
        switch self {
        case .voiceStickCloud:
            return "VoiceStick Cloud"
        case .volcengine:
            return "Volcengine"
        }
    }
}

enum InteractionMode: String {
    case holdToTalk = "hold_to_talk"
    case clickToTalk = "click_to_talk"

    var displayName: String {
        switch self {
        case .holdToTalk:
            return "Hold to Talk"
        case .clickToTalk:
            return "Click to Talk"
        }
    }
}

enum OverlayThemeColor: String, CaseIterable {
    case white
    case pink
    case green
    case yellow
    case blue
    case purple

    var displayName: String {
        switch self {
        case .white:
            return "White"
        case .pink:
            return "Pink"
        case .green:
            return "Green"
        case .yellow:
            return "Yellow"
        case .blue:
            return "Blue"
        case .purple:
            return "Purple"
        }
    }
}

enum OverlayPosition: String, CaseIterable {
    case center
    case topLeft = "top_left"
    case topRight = "top_right"
    case bottomLeft = "bottom_left"
    case bottomRight = "bottom_right"

    var displayName: String {
        switch self {
        case .center:
            return "Center"
        case .topLeft:
            return "Top Left"
        case .topRight:
            return "Top Right"
        case .bottomLeft:
            return "Bottom Left"
        case .bottomRight:
            return "Bottom Right"
        }
    }
}

enum OutputTarget: String, CaseIterable {
    case focusedApp = "focused_app"
    case subtitle

    var displayName: String {
        switch self {
        case .focusedApp:
            return "Focused App"
        case .subtitle:
            return "Subtitle"
        }
    }
}

enum TextTransform: String, CaseIterable {
    case original
    case translate

    var displayName: String {
        switch self {
        case .original:
            return "Original"
        case .translate:
            return "Translate"
        }
    }
}

struct OutputProfile: Equatable {
    var target: OutputTarget
    var transform: TextTransform
    var translationTarget: String

    static let `default` = OutputProfile(
        target: .focusedApp,
        transform: .original,
        translationTarget: "en"
    )

    var usesSubtitleASR: Bool {
        target == .subtitle
    }
}

struct AppConfig {
    var asrProvider: ASRProvider
    var voiceStickAPIKey: String
    var voiceStickCloudURL: String
    var volcengineAPIKey: String
    var llmBaseURL: String
    var llmAPIKey: String
    var llmModel: String
    var interactionMode: InteractionMode
    var resourceID: String
    var asrHotwords: [String]
    var pairedDeviceIDs: [String]
    var deviceThemeColors: [String: OverlayThemeColor]
    var deviceOverlayPositions: [String: OverlayPosition]
    var defaultOutputProfile: OutputProfile
    var deviceOutputProfiles: [String: OutputProfile]
    var autoEnter: Bool
    var debugAudioCache: Bool
    var debugAudioDirectory: URL

    static var configDirectory: URL {
        FileManager.default
            .homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/VoiceStick", isDirectory: true)
    }

    static var configURL: URL {
        configDirectory.appendingPathComponent("config.toml")
    }

    static var defaultDebugAudioDirectory: URL {
        configDirectory.appendingPathComponent("DebugAudio", isDirectory: true)
    }

    static let supportedResourceIDs = [
        "volc.seedasr.sauc.duration",
        "volc.seedasr.sauc.concurrent",
        "volc.bigasr.sauc.duration",
        "volc.bigasr.sauc.concurrent"
    ]

    static let defaultVoiceStickCloudURL = "wss://api.xiaozhi.me/voicestick/asr/"
    static let volcengineWebSocketURL = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"
    static let websiteURL = URL(string: "https://78.github.io/voicestick/")!
    static let firmwareManifestURL = URL(
        string: "https://xiaozhi-voice-assistant.oss-cn-shenzhen.aliyuncs.com/voicestick/firmwares/latest/manifest.json"
    )!
    static let minimumCompatibleFirmwareVersion = "0.3.0"

    static var configExists: Bool {
        FileManager.default.fileExists(atPath: configURL.path)
    }

    static var defaults: AppConfig {
        AppConfig(
            asrProvider: .voiceStickCloud,
            voiceStickAPIKey: "",
            voiceStickCloudURL: defaultVoiceStickCloudURL,
            volcengineAPIKey: "",
            llmBaseURL: "https://api.openai.com/v1",
            llmAPIKey: "",
            llmModel: "gpt-5.5",
            interactionMode: .holdToTalk,
            resourceID: supportedResourceIDs[0],
            asrHotwords: [],
            pairedDeviceIDs: [],
            deviceThemeColors: [:],
            deviceOverlayPositions: [:],
            defaultOutputProfile: .default,
            deviceOutputProfiles: [:],
            autoEnter: true,
            debugAudioCache: false,
            debugAudioDirectory: defaultDebugAudioDirectory
        )
    }

    static func load() -> AppConfig {
        let defaults = Self.defaults

        guard let text = try? String(contentsOf: configURL) else {
            return defaults
        }

        guard let file = try? TOMLDecoder().decode(ConfigFile.self, from: TOMLTable(string: text)) else {
            return loadLegacy(text: text, defaults: defaults)
        }

        return AppConfig(
            asrProvider: asrProviderValue(file.asr_provider, default: defaults.asrProvider),
            voiceStickAPIKey: file.voicestick_api_key ?? defaults.voiceStickAPIKey,
            voiceStickCloudURL: file.voicestick_cloud_url ?? defaults.voiceStickCloudURL,
            volcengineAPIKey: file.volcengine_api_key ?? file.api_key ?? defaults.volcengineAPIKey,
            llmBaseURL: file.llm_base_url ?? defaults.llmBaseURL,
            llmAPIKey: file.llm_api_key ?? defaults.llmAPIKey,
            llmModel: file.llm_model ?? defaults.llmModel,
            interactionMode: interactionModeValue(file.interaction_mode, default: defaults.interactionMode),
            resourceID: resourceIDValue(file.resource_id, default: defaults.resourceID),
            asrHotwords: hotwordList(file.asr_hotwords ?? ""),
            pairedDeviceIDs: deviceIDList(file.paired_device_ids ?? ""),
            deviceThemeColors: deviceThemeColorMap(file.device_theme_colors ?? ""),
            deviceOverlayPositions: deviceOverlayPositionMap(file.device_overlay_positions ?? ""),
            defaultOutputProfile: outputProfile(
                target: file.output?.target ?? file.output_target,
                transform: file.output?.transform ?? file.text_transform,
                translationTarget: file.output?.translation_target ?? file.translation_target,
                default: defaults.defaultOutputProfile
            ),
            deviceOutputProfiles: deviceOutputProfileMap(
                file.device,
                defaultProfile: outputProfile(
                    target: file.output?.target ?? file.output_target,
                    transform: file.output?.transform ?? file.text_transform,
                    translationTarget: file.output?.translation_target ?? file.translation_target,
                    default: defaults.defaultOutputProfile
                )
            ),
            autoEnter: file.auto_enter ?? defaults.autoEnter,
            debugAudioCache: file.debug_audio_cache ?? defaults.debugAudioCache,
            debugAudioDirectory: directoryValue(file.debug_audio_dir, default: defaults.debugAudioDirectory)
        )
    }

    func save() throws {
        try FileManager.default.createDirectory(at: Self.configDirectory, withIntermediateDirectories: true)
        let text = """
        asr_provider = "\(asrProvider.rawValue)"
        voicestick_api_key = "\(voiceStickAPIKey.tomlEscaped)"
        voicestick_cloud_url = "\(voiceStickCloudURL.tomlEscaped)"
        volcengine_api_key = "\(volcengineAPIKey.tomlEscaped)"
        llm_base_url = "\(llmBaseURL.tomlEscaped)"
        llm_api_key = "\(llmAPIKey.tomlEscaped)"
        llm_model = "\(llmModel.tomlEscaped)"
        interaction_mode = "\(interactionMode.rawValue)"
        resource_id = "\(resourceID.tomlEscaped)"
        asr_hotwords = "\(asrHotwords.joined(separator: ",").tomlEscaped)"
        paired_device_ids = "\(pairedDeviceIDs.joined(separator: ",").tomlEscaped)"
        device_theme_colors = "\(deviceThemeColorText.tomlEscaped)"
        device_overlay_positions = "\(deviceOverlayPositionText.tomlEscaped)"
        auto_enter = \(autoEnter.tomlValue)
        debug_audio_cache = \(debugAudioCache.tomlValue)
        debug_audio_dir = "\(debugAudioDirectory.path.tomlEscaped)"

        [output]
        target = "\(defaultOutputProfile.target.rawValue)"
        transform = "\(defaultOutputProfile.transform.rawValue)"
        translation_target = "\(defaultOutputProfile.translationTarget.tomlEscaped)"
        """
        let deviceText = deviceOutputProfileText
        try (text + deviceText).write(to: Self.configURL, atomically: true, encoding: .utf8)
    }

    private static func loadLegacy(text: String, defaults: AppConfig) -> AppConfig {
        var values: [String: String] = [:]
        for rawLine in text.split(separator: "\n") {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            guard !line.isEmpty, !line.hasPrefix("#") else { continue }
            let parts = line.split(separator: "=", maxSplits: 1).map(String.init)
            guard parts.count == 2 else { continue }
            values[parts[0].trimmingCharacters(in: .whitespaces)] =
                parts[1].trimmingCharacters(in: .whitespaces).trimmingCharacters(in: CharacterSet(charactersIn: "\""))
        }

        return AppConfig(
            asrProvider: asrProviderValue(values["asr_provider"], default: defaults.asrProvider),
            voiceStickAPIKey: values["voicestick_api_key"] ?? defaults.voiceStickAPIKey,
            voiceStickCloudURL: values["voicestick_cloud_url"] ?? defaults.voiceStickCloudURL,
            volcengineAPIKey: values["volcengine_api_key"] ?? values["api_key"] ?? defaults.volcengineAPIKey,
            llmBaseURL: values["llm_base_url"] ?? defaults.llmBaseURL,
            llmAPIKey: values["llm_api_key"] ?? defaults.llmAPIKey,
            llmModel: values["llm_model"] ?? defaults.llmModel,
            interactionMode: interactionModeValue(values["interaction_mode"], default: defaults.interactionMode),
            resourceID: resourceIDValue(values["resource_id"], default: defaults.resourceID),
            asrHotwords: hotwordList(values["asr_hotwords"] ?? ""),
            pairedDeviceIDs: deviceIDList(values["paired_device_ids"] ?? ""),
            deviceThemeColors: deviceThemeColorMap(values["device_theme_colors"] ?? ""),
            deviceOverlayPositions: deviceOverlayPositionMap(values["device_overlay_positions"] ?? ""),
            defaultOutputProfile: outputProfile(
                target: values["output_target"],
                transform: values["text_transform"],
                translationTarget: values["translation_target"],
                default: defaults.defaultOutputProfile
            ),
            deviceOutputProfiles: [:],
            autoEnter: boolValue(values["auto_enter"], default: defaults.autoEnter),
            debugAudioCache: boolValue(values["debug_audio_cache"], default: defaults.debugAudioCache),
            debugAudioDirectory: directoryValue(values["debug_audio_dir"], default: defaults.debugAudioDirectory)
        )
    }

    static func openConfigDirectory() {
        try? FileManager.default.createDirectory(at: configDirectory, withIntermediateDirectories: true)
        NSWorkspace.shared.open(configDirectory)
    }

    static func openDebugAudioDirectory(_ directory: URL = defaultDebugAudioDirectory) {
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        NSWorkspace.shared.open(directory)
    }

    private static func boolValue(_ text: String?, default defaultValue: Bool) -> Bool {
        guard let text else { return defaultValue }
        switch text.lowercased() {
        case "true", "yes", "1", "on":
            return true
        case "false", "no", "0", "off":
            return false
        default:
            return defaultValue
        }
    }

    private static func directoryValue(_ text: String?, default defaultValue: URL) -> URL {
        guard let text, !text.isEmpty else { return defaultValue }
        let expanded = (text as NSString).expandingTildeInPath
        return URL(fileURLWithPath: expanded, isDirectory: true)
    }

    private static func asrProviderValue(_ text: String?, default defaultValue: ASRProvider) -> ASRProvider {
        guard let text, let provider = ASRProvider(rawValue: text) else { return defaultValue }
        return provider
    }

    private static func interactionModeValue(_ text: String?, default defaultValue: InteractionMode) -> InteractionMode {
        guard let text, let mode = InteractionMode(rawValue: text) else { return defaultValue }
        return mode
    }

    private static func outputTargetValue(_ text: String?, default defaultValue: OutputTarget) -> OutputTarget {
        guard let text, let target = OutputTarget(rawValue: text) else { return defaultValue }
        return target
    }

    private static func textTransformValue(_ text: String?, default defaultValue: TextTransform) -> TextTransform {
        guard let text, let transform = TextTransform(rawValue: text) else { return defaultValue }
        return transform
    }

    private static func outputProfile(
        target: String?,
        transform: String?,
        translationTarget: String?,
        default defaultValue: OutputProfile
    ) -> OutputProfile {
        OutputProfile(
            target: outputTargetValue(target, default: defaultValue.target),
            transform: textTransformValue(transform, default: defaultValue.transform),
            translationTarget: (translationTarget?.trimmingCharacters(in: .whitespacesAndNewlines)).flatMap {
                $0.isEmpty ? nil : $0
            } ?? defaultValue.translationTarget
        )
    }

    private static func resourceIDValue(_ text: String?, default defaultValue: String) -> String {
        guard let text, supportedResourceIDs.contains(text) else { return defaultValue }
        return text
    }

    static func normalizedDeviceID(_ text: String) -> String {
        let upper = text.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        if upper.hasPrefix("VS-") {
            return String(upper.dropFirst(3).prefix(4))
        }
        return String(upper.prefix(4))
    }

    static func deviceIDList(_ text: String) -> [String] {
        text.split(separator: ",")
            .map { normalizedDeviceID(String($0)) }
            .filter { $0.count == 4 && $0.allSatisfy(\.isHexDigit) }
            .reduce(into: []) { ids, id in
                if !ids.contains(id) {
                    ids.append(id)
                }
            }
    }

    static func hotwordList(_ text: String) -> [String] {
        text.split { character in
            character == "," || character == "\n" || character == "\r"
        }
            .map { String($0).trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
            .reduce(into: []) { hotwords, hotword in
                if !hotwords.contains(hotword) {
                    hotwords.append(hotword)
                }
            }
    }

    static func deviceThemeColorMap(_ text: String) -> [String: OverlayThemeColor] {
        text.split(separator: ",").reduce(into: [:]) { colorsByDeviceID, rawPair in
            let parts = rawPair.split(separator: ":", maxSplits: 1).map(String.init)
            guard parts.count == 2 else { return }
            let deviceID = normalizedDeviceID(parts[0])
            let colorName = parts[1].trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
            guard deviceID.count == 4,
                  deviceID.allSatisfy(\.isHexDigit),
                  let color = OverlayThemeColor(rawValue: colorName) else { return }
            colorsByDeviceID[deviceID] = color
        }
    }

    func themeColor(for deviceID: String?) -> OverlayThemeColor {
        guard let deviceID else { return .white }
        return deviceThemeColors[Self.normalizedDeviceID(deviceID)] ?? .white
    }

    static func deviceOverlayPositionMap(_ text: String) -> [String: OverlayPosition] {
        text.split(separator: ",").reduce(into: [:]) { positionsByDeviceID, rawPair in
            let parts = rawPair.split(separator: ":", maxSplits: 1).map(String.init)
            guard parts.count == 2 else { return }
            let deviceID = normalizedDeviceID(parts[0])
            let positionName = parts[1].trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
            guard deviceID.count == 4,
                  deviceID.allSatisfy(\.isHexDigit),
                  let position = OverlayPosition(rawValue: positionName) else { return }
            positionsByDeviceID[deviceID] = position
        }
    }

    func overlayPosition(for deviceID: String?) -> OverlayPosition {
        guard let deviceID else { return .center }
        return deviceOverlayPositions[Self.normalizedDeviceID(deviceID)] ?? .center
    }

    func outputProfile(for deviceID: String?) -> OutputProfile {
        guard let deviceID, let deviceProfile = deviceOutputProfiles[Self.normalizedDeviceID(deviceID)] else {
            return defaultOutputProfile
        }
        return OutputProfile(
            target: defaultOutputProfile.target,
            transform: deviceProfile.transform,
            translationTarget: deviceProfile.translationTarget
        )
    }

    private static func deviceOutputProfileMap(
        _ devices: [String: DeviceConfigFile]?,
        defaultProfile: OutputProfile
    ) -> [String: OutputProfile] {
        guard let devices else { return [:] }
        return devices.reduce(into: [:]) { profiles, pair in
            let deviceID = normalizedDeviceID(pair.key)
            guard deviceID.count == 4, deviceID.allSatisfy(\.isHexDigit), let output = pair.value.output else {
                return
            }
            profiles[deviceID] = outputProfile(
                target: nil,
                transform: output.transform,
                translationTarget: output.translation_target,
                default: defaultProfile
            )
        }
    }

    private var deviceThemeColorText: String {
        deviceThemeColors
            .filter { pairedDeviceIDs.contains($0.key) && $0.value != .white }
            .sorted { $0.key < $1.key }
            .map { "\($0.key):\($0.value.rawValue)" }
            .joined(separator: ",")
    }

    private var deviceOverlayPositionText: String {
        deviceOverlayPositions
            .filter { pairedDeviceIDs.contains($0.key) && $0.value != .center }
            .sorted { $0.key < $1.key }
            .map { "\($0.key):\($0.value.rawValue)" }
            .joined(separator: ",")
    }

    private var deviceOutputProfileText: String {
        deviceOutputProfiles
            .filter { pairedDeviceIDs.contains($0.key) && $0.value != defaultOutputProfile }
            .sorted { $0.key < $1.key }
            .map { deviceID, profile in
                """

                [device.\(deviceID).output]
                transform = "\(profile.transform.rawValue)"
                translation_target = "\(profile.translationTarget.tomlEscaped)"
                """
            }
            .joined(separator: "\n")
    }
}

private struct ConfigFile: Decodable {
    var asr_provider: String?
    var voicestick_api_key: String?
    var voicestick_cloud_url: String?
    var volcengine_api_key: String?
    var api_key: String?
    var llm_base_url: String?
    var llm_api_key: String?
    var llm_model: String?
    var interaction_mode: String?
    var output_target: String?
    var text_transform: String?
    var translation_target: String?
    var resource_id: String?
    var asr_hotwords: String?
    var paired_device_ids: String?
    var device_theme_colors: String?
    var device_overlay_positions: String?
    var auto_enter: Bool?
    var debug_audio_cache: Bool?
    var debug_audio_dir: String?
    var output: OutputConfigFile?
    var device: [String: DeviceConfigFile]?
}

private struct OutputConfigFile: Decodable {
    var target: String?
    var transform: String?
    var translation_target: String?
}

private struct DeviceConfigFile: Decodable {
    var output: OutputConfigFile?
}

private extension Bool {
    var tomlValue: String { self ? "true" : "false" }
}

private extension String {
    var tomlEscaped: String {
        replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
    }
}
