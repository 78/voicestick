#include "ble_protocol.h"

#include "../../shared/ble_json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace voicestick {

namespace {

std::string TrimCopy(std::string_view text) {
    auto begin = text.begin();
    auto end = text.end();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) ++begin;
    while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1)))) --end;
    return std::string(begin, end);
}

std::string Uppercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

bool IsHex4(std::string_view text) {
    return text.size() == 4 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

bool IsDeviceUuid(std::string_view uuid) {
    if (uuid.size() != 36) return false;
    for (std::size_t index = 0; index < uuid.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (uuid[index] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(uuid[index]))) {
            return false;
        }
    }
    return true;
}

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char ch : text) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return out;
}

std::int16_t ReadLeI16(std::span<const std::uint8_t> bytes) {
    return static_cast<std::int16_t>(ReadLe16(bytes));
}

std::optional<DeviceBatteryStatus> ParseBatteryObject(const cJSON* obj) {
    if (!cJSON_IsObject(obj)) return std::nullopt;
    const auto percent = ble_json::Unsigned<std::uint32_t>(obj, "percent");
    const auto charging = ble_json::Bool(obj, "charging");
    const auto usb_powered = ble_json::Bool(obj, "usb_powered");
    if (!percent || *percent > 100 || !charging || !usb_powered) return std::nullopt;
    return DeviceBatteryStatus{*percent, *charging, *usb_powered};
}

std::optional<DeviceInfoResult> ParseDeviceInfoResult(const cJSON* result) {
    if (!cJSON_IsObject(result)) return std::nullopt;
    const auto proto_ver = ble_json::Unsigned<std::uint32_t>(result, "protocol_version");
    if (!proto_ver) return std::nullopt;

    DeviceInfoResult info;
    info.protocol_versions.push_back(*proto_ver);
    info.hardware = ble_json::String(result, "hardware");
    info.firmware_version = ble_json::String(result, "firmware_version");
    auto uuid = ble_json::String(result, "device_uuid");
    if (IsDeviceUuid(uuid)) info.device_uuid = Uppercase(std::move(uuid));

    if (const auto* buttons = ble_json::ChildArray(result, "buttons")) {
        for (const cJSON* item = buttons->child; item; item = item->next) {
            if (const auto* name = cJSON_GetStringValue(item)) {
                info.buttons.emplace_back(name);
            }
        }
    }
    if (const auto* streams = ble_json::ChildObject(result, "streams")) {
        info.has_audio_in = ble_json::ChildObject(streams, "audio_in") != nullptr;
        if (const auto* imu = ble_json::ChildObject(streams, "imu")) {
            DeviceImuCapabilities imu_caps;
            imu_caps.accel_lsb_per_g = ble_json::Unsigned<std::uint32_t>(imu, "accel_lsb_per_g").value_or(0);
            imu_caps.gyro_lsb_per_dps = ble_json::Double(imu, "gyro_lsb_per_dps").value_or(0.0);
            if (const auto* rates = ble_json::ChildArray(imu, "rates_hz")) {
                for (const cJSON* item = rates->child; item; item = item->next) {
                    if (cJSON_IsNumber(item) && item->valuedouble > 0 &&
                        std::trunc(item->valuedouble) == item->valuedouble) {
                        imu_caps.max_rate_hz = std::max(
                            imu_caps.max_rate_hz,
                            static_cast<std::uint32_t>(item->valuedouble));
                    }
                }
            }
            if (const auto* channels = ble_json::ChildArray(imu, "channels")) {
                for (const cJSON* item = channels->child; item; item = item->next) {
                    if (const auto* ch = cJSON_GetStringValue(item); ch && std::string_view(ch) == "gyro") {
                        imu_caps.has_gyro = true;
                    }
                }
            }
            if (imu_caps.accel_lsb_per_g > 0) {
                info.imu = imu_caps;
            }
        }
    }
    info.has_display = ble_json::ChildObject(result, "display") != nullptr;
    if (const auto* battery = ble_json::ChildObject(result, "battery")) {
        info.battery = ParseBatteryObject(battery);
    }
    return info;
}

} // namespace

