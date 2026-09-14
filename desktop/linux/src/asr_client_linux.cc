#include "asr_client_linux.h"

#include "asr_protocol.h"
#include "http_client_linux.h"
#include "log.h"
#include "ui_dispatch.h"

#include <libsoup/soup.h>

#include <random>

namespace voicestick {

namespace {

std::string WebsocketHttpUrl(std::string url) {
    if (StartsWithScheme(url, "wss://")) return "https://" + url.substr(6);
    if (StartsWithScheme(url, "ws://")) return "http://" + url.substr(5);
    return url;
}

} // namespace

AsrClientLinux::AsrClientLinux(AppConfig config) : config_(std::move(config)) {}

AsrClientLinux::~AsrClientLinux() {
    Cancel();
    stop_ = true;
    if (worker_.joinable()) worker_.join();
}

std::string AsrClientLinux::GenerateSessionId() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    char buf[17] = {};
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(gen()));
    return buf;
}

void AsrClientLinux::SetLastStartError(std::string message) {
    std::lock_guard lock(mutex_);
    last_start_error_ = std::move(message);
}

std::string AsrClientLinux::LastStartError() const {
    std::lock_guard lock(mutex_);
    return last_start_error_;
}

bool AsrClientLinux::Start(AsrSessionOptions options) {
    SetLastStartError({});
    if (config_.ActiveApiKey().empty()) {
        SetLastStartError("Missing ASR API key");
        return false;
    }
    session_options_ = std::move(options);
    if (session_options_.hotwords.empty()) session_options_.hotwords = config_.asr_hotwords;
    {
        std::lock_guard lock(mutex_);
        if (session_active_) {
            last_start_error_ = "ASR session already active";
            return false;
        }
        current_session_id_ = GenerateSessionId();
        latest_transcript_.clear();
        emitted_segment_keys_.clear();
        queued_.clear();
        session_active_ = true;
        stop_ = false;
    }
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this] { RunLoop(); });
    return true;
}

void AsrClientLinux::SendOggOpusChunk(std::span<const std::uint8_t> data, bool is_last) {
    std::lock_guard lock(mutex_);
    queued_.push_back(QueuedChunk{ByteVector(data.begin(), data.end()), is_last});
}

void AsrClientLinux::Cancel() {
    std::lock_guard lock(mutex_);
    queued_.clear();
    session_active_ = false;
    stop_ = true;
}

void AsrClientLinux::Fail(const std::string& message) {
    SetLastStartError(message);
    {
        std::lock_guard lock(mutex_);
        session_active_ = false;
        stop_ = true;
    }
    if (on_error) {
        RunOnUiThread([this, message] { if (on_error) on_error(message); });
    }
}

