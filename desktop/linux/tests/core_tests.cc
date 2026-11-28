#include "asr_protocol.h"
#include "ble_protocol.h"
#include "byte_utils.h"
#include "firmware_manifest.h"
#include "ogg_opus_muxer.h"
#include "voice_stick_coordinator.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace voicestick;

namespace {

struct SentDisplay {
    std::string scene;
    std::string text;
    std::optional<std::string> device_id;
};

class FakeBleCentral : public BleCentral {
public:
    void Start() override {
        if (announce_v2) {
            AnnounceV2("5A74");
            AnnounceV2("6B85");
        }
    }
    void AnnounceV2(const std::string& device_id, bool supported = true) {
        if (supported) {
            DeviceInfoResult info;
            info.protocol_versions = {2};
            info.hardware = "m5stack_stick_s3";
            info.firmware_version = "0.5.0";
            info.buttons = {"primary", "secondary"};
            info.has_audio_in = true;
            info.has_display = true;
            ready_device_ids.insert(device_id);
            if (on_device_info) on_device_info(device_id, info);
            if (on_device_ready) on_device_ready(device_id, info);
        } else {
            LegacyDeviceInfo legacy;
            legacy.hardware = "m5stack_stick_s3";
            legacy.firmware_version = "0.4.2";
            ready_device_ids.erase(device_id);
            if (on_legacy_device_info) on_legacy_device_info(device_id, legacy);
        }
    }
    bool announce_v2 = true;
    void UpdatePairedDeviceIds(const std::vector<std::string>& ids) override {
        paired_device_ids = ids;
    }
    void ConnectPairedDevice(const std::string&,
                             std::uint64_t,
                             BluetoothAddressKind,
                             const std::string&) override {}
    void SetDisplay(const std::string& scene,
                    const std::string& text,
                    const std::optional<std::string>& device_id) override {
        sent_displays.push_back(SentDisplay{scene, text, device_id});
    }
    void StartAudioIn(const std::string& device_id, InteractionMode mode) override {
        started_audio_in.emplace_back(device_id, mode);
    }
    void StopAudioIn(const std::string& device_id) override {
        stopped_audio_in.push_back(device_id);
    }
    std::vector<std::string> ConnectedReadyDeviceIds() const override {
        return std::vector<std::string>(ready_device_ids.begin(), ready_device_ids.end());
    }
    void UpdateFirmware(ByteVector,
                        const std::string&,
                        std::function<void(FirmwareUpdateProgress)>,
                        std::function<void(bool, std::string)> completion) override {
        completion(false, "not implemented");
    }
    void CancelFirmwareUpdate() override {}
    bool IsConnected(const std::string& device_id) const override {
        return connected_device_ids.contains(device_id);
    }

    std::vector<std::string> paired_device_ids;
    std::set<std::string> connected_device_ids;
    std::set<std::string> ready_device_ids;
    std::vector<SentDisplay> sent_displays;
    std::vector<std::pair<std::string, InteractionMode>> started_audio_in;
    std::vector<std::string> stopped_audio_in;
};

class FakeAsrClient : public AsrClient {
public:
    bool Start(AsrSessionOptions options = {}) override {
        last_options = std::move(options);
        started = true;
        return start_result;
    }
    void SendOggOpusChunk(std::span<const std::uint8_t>, bool is_last) override {
        ++sent_chunks;
        last_chunk_was_final = is_last;
    }
    void Cancel() override {
        cancelled = true;
    }
    std::string LastStartError() const override {
        return start_error;
    }

