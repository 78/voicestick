#pragma once

#include "byte_utils.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace voicestick {

struct AudioStreamFrame {
    std::uint16_t stream_id = 0;
    std::uint8_t flags = 0;
    std::uint32_t seq = 0;
    std::uint32_t timestamp_ms = 0;
    ByteVector payload;

    bool IsStart() const { return (flags & 0x01) != 0; }
    bool IsEnd() const { return (flags & 0x02) != 0; }
    bool IsFlush() const { return (flags & 0x04) != 0; }
};

struct ImuStreamFrame {
    std::uint16_t stream_id = 0;
    std::uint16_t seq = 0;
    std::uint32_t timestamp_ms = 0;
    std::int16_t ax = 0;
    std::int16_t ay = 0;
    std::int16_t az = 0;
    std::optional<std::int16_t> gx;
    std::optional<std::int16_t> gy;
    std::optional<std::int16_t> gz;
};

struct InputStreamFrame {
    std::uint16_t stream_id = 0;
    std::uint16_t seq = 0;
    std::uint32_t timestamp_ms = 0;
    std::uint32_t buttons_mask = 0;
};

struct DeviceBatteryStatus {
    std::uint32_t percent = 0;
    bool charging = false;
    bool usb_powered = false;

    bool operator==(const DeviceBatteryStatus&) const = default;
};

struct DeviceImuCapabilities {
    std::uint32_t accel_lsb_per_g = 0;
    double gyro_lsb_per_dps = 0.0;
    std::uint32_t max_rate_hz = 0;
    bool has_gyro = false;
};

struct DeviceInfoResult {
    std::vector<std::uint32_t> protocol_versions;
    std::string hardware;
    std::string firmware_version;
    std::string device_uuid;
    std::vector<std::string> buttons;
    bool has_audio_in = false;
    bool has_display = false;
    std::optional<DeviceImuCapabilities> imu;
    std::optional<DeviceBatteryStatus> battery;

    bool SupportsProtocolV2() const;
};

struct LegacyDeviceInfo {
    std::string hardware;
    std::string firmware_version;
    std::string device_uuid;
};

struct ControlRpcResponse {
    std::uint32_t id = 0;
    bool is_error = false;
    std::string error_code;
    std::string error_message;
    std::optional<std::uint16_t> stream_id;
    std::optional<std::uint32_t> buttons_mask;
    std::optional<DeviceInfoResult> device_info;
};

struct ControlEvent {
    std::string event;
    std::optional<DeviceBatteryStatus> battery;
    std::optional<std::uint16_t> stream_id;
    std::string reason;
};

struct ControlMessage {
    enum class Kind {
        kRpcResponse,
        kEvent,
    };
    Kind kind = Kind::kEvent;
    ControlRpcResponse rpc_response;
    ControlEvent control_event;
};

class ControlReassembler {
public:
    static constexpr std::size_t kMaxMessageBytes = 1024;
    static constexpr std::chrono::milliseconds kTimeout{2000};

    void Reset();
    std::optional<std::string> Push(
        std::span<const std::uint8_t> data,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

private:
    bool active_ = false;
    std::uint8_t msg_id_ = 0;
    std::uint8_t expected_idx_ = 0;
    std::uint8_t total_cnt_ = 0;
    std::chrono::steady_clock::time_point started_at_{};
    ByteVector buffer_;
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
    static constexpr std::uint8_t protocol_version = 2;
    static constexpr std::size_t max_control_message_bytes = 1024;

    static constexpr const wchar_t* service_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100";
    static constexpr const wchar_t* stream_tx_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5101";
    static constexpr const wchar_t* control_tx_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102";
    static constexpr const wchar_t* control_rx_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103";
    static constexpr const wchar_t* ota_rx_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5104";
    static constexpr const wchar_t* ota_state_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5105";
    static constexpr const wchar_t* stream_rx_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5109";

    static constexpr std::uint8_t type_nop = 0x00;
    static constexpr std::uint8_t type_audio_in = 0x01;
    static constexpr std::uint8_t type_audio_out = 0x02;
    static constexpr std::uint8_t type_imu = 0x03;
    static constexpr std::uint8_t type_input = 0x04;
    static constexpr std::uint8_t type_control_json = 0x10;
    static constexpr std::uint8_t type_control_chunk = 0x1F;

    static constexpr std::uint8_t ota_type_begin = 0x20;
    static constexpr std::uint8_t ota_type_data = 0x21;
    static constexpr std::uint8_t ota_type_end = 0x22;
    static constexpr std::uint8_t ota_type_abort = 0x23;
    static constexpr std::uint8_t ota_type_state = 0x30;

    static std::optional<AudioStreamFrame> ParseAudioStreamFrame(std::span<const std::uint8_t> data);
    static std::optional<ImuStreamFrame> ParseImuStreamFrame(std::span<const std::uint8_t> data);
    static std::optional<InputStreamFrame> ParseInputStreamFrame(std::span<const std::uint8_t> data);
    static std::optional<LegacyDeviceInfo> ParseLegacyDeviceInfo(std::span<const std::uint8_t> data);
    static std::optional<ControlMessage> ParseControlMessage(std::string_view json);
    static std::optional<FirmwareOtaStateEvent> ParseFirmwareOtaStateEvent(std::span<const std::uint8_t> data);

    static std::vector<ByteVector> EncodeControlFrames(
        std::string_view json, std::size_t max_write_len, std::uint8_t& next_msg_id);

    static std::string RpcDeviceGetInfo(std::uint32_t id);
    static std::string RpcDeviceReset(std::uint32_t id);
    static std::string RpcStreamStartInput(std::uint32_t id);
    static std::string RpcStreamStartAudioIn(std::uint32_t id, std::string_view trigger,
                                             std::string_view button = "primary");
    static std::string RpcStreamStartImu(std::uint32_t id, std::string_view axes,
                                         int rate_hz, int lease_ms);
    static std::string RpcStreamRenew(std::uint32_t id, std::uint16_t stream_id, int lease_ms);
    static std::string RpcStreamStop(std::uint32_t id, std::uint16_t stream_id);
    static std::string RpcDisplaySet(std::uint32_t id, std::string_view scene, std::string_view text);
    static ByteVector LegacyGetDeviceInfoPayload();

    static ByteVector OtaBeginPayload(std::uint32_t image_size, std::uint32_t transfer_id);
    static ByteVector OtaDataPayload(std::uint32_t transfer_id, std::uint32_t offset, std::span<const std::uint8_t> chunk);
    static ByteVector OtaEndPayload(std::uint32_t transfer_id, std::uint32_t image_size);
    static ByteVector OtaAbortPayload(std::uint32_t transfer_id);
    static std::optional<std::string> DeviceIdFromName(std::string_view name);
    static std::string NormalizeDeviceId(std::string_view text);
};

} // namespace voicestick