void AsrClientLinux::RunLoop() {
    auto* context = g_main_context_new();
    g_main_context_push_thread_default(context);
    auto* loop = g_main_loop_new(context, FALSE);

    const auto url = WebsocketHttpUrl(config_.ActiveWebsocketUrl());
    SoupSession* session = soup_session_new_with_options("timeout", 15, "user-agent", "VoiceStick/Linux", nullptr);
    SoupMessage* message = soup_message_new(SOUP_METHOD_GET, url.c_str());
    if (!message) {
        Fail("Invalid ASR URL");
        g_object_unref(session);
        g_main_loop_unref(loop);
        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
        return;
    }
    auto* headers = soup_message_get_request_headers(message);
    soup_message_headers_replace(headers, "X-Api-Key", config_.ActiveApiKey().c_str());
    soup_message_headers_replace(headers, "X-Api-Request-Id", GenerateSessionId().c_str());
    soup_message_headers_replace(headers, "X-Api-Sequence", "-1");
    if (config_.asr_provider == AsrProvider::kVoiceStickCloud) {
        if (!config_.paired_device_ids.empty()) {
            soup_message_headers_replace(headers, "X-Device-Id", config_.paired_device_ids.front().c_str());
        }
    } else {
        soup_message_headers_replace(headers, "X-Api-Resource-Id", config_.resource_id.c_str());
    }

    struct LoopState {
        AsrClientLinux* self;
        GMainLoop* loop;
        SoupWebsocketConnection* socket = nullptr;
        bool started = false;
        bool finishing = false;
    } state{this, loop};

    soup_session_websocket_connect_async(
        session, message, nullptr, nullptr, G_PRIORITY_DEFAULT, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            auto* state = static_cast<LoopState*>(data);
            GError* error = nullptr;
            state->socket = soup_session_websocket_connect_finish(SOUP_SESSION(source), result, &error);
            if (!state->socket) {
                state->self->Fail(error && error->message ? error->message : "ASR WebSocket failed");
                if (error) g_error_free(error);
                g_main_loop_quit(state->loop);
                return;
            }
            soup_websocket_connection_set_keepalive_interval(state->socket, 15);
            g_signal_connect(state->socket, "message", G_CALLBACK(+[](SoupWebsocketConnection*,
                                                                      gint type,
                                                                      GBytes* bytes,
                                                                      gpointer user) {
                if (type != SOUP_WEBSOCKET_DATA_BINARY) return;
                auto* state = static_cast<LoopState*>(user);
                gsize size = 0;
                const auto* data = static_cast<const std::uint8_t*>(g_bytes_get_data(bytes, &size));
                if (!data || size < 4) return;
                auto event = AsrProtocol::ParseEventResponse(std::span(data, size));
                if (!event) {
                    auto response = AsrProtocol::ParseResponse(std::span(data, size));
                    if (response && response->is_error) {
                        if (response->upgrade_url && state->self->on_upgrade_url) {
                            RunOnUiThread([self = state->self, url = *response->upgrade_url,
                                           text = response->text] {
                                if (self->on_upgrade_url) self->on_upgrade_url(url, text);
                            });
                        }
                        state->self->Fail(response->text);
                    }
                    return;
                }
                switch (event->event.value_or(static_cast<AsrEvent>(0))) {
                case AsrEvent::kConnectionStarted: {
                    std::string session_id;
                    {
                        std::lock_guard lock(state->self->mutex_);
                        session_id = state->self->current_session_id_;
                    }
                    auto frame = AsrProtocol::MakeStartSessionFrame(
                        state->self->config_, session_id, state->self->session_options_);
                    soup_websocket_connection_send_binary(state->socket, frame.data(), frame.size());
                    break;
                }
                case AsrEvent::kSessionStarted:
                    state->started = true;
                    break;
                case AsrEvent::kAsrResponse:
                case AsrEvent::kAsrInfo: {
                    auto transcript = AsrProtocol::ExtractTranscript(event->payload_text);
                    std::vector<AsrSegment> segments;
                    {
                        std::lock_guard lock(state->self->mutex_);
                        if (!transcript.empty()) state->self->latest_transcript_ = transcript;
                        segments = AsrProtocol::ExtractNewDefiniteSegments(
                            event->payload_text, &state->self->emitted_segment_keys_);
                    }
                    if (!transcript.empty() && state->self->on_partial) {
                        RunOnUiThread([self = state->self, transcript] {
                            if (self->on_partial) self->on_partial(transcript);
                        });
                    }
                    for (const auto& segment : segments) {
                        if (state->self->on_segment) {
                            RunOnUiThread([self = state->self, segment] {
                                if (self->on_segment) self->on_segment(segment);
                            });
                        }
                    }
                    break;
                }
                case AsrEvent::kSessionFinished: {
                    std::string final_text;
                    {
                        std::lock_guard lock(state->self->mutex_);
                        final_text = AsrProtocol::ExtractTranscript(event->payload_text);
                        if (final_text.empty()) final_text = state->self->latest_transcript_;
                        state->self->session_active_ = false;
                    }
                    if (state->self->on_final) {
                        RunOnUiThread([self = state->self, final_text] {
                            if (self->on_final) self->on_final(final_text);
                        });
                    }
                    g_main_loop_quit(state->loop);
                    break;
                }
                case AsrEvent::kConnectionFailed:
                case AsrEvent::kSessionCanceled:
                    state->self->Fail(event->payload_text.empty() ? "ASR failed" : event->payload_text);
                    g_main_loop_quit(state->loop);
                    break;
                default:
                    break;
                }
            }), state);
            g_signal_connect(state->socket, "closed", G_CALLBACK(+[](SoupWebsocketConnection*, gpointer user) {
                auto* state = static_cast<LoopState*>(user);
                g_main_loop_quit(state->loop);
            }), state);

            auto start = AsrProtocol::MakeStartConnectionFrame(state->self->config_,
                                                               state->self->session_options_);
            soup_websocket_connection_send_binary(state->socket, start.data(), start.size());
        },
        &state);

    const guint flush = g_timeout_add(20, [](gpointer data) -> gboolean {
        auto* state = static_cast<LoopState*>(data);
        if (state->self->stop_) {
            g_main_loop_quit(state->loop);
            return G_SOURCE_REMOVE;
        }
        if (!state->started || !state->socket) return G_SOURCE_CONTINUE;
        std::vector<QueuedChunk> chunks;
        std::string session_id;
        {
            std::lock_guard lock(state->self->mutex_);
            chunks.swap(state->self->queued_);
            session_id = state->self->current_session_id_;
        }
        for (const auto& chunk : chunks) {
            auto frame = AsrProtocol::MakeTaskRequestFrame(chunk.data, session_id);
            soup_websocket_connection_send_binary(state->socket, frame.data(), frame.size());
            if (chunk.is_last && !state->finishing) {
                state->finishing = true;
                auto finish = AsrProtocol::MakeFinishSessionFrame(
                    state->self->config_, session_id, state->self->session_options_);
                soup_websocket_connection_send_binary(state->socket, finish.data(), finish.size());
            }
        }
        return G_SOURCE_CONTINUE;
    }, &state);

    g_main_loop_run(loop);
    g_source_remove(flush);
    if (state.socket) {
        soup_websocket_connection_close(state.socket, SOUP_WEBSOCKET_CLOSE_NORMAL, nullptr);
        g_object_unref(state.socket);
    }
    g_object_unref(message);
    g_object_unref(session);
    {
        std::lock_guard lock(mutex_);
        session_active_ = false;
    }
    g_main_loop_unref(loop);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
}

} // namespace voicestick