    bool start_result = true;
    std::string start_error;
    bool started = false;
    bool cancelled = false;
    int sent_chunks = 0;
    bool last_chunk_was_final = false;
    AsrSessionOptions last_options;
};

class FakeUi : public VoiceStickUi {
public:
    void SetStatus(const std::string& status) override {
        statuses.push_back(status);
    }
    void SetConnectedDevices(const std::vector<ConnectedDevice>& devices) override {
        connected_devices = devices;
    }
    void SetDeviceInfo(const DeviceInfo& info) override {
        device_infos.push_back(info);
    }
    void SetFirmwareInfo(const std::map<std::string, DeviceFirmwareInfo>& info_by_device_id) override {
        firmware_info_by_device_id = info_by_device_id;
    }
    void SetPairingError(const std::string& device_id, const std::string& message) override {
        pairing_errors.push_back(device_id + ":" + message);
    }
    void ShowFirmwareUpdatePrompt(const std::string& device_id,
                                  const std::string& current_version,
                                  const std::string& latest_version,
                                  bool is_below_minimum) override {
        firmware_update_prompts.push_back(device_id + ":" + current_version + ":" + latest_version +
                                          (is_below_minimum ? ":minimum" : ":latest"));
    }
    void SetPairedDeviceIds(const std::vector<std::string>& ids) override {
        paired_device_ids = ids;
    }
    void SetHasRecoverableInput(bool has_recoverable_input) override {
        has_recoverable_input_set = has_recoverable_input;
    }
    void ShowListening(const std::optional<std::string>&) override {
        ++show_listening_count;
    }
    void ShowPartial(const std::string& text, const std::optional<std::string>&) override {
        partials.push_back(text);
    }
    void ShowFinalCountdown(const std::string& text,
                            const std::optional<std::string>&,
                            std::function<void()> on_complete) override {
        final_countdowns.push_back(text);
        final_countdown_completion = std::move(on_complete);
    }
    void ShowPausedFinal(const std::string& text, const std::optional<std::string>&) override {
        paused_finals.push_back(text);
    }
    void ShowError(const std::string& text,
                   const std::optional<std::string>&,
                   std::function<void()> on_complete) override {
        errors.push_back(text);
        error_completion = std::move(on_complete);
    }
    void ShowCloudUpgrade(const std::string& message,
                          const std::string& url,
                          const std::optional<std::string>&) override {
        cloud_upgrades.push_back(message + "|" + url);
    }
    void HideOverlay(std::function<void()> on_hidden = {}) override {
        ++hide_overlay_count;
        if (on_hidden) on_hidden();
    }
    void ShowSubtitle(const std::string& text,
                      const std::string& device_id,
                      OverlayThemeColor color) override {
        subtitles.push_back(device_id + ":" + text + ":" + OverlayThemeColorName(color));
    }
    void HideSubtitles() override {
        ++hide_subtitles_count;
    }

    std::vector<std::string> statuses;
    std::vector<ConnectedDevice> connected_devices;
    std::vector<DeviceInfo> device_infos;
    std::map<std::string, DeviceFirmwareInfo> firmware_info_by_device_id;
    std::vector<std::string> pairing_errors;
    std::vector<std::string> firmware_update_prompts;
    std::vector<std::string> paired_device_ids;
    std::vector<std::string> partials;
    std::vector<std::string> cloud_upgrades;
    std::vector<std::string> final_countdowns;
    std::vector<std::string> paused_finals;
    std::vector<std::string> errors;
    std::vector<std::string> subtitles;
    std::function<void()> final_countdown_completion;
    std::function<void()> error_completion;
    bool has_recoverable_input_set = false;
    int show_listening_count = 0;
    int hide_overlay_count = 0;
    int hide_subtitles_count = 0;
};

class FakeInputInjector : public InputInjector {
public:
    void Paste(const std::string& text, bool press_enter) override {
        pasted_text = text;
        pasted_enter = press_enter;
    }