bool DeviceInfoResult::SupportsProtocolV2() const {
    return std::find(protocol_versions.begin(), protocol_versions.end(),
                     BleProtocol::protocol_version) != protocol_versions.end();
}

void ControlReassembler::Reset() {
    active_ = false;
    msg_id_ = 0;
    expected_idx_ = 0;
    total_cnt_ = 0;
    started_at_ = {};
    buffer_.clear();
}

std::optional<std::string> ControlReassembler::Push(
    std::span<const std::uint8_t> data,
    std::chrono::steady_clock::time_point now) {
    if (data.size() < 4 || data[0] != BleProtocol::protocol_version) return std::nullopt;
    const std::uint8_t type = data[1];
    const std::uint16_t payload_len = ReadLe16(data.subspan(2, 2));
    if (data.size() < 4u + payload_len) return std::nullopt;

    if (type == BleProtocol::type_control_json) {
        if (payload_len > kMaxMessageBytes) return std::nullopt;
        return Utf8FromBytes(data.subspan(4, payload_len));
    }
    if (type != BleProtocol::type_control_chunk) return std::nullopt;
    if (payload_len < 3) return std::nullopt;

    const std::uint8_t msg_id = data[4];
    const std::uint8_t idx = data[5];
    const std::uint8_t cnt = data[6];
    if (cnt == 0 || idx >= cnt) {
        Reset();
        return std::nullopt;
    }
    const auto slice = data.subspan(7, payload_len - 3);

    if (active_ && (now - started_at_ > kTimeout || msg_id != msg_id_)) {
        Reset();
    }
    if (idx == 0) {
        Reset();
        if (slice.size() > kMaxMessageBytes) return std::nullopt;
        active_ = true;
        msg_id_ = msg_id;
        expected_idx_ = 1;
        total_cnt_ = cnt;
        started_at_ = now;
        buffer_.assign(slice.begin(), slice.end());
    } else {
        if (!active_ || msg_id != msg_id_ || idx != expected_idx_ || cnt != total_cnt_) {
            Reset();
            return std::nullopt;
        }
        if (buffer_.size() + slice.size() > kMaxMessageBytes) {
            Reset();
            return std::nullopt;
        }
        buffer_.insert(buffer_.end(), slice.begin(), slice.end());
        ++expected_idx_;
    }

    if (expected_idx_ == total_cnt_) {
        std::string result = Utf8FromBytes(buffer_);
        Reset();
        return result;
    }
    return std::nullopt;
}

std::optional<AudioStreamFrame> BleProtocol::ParseAudioStreamFrame(std::span<const std::uint8_t> data) {
    if (data.size() < 18 || data[0] != protocol_version || data[1] != type_audio_in) {
        return std::nullopt;
    }
    const std::uint16_t header_len = ReadLe16(data.subspan(2, 2));
    if (header_len < 18 || data.size() < header_len) return std::nullopt;
    const std::uint16_t payload_len = ReadLe16(data.subspan(16, 2));
    if (data.size() < static_cast<std::size_t>(header_len) + payload_len) return std::nullopt;

    AudioStreamFrame frame;
    frame.stream_id = ReadLe16(data.subspan(4, 2));
    frame.flags = data[6];
    frame.seq = ReadLe32(data.subspan(8, 4));
    frame.timestamp_ms = ReadLe32(data.subspan(12, 4));
    frame.payload.assign(data.begin() + header_len, data.begin() + header_len + payload_len);
    return frame;
}

std::optional<ImuStreamFrame> BleProtocol::ParseImuStreamFrame(std::span<const std::uint8_t> data) {
    if (data.size() < 18 || data[0] != protocol_version || data[1] != type_imu) {
        return std::nullopt;
    }
    const std::uint16_t payload_len = ReadLe16(data.subspan(2, 2));
    if ((payload_len != 14 && payload_len != 20) || data.size() < 4u + payload_len) {
        return std::nullopt;
    }
    ImuStreamFrame frame;
    frame.stream_id = ReadLe16(data.subspan(4, 2));
    frame.seq = ReadLe16(data.subspan(6, 2));
    frame.timestamp_ms = ReadLe32(data.subspan(8, 4));
    frame.ax = ReadLeI16(data.subspan(12, 2));
    frame.ay = ReadLeI16(data.subspan(14, 2));
    frame.az = ReadLeI16(data.subspan(16, 2));
    if (payload_len == 20) {
        frame.gx = ReadLeI16(data.subspan(18, 2));
        frame.gy = ReadLeI16(data.subspan(20, 2));
        frame.gz = ReadLeI16(data.subspan(22, 2));
    }
    return frame;
}

