#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace voicestick {

enum class AsrProvider {
    kVoiceStickCloud,
    kVolcengine,
};

enum class InteractionMode {
    kHoldToTalk,
    kClickToTalk,
};

enum class OverlayThemeColor {
    kWhite,
    kPink,
    kGreen,
    kYellow,
    kBlue,
    kPurple,
};

enum class OverlayPosition {
    kCenter,
    kTopLeft,
    kTopRight,
    kBottomLeft,
    kBottomRight,
};

enum class OutputTarget {
    kFocusedApp,
    kSubtitle,
};

enum class TextTransform {
    kOriginal,
    kTranslate,
};

enum class BluetoothAddressKind : std::uint8_t {
    kUnspecified = 0,
    kPublic = 1,
    kRandom = 2,
};

struct PairedDeviceEntry {
    std::string device_id;
    std::uint64_t bluetooth_address = 0;
    BluetoothAddressKind address_kind = BluetoothAddressKind::kUnspecified;
    std::string name;
    std::string hardware;
    std::string firmware_version;
};

struct OutputProfile {
    OutputTarget target = OutputTarget::kFocusedApp;
    TextTransform transform = TextTransform::kOriginal;
    std::string translation_target = "en";

    bool operator==(const OutputProfile& other) const = default;
};

struct AppConfig {
    static constexpr std::string_view minimum_compatible_firmware_version = "0.3.0";

    AsrProvider asr_provider = AsrProvider::kVolcengine;
    std::string voicestick_api_key;
    std::string voicestick_cloud_url = "wss://api.xiaozhi.me/voicestick/asr/";
    std::string volcengine_api_key;
    std::string llm_base_url = "https://api.openai.com/v1";
    std::string llm_api_key;
    std::string llm_model = "gpt-5.5";
    InteractionMode interaction_mode = InteractionMode::kHoldToTalk;
    std::string resource_id = "volc.seedasr.sauc.duration";
    std::vector<std::string> asr_hotwords;
    std::vector<std::string> paired_device_ids;
    std::vector<PairedDeviceEntry> paired_devices;
    std::map<std::string, OverlayThemeColor> device_theme_colors;
    std::map<std::string, OverlayPosition> device_overlay_positions;
    OutputProfile default_output_profile;
    std::map<std::string, OutputProfile> device_output_profiles;
    bool auto_enter = true;
    bool debug_audio_cache = false;
    std::filesystem::path debug_audio_directory;

    static std::filesystem::path ConfigDirectory();
    static std::filesystem::path ConfigPath();
    static std::filesystem::path DefaultDebugAudioDirectory();
    static AppConfig Defaults();
    static AppConfig Load();
    static const std::vector<std::string>& SupportedResourceIds();

    void Save() const;
    void SavePairedDevice(const PairedDeviceEntry& entry);
    void SavePairedDeviceInfo(const std::string& device_id,
                              const std::string& hardware,
                              const std::string& firmware_version);
    void RemovePairedDevice(const std::string& device_id);
    std::string ActiveApiKey() const;
    std::string ActiveWebsocketUrl() const;
    OutputProfile OutputProfileForDevice(const std::optional<std::string>& device_id) const;
};

std::string AsrProviderName(AsrProvider provider);
AsrProvider AsrProviderFromName(std::string_view name);
std::string InteractionModeName(InteractionMode mode);
InteractionMode InteractionModeFromName(std::string_view name);
std::string OverlayThemeColorName(OverlayThemeColor color);
OverlayThemeColor OverlayThemeColorFromName(std::string_view name);
std::string OverlayThemeColorDisplayName(OverlayThemeColor color);
std::string OverlayPositionName(OverlayPosition position);
OverlayPosition OverlayPositionFromName(std::string_view name);
std::string OverlayPositionDisplayName(OverlayPosition position);
std::string OutputTargetName(OutputTarget target);
OutputTarget OutputTargetFromName(std::string_view name);
std::string OutputTargetDisplayName(OutputTarget target);
std::string TextTransformName(TextTransform transform);
TextTransform TextTransformFromName(std::string_view name);
std::string TextTransformDisplayName(TextTransform transform);
std::vector<std::string> ParseDeviceIdList(std::string_view text);
std::vector<std::string> ParseHotwordList(std::string_view text);

} // namespace voicestick