    std::string pasted_text;
    bool pasted_enter = false;
};

AudioStreamFrame AudioStartFrame(std::uint16_t stream_id = 1, std::uint32_t seq = 0) {
    AudioStreamFrame frame;
    frame.stream_id = stream_id;
    frame.seq = seq;
    frame.flags = 0x01;
    return frame;
}

AudioStreamFrame AudioDataFrame(std::uint16_t stream_id, std::uint32_t seq, bool is_end = false) {
    AudioStreamFrame frame;
    frame.stream_id = stream_id;
    frame.seq = seq;
    frame.flags = is_end ? 0x02 : 0;
    frame.payload = {1, 2, 3, 4};
    return frame;
}

AudioStreamFrame EmptyEndFrame(std::uint16_t stream_id, std::uint32_t seq) {
    AudioStreamFrame frame;
    frame.stream_id = stream_id;
    frame.seq = seq;
    frame.flags = 0x02;
    return frame;
}

bool HasDisplayScene(const FakeBleCentral& ble, const std::string& scene, const std::string& device_id) {
    return std::any_of(ble.sent_displays.begin(), ble.sent_displays.end(),
                       [&](const SentDisplay& sent) {
                           return sent.scene == scene &&
                                  sent.device_id.has_value() &&
                                  *sent.device_id == device_id;
                       });
}

bool HasDisplayText(const FakeBleCentral& ble,
                    const std::string& scene,
                    const std::string& text,
                    const std::string& device_id) {
    return std::any_of(ble.sent_displays.begin(), ble.sent_displays.end(),
                       [&](const SentDisplay& sent) {
                           return sent.scene == scene &&
                                  sent.text == text &&
                                  sent.device_id.has_value() &&
                                  *sent.device_id == device_id;
                       });
}

void TestDeviceIds() {
    assert(BleProtocol::NormalizeDeviceId("vs-c3d8") == "C3D8");
    assert(BleProtocol::NormalizeDeviceId("09af") == "09AF");
    assert(!BleProtocol::DeviceIdFromName("Other").has_value());
    assert(BleProtocol::DeviceIdFromName("VS-C3D8").value() == "C3D8");
}

void TestAudioAndInputFrameParsingV2() {
    ByteVector frame = {2, 0x01, 18, 0};
    AppendLe16(frame, 4);
    frame.push_back(0x03);
    frame.push_back(0);
    AppendLe32(frame, 7);
    AppendLe32(frame, 12345);
    AppendLe16(frame, 3);
    frame.push_back(10);
    frame.push_back(11);
    frame.push_back(12);
    auto parsed = BleProtocol::ParseAudioStreamFrame(frame);
    assert(parsed.has_value());
    assert(parsed->stream_id == 4);
    assert(parsed->seq == 7);
    assert(parsed->timestamp_ms == 12345);
    assert(parsed->IsStart() && parsed->IsEnd());
    assert(parsed->payload.size() == 3);

    frame[2] = 21;
    frame.insert(frame.begin() + 18, {0xAA, 0xBB, 0xCC});
    const auto extended = BleProtocol::ParseAudioStreamFrame(frame);
    assert(extended && extended->payload == parsed->payload);
    frame.pop_back();
    assert(!BleProtocol::ParseAudioStreamFrame(frame));

    ByteVector input_frame = {2, 0x04, 12, 0};
    AppendLe16(input_frame, 1);
    AppendLe16(input_frame, 5);
    AppendLe32(input_frame, 200);
    AppendLe32(input_frame, 0x01);
    auto parsed_input = BleProtocol::ParseInputStreamFrame(input_frame);
    assert(parsed_input && parsed_input->stream_id == 1 && parsed_input->buttons_mask == 1);
}

void TestControlParsingAndChunkingV2() {
    const auto rpc = BleProtocol::ParseControlMessage(
        R"({"id":1,"result":{"protocol_version":2,"hardware":"voicestick_c3","firmware_version":"0.5.0",)"
        R"("buttons":["primary","secondary"],)"
        R"("streams":{"input":{},"audio_in":{"codecs":["opus"],"frame_ms":[60]}},)"
        R"("display":{"scenes":["idle","recording","busy","error"]}}})");
    assert(rpc && rpc->kind == ControlMessage::Kind::kRpcResponse && !rpc->rpc_response.is_error);
    assert(rpc->rpc_response.device_info && rpc->rpc_response.device_info->SupportsProtocolV2());

    const auto closed = BleProtocol::ParseControlMessage(
        R"({"event":"stream.closed","data":{"stream_id":2,"reason":"ota"}})");
    assert(closed && closed->kind == ControlMessage::Kind::kEvent);
    assert(closed->control_event.stream_id == 2 && closed->control_event.reason == "ota");

    assert(!BleProtocol::ParseControlMessage(R"({"id":1,"event":"stream.closed","data":{"stream_id":2}})"));

    std::uint8_t next_msg_id = 1;
    const std::string long_rpc = BleProtocol::RpcDisplaySet(2, "busy", std::string(64, 'a'));
    const auto chunks = BleProtocol::EncodeControlFrames(long_rpc, 20, next_msg_id);
    assert(chunks.size() > 1);
    ControlReassembler reassembler;
    std::optional<std::string> assembled;
    for (const auto& chunk : chunks) {
        assembled = reassembler.Push(chunk);
    }
    assert(assembled && *assembled == long_rpc);
}

void TestOggMuxer() {
    OggOpusMuxer muxer(16000, 1);
    ByteVector opus = {1, 2, 3, 4};
    auto ogg = muxer.Append(opus, false);
    assert(ogg.size() > 64);
    assert(std::string(reinterpret_cast<const char*>(ogg.data()), 4) == "OggS");
    auto tail = muxer.Finish();
    assert(std::string(reinterpret_cast<const char*>(tail.data()), 4) == "OggS");
}

void TestAsrProtocol() {
    AppConfig config = AppConfig::Defaults();
    config.asr_hotwords = {"小智", "VoiceStick"};
    auto event_payload = [](const ByteVector& frame, const std::string& session_id) {
        const std::size_t payload_size_offset = 12 + session_id.size();
        const auto payload_size = ReadBe32(std::span(frame.data() + payload_size_offset, 4));
        const std::size_t payload_offset = payload_size_offset + 4;
        assert(payload_size == frame.size() - payload_offset);
        return std::string(reinterpret_cast<const char*>(frame.data() + payload_offset),
                           frame.size() - payload_offset);
    };

    const std::string payload_session_id = "payload-session";
    auto request = AsrProtocol::MakeStartSessionFrame(config, payload_session_id);
    assert(request.size() > 16 + payload_session_id.size());
    assert(request[0] == 0x11);
    assert((request[1] >> 4) == 0x01);
    assert(ReadBe32(std::span(request.data() + 4, 4)) == 100);
    const auto payload = event_payload(request, payload_session_id);
    assert(payload.find("\"corpus\"") != std::string::npos);
    assert(payload.find("\\\"hotwords\\\"") != std::string::npos);
    assert(payload.find("\\\"word\\\":\\\"VoiceStick\\\"") != std::string::npos);

    const std::string body =
        "{\"error\":\"invalid_token\",\"message\":\"VoiceStick Cloud API key is invalid.\","
        "\"upgrade_url\":\"https://example.test/upgrade\"}";
    ByteVector response = {0x11, 0xf0, 0x10, 0x00};
    AppendBe32(response, 44002);
    AppendBe32(response, static_cast<std::uint32_t>(body.size()));
    response.insert(response.end(), body.begin(), body.end());
    auto parsed = AsrProtocol::ParseResponse(response);
    assert(parsed.has_value());
    assert(parsed->is_error);
    assert(parsed->text == "ASR 44002: VoiceStick Cloud API key is invalid.");
    assert(parsed->upgrade_url && *parsed->upgrade_url == "https://example.test/upgrade");

    auto start_connection = AsrProtocol::MakeStartConnectionFrame(config);
    assert(start_connection.size() > 12);
    assert((start_connection[1] >> 4) == 0x01);
    assert((start_connection[1] & 0x0f) == 0x04);
    assert(ReadBe32(std::span(start_connection.data() + 4, 4)) == 1);

    const std::string session_id = "session-1";
    auto start_session = AsrProtocol::MakeStartSessionFrame(config, session_id);
    assert(ReadBe32(std::span(start_session.data() + 4, 4)) == 100);
    assert(ReadBe32(std::span(start_session.data() + 8, 4)) == session_id.size());
    assert(std::string(reinterpret_cast<const char*>(start_session.data() + 12),
                       session_id.size()) == session_id);

    ByteVector opus = {1, 2, 3};
    auto task = AsrProtocol::MakeTaskRequestFrame(opus, session_id);
    assert((task[1] >> 4) == 0x02);
    assert(ReadBe32(std::span(task.data() + 4, 4)) == 200);

    const std::string event_body = "{\"result\":{\"text\":\"hi\"}}";
    ByteVector event_response = {0x11, 0x94, 0x10, 0x00};
    AppendBe32(event_response, 451);
    AppendBe32(event_response, static_cast<std::uint32_t>(session_id.size()));
    event_response.insert(event_response.end(), session_id.begin(), session_id.end());
    AppendBe32(event_response, static_cast<std::uint32_t>(event_body.size()));
    event_response.insert(event_response.end(), event_body.begin(), event_body.end());
    auto parsed_event = AsrProtocol::ParseEventResponse(event_response);
    assert(parsed_event.has_value());
    assert(parsed_event->event == AsrEvent::kAsrResponse);
    assert(parsed_event->session_id == session_id);
    assert(AsrProtocol::ExtractTranscript(parsed_event->payload_text) == "hi");

    AsrSessionOptions options;
    options.hotwords = {"VoiceStick"};
    options.show_utterances = true;
    options.result_type = AsrResultType::kSingle;
    const std::string utterance_session_id = "utterance-session";
    auto utterance_request = AsrProtocol::MakeStartSessionFrame(config, utterance_session_id, options);
    const auto utterance_payload = event_payload(utterance_request, utterance_session_id);
    assert(utterance_payload.find("\"show_utterances\":true") != std::string::npos);
    assert(utterance_payload.find("\"result_type\":\"single\"") != std::string::npos);

    const std::string segment_json =
        "{\"result\":{\"text\":\"hello world\",\"utterances\":["
        "{\"text\":\"hello\",\"definite\":true,\"start_time\":0,\"end_time\":500},"
        "{\"text\":\"world\",\"definite\":false,\"start_time\":500,\"end_time\":900}]}}";
    auto segments = AsrProtocol::ExtractSegments(segment_json);
    assert(segments.size() == 2);
    assert(segments[0].text == "hello");
    assert(segments[0].definite);
    std::set<std::string> emitted;
    auto definite = AsrProtocol::ExtractNewDefiniteSegments(segment_json, &emitted);
    assert(definite.size() == 1);
    assert(AsrProtocol::ExtractNewDefiniteSegments(segment_json, &emitted).empty());
}

void TestAppConfig() {
    AppConfig cloud = AppConfig::Defaults();
    assert(cloud.asr_provider == AsrProvider::kVoiceStickCloud);
    cloud.asr_provider = AsrProvider::kVoiceStickCloud;
    cloud.voicestick_cloud_url = "";
    assert(cloud.ActiveWebsocketUrl() == "wss://api.xiaozhi.me/voicestick/asr/");

    cloud.voicestick_cloud_url = "  wss://example.test/asr?token=1  ";
    assert(cloud.ActiveWebsocketUrl() == "wss://example.test/asr?token=1");

    AppConfig volcengine = AppConfig::Defaults();
    volcengine.asr_provider = AsrProvider::kVolcengine;
    assert(volcengine.ActiveWebsocketUrl().starts_with("wss://openspeech.bytedance.com/"));

    PairedDeviceEntry entry;
    entry.device_id = "5A74";
    entry.bluetooth_address = 0x70041DDA5A76;
    entry.address_kind = BluetoothAddressKind::kPublic;
    entry.name = "VS-5A74";
    entry.hardware = "stick_s3";
    entry.firmware_version = "0.1.2";
    AppConfig cache = AppConfig::Defaults();
    cache.paired_devices.push_back(entry);
    cache.paired_device_ids.push_back(entry.device_id);
    assert(cache.paired_devices.front().hardware == "stick_s3");
    assert(cache.paired_devices.front().firmware_version == "0.1.2");
    assert(OverlayThemeColorFromName("pink") == OverlayThemeColor::kPink);
    assert(OverlayThemeColorName(OverlayThemeColor::kGreen) == "green");
    assert(OverlayThemeColorDisplayName(OverlayThemeColor::kYellow) == "Yellow");
    assert(OverlayPositionFromName("top_right") == OverlayPosition::kTopRight);
    assert(OverlayPositionName(OverlayPosition::kBottomLeft) == "bottom_left");
    assert(OverlayPositionDisplayName(OverlayPosition::kCenter) == "Center");
    cache.default_output_profile.target = OutputTarget::kSubtitle;
    cache.default_output_profile.transform = TextTransform::kOriginal;
    cache.device_output_profiles["5A74"] = OutputProfile{
        OutputTarget::kSubtitle,
        TextTransform::kTranslate,
        "zh-Hans",
    };
    auto profile = cache.OutputProfileForDevice(std::optional<std::string>("5A74"));
    assert(profile.target == OutputTarget::kSubtitle);
    assert(profile.transform == TextTransform::kTranslate);
    assert(profile.translation_target == "zh-Hans");
    assert(OutputTargetName(OutputTarget::kFocusedApp) == "focused_app");
    assert(TextTransformFromName("translate") == TextTransform::kTranslate);
    const auto hotwords = ParseHotwordList(" 小智,VoiceStick\r\n小智\n豆包 ");
    assert((hotwords == std::vector<std::string>{"小智", "VoiceStick", "豆包"}));
    assert(AppConfig::Defaults().confirmation_countdown_ms == 1000);
}

void TestFirmwareManifestParsingAndVersionCompare() {
    const std::string json =
        "{\"hardware\":\"sticks3\",\"version\":\"0.2.3\",\"ota_url\":\"https://example.test/ota.bin\","
        "\"ota_sha256\":\"abc\",\"ota_size\":123,\"merged_url\":\"https://example.test/merged.bin\","
        "\"merged_sha256\":\"def\",\"merged_size\":456}";
    auto manifest = ParseFirmwareManifest(json);
    assert(manifest.has_value());
    assert(manifest->hardware == "sticks3");
    assert(manifest->version == "0.2.3");
    assert(manifest->ota_size == 123);
    assert(FirmwareVersion::IsOlderThan("0.2.2", "0.2.3"));
    assert(FirmwareVersion::IsOlderThan("0.2.3-beta", "0.2.3"));
    assert(!FirmwareVersion::IsOlderThan("0.2.3", "0.2.3-beta"));
    assert(IsFirmwareHardwareCompatible("sticks3", "0.1.2", "stick_s3"));
    assert(IsFirmwareHardwareCompatible("", "0.1.2", "stick_s3"));
    assert(IsFirmwareHardwareCompatible("", "", "stick_s3"));
}

void TestFirmwareLinuxAppVersionRequirement() {
    const std::string required_fields =
        R"("hardware":"sticks3","version":"1.0.0","ota_url":"https://example.test/ota.bin",)"
        R"("ota_sha256":"abc","ota_size":123)";
    const auto unrestricted = ParseFirmwareManifest("{" + required_fields + "}");
    assert(unrestricted && unrestricted->SupportsLinuxAppVersion(""));
    const auto other_platform = ParseFirmwareManifest(
        "{" + required_fields + R"(,"minimum_app_versions":{"macos":"0.4.0"}})");
    assert(other_platform && other_platform->SupportsLinuxAppVersion("0.1.0"));
    const auto restricted = ParseFirmwareManifest(
        "{" + required_fields + R"(,"minimum_app_versions":{"linux":"0.4.0"}})");
    assert(restricted && restricted->minimum_linux_app_version == "0.4.0");
    assert(!restricted->SupportsLinuxAppVersion("0.3.9"));
    assert(!restricted->SupportsLinuxAppVersion("0.4.0-beta"));
    assert(restricted->SupportsLinuxAppVersion("0.4.0"));
    assert(restricted->SupportsLinuxAppVersion("0.4.1"));
    assert(!ParseFirmwareManifest(
        "{" + required_fields + R"(,"minimum_app_versions":{"linux":"invalid"}})"));
}

void TestCoordinatorRequiresInputProtocolOnEachConnection() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    ble_ptr->announce_v2 = false;
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble),
                                      std::make_unique<FakeAsrClient>(), &ui, &input);
    coordinator.Start();

    ble_ptr->AnnounceV2("5A74", false);
    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 0));
    assert(ui.show_listening_count == 0);

    ble_ptr->AnnounceV2("5A74", true);
    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 0));
    assert(ui.show_listening_count == 1);

    ble_ptr->on_connection_change({});
    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 1));
    assert(ui.show_listening_count == 1);
}