std::optional<InputStreamFrame> BleProtocol::ParseInputStreamFrame(std::span<const std::uint8_t> data) {
    if (data.size() < 16 || data[0] != protocol_version || data[1] != type_input) {
        return std::nullopt;
    }
    const std::uint16_t payload_len = ReadLe16(data.subspan(2, 2));
    if (payload_len < 12 || data.size() < 4u + payload_len) return std::nullopt;

    InputStreamFrame frame;
    frame.stream_id = ReadLe16(data.subspan(4, 2));
    frame.seq = ReadLe16(data.subspan(6, 2));
    frame.timestamp_ms = ReadLe32(data.subspan(8, 4));
    frame.buttons_mask = ReadLe32(data.subspan(12, 4));
    return frame;
}

std::optional<LegacyDeviceInfo> BleProtocol::ParseLegacyDeviceInfo(std::span<const std::uint8_t> data) {
    if (data.size() < 4 || data[0] != 1 || data[1] != 0x10) return std::nullopt;
    const std::uint16_t payload_len = ReadLe16(data.subspan(2, 2));
    if (data.size() < 4u + payload_len) return std::nullopt;
    const auto json = Utf8FromBytes(data.subspan(4, payload_len));
    const auto root = ble_json::ParseObject(json);
    if (!root || ble_json::String(root.get(), "event") != "device_info") return std::nullopt;

    LegacyDeviceInfo info;
    info.hardware = ble_json::String(root.get(), "hardware");
    info.firmware_version = ble_json::String(root.get(), "firmware_version");
    auto uuid = ble_json::String(root.get(), "device_uuid");
    if (IsDeviceUuid(uuid)) info.device_uuid = Uppercase(std::move(uuid));
    return info;
}

std::optional<ControlMessage> BleProtocol::ParseControlMessage(std::string_view json) {
    const auto root = ble_json::ParseObject(std::string(json));
    if (!root) return std::nullopt;

    const auto id = ble_json::Unsigned<std::uint32_t>(root.get(), "id");
    const auto event_name = ble_json::String(root.get(), "event");

    // Disjoint top-level message shapes: RPC responses have "id" and no "event";
    // system events have "event" and no "id".
    if (id && !event_name.empty()) return std::nullopt;

    if (id) {
        ControlMessage msg;
        msg.kind = ControlMessage::Kind::kRpcResponse;
        msg.rpc_response.id = *id;
        if (const auto* err = ble_json::ChildObject(root.get(), "error")) {
            msg.rpc_response.is_error = true;
            msg.rpc_response.error_code = ble_json::String(err, "code");
            msg.rpc_response.error_message = ble_json::String(err, "message");
            return msg;
        }
        if (const auto* result = ble_json::ChildObject(root.get(), "result")) {
            msg.rpc_response.stream_id = ble_json::Unsigned<std::uint16_t>(result, "stream_id");
            msg.rpc_response.buttons_mask = ble_json::Unsigned<std::uint32_t>(result, "buttons_mask");
            msg.rpc_response.device_info = ParseDeviceInfoResult(result);
            return msg;
        }
        return std::nullopt;
    }

    if (!event_name.empty()) {
        const auto* data = ble_json::ChildObject(root.get(), "data");
        ControlMessage msg;
        msg.kind = ControlMessage::Kind::kEvent;
        msg.control_event.event = event_name;
        if (event_name == "battery.changed") {
            const auto battery = ParseBatteryObject(data);
            if (!battery) return std::nullopt;
            msg.control_event.battery = battery;
        } else if (event_name == "stream.closed") {
            if (!data) return std::nullopt;
            const auto stream_id = ble_json::Unsigned<std::uint16_t>(data, "stream_id");
            if (!stream_id) return std::nullopt;
            msg.control_event.stream_id = stream_id;
            msg.control_event.reason = ble_json::String(data, "reason");
        }
        return msg;
    }

    return std::nullopt;
}

