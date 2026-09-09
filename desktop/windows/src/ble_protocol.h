#pragma once

#include "byte_utils.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace voicestick {

struct AudioFrame {
    std::uint32_t session_id = 0;
    std::uint32_t seq = 0;
    std::uint8_t flags = 0;
    ByteVector payload;

    bool IsStart() const { return (flags & 0x01) != 0; }
    bool IsEnd() const { return (flags & 0x02) != 0; }
};

struct StateEvent {
    std::string event;
    std::string button;
    std::optional<std::uint32_t> session_id;
    std::optional<std::uint32_t> duration_ms;
    std::string hardware;
    std::string firmware_version;
};

struct FirmwareOtaStateEvent {
    std::string event;
    std::optional<std::uint32_t> transfer_id;
    std::optional<std::uint32_t> written;
    std::optional<std::uint32_t> size;
    std::string code;
    std::optional<std::uint32_t> reboot_ms;
};

class BleProtocol {
public:
    static constexpr const wchar_t* service_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100";
    static constexpr const wchar_t* audio_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5101";
    static constexpr const wchar_t* state_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102";
    static constexpr const wchar_t* control_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103";
    static constexpr const wchar_t* ota_rx_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5104";
    static constexpr const wchar_t* ota_state_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5105";
    static constexpr std::uint8_t ota_type_begin = 0x20;
    static constexpr std::uint8_t ota_type_data = 0x21;
    static constexpr std::uint8_t ota_type_end = 0x22;
    static constexpr std::uint8_t ota_type_abort = 0x23;
    static constexpr std::uint8_t ota_type_state = 0x30;

    static std::optional<AudioFrame> ParseAudioFrame(std::span<const std::uint8_t> data);
    static std::optional<StateEvent> ParseStateEvent(std::span<const std::uint8_t> data);
    static std::optional<FirmwareOtaStateEvent> ParseFirmwareOtaStateEvent(std::span<const std::uint8_t> data);
    static ByteVector UiStatePayload(std::string_view state, std::string_view text);
    static ByteVector InteractionModePayload(std::string_view mode);
    static ByteVector OtaBeginPayload(std::uint32_t image_size, std::uint32_t transfer_id);
    static ByteVector OtaDataPayload(std::uint32_t transfer_id, std::uint32_t offset, std::span<const std::uint8_t> chunk);
    static ByteVector OtaEndPayload(std::uint32_t transfer_id, std::uint32_t image_size);
    static ByteVector OtaAbortPayload(std::uint32_t transfer_id);
    static std::optional<std::string> DeviceIdFromName(std::string_view name);
    static std::string NormalizeDeviceId(std::string_view text);
};

} // namespace voicestick