void TestCoordinatorCancelsShortPrimaryPress() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 0));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(1, 1));

    assert(asr_ptr->cancelled);
    assert(ui.show_listening_count == 1);
    assert(ui.hide_overlay_count == 1);
    assert(HasDisplayScene(*ble_ptr, "recording", "5A74"));
    assert(HasDisplayScene(*ble_ptr, "idle", "5A74"));
}

void TestCoordinatorPrimaryDuringFinalizingRefreshesThinking() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 0));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(1, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(1, 2));
    assert(asr_ptr->started);
    assert(asr_ptr->last_chunk_was_final);

    const auto before = ble_ptr->sent_displays.size();
    ble_ptr->on_button_event("5A74", "primary", true, 1600);

    assert(ble_ptr->sent_displays.size() == before + 1);
    assert(ble_ptr->sent_displays.back().scene == "busy");
    assert(ble_ptr->sent_displays.back().device_id == std::optional<std::string>("5A74"));
}

void TestCoordinatorSecondaryCancelsFinalizing() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 0));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(1, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(1, 2));

    ble_ptr->on_button_event("5A74", "secondary", true, 1000);
    ble_ptr->on_button_event("5A74", "secondary", false, 1100);

    assert(asr_ptr->cancelled);
    assert(ui.hide_overlay_count == 1);
    assert(HasDisplayScene(*ble_ptr, "idle", "5A74"));
}