std::optional<FirmwareOtaStateEvent> BleProtocol::ParseFirmwareOtaStateEvent(std::span<const std::uint8_t> data) {
    if (data.size() < 4 || data[0] != 1 || data[1] != ota_type_state) return std::nullopt;
    const auto payload_len = ReadLe16(data.subspan(2, 2));
    if (data.size() < 4u + payload_len) return std::nullopt;
    const auto json = Utf8FromBytes(data.subspan(4, payload_len));
    const auto root = ble_json::ParseObject(json);
    if (!root) return std::nullopt;
    FirmwareOtaStateEvent event;
    event.event = ble_json::String(root.get(), "event");
    if (event.event.empty()) return std::nullopt;
    event.transfer_id = ble_json::Unsigned<std::uint32_t>(root.get(), "transfer_id");
    event.written = ble_json::Unsigned<std::uint32_t>(root.get(), "written");
    event.size = ble_json::Unsigned<std::uint32_t>(root.get(), "size");
    event.code = ble_json::String(root.get(), "code");
    event.reboot_ms = ble_json::Unsigned<std::uint32_t>(root.get(), "reboot_ms");
    return event;
}

std::vector<ByteVector> BleProtocol::EncodeControlFrames(
    std::string_view json, std::size_t max_write_len, std::uint8_t& next_msg_id) {
    std::vector<ByteVector> frames;
    if (json.empty() || json.size() > max_control_message_bytes) return frames;
    const std::size_t effective_mtu = std::max<std::size_t>(max_write_len, 20);

    if (4 + json.size() <= effective_mtu) {
        ByteVector frame;
        frame.reserve(4 + json.size());
        frame.push_back(protocol_version);
        frame.push_back(type_control_json);
        AppendLe16(frame, static_cast<std::uint16_t>(json.size()));
        AppendAscii(frame, json);
        frames.push_back(std::move(frame));
        return frames;
    }

    const std::size_t max_slice = effective_mtu > 7 ? effective_mtu - 7 : 13;
    const std::size_t count = (json.size() + max_slice - 1) / max_slice;
    if (count > 255) return frames;

    const std::uint8_t msg_id = next_msg_id++;
    for (std::size_t idx = 0; idx < count; ++idx) {
        const std::size_t offset = idx * max_slice;
        const std::size_t slice_len = std::min(max_slice, json.size() - offset);
        ByteVector frame;
        frame.reserve(7 + slice_len);
        frame.push_back(protocol_version);
        frame.push_back(type_control_chunk);
        AppendLe16(frame, static_cast<std::uint16_t>(3 + slice_len));
        frame.push_back(msg_id);
        frame.push_back(static_cast<std::uint8_t>(idx));
        frame.push_back(static_cast<std::uint8_t>(count));
        AppendAscii(frame, json.substr(offset, slice_len));
        frames.push_back(std::move(frame));
    }
    return frames;
}

std::string BleProtocol::RpcDeviceGetInfo(std::uint32_t id) {
    return "{\"id\":" + std::to_string(id) + ",\"method\":\"device.get_info\",\"params\":{}}";
}

std::string BleProtocol::RpcDeviceReset(std::uint32_t id) {
    return "{\"id\":" + std::to_string(id) + ",\"method\":\"device.reset\",\"params\":{}}";
}

std::string BleProtocol::RpcStreamStartInput(std::uint32_t id) {
    return "{\"id\":" + std::to_string(id) +
           ",\"method\":\"stream.start\",\"params\":{\"kind\":\"input\"}}";
}

