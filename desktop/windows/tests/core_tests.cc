#include "asr_protocol.h"
#include "ble_protocol.h"
#include "byte_utils.h"
#include "firmware_manifest.h"
#include "ogg_opus_muxer.h"
#include "openai_realtime_protocol.h"
#include "voice_stick_coordinator.h"
#include "../../shared/shake_detector.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
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
    void AnnounceV2(const std::string& device_id, bool supported = true, bool with_imu = true) {
        if (supported) {
            DeviceInfoResult info;
            info.protocol_versions = {2};
            info.hardware = "m5stack_stick_s3";
            info.firmware_version = "0.5.0";
            info.buttons = {"primary", "secondary"};
            info.has_audio_in = true;
            info.has_display = true;
            if (with_imu) {
                info.imu = DeviceImuCapabilities{8192, 16.384, 100, true};
            }
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
    void StartImu(const std::string& device_id,
                  const std::string& axes,
                  int rate_hz,
                  int lease_ms) override {
        started_imu.push_back(device_id + ":" + axes + ":" + std::to_string(rate_hz) + ":" + std::to_string(lease_ms));
    }
    void RenewImu(const std::string&, int) override {}
    void StopImu(const std::string& device_id) override {
        stopped_imu.push_back(device_id);
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
    std::vector<std::string> started_imu;
    std::vector<std::string> stopped_imu;
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
    void SetFirmwareInfo(const std::map<std::string, DeviceFirmwareInfo>& info_by_device_id,
                         std::uint64_t) override {
        firmware_info_by_device_id = info_by_device_id;
    }
    void SetDeviceBatteryStatus(const std::string& device_id, const DeviceBatteryStatus& status) override {
        battery_status_by_device_id[device_id] = status;
    }
    void SetPairingError(const std::string& device_id, const std::string& message) override {
        pairing_errors.push_back(device_id + ":" + message);
    }
    void SetPairedDeviceIds(const std::vector<std::string>& ids) override {
        paired_device_ids = ids;
    }
    void SetHasRecoverableInput(bool has_recoverable_input) override {
        has_recoverable_input_set = has_recoverable_input;
    }
    void ShowListening(const OutputProfile&, const std::optional<std::string>&) override {
        ++show_listening_count;
    }
    void ShowPartial(const std::string& text, const std::optional<std::string>&) override {
        partials.push_back(text);
    }
    void ShowFinalCountdown(const std::string& text,
                            const std::optional<std::string>&,
                            int duration_ms, std::function<void()> on_complete) override {
        countdown_duration_ms = duration_ms;
        final_countdowns.push_back(text);
        final_countdown_completion = std::move(on_complete);
    }
    void ShowPausedFinal(const std::string& text, const std::optional<std::string>&) override {
        paused_finals.push_back(text);
    }
    std::optional<std::string> EditedConfirmationText() const override { return edited_confirmation; }
    void ConcealConfirmation() override { ++concealed_confirmations; }
    void ShowPasteFailure() override { ++paste_failures; }
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
    std::map<std::string, DeviceBatteryStatus> battery_status_by_device_id;
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
    std::optional<std::string> edited_confirmation;
    int countdown_duration_ms = 0;
    int paste_failures = 0;
    int concealed_confirmations = 0;
    int show_listening_count = 0;
    int hide_overlay_count = 0;
    int hide_subtitles_count = 0;
};

class FakeInputInjector : public InputInjector {
public:
    bool Paste(const std::string& text, bool press_enter) override {
        if (!paste_succeeds) return false;
        pasted_text = text;
        pasted_enter = press_enter;
        return true;
    }

    bool PasteConfirmation(const std::string& text, bool press_enter, bool use_current_field,
                           const std::function<void()>& conceal) override {
        attempted_enter = press_enter;
        used_current_field = use_current_field;
        if (conceal) conceal();
        return Paste(text, press_enter);
    }

    bool paste_succeeds = true;
    bool attempted_enter = false;
    bool used_current_field = false;

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

void EmitShakeSamples(FakeBleCentral& ble, const std::string& device_id) {
    const std::pair<std::int16_t, std::uint32_t> samples[] = {
        {0, 100}, {16000, 120}, {-16000, 220}, {16000, 320},
    };
    std::uint16_t seq = 0;
    for (const auto& [ax, ts_ms] : samples) {
        ImuStreamFrame frame;
        frame.stream_id = 3;
        frame.seq = seq++;
        frame.timestamp_ms = ts_ms;
        frame.ax = ax;
        ble.on_imu_frame(device_id, frame);
    }
}

void TestDeviceIds() {
    assert(BleProtocol::NormalizeDeviceId("vs-c3d8") == "C3D8");
    assert(BleProtocol::NormalizeDeviceId("09af") == "09AF");
    assert(!BleProtocol::DeviceIdFromName("Other").has_value());
    assert(BleProtocol::DeviceIdFromName("VS-C3D8").value() == "C3D8");
}

void TestBatteryAndControlEventsV2() {
    auto msg = BleProtocol::ParseControlMessage(
        R"({"event":"battery.changed","data":{"percent":73,"charging":true,"usb_powered":true}})");
    assert(msg && msg->kind == ControlMessage::Kind::kEvent);
    assert((msg->control_event.battery == DeviceBatteryStatus{73, true, true}));

    msg = BleProtocol::ParseControlMessage(
        R"({"event":"battery.changed","data":{"percent":0,"charging":false,"usb_powered":false}})");
    assert(msg && (msg->control_event.battery == DeviceBatteryStatus{0, false, false}));
    assert(!BleProtocol::ParseControlMessage(
        R"({"event":"battery.changed","data":{"percent":101,"charging":false,"usb_powered":false}})"));
    assert(!BleProtocol::ParseControlMessage(
        R"({"id":1,"event":"battery.changed","data":{"percent":50,"charging":false,"usb_powered":false}})"));

    auto closed = BleProtocol::ParseControlMessage(
        R"({"event":"stream.closed","data":{"stream_id":7,"reason":"lease_expired"}})");
    assert(closed && closed->kind == ControlMessage::Kind::kEvent);
    assert(closed->control_event.stream_id == 7);
    assert(closed->control_event.reason == "lease_expired");

    auto rpc = BleProtocol::ParseControlMessage(
        R"({"id":1,"result":{"protocol_version":2,"hardware":"m5stack_stick_s3","firmware_version":"0.5.0",)"
        R"("device_uuid":"5a8f3b0e-2c71-4e6b-9d12-7f84b06d3ac1","max_message_bytes":1024,)"
        R"("buttons":["primary","secondary"],)"
        R"("streams":{"input":{},"audio_in":{"codecs":["opus"],"sample_rates":[16000],"channels":[1],"frame_ms":[60]},)"
        R"("imu":{"channels":["accel","gyro"],"rates_hz":[50,100],"accel_lsb_per_g":8192,"gyro_lsb_per_dps":16.384,"max_lease_ms":5000}},)"
        R"("display":{"scenes":["idle","recording","busy","error"]},)"
        R"("battery":{"percent":88,"charging":false,"usb_powered":true}}})");
    assert(rpc && rpc->kind == ControlMessage::Kind::kRpcResponse && !rpc->rpc_response.is_error);
    assert(rpc->rpc_response.device_info && rpc->rpc_response.device_info->SupportsProtocolV2());
    assert(rpc->rpc_response.device_info->device_uuid == "5A8F3B0E-2C71-4E6B-9D12-7F84B06D3AC1");
    assert((rpc->rpc_response.device_info->buttons == std::vector<std::string>{"primary", "secondary"}));
    assert(rpc->rpc_response.device_info->imu && rpc->rpc_response.device_info->imu->has_gyro);
    assert((rpc->rpc_response.device_info->battery == DeviceBatteryStatus{88, false, true}));

    auto err = BleProtocol::ParseControlMessage(
        R"({"id":9,"error":{"code":"busy","message":"ota active"}})");
    assert(err && err->kind == ControlMessage::Kind::kRpcResponse && err->rpc_response.is_error);
    assert(err->rpc_response.error_code == "busy");

    // Legacy v1 device_info still parses for 0.4.x OTA prompt.
    const std::string legacy_json =
        R"({"event":"device_info","hardware":"voicestick_c3","firmware_version":"0.4.0",)"
        R"("device_uuid":"5a8f3b0e-2c71-4e6b-9d12-7f84b06d3ac1"})";
    ByteVector legacy_frame = {1, 0x10};
    AppendLe16(legacy_frame, static_cast<std::uint16_t>(legacy_json.size()));
    AppendAscii(legacy_frame, legacy_json);
    auto legacy = BleProtocol::ParseLegacyDeviceInfo(legacy_frame);
    assert(legacy && legacy->hardware == "voicestick_c3" && legacy->firmware_version == "0.4.0");
    assert(legacy->device_uuid == "5A8F3B0E-2C71-4E6B-9D12-7F84B06D3AC1");

    // Chunked control round-trip.
    std::uint8_t next_msg_id = 1;
    const std::string long_rpc = BleProtocol::RpcDisplaySet(5, "busy", std::string(70, 'x'));
    auto chunks = BleProtocol::EncodeControlFrames(long_rpc, 20, next_msg_id);
    assert(chunks.size() > 1);
    ControlReassembler reassembler;
    std::optional<std::string> assembled;
    for (const auto& chunk : chunks) {
        assembled = reassembler.Push(chunk);
    }
    assert(assembled && *assembled == long_rpc);

    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::make_unique<FakeAsrClient>(),
                                       &ui, &input);
    coordinator.Start();
    ble_ptr->on_battery_changed("C3D8", DeviceBatteryStatus{0, false, false});
    assert((ui.battery_status_by_device_id.at("C3D8") == DeviceBatteryStatus{0, false, false}));
}

void TestStreamFramesAndShakeDetector() {
    ByteVector audio = {2, 0x01, 18, 0};
    AppendLe16(audio, 4);
    audio.push_back(0x03);
    audio.push_back(0);
    AppendLe32(audio, 7);
    AppendLe32(audio, 12345);
    AppendLe16(audio, 3);
    audio.push_back(10);
    audio.push_back(11);
    audio.push_back(12);
    auto parsed = BleProtocol::ParseAudioStreamFrame(audio);
    assert(parsed.has_value());
    assert(parsed->stream_id == 4);
    assert(parsed->seq == 7);
    assert(parsed->timestamp_ms == 12345);
    assert(parsed->IsStart() && parsed->IsEnd());
    assert(parsed->payload.size() == 3);

    audio[2] = 21;
    audio.insert(audio.begin() + 18, {0xAA, 0xBB, 0xCC});
    const auto extended = BleProtocol::ParseAudioStreamFrame(audio);
    assert(extended && extended->payload == parsed->payload);
    audio.pop_back();
    assert(!BleProtocol::ParseAudioStreamFrame(audio));

    ByteVector input_frame = {2, 0x04, 12, 0};
    AppendLe16(input_frame, 1);
    AppendLe16(input_frame, 9);
    AppendLe32(input_frame, 555);
    AppendLe32(input_frame, 0x03);
    auto parsed_input = BleProtocol::ParseInputStreamFrame(input_frame);
    assert(parsed_input && parsed_input->stream_id == 1 && parsed_input->seq == 9);
    assert(parsed_input->timestamp_ms == 555 && parsed_input->buttons_mask == 0x03);

    ByteVector imu3 = {2, 0x03, 14, 0};
    AppendLe16(imu3, 2);
    AppendLe16(imu3, 4);
    AppendLe32(imu3, 999);
    AppendLe16(imu3, static_cast<std::uint16_t>(static_cast<std::int16_t>(-100)));
    AppendLe16(imu3, 200);
    AppendLe16(imu3, 8192);
    auto parsed_imu3 = BleProtocol::ParseImuStreamFrame(imu3);
    assert(parsed_imu3 && parsed_imu3->ax == -100 && parsed_imu3->ay == 200 && parsed_imu3->az == 8192);
    assert(!parsed_imu3->gx.has_value());

    ShakeDetector detector;
    assert(!detector.Sample(0, 0, 8192, 8192, 100'000));
    assert(!detector.Sample(16000, 0, 8192, 8192, 120'000));
    assert(!detector.Sample(-16000, 0, 8192, 8192, 220'000));
    assert(detector.Sample(16000, 0, 8192, 8192, 320'000));
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
    assert(utterance_payload.find("voicestick_text") == std::string::npos);
    options.cloud_text_profile = OutputProfile{OutputTarget::kFocusedApp, TextTransform::kTranslate, "zh-Hans"};
    auto translated = AsrProtocol::MakeStartSessionFrame(config, utterance_session_id, options);
    assert(event_payload(translated, utterance_session_id).find(
        R"("voicestick_text":{"operation":"translate","target_language":"zh-Hans"})") != std::string::npos);
    options.cloud_text_profile->transform = TextTransform::kRefine;
    auto refined = AsrProtocol::MakeStartSessionFrame(config, utterance_session_id, options);
    assert(event_payload(refined, utterance_session_id).find(
        R"("voicestick_text":{"operation":"refine"})") != std::string::npos);

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

void TestOpenAIRealtimeProtocol() {
    const auto update = OpenAITranscriptionSessionUpdate({"VoiceStick", "<invalid>"});
    assert(update.find(R"("type":"session.update")") != std::string::npos);
    assert(update.find(R"("rate":24000)") != std::string::npos);
    assert(update.find(R"("model":"gpt-live-transcribe")") != std::string::npos);
    assert(update.find(R"("keywords":["VoiceStick"])") != std::string::npos);

    OpenAIOpusDecoder decoder;
    assert(decoder.Reset());
    OggOpusMuxer muxer(16000, 1);
    const auto ogg = muxer.Append(ByteVector{0xf8, 0xff, 0xfe}, false);
    const auto pcm = decoder.Decode(ogg);
    assert(pcm && pcm->size() == 480);
    const auto end = decoder.Decode(muxer.Finish());
    assert(end && end->empty());
    assert(!decoder.Decode(ByteVector{0, 1, 2}));

    const std::int16_t samples[] = {0x1234, -2};
    assert(OpenAIAudioAppend(samples) == R"({"type":"input_audio_buffer.append","audio":"NBL+/w=="})");
}

void TestAppConfig() {
    AppConfig cloud = AppConfig::Defaults();
    assert(cloud.asr_provider == AsrProvider::kVoiceStickCloud);
    cloud.asr_provider = AsrProvider::kVoiceStickCloud;
    cloud.voicestick_cloud_url = "";
    assert(cloud.ActiveWebsocketUrl() == "wss://api.xiaozhi.me/voicestick/asr/");

    cloud.voicestick_cloud_url = "  wss://example.test/asr?token=1  ";
    assert(cloud.ActiveWebsocketUrl() == "wss://example.test/asr?token=1");

    AppConfig migrated = cloud;
    migrated.asr_provider = AsrProviderFromName("volcengine");
    migrated.volcengine_api_key = "legacy-key";
    assert(migrated.asr_provider == AsrProvider::kVoiceStickCloud);
    assert(migrated.ActiveApiKey().empty());
    assert(migrated.ActiveWebsocketUrl() == cloud.ActiveWebsocketUrl());

    AppConfig openai = cloud;
    openai.asr_provider = AsrProviderFromName("openai");
    openai.SetActiveApiKey(" example ");
    assert(openai.ActiveApiKey() == "example");
    assert(openai.ActiveWebsocketUrl() == "wss://api.openai.com/v1/realtime?intent=transcription");
    assert(TextTransformFromName("polish") == TextTransform::kRefine);
    assert(TextTransformFromName("refine") == TextTransform::kRefine);
    assert(TextTransformName(TextTransform::kRefine) == "polish");

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
    const auto stick_url = FirmwareManifestClient::ManifestUrlForHardware("stick_s3");
    const auto c3_url = FirmwareManifestClient::ManifestUrlForHardware("voicestick_c3");
    const auto magiclick_url = FirmwareManifestClient::ManifestUrlForHardware("magiclick_2p5");
    assert(stick_url.ends_with("/m5stack_stick_s3/latest/manifest.json"));
    assert(c3_url.ends_with("/voicestick_c3/latest/manifest.json"));
    assert(magiclick_url.ends_with("/magiclick_2p5/latest/manifest.json"));
    assert(FirmwareManifestClient::ManifestUrlForHardware("MagiClick 2P5") == magiclick_url);
    assert(FirmwareManifestClient::ManifestUrlForHardware("sticks3") == stick_url);
    assert(FirmwareManifestClient::ManifestUrlForHardware("m5stack_stick_s3") == stick_url);
    assert(FirmwareManifestClient::ManifestUrlForHardware("").empty());
    assert(FirmwareManifestClient::ManifestUrlForHardware("unknown").empty());
    assert(!IsFirmwareHardwareCompatible("voicestick_c3", "0.3.7", "stick_s3"));
    assert(!IsFirmwareHardwareCompatible("stick_s3", "0.3.7", "voicestick_c3"));
    assert(IsFirmwareHardwareCompatible("magiclick_2p5", "0.4.2", "magiclick_2p5"));
    assert(!IsFirmwareHardwareCompatible("magiclick_2p5", "0.4.2", "stick_s3"));
}

void TestFirmwareWindowsAppVersionRequirement() {
    const std::string required_fields =
        R"("hardware":"sticks3","version":"1.0.0","ota_url":"https://example.test/ota.bin",)"
        R"("ota_sha256":"abc","ota_size":123)";
    const auto legacy_manifest = ParseFirmwareManifest("{" + required_fields + "}");
    assert(legacy_manifest);
    assert(legacy_manifest->CheckWindowsAppVersion("") == FirmwareAppCompatibility::kSupported);

    for (const auto* versions : {R"({"macos":"0.3.7"})", R"({})", R"(null)"}) {
        const auto manifest = ParseFirmwareManifest("{" + required_fields + ",\"minimum_app_versions\":" +
                                                    versions + "}");
        assert(manifest);
        assert(manifest->CheckWindowsAppVersion("0.1.0") == FirmwareAppCompatibility::kSupported);
    }
    for (const auto* versions : {R"("0.3.7")", R"({"windows":123})",
                                 R"({"windows":null})", R"({"windows":""})", R"({"windows":"invalid"})"}) {
        const auto manifest = ParseFirmwareManifest("{" + required_fields + ",\"minimum_app_versions\":" +
                                                    versions + "}");
        assert(!manifest);
    }

    const auto manifest = ParseFirmwareManifest(
        "{" + required_fields + R"(,"minimum_app_versions":{"macos":"0.3.5","windows":"0.3.7"}})");
    assert(manifest);
    assert(manifest->minimum_windows_app_version == "0.3.7");
    assert(manifest->CheckWindowsAppVersion("0.3.6") == FirmwareAppCompatibility::kAppUpdateRequired);
    assert(manifest->CheckWindowsAppVersion("0.3.7-beta") == FirmwareAppCompatibility::kAppUpdateRequired);
    assert(manifest->CheckWindowsAppVersion("") == FirmwareAppCompatibility::kAppUpdateRequired);
    assert(manifest->CheckWindowsAppVersion("0.3.7") == FirmwareAppCompatibility::kSupported);
    assert(manifest->CheckWindowsAppVersion("0.3.10") == FirmwareAppCompatibility::kSupported);
}

void TestFirmwareAvailabilityForDeviceMenuAndReminders() {
    DeviceFirmwareInfo device;
    device.hardware = "voicestick_c3";
    device.current_version = "0.3.7";
    device.reported_on_connection = true;
    FirmwareManifest manifest;
    manifest.hardware = "voicestick_c3";
    manifest.version = "0.4.0";

    auto result = EvaluateFirmwareAvailability(device, &manifest, {}, false, "0.1.0");
    assert(result.latest_version == "0.4.0" && result.update_available);
    assert(result.required_app_version.empty());

    manifest.minimum_windows_app_version = "0.4.0";
    result = EvaluateFirmwareAvailability(device, &manifest, {}, false, "0.3.7");
    assert(result.required_app_version == "0.4.0" && !result.update_available);

    result = EvaluateFirmwareAvailability(device, &manifest, {}, false, "0.4.0");
    assert(result.required_app_version.empty() && result.update_available);

    result = EvaluateFirmwareAvailability(device, &manifest, {}, true, "0.4.0");
    assert(result.is_checking && !result.update_available && result.latest_version.empty());

    result = EvaluateFirmwareAvailability(device, &manifest, "Connection failed.", false, "0.4.0");
    assert(result.error_message == "Connection failed." && !result.update_available);

    device.current_version = "0.4.0";
    result = EvaluateFirmwareAvailability(device, &manifest, {}, false, "0.4.0");
    assert(result.required_app_version.empty() && !result.update_available);

    manifest.hardware = "stick_s3";
    result = EvaluateFirmwareAvailability(device, &manifest, {}, false, "0.4.0");
    assert(!result.error_message.empty() && !result.update_available);
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

void TestCoordinatorPrimaryPausesAndHostShakeCancelsConfirmation() {
    auto config = AppConfig::Defaults();
    config.auto_enter = true;
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 0));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(1, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(1, 2));
    asr_ptr->on_final({"hello"});

    // Entering pending confirmation stops audio_in so primary button pauses instead of recording.
    assert(!ble_ptr->stopped_audio_in.empty() && ble_ptr->stopped_audio_in.back() == "5A74");
    ble_ptr->on_button_event("5A74", "primary", true, 2000);

    assert(ui.final_countdowns.size() == 1);
    assert(ui.final_countdowns.back() == "hello");
    assert(ui.paused_finals.size() == 1);
    assert(ui.paused_finals.back() == "hello");
    assert(ble_ptr->sent_displays.back().scene == "busy");
    assert(ui.countdown_duration_ms == 1000);
    assert(!ble_ptr->started_imu.empty());

    ui.final_countdown_completion();
    assert(input.pasted_text.empty());

    // IMU shake from another device does not cancel.
    EmitShakeSamples(*ble_ptr, "6B85");
    assert(ui.hide_overlay_count == 0);

    // IMU shake from active device cancels paused confirmation and stops IMU.
    EmitShakeSamples(*ble_ptr, "5A74");
    assert(ui.hide_overlay_count == 1);
    assert(!ble_ptr->stopped_imu.empty() && ble_ptr->stopped_imu.back() == "5A74");

    assert(coordinator.RestoreLastInputConfirmation());
    ui.edited_confirmation = "edited";
    input.paste_succeeds = false;
    coordinator.ConfirmPendingPaste();
    assert(ui.paused_finals.size() == 2);
    assert(input.attempted_enter && !input.used_current_field);
    assert(ui.concealed_confirmations == 1);
    assert(ui.paste_failures == 1);
    assert(input.pasted_text.empty());

    input.paste_succeeds = true;
    ble_ptr->on_button_event("5A74", "primary", true, 3000);
    assert(input.pasted_text == "edited");
    assert(input.used_current_field && !input.pasted_enter);
    assert(ui.concealed_confirmations == 2);
    assert(ui.hide_overlay_count == 2);
}

void TestCoordinatorCloudOutputWithManualAndImmediateConfirmation() {
    auto config = AppConfig::Defaults();
    config.default_output_profile.transform = TextTransform::kTranslate;
    config.send_confirmation_ms = -1;
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    const auto transcribe = [&](std::string text) {
        ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 0));
        ble_ptr->on_audio_frame("5A74", AudioDataFrame(1, 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(520));
        ble_ptr->on_audio_frame("5A74", EmptyEndFrame(1, 2));
        assert(asr_ptr->last_options.cloud_text_profile == config.default_output_profile);
        asr_ptr->on_final({std::move(text), true});
    };

    transcribe("cloud translation");
    assert(ui.paused_finals == std::vector<std::string>{"cloud translation"});
    assert(input.pasted_text.empty());
    assert(ui.final_countdowns.empty());
    coordinator.ConfirmPendingPaste();
    assert(input.pasted_text == "cloud translation");

    coordinator.SetSendConfirmationDuration(0);
    transcribe("another translation");
    assert(input.pasted_text == "another translation");
    assert(ui.paused_finals.size() == 1);
    assert(ui.final_countdowns.empty());
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
    subtitle_asr_ptr->on_final({"hello subtitle"});

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

    subtitle_asrs[0]->on_final({"first late subtitle"});
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

void TestCoordinatorKeepsPartialForManualConfirmationAfterAsrError() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");

    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 0));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(1, 1));
    asr_ptr->on_partial("already recognized");
    asr_ptr->on_error("ASR timeout", "already recognized");
    asr_ptr->on_partial("late update");
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(1, 2));

    assert(asr_ptr->cancelled);
    assert(ui.partials == std::vector<std::string>{"already recognized"});
    assert(ui.paused_finals == std::vector<std::string>{"already recognized"});
    assert(ui.errors.empty());
    assert(ui.final_countdowns.empty());
    assert(input.pasted_text.empty());

    ble_ptr->connected_device_ids.clear();
    ble_ptr->on_connection_change({});
    assert(ui.hide_overlay_count == 0);
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VoiceStick"}});
    ble_ptr->AnnounceV2("5A74");
    assert(ble_ptr->sent_displays.back().scene == "busy");
    ble_ptr->on_button_event("5A74", "primary", true, 3000);
    assert(input.pasted_text == "already recognized");

    ble_ptr->on_audio_frame("5A74", AudioStartFrame(1, 3));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(1, 4));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(1, 5));
    asr_ptr->on_error("ASR timeout", "discard this partial");
    assert(ui.paused_finals.back() == "discard this partial");

    ble_ptr->on_button_event("5A74", "secondary", true, 4000);
    ble_ptr->on_button_event("5A74", "secondary", false, 4100);
    assert(ble_ptr->sent_displays.back().scene == "idle");
    assert(input.pasted_text == "already recognized");
    assert(coordinator.RestoreLastInputConfirmation());
    assert(ui.paused_finals.back() == "already recognized");
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
    asr_ptr->on_error("ASR 44002: VoiceStick Cloud API key is invalid.", {});
    asr_ptr->on_upgrade_url("https://example.test/upgrade",
                            "ASR 44002: VoiceStick Cloud API key is invalid.");

    assert(HasDisplayText(*ble_ptr, "error", "ASR 44002: VoiceStick Cloud API key is invalid.", "5A74"));
    assert(HasDisplayScene(*ble_ptr, "idle", "5A74"));
    assert(!ui.cloud_upgrades.empty());
}

} // namespace

int main() {
    _set_error_mode(_OUT_TO_STDERR);
    TestDeviceIds();
    TestBatteryAndControlEventsV2();
    TestStreamFramesAndShakeDetector();
    TestOggMuxer();
    TestAsrProtocol();
    TestOpenAIRealtimeProtocol();
    TestAppConfig();
    TestFirmwareManifestParsingAndVersionCompare();
    TestFirmwareWindowsAppVersionRequirement();
    TestFirmwareAvailabilityForDeviceMenuAndReminders();
    TestCoordinatorRequiresInputProtocolOnEachConnection();
    TestCoordinatorCancelsShortPrimaryPress();
    TestCoordinatorPrimaryDuringFinalizingRefreshesThinking();
    TestCoordinatorSecondaryCancelsFinalizing();
    TestCoordinatorAcceptsAudioFramesUntilEnd();
    TestCoordinatorPrimaryPausesAndHostShakeCancelsConfirmation();
    TestCoordinatorCloudOutputWithManualAndImmediateConfirmation();
    TestCoordinatorOtherDeviceDuringRecordingGetsReady();
    TestCoordinatorSubtitleOutputSkipsPaste();
    TestCoordinatorSubtitleFinalDoesNotBlockNextSession();
    TestCoordinatorShortSubtitleEndReturnsReady();
    TestCoordinatorMainPartialSentToDeviceOnlyAfterFinalAudio();
    TestCoordinatorShowsDetailedAsrStartError();
    TestCoordinatorKeepsPartialForManualConfirmationAfterAsrError();
    TestCoordinatorCloudUpgradeRecoversDeviceAfterAsrError();
    return 0;
}