void TestCoordinatorAcceptsAudioFramesUntilEnd() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 0));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(1, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(1, 2));
    assert(asr_ptr->started);
    assert(!asr_ptr->last_chunk_was_final);

    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(1, 3));
    assert(asr_ptr->sent_chunks >= 3);
    assert(asr_ptr->last_chunk_was_final);
}

void TestCoordinatorPrimaryPausesPendingConfirmation() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 0));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(1, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(1, 2));
    asr_ptr->on_final("hello");

    assert(!ble_ptr->stopped_audio_in.empty() && ble_ptr->stopped_audio_in.back() == "5A74");
    ble_ptr->on_button_event("5A74", "primary", true, 2000);

    assert(ui.final_countdowns.size() == 1);
    assert(ui.final_countdowns.back() == "hello");
    assert(ui.paused_finals.size() == 1);
    assert(ui.paused_finals.back() == "hello");
    assert(ble_ptr->sent_displays.back().scene == "busy");
}

void TestCoordinatorZeroCountdownHidesOverlayAndPastes() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.confirmation_countdown_ms = 0;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 0));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(1, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(1, 2));
    const int hides_before = ui.hide_overlay_count;
    asr_ptr->on_final("hello");

    assert(ui.final_countdowns.empty());
    assert(ui.hide_overlay_count > hides_before);
    assert(input.pasted_text == "hello");
}