std::string BleProtocol::RpcStreamStartAudioIn(std::uint32_t id, std::string_view trigger,
                                               std::string_view button) {
    std::string_view mode = trigger;
    if (mode == "hold") mode = "button_hold";
    else if (mode == "toggle") mode = "button_toggle";
    return "{\"id\":" + std::to_string(id) +
           ",\"method\":\"stream.start\",\"params\":{\"kind\":\"audio_in\","
           "\"format\":{\"codec\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_ms\":60},"
           "\"trigger\":{\"mode\":\"" + JsonEscape(mode) + "\",\"button\":\"" +
           JsonEscape(button) + "\"},\"local_display_feedback\":true}}";
}

std::string BleProtocol::RpcStreamStartImu(std::uint32_t id, std::string_view axes,
                                           int rate_hz, int lease_ms) {
    const bool include_gyro = (axes == "6axis" || axes == "accel_gyro");
    const std::string_view channels = include_gyro ? "[\"accel\",\"gyro\"]" : "[\"accel\"]";
    return "{\"id\":" + std::to_string(id) +
           ",\"method\":\"stream.start\",\"params\":{\"kind\":\"imu\","
           "\"format\":{\"channels\":" + std::string(channels) +
           ",\"rate_hz\":" + std::to_string(rate_hz) +
           "},\"lease_ms\":" + std::to_string(lease_ms) + "}}";
}

std::string BleProtocol::RpcStreamRenew(std::uint32_t id, std::uint16_t stream_id, int /*lease_ms*/) {
    return "{\"id\":" + std::to_string(id) +
           ",\"method\":\"stream.renew\",\"params\":{\"stream_id\":" +
           std::to_string(stream_id) + "}}";
}

std::string BleProtocol::RpcStreamStop(std::uint32_t id, std::uint16_t stream_id) {
    return "{\"id\":" + std::to_string(id) +
           ",\"method\":\"stream.stop\",\"params\":{\"stream_id\":" +
           std::to_string(stream_id) + "}}";
}

std::string BleProtocol::RpcDisplaySet(std::uint32_t id, std::string_view scene, std::string_view text) {
    return "{\"id\":" + std::to_string(id) +
           ",\"method\":\"display.set\",\"params\":{\"scene\":\"" +
           JsonEscape(scene) + "\",\"text\":\"" + JsonEscape(text) + "\"}}";
}

ByteVector BleProtocol::LegacyGetDeviceInfoPayload() {
    const std::string_view json = R"({"event":"get_device_info"})";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::OtaBeginPayload(std::uint32_t image_size, std::uint32_t transfer_id) {
    ByteVector data = {1, ota_type_begin, 12, 0};
    AppendLe32(data, image_size);
    AppendLe32(data, transfer_id);
    return data;
}

ByteVector BleProtocol::OtaDataPayload(std::uint32_t transfer_id,
                                       std::uint32_t offset,
                                       std::span<const std::uint8_t> chunk) {
    ByteVector data = {1, ota_type_data, 12, 0};
    AppendLe32(data, transfer_id);
    AppendLe32(data, offset);
    data.insert(data.end(), chunk.begin(), chunk.end());
    return data;
}

ByteVector BleProtocol::OtaEndPayload(std::uint32_t transfer_id, std::uint32_t image_size) {
    ByteVector data = {1, ota_type_end, 12, 0};
    AppendLe32(data, transfer_id);
    AppendLe32(data, image_size);
    return data;
}

ByteVector BleProtocol::OtaAbortPayload(std::uint32_t transfer_id) {
    ByteVector data = {1, ota_type_abort, 8, 0};
    AppendLe32(data, transfer_id);
    return data;
}

std::optional<std::string> BleProtocol::DeviceIdFromName(std::string_view name) {
    auto value = Uppercase(TrimCopy(name));
    if (!value.starts_with("VS-")) return std::nullopt;
    value = value.substr(3, 4);
    if (!IsHex4(value)) return std::nullopt;
    return value;
}

std::string BleProtocol::NormalizeDeviceId(std::string_view text) {
    auto value = Uppercase(TrimCopy(text));
    if (value.starts_with("VS-")) value = value.substr(3);
    value = value.substr(0, std::min<std::size_t>(4, value.size()));
    return IsHex4(value) ? value : std::string();
}

} // namespace voicestick
