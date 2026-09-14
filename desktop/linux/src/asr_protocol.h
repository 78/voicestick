#pragma once

#include "app_config.h"
#include "byte_utils.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace voicestick {

enum class AsrEvent : std::uint32_t {
    kStartConnection = 1,
    kFinishConnection = 2,
    kConnectionStarted = 50,
    kConnectionFailed = 51,
    kConnectionFinished = 52,
    kStartSession = 100,
    kCancelSession = 101,
    kFinishSession = 102,
    kSessionStarted = 150,
    kSessionCanceled = 151,
    kSessionFinished = 152,
    kUsageResponse = 154,
    kTaskRequest = 200,
    kAsrInfo = 450,
    kAsrResponse = 451,
    kAsrEnd = 459,
};

struct AsrResponse {
    bool is_error = false;
    bool is_final = false;
    std::string text;
    std::optional<std::string> upgrade_url;
};

struct AsrEventResponse {
    std::uint32_t event_id = 0;
    std::optional<AsrEvent> event;
    std::string session_id;
    std::string payload_text;
};

enum class AsrResultType {
    kFull,
    kSingle,
};

struct AsrSessionOptions {
    std::vector<std::string> hotwords;
    AsrResultType result_type = AsrResultType::kFull;
    bool show_utterances = false;
};

struct AsrSegment {
    std::string text;
    bool definite = false;
    std::optional<int> start_time;
    std::optional<int> end_time;
};

class AsrProtocol {
public:
    static ByteVector MakeStartConnectionFrame(const AppConfig& config,
                                               const AsrSessionOptions& options = {});
    static ByteVector MakeFinishConnectionFrame(const AppConfig& config,
                                                const AsrSessionOptions& options = {});
    static ByteVector MakeStartSessionFrame(const AppConfig& config,
                                            std::string_view session_id,
                                            const AsrSessionOptions& options = {});
    static ByteVector MakeFinishSessionFrame(const AppConfig& config,
                                             std::string_view session_id,
                                             const AsrSessionOptions& options = {});
    static ByteVector MakeCancelSessionFrame(const AppConfig& config,
                                             std::string_view session_id,
                                             const AsrSessionOptions& options = {});
    static ByteVector MakeTaskRequestFrame(std::span<const std::uint8_t> ogg_data,
                                           std::string_view session_id);
    static std::optional<AsrResponse> ParseResponse(std::span<const std::uint8_t> data);
    static std::optional<AsrEventResponse> ParseEventResponse(std::span<const std::uint8_t> data);
    static std::string ExtractTranscript(std::string_view json_or_text);
    static std::vector<AsrSegment> ExtractSegments(std::string_view json_or_text);
    static std::vector<AsrSegment> ExtractNewDefiniteSegments(
        std::string_view json_or_text,
        std::set<std::string>* emitted_segment_keys);
    static std::string SegmentKey(const AsrSegment& segment);

private:
    static std::string SessionPayload(const AppConfig& config, const AsrSessionOptions& options);
    static std::string ConnectionPayload(const AppConfig& config, const AsrSessionOptions& options);
    static ByteVector MakeEventFrame(std::uint8_t message_type,
                                      AsrEvent event,
                                      std::string_view session_id,
                                      std::uint8_t serialization,
                                      std::span<const std::uint8_t> payload);
};

} // namespace voicestick