void TestCoordinatorDisconnectDuringRecordingPausesConfirmation() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 0));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(1, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_connection_change({});
    asr_ptr->on_final("hello");

    assert(ui.paused_finals.size() == 1);
    assert(ui.paused_finals.back() == "hello");
    assert(ui.final_countdowns.empty());
    assert(input.pasted_text.empty());
}

void TestCoordinatorOtherDeviceDuringRecordingGetsReady() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 0));

    const auto before = ble_ptr->sent_displays.size();
    ble_ptr->on_audio_frame("6B85", AudioStartFrame(2, 0));

    assert(ble_ptr->sent_displays.size() == before + 1);
    assert(ble_ptr->sent_displays.back().scene == "idle");
    assert(ble_ptr->sent_displays.back().device_id == std::optional<std::string>("6B85"));
}

void TestCoordinatorSubtitleOutputSkipsPaste() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto primary_asr = std::make_unique<FakeAsrClient>();
    FakeAsrClient* subtitle_asr_ptr = nullptr;
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kSubtitle;
    config.interaction_mode = InteractionMode::kClickToTalk;
    config.device_theme_colors["5A74"] = OverlayThemeColor::kBlue;
    VoiceStickCoordinator coordinator(
        config,
        std::move(ble),
        std::move(primary_asr),
        &ui,
        &input,
        [&](const AppConfig&) {
            auto asr = std::make_unique<FakeAsrClient>();
            subtitle_asr_ptr = asr.get();
            return asr;
        });
    coordinator.Start();

    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 0));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(1, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    assert(subtitle_asr_ptr != nullptr);
    assert(!subtitle_asr_ptr->started);
    subtitle_asr_ptr->on_partial("early subtitle");
    assert(!ui.partials.empty());
    assert(ui.partials.back() == "early subtitle");
    assert(ble_ptr->sent_displays.back().text != "early subtitle");
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(1, 2));
    assert(subtitle_asr_ptr->started);
    assert(subtitle_asr_ptr->last_options.show_utterances);
    assert(subtitle_asr_ptr->last_options.result_type == AsrResultType::kSingle);
    subtitle_asr_ptr->on_partial("interim subtitle");
    assert(!ui.partials.empty());
    assert(ui.partials.back() == "interim subtitle");
    subtitle_asr_ptr->on_final("hello subtitle");

    assert(input.pasted_text.empty());
    assert(!ui.subtitles.empty());
    assert(ui.subtitles.back() == "5A74:hello subtitle:blue");
    assert(ui.hide_overlay_count > 0);
    assert(HasDisplayScene(*ble_ptr, "idle", "5A74"));
}

