#include "ble_protocol.h"

#include <algorithm>
#include <cctype>
#include <charconv>

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

std::string JsonStringValue(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    auto key_pos = json.find(needle);
    if (key_pos == std::string_view::npos) return {};
    auto colon = json.find(':', key_pos + needle.size());
    if (colon == std::string_view::npos) return {};
    auto first_quote = json.find('"', colon + 1);
    if (first_quote == std::string_view::npos) return {};
    std::string out;
    bool escaped = false;
    for (auto i = first_quote + 1; i < json.size(); ++i) {
        char ch = json[i];
        if (escaped) {
            switch (ch) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(ch); break;
            }
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            return out;
        } else {
            out.push_back(ch);
        }
    }
    return {};
}

std::optional<std::uint32_t> JsonU32Value(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    auto key_pos = json.find(needle);
    if (key_pos == std::string_view::npos) return std::nullopt;
    auto colon = json.find(':', key_pos + needle.size());
    if (colon == std::string_view::npos) return std::nullopt;
    auto begin = colon + 1;
    while (begin < json.size() && std::isspace(static_cast<unsigned char>(json[begin]))) ++begin;
    auto end = begin;
    while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end]))) ++end;
    if (begin == end) return std::nullopt;
    std::uint32_t value = 0;
    auto result = std::from_chars(json.data() + begin, json.data() + end, value);
    if (result.ec != std::errc()) return std::nullopt;
    return value;
}

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char ch : text) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

} // namespace

std::optional<AudioFrame> BleProtocol::ParseAudioFrame(std::span<const std::uint8_t> data) {
    if (data.size() < 16 || data[0] != 1 || data[1] != 0x01) return std::nullopt;
    const auto header_len = ReadLe16(data.subspan(2, 2));
    if (header_len != 16 || data.size() < header_len) return std::nullopt;
    const auto payload_len = ReadLe16(data.subspan(14, 2));
    if (data.size() < 16u + payload_len) return std::nullopt;
    AudioFrame frame;
    frame.session_id = ReadLe32(data.subspan(4, 4));
    frame.seq = ReadLe32(data.subspan(8, 4));
    frame.flags = data[12];
    frame.payload.assign(data.begin() + 16, data.begin() + 16 + payload_len);
    return frame;
}

std::optional<StateEvent> BleProtocol::ParseStateEvent(std::span<const std::uint8_t> data) {
    if (data.size() < 4 || data[0] != 1 || data[1] != 0x10) return std::nullopt;
    const auto payload_len = ReadLe16(data.subspan(2, 2));
    if (data.size() < 4u + payload_len) return std::nullopt;
    const auto json = Utf8FromBytes(data.subspan(4, payload_len));
    StateEvent event;
    event.event = JsonStringValue(json, "event");
    if (event.event.empty()) return std::nullopt;
    event.button = JsonStringValue(json, "button");
    event.session_id = JsonU32Value(json, "session_id");
    event.duration_ms = JsonU32Value(json, "duration_ms");
    event.hardware = JsonStringValue(json, "hardware");
    event.firmware_version = JsonStringValue(json, "firmware_version");
    return event;
}

std::optional<FirmwareOtaStateEvent> BleProtocol::ParseFirmwareOtaStateEvent(std::span<const std::uint8_t> data) {
    if (data.size() < 4 || data[0] != 1 || data[1] != ota_type_state) return std::nullopt;
    const auto payload_len = ReadLe16(data.subspan(2, 2));
    if (data.size() < 4u + payload_len) return std::nullopt;
    const auto json = Utf8FromBytes(data.subspan(4, payload_len));
    FirmwareOtaStateEvent event;
    event.event = JsonStringValue(json, "event");
    if (event.event.empty()) return std::nullopt;
    event.transfer_id = JsonU32Value(json, "transfer_id");
    event.written = JsonU32Value(json, "written");
    event.size = JsonU32Value(json, "size");
    event.code = JsonStringValue(json, "code");
    event.reboot_ms = JsonU32Value(json, "reboot_ms");
    return event;
}

ByteVector BleProtocol::UiStatePayload(std::string_view state, std::string_view text) {
    const auto json = std::string("{\"event\":\"ui_state\",\"state\":\"") +
                      JsonEscape(state) + "\",\"text\":\"" + JsonEscape(text) + "\"}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::InteractionModePayload(std::string_view mode) {
    const auto json = std::string("{\"event\":\"interaction_mode\",\"mode\":\"") +
                      JsonEscape(mode) + "\"}";
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