void TestCoordinatorSubtitleFinalDoesNotBlockNextSession() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto primary_asr = std::make_unique<FakeAsrClient>();
    std::vector<FakeAsrClient*> subtitle_asrs;
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kSubtitle;
    config.interaction_mode = InteractionMode::kHoldToTalk;
    config.device_theme_colors["5A74"] = OverlayThemeColor::kBlue;
    VoiceStickCoordinator coordinator(
        config,
        std::move(ble),
        std::move(primary_asr),
        &ui,
        &input,
        [&](const AppConfig&) {
            auto asr = std::make_unique<FakeAsrClient>();
            subtitle_asrs.push_back(asr.get());
            return asr;
        });
    coordinator.Start();

    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 0));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(1, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(1, 2));
    assert(subtitle_asrs.size() == 1);
    assert(subtitle_asrs[0]->started);
    assert(HasDisplayScene(*ble_ptr, "idle", "5A74"));

    const auto idle_count_after_first_release = std::count_if(
        ble_ptr->sent_displays.begin(), ble_ptr->sent_displays.end(), [](const SentDisplay& sent) {
            return sent.scene == "idle" && sent.device_id == std::optional<std::string>("5A74");
        });

    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 3));
    assert(subtitle_asrs.size() == 2);
    assert(ble_ptr->sent_displays.back().scene == "recording");

    subtitle_asrs[0]->on_final("first late subtitle");
    assert(!ui.subtitles.empty());
    assert(ui.subtitles.back() == "5A74:first late subtitle:blue");
    const auto idle_count_after_late_final = std::count_if(
        ble_ptr->sent_displays.begin(), ble_ptr->sent_displays.end(), [](const SentDisplay& sent) {
            return sent.scene == "idle" && sent.device_id == std::optional<std::string>("5A74");
        });
    assert(idle_count_after_late_final == idle_count_after_first_release);
}

void TestCoordinatorShortSubtitleEndReturnsReady() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto primary_asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kSubtitle;
    config.interaction_mode = InteractionMode::kHoldToTalk;
    VoiceStickCoordinator coordinator(
        config,
        std::move(ble),
        std::move(primary_asr),
        &ui,
        &input,
        [](const AppConfig&) {
            return std::make_unique<FakeAsrClient>();
        });
    coordinator.Start();

    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 0));
    assert(ble_ptr->sent_displays.back().scene == "recording");
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(1, 1));

    assert(ui.hide_overlay_count > 0);
    assert(!ui.statuses.empty());
    assert(ui.statuses.back() == "Ready");
    assert(HasDisplayScene(*ble_ptr, "idle", "5A74"));
}

void TestCoordinatorMainPartialSentToDeviceOnlyAfterFinalAudio() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 0));
    asr_ptr->on_partial("early");
    assert(!ui.partials.empty());
    assert(ui.partials.back() == "early");
    assert(ble_ptr->sent_displays.back().text != "early");

    ble_ptr->on_audio_frame("5A74", AudioDataFrame(1, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(1, 2));
    asr_ptr->on_partial("late");

    assert(ble_ptr->sent_displays.back().scene == "busy");
    assert(ble_ptr->sent_displays.back().text == "late");
}

void TestCoordinatorShowsDetailedAsrStartError() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    asr_ptr->start_result = false;
    asr_ptr->start_error = "Missing ASR API key";
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 0));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(1, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(1, 2));

    assert(!ui.errors.empty());
    assert(ui.errors.back() == "Missing ASR API key");
    assert(HasDisplayText(*ble_ptr, "error", "Missing ASR API key", "5A74"));
}

void TestCoordinatorCloudUpgradeRecoversDeviceAfterAsrError() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 0));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(1, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(1, 2));
    asr_ptr->on_error("ASR 44002: VoiceStick Cloud API key is invalid.");
    asr_ptr->on_upgrade_url("https://example.test/upgrade",
                            "ASR 44002: VoiceStick Cloud API key is invalid.");

    assert(HasDisplayText(*ble_ptr, "error", "ASR 44002: VoiceStick Cloud API key is invalid.", "5A74"));
    assert(HasDisplayScene(*ble_ptr, "idle", "5A74"));
    assert(!ui.cloud_upgrades.empty());
}

} // namespace

int main() {
    TestDeviceIds();
    TestAudioAndInputFrameParsingV2();
    TestControlParsingAndChunkingV2();
    TestOggMuxer();
    TestAsrProtocol();
    TestAppConfig();
    TestFirmwareManifestParsingAndVersionCompare();
    TestFirmwareLinuxAppVersionRequirement();
    TestCoordinatorRequiresInputProtocolOnEachConnection();
    TestCoordinatorCancelsShortPrimaryPress();
    TestCoordinatorPrimaryDuringFinalizingRefreshesThinking();
    TestCoordinatorSecondaryCancelsFinalizing();
    TestCoordinatorAcceptsAudioFramesUntilEnd();
    TestCoordinatorPrimaryPausesPendingConfirmation();
    TestCoordinatorZeroCountdownHidesOverlayAndPastes();
    TestCoordinatorDisconnectDuringRecordingPausesConfirmation();
    TestCoordinatorOtherDeviceDuringRecordingGetsReady();
    TestCoordinatorSubtitleOutputSkipsPaste();
    TestCoordinatorSubtitleFinalDoesNotBlockNextSession();
    TestCoordinatorShortSubtitleEndReturnsReady();
    TestCoordinatorMainPartialSentToDeviceOnlyAfterFinalAudio();
    TestCoordinatorShowsDetailedAsrStartError();
    TestCoordinatorCloudUpgradeRecoversDeviceAfterAsrError();
    return 0;
}
