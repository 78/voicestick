#include "ble_central_bluez.h"

#include "log.h"
#include "ui_dispatch.h"

#include <gio/gio.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace voicestick {

namespace {

constexpr const char* kBluez = "org.bluez";
constexpr const char* kAdapterIface = "org.bluez.Adapter1";
constexpr const char* kDeviceIface = "org.bluez.Device1";
constexpr const char* kGattCharIface = "org.bluez.GattCharacteristic1";
constexpr const char* kServiceUuid = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100";
constexpr const char* kStreamTxUuid = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5101";
constexpr const char* kControlTxUuid = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102";
constexpr const char* kControlRxUuid = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103";
constexpr const char* kOtaRxUuid = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5104";
constexpr const char* kOtaStateUuid = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5105";

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool UuidMatch(std::string_view left, std::string_view right) {
    return Lower(std::string(left)) == Lower(std::string(right));
}

std::uint64_t AddressToUint64(const std::string& address) {
    std::uint64_t value = 0;
    unsigned bytes[6] = {};
    if (std::sscanf(address.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
                    &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]) != 6) {
        return 0;
    }
    for (unsigned byte : bytes) {
        value = (value << 8) | byte;
    }
    return value;
}

std::string AddressFromUint64(std::uint64_t address) {
    char buf[18] = {};
    std::snprintf(buf, sizeof(buf), ":%02X:%02X:%02X:%02X:%02X:%02X",
                  static_cast<unsigned>((address >> 40) & 0xff),
                  static_cast<unsigned>((address >> 32) & 0xff),
                  static_cast<unsigned>((address >> 24) & 0xff),
                  static_cast<unsigned>((address >> 16) & 0xff),
                  static_cast<unsigned>((address >> 8) & 0xff),
                  static_cast<unsigned>(address & 0xff));
    return buf + 1;
}

GVariant* GetProperty(GDBusProxy* proxy, const char* name) {
    return g_dbus_proxy_get_cached_property(proxy, name);
}

std::string PropertyString(GDBusProxy* proxy, const char* name) {
    auto* value = GetProperty(proxy, name);
    if (!value) return {};
    const char* text = g_variant_get_string(value, nullptr);
    std::string result = text ? text : "";
    g_variant_unref(value);
    return result;
}

bool PropertyBool(GDBusProxy* proxy, const char* name) {
    auto* value = GetProperty(proxy, name);
    if (!value) return false;
    const bool result = g_variant_get_boolean(value);
    g_variant_unref(value);
    return result;
}

int PropertyInt16(GDBusProxy* proxy, const char* name) {
    auto* value = GetProperty(proxy, name);
    if (!value) return 0;
    const int result = g_variant_get_int16(value);
    g_variant_unref(value);
    return result;
}

GDBusProxy* MakeProxy(GDBusConnection* connection, const char* path, const char* iface) {
    GError* error = nullptr;
    auto* proxy = g_dbus_proxy_new_sync(connection, G_DBUS_PROXY_FLAGS_NONE, nullptr,
                                        kBluez, path, iface, nullptr, &error);
    if (error) {
        LogBle(std::string("proxy failed: ") + error->message);
        g_error_free(error);
    }
    return proxy;
}

bool Call(GDBusProxy* proxy, const char* method, GVariant* args = nullptr) {
    GError* error = nullptr;
    auto* result = g_dbus_proxy_call_sync(proxy, method, args, G_DBUS_CALL_FLAGS_NONE,
                                          15000, nullptr, &error);
    if (error) {
        LogBle(std::string(method) + " failed: " + error->message);
        g_error_free(error);
        return false;
    }
    if (result) g_variant_unref(result);
    return true;
}

std::string TruncateUiText(std::string_view text) {
    // Firmware copies ui_state text into a 96-byte buffer. A long transcript
    // only needs to travel as a hint; sending the whole paragraph exceeds the
    // GATT write and BlueZ drops the session.
    constexpr std::size_t kMax = 80;
    if (text.size() <= kMax) return std::string(text);
    std::size_t end = kMax;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return std::string(text.substr(0, end));
}

GVariant* WriteValueArgs(const ByteVector& payload, const char* type = "request") {
    GVariantBuilder bytes;
    g_variant_builder_init(&bytes, G_VARIANT_TYPE("ay"));
    for (auto byte : payload) g_variant_builder_add(&bytes, "y", byte);
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&opts, "{sv}", "type", g_variant_new_string(type));
    return g_variant_new("(@aya{sv})", g_variant_builder_end(&bytes), &opts);
}

bool WriteCharacteristic(GDBusConnection* connection, const std::string& path, const ByteVector& payload) {
    auto* proxy = MakeProxy(connection, path.c_str(), kGattCharIface);
    if (!proxy) return false;
    const bool ok = Call(proxy, "WriteValue", WriteValueArgs(payload));
    g_object_unref(proxy);
    return ok;
}

struct FrameWriteContext {
    GDBusConnection* connection = nullptr;
    std::string path;
    std::vector<ByteVector> frames;
    std::size_t index = 0;
};

void WriteNextFrame(FrameWriteContext* ctx);

void WriteNextFrame(FrameWriteContext* ctx) {
    if (!ctx || ctx->index >= ctx->frames.size()) {
        if (ctx) {
            if (ctx->connection) g_object_unref(ctx->connection);
            delete ctx;
        }
        return;
    }
    auto* proxy = MakeProxy(ctx->connection, ctx->path.c_str(), kGattCharIface);
    if (!proxy) {
        if (ctx->connection) g_object_unref(ctx->connection);
        delete ctx;
        return;
    }
    const auto& frame = ctx->frames[ctx->index++];
    g_dbus_proxy_call(proxy, "WriteValue", WriteValueArgs(frame), G_DBUS_CALL_FLAGS_NONE, 3000, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer user_data) {
            auto* ctx = static_cast<FrameWriteContext*>(user_data);
            GError* error = nullptr;
            auto* reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
            if (error) {
                LogBle(std::string("WriteValue failed: ") + error->message);
                g_error_free(error);
            }
            if (reply) g_variant_unref(reply);
            g_object_unref(source);
            WriteNextFrame(ctx);
        }, ctx);
}

void WriteFramesAsync(GDBusConnection* connection, const std::string& path, std::vector<ByteVector> frames) {
    if (!connection || path.empty() || frames.empty()) return;
    auto* ctx = new FrameWriteContext{
        static_cast<GDBusConnection*>(g_object_ref(connection)),
        path,
        std::move(frames),
        0,
    };
    WriteNextFrame(ctx);
}

} // namespace

struct BleCentralBluez::Impl {
    using RpcCallback = std::function<void(std::optional<ControlRpcResponse>)>;

    struct DeviceProtocolState {
        std::size_t max_write_len = 20;
        ControlReassembler control_reassembler;
        std::uint8_t next_control_msg_id = 1;
        std::uint32_t next_rpc_id = 1;
        std::map<std::uint32_t, RpcCallback> pending_rpcs;
        std::optional<DeviceInfoResult> info;
        std::uint16_t input_stream_id = 0;
        std::uint16_t audio_in_stream_id = 0;
        std::optional<std::uint16_t> last_input_seq;
        std::uint32_t buttons_mask = 0;
        bool initialized = false;
        std::optional<std::string> last_display_scene;
        std::optional<std::string> last_display_text;
    };

    BleCentralBluez* owner = nullptr;
    GDBusConnection* connection = nullptr;
    GDBusProxy* adapter = nullptr;
    std::string adapter_path;
    guint object_signal = 0;
    guint removed_signal = 0;
    guint props_signal = 0;
    guint sleep_signal = 0;
    guint watch_timeout = 0;
    mutable std::mutex mutex;
    std::set<std::string> paired_ids;
    std::map<std::string, ScannedBleDevice> scanned;
    std::map<std::string, std::string> sessions;  // device_id -> device path
    std::map<std::string, std::string> control_paths;
    std::map<std::string, std::string> ota_rx_paths;
    std::map<std::string, DeviceProtocolState> proto_states;
    std::map<std::string, guint> notify_subs;
    std::map<std::string, std::vector<GDBusProxy*>> notify_proxies;
    std::map<std::string, gint64> last_connect_try;
    std::set<std::string> cancelled;
    std::set<std::string> connecting;
    std::vector<std::pair<std::string, AudioStreamFrame>> audio_queue;
    guint audio_idle = 0;
    std::atomic<bool> alive{true};
    bool pairing_scan = false;
    bool discovering = false;
    bool firmware_cancel = false;

    void PublishScan() {
        if (!owner || !owner->on_scan_updated) return;
        std::vector<ScannedBleDevice> devices;
        for (const auto& [_, device] : scanned) devices.push_back(device);
        RunOnUiThread([owner = owner, devices = std::move(devices)] {
            if (owner->on_scan_updated) owner->on_scan_updated(devices);
        });
    }

    void PublishConnections() {
        if (!owner || !owner->on_connection_change) return;
        std::vector<ConnectedDevice> devices;
        for (const auto& [id, path] : sessions) {
            devices.push_back(ConnectedDevice{id, "VS-" + id});
        }
        RunOnUiThread([owner = owner, devices = std::move(devices)] {
            if (owner->on_connection_change) owner->on_connection_change(devices);
        });
    }

    void EnsureDiscovery() {
        if (!adapter) return;
        discovering = PropertyBool(adapter, "Discovering");
        if (discovering) return;
        GVariantBuilder filter;
        g_variant_builder_init(&filter, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&filter, "{sv}", "Transport", g_variant_new_string("le"));
        g_variant_builder_add(&filter, "{sv}", "DuplicateData", g_variant_new_boolean(TRUE));
        Call(adapter, "SetDiscoveryFilter", g_variant_new("(a{sv})", &filter));
        discovering = Call(adapter, "StartDiscovery");
        if (discovering) LogBle("Started BLE discovery");
    }

    void MaybeStopDiscovery() {
        if (!adapter) return;
        discovering = PropertyBool(adapter, "Discovering");
        if (!discovering) return;
        std::lock_guard lock(mutex);
        if (pairing_scan || sessions.size() < paired_ids.size()) return;
        Call(adapter, "StopDiscovery");
        discovering = false;
    }

    void CleanupNotify(const std::string& device_id) {
        auto it = notify_proxies.find(device_id);
        if (it == notify_proxies.end()) return;
        for (auto* proxy : it->second) {
            if (const char* path = g_dbus_proxy_get_object_path(proxy)) {
                notify_subs.erase(path);
            }
            g_signal_handlers_disconnect_by_data(proxy, this);
            g_object_unref(proxy);
        }
        notify_proxies.erase(it);
    }

    std::string DeviceIdForPath(const std::string& path) const {
        for (const auto& [id, session_path] : sessions) {
            if (session_path == path) return id;
        }
        for (const auto& [id, device] : scanned) {
            if (device.object_path == path) return id;
        }
        return {};
    }

    void DropSession(const std::string& device_id, const char* reason) {
        bool had = false;
        {
            std::lock_guard lock(mutex);
            had = sessions.erase(device_id) > 0;
            control_paths.erase(device_id);
            ota_rx_paths.erase(device_id);
            proto_states.erase(device_id);
        }
        CleanupNotify(device_id);
        if (had) {
            LogBle(std::string("Disconnected ") + device_id + " (" + reason + ")");
            PublishConnections();
        }
        EnsureDiscovery();
    }

    void DropSessionByPath(const std::string& path, const char* reason) {
        std::string device_id;
        {
            std::lock_guard lock(mutex);
            device_id = DeviceIdForPath(path);
        }
        if (!device_id.empty()) DropSession(device_id, reason);
    }

    void HandleDevice(const std::string& path) {
        auto* proxy = MakeProxy(connection, path.c_str(), kDeviceIface);
        if (!proxy) return;
        const auto name = PropertyString(proxy, "Name");
        const auto alias = PropertyString(proxy, "Alias");
        const auto address = PropertyString(proxy, "Address");
        const auto address_type = Lower(PropertyString(proxy, "AddressType"));
        const int rssi = PropertyInt16(proxy, "RSSI");
        auto device_id = BleProtocol::DeviceIdFromName(name.empty() ? alias : name);
        if (!device_id && !name.empty()) device_id = BleProtocol::DeviceIdFromName(alias);
        if (device_id && device_id->size() == 4) {
            ScannedBleDevice scanned_device;
            scanned_device.device_id = *device_id;
            scanned_device.name = name.empty() ? alias : name;
            scanned_device.bluetooth_address = AddressToUint64(address);
            scanned_device.address_kind = address_type == "random"
                                              ? BluetoothAddressKind::kRandom
                                              : BluetoothAddressKind::kPublic;
            scanned_device.rssi = rssi;
            scanned_device.object_path = path;
            {
                std::lock_guard lock(mutex);
                scanned[*device_id] = scanned_device;
            }
            if (pairing_scan) PublishScan();
            bool should_connect = false;
            {
                std::lock_guard lock(mutex);
                should_connect = paired_ids.contains(*device_id) &&
                                 !sessions.contains(*device_id) &&
                                 !connecting.contains(*device_id) &&
                                 !cancelled.contains(*device_id);
            }
            if (should_connect) ConnectPath(*device_id, path);
        }
        g_object_unref(proxy);
    }

    void ConnectPath(const std::string& device_id, const std::string& path) {
        {
            std::lock_guard lock(mutex);
            if (!alive || sessions.contains(device_id) || connecting.contains(device_id) ||
                cancelled.contains(device_id)) {
                return;
            }
            const gint64 now = g_get_monotonic_time();
            if (auto it = last_connect_try.find(device_id);
                it != last_connect_try.end() && now - it->second < 1500 * 1000) {
                return;
            }
            last_connect_try[device_id] = now;
            connecting.insert(device_id);
        }
        LogBle("Connecting " + device_id + " at " + path);
        // BlueZ Connect + service discovery can take several seconds. Never do
        // that on the GTK thread, and never while holding mutex (Finish used
        // to deadlock here).
        std::thread([this, device_id, path] {
            bool resolved = false;
            auto* device = MakeProxy(connection, path.c_str(), kDeviceIface);
            if (device) {
                const bool connected = PropertyBool(device, "Connected") || Call(device, "Connect");
                if (connected) {
                    for (int i = 0; i < 40 && alive && !PropertyBool(device, "ServicesResolved"); ++i) {
                        g_usleep(100 * 1000);
                    }
                    resolved = PropertyBool(device, "ServicesResolved");
                }
                g_object_unref(device);
            }
            RunOnUiThread([this, device_id, path, resolved] {
                FinishConnect(device_id, path, resolved);
            });
        }).detach();
    }

    void FinishConnect(const std::string& device_id, const std::string& path, bool resolved) {
        {
            std::lock_guard lock(mutex);
            connecting.erase(device_id);
            if (!alive || cancelled.contains(device_id)) return;
        }
        if (!resolved) {
            LogBle("Connect failed for " + device_id);
            if (pairing_scan && owner && owner->on_connection_error) {
                owner->on_connection_error(device_id, "Failed to connect");
            }
            EnsureDiscovery();
            return;
        }
        if (!SubscribeGatt(device_id, path)) {
            LogBle("GATT subscribe failed for " + device_id);
            if (auto* device = MakeProxy(connection, path.c_str(), kDeviceIface)) {
                Call(device, "Disconnect");
                g_object_unref(device);
            }
            EnsureDiscovery();
            return;
        }
        {
            std::lock_guard lock(mutex);
            sessions[device_id] = path;
        }
        LogBle("Connected " + device_id + "; starting Protocol v2 handshake");
        PublishConnections();
        StartV2Handshake(device_id);
        MaybeStopDiscovery();
    }

    struct RpcTimeoutCtx {
        Impl* impl;
        std::string device_id;
        std::uint32_t rpc_id;
    };

    void SendRpc(const std::string& device_id,
                 std::function<std::string(std::uint32_t)> build_json,
                 RpcCallback completion = {},
                 guint timeout_ms = 3000) {
        std::string control_path;
        std::uint32_t rpc_id = 0;
        std::vector<ByteVector> frames;
        {
            std::lock_guard lock(mutex);
            auto path_it = control_paths.find(device_id);
            if (path_it == control_paths.end()) {
                if (completion) completion(std::nullopt);
                return;
            }
            control_path = path_it->second;
            auto& state = proto_states[device_id];
            rpc_id = state.next_rpc_id++;
            if (state.next_rpc_id == 0) state.next_rpc_id = 1;
            const std::string json = build_json(rpc_id);
            frames = BleProtocol::EncodeControlFrames(json, state.max_write_len, state.next_control_msg_id);
            if (frames.empty()) {
                if (completion) completion(std::nullopt);
                return;
            }
            if (completion) {
                state.pending_rpcs[rpc_id] = std::move(completion);
            }
        }
        WriteFramesAsync(connection, control_path, std::move(frames));
        if (timeout_ms > 0) {
            auto* ctx = new RpcTimeoutCtx{this, device_id, rpc_id};
            g_timeout_add(timeout_ms, [](gpointer user_data) -> gboolean {
                auto* ctx = static_cast<RpcTimeoutCtx*>(user_data);
                RpcCallback cb;
                if (ctx->impl->alive) {
                    std::lock_guard lock(ctx->impl->mutex);
                    auto state_it = ctx->impl->proto_states.find(ctx->device_id);
                    if (state_it != ctx->impl->proto_states.end()) {
                        auto rpc_it = state_it->second.pending_rpcs.find(ctx->rpc_id);
                        if (rpc_it != state_it->second.pending_rpcs.end()) {
                            cb = std::move(rpc_it->second);
                            state_it->second.pending_rpcs.erase(rpc_it);
                        }
                    }
                }
                delete ctx;
                if (cb) cb(std::nullopt);
                return G_SOURCE_REMOVE;
            }, ctx);
        }
    }

    void ProbeLegacyDeviceInfo(const std::string& device_id) {
        std::string control_path;
        {
            std::lock_guard lock(mutex);
            auto it = control_paths.find(device_id);
            if (it == control_paths.end()) return;
            control_path = it->second;
        }
        WriteFramesAsync(connection, control_path, {
            BleProtocol::LegacyGetDeviceInfoPayload(),
            BleProtocol::LegacyUiStateReadyPayload(),
        });
        LogBle("Legacy get_device_info + ready sent to " + device_id);
    }

    void StartV2Handshake(const std::string& device_id) {
        SendRpc(device_id, [](std::uint32_t rpc_id) {
            return BleProtocol::RpcDeviceGetInfo(rpc_id);
        }, [this, device_id](std::optional<ControlRpcResponse> resp) {
            if (!alive) return;
            if (!resp || resp->is_error || !resp->device_info || !resp->device_info->SupportsProtocolV2()) {
                LogBle("v2 device.get_info fallback to legacy probe for " + device_id);
                ProbeLegacyDeviceInfo(device_id);
                return;
            }
            auto info = *resp->device_info;
            {
                std::lock_guard lock(mutex);
                if (!sessions.contains(device_id)) return;
                proto_states[device_id].info = info;
            }
            if (owner && owner->on_device_info) owner->on_device_info(device_id, info);

            SendRpc(device_id, [](std::uint32_t rpc_id) {
                return BleProtocol::RpcDeviceReset(rpc_id);
            }, [this, device_id, info](std::optional<ControlRpcResponse> reset_resp) {
                if (!alive || !reset_resp || reset_resp->is_error) {
                    LogBle("v2 device.reset failed for " + device_id);
                    return;
                }
                SendRpc(device_id, [](std::uint32_t rpc_id) {
                    return BleProtocol::RpcStreamStartInput(rpc_id);
                }, [this, device_id, info](std::optional<ControlRpcResponse> input_resp) {
                    if (!alive || !input_resp || input_resp->is_error || !input_resp->stream_id) {
                        LogBle("v2 stream.start(input) failed for " + device_id);
                        return;
                    }
                    {
                        std::lock_guard lock(mutex);
                        auto it = proto_states.find(device_id);
                        if (it == proto_states.end()) return;
                        it->second.input_stream_id = *input_resp->stream_id;
                        it->second.last_input_seq.reset();
                        it->second.buttons_mask = input_resp->buttons_mask.value_or(0);
                        it->second.initialized = true;
                    }
                    LogBle("v2 handshake ready for " + device_id);
                    if (owner && owner->on_device_ready) owner->on_device_ready(device_id, info);
                });
            });
        }, 1500);
    }

    void HandleControlMessage(const std::string& device_id, ControlMessage message) {
        if (message.kind == ControlMessage::Kind::kRpcResponse) {
            RpcCallback cb;
            {
                std::lock_guard lock(mutex);
                auto state_it = proto_states.find(device_id);
                if (state_it != proto_states.end()) {
                    auto rpc_it = state_it->second.pending_rpcs.find(message.rpc_response.id);
                    if (rpc_it != state_it->second.pending_rpcs.end()) {
                        cb = std::move(rpc_it->second);
                        state_it->second.pending_rpcs.erase(rpc_it);
                    }
                }
            }
            if (cb) cb(std::move(message.rpc_response));
            return;
        }
        const auto& ev = message.control_event;
        if (ev.event == "stream.closed" && ev.stream_id) {
            {
                std::lock_guard lock(mutex);
                auto state_it = proto_states.find(device_id);
                if (state_it != proto_states.end()) {
                    if (state_it->second.audio_in_stream_id == *ev.stream_id) {
                        state_it->second.audio_in_stream_id = 0;
                    }
                    if (state_it->second.input_stream_id == *ev.stream_id) {
                        state_it->second.input_stream_id = 0;
                    }
                }
            }
            if (owner && owner->on_stream_closed) {
                owner->on_stream_closed(device_id, *ev.stream_id, ev.reason);
            }
        }
    }

    void HandleInputFrame(const std::string& device_id, const InputStreamFrame& frame) {
        bool has_gap = false;
        std::uint32_t prev_mask = 0;
        std::vector<std::string> buttons;
        {
            std::lock_guard lock(mutex);
            auto state_it = proto_states.find(device_id);
            if (state_it == proto_states.end()) return;
            auto& state = state_it->second;
            if (state.input_stream_id == 0 || state.input_stream_id != frame.stream_id) return;
            if (state.last_input_seq.has_value() &&
                static_cast<std::uint16_t>(*state.last_input_seq + 1) != frame.seq) {
                has_gap = true;
            }
            state.last_input_seq = frame.seq;
            prev_mask = state.buttons_mask;
            state.buttons_mask = frame.buttons_mask;
            if (state.info) buttons = state.info->buttons;
        }
        if (has_gap && owner && owner->on_input_gap) {
            owner->on_input_gap(device_id);
        }
        const std::uint32_t diff = prev_mask ^ frame.buttons_mask;
        if (diff == 0 || !owner || !owner->on_button_event) return;
        for (std::size_t bit = 0; bit < buttons.size() && bit < 32; ++bit) {
            const std::uint32_t mask = 1u << bit;
            if ((diff & mask) != 0) {
                const bool is_down = (frame.buttons_mask & mask) != 0;
                owner->on_button_event(device_id, buttons[bit], is_down, frame.timestamp_ms);
            }
        }
    }

    bool SubscribeGatt(const std::string& device_id, const std::string& device_path) {
        GError* error = nullptr;
        auto* result = g_dbus_connection_call_sync(
            connection, kBluez, "/", "org.freedesktop.DBus.ObjectManager",
            "GetManagedObjects", nullptr, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
            G_DBUS_CALL_FLAGS_NONE, 8000, nullptr, &error);
        if (!result) {
            if (error) {
                LogBle(error->message);
                g_error_free(error);
            }
            return false;
        }
        GVariant* objects = nullptr;
        g_variant_get(result, "(@a{oa{sa{sv}}})", &objects);
        GVariantIter iter;
        g_variant_iter_init(&iter, objects);
        const gchar* path = nullptr;
        GVariant* ifaces = nullptr;
        std::string stream_tx;
        std::string control_tx;
        std::string control_rx;
        std::string ota_rx;
        std::string ota_state;
        std::size_t control_mtu = 20;
        while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
            if (std::string(path).rfind(device_path, 0) != 0) {
                g_variant_unref(ifaces);
                continue;
            }
            GVariant* props = nullptr;
            if (g_variant_lookup(ifaces, kGattCharIface, "@a{sv}", &props)) {
                const gchar* uuid = nullptr;
                if (g_variant_lookup(props, "UUID", "&s", &uuid) && uuid) {
                    if (UuidMatch(uuid, kStreamTxUuid)) stream_tx = path;
                    if (UuidMatch(uuid, kControlTxUuid)) control_tx = path;
                    if (UuidMatch(uuid, kControlRxUuid)) {
                        control_rx = path;
                        guint16 mtu = 0;
                        if (g_variant_lookup(props, "MTU", "q", &mtu) && mtu > 3) {
                            control_mtu = static_cast<std::size_t>(mtu) - 3;
                        }
                    }
                    if (UuidMatch(uuid, kOtaRxUuid)) ota_rx = path;
                    if (UuidMatch(uuid, kOtaStateUuid)) ota_state = path;
                }
                g_variant_unref(props);
            }
            g_variant_unref(ifaces);
        }
        g_variant_unref(objects);
        g_variant_unref(result);
        if (stream_tx.empty() || control_tx.empty() || control_rx.empty()) {
            LogBle("Missing VoiceStick characteristics on " + device_id);
            return false;
        }
        {
            std::lock_guard lock(mutex);
            control_paths[device_id] = control_rx;
            ota_rx_paths[device_id] = ota_rx;
            DeviceProtocolState state;
            state.max_write_len = std::max<std::size_t>(control_mtu, 20);
            proto_states[device_id] = std::move(state);
        }
        StartNotify(device_id, control_tx, false);
        StartNotify(device_id, stream_tx, true);
        if (!ota_state.empty()) StartNotify(device_id, ota_state, false);
        return true;
    }

    void StartNotify(const std::string& device_id, const std::string& path, bool) {
        auto* proxy = MakeProxy(connection, path.c_str(), kGattCharIface);
        if (!proxy) return;
        const guint sub = g_signal_connect(proxy, "g-properties-changed",
            reinterpret_cast<GCallback>(+[](GDBusProxy* proxy, GVariant* changed, GStrv, gpointer data) {
                auto* impl = static_cast<Impl*>(data);
                GVariant* value = nullptr;
                if (!g_variant_lookup(changed, "Value", "@ay", &value)) return;
                gsize n = 0;
                const auto* bytes = static_cast<const std::uint8_t*>(g_variant_get_fixed_array(value, &n, 1));
                ByteVector payload(bytes, bytes + n);
                g_variant_unref(value);
                const auto uuid = PropertyString(proxy, "UUID");
                auto* owner = impl->owner;
                if (!owner) return;
                const auto dev_id = impl->DeviceIdForProxy(proxy);
                if (UuidMatch(uuid, kStreamTxUuid)) {
                    if (payload.size() < 4 || payload[0] != BleProtocol::protocol_version) return;
                    if (payload[1] == BleProtocol::type_nop) return;
                    if (payload[1] == BleProtocol::type_audio_in) {
                        if (auto frame = BleProtocol::ParseAudioStreamFrame(payload)) {
                            bool accept = true;
                            {
                                std::lock_guard lock(impl->mutex);
                                auto it = impl->proto_states.find(dev_id);
                                if (it != impl->proto_states.end() &&
                                    it->second.audio_in_stream_id != 0 &&
                                    it->second.audio_in_stream_id != frame->stream_id) {
                                    accept = false;
                                } else if (it != impl->proto_states.end() && frame->IsStart()) {
                                    it->second.last_display_scene = "recording";
                                    it->second.last_display_text = "";
                                }
                            }
                            if (!accept) return;
                            impl->audio_queue.emplace_back(dev_id, std::move(*frame));
                            if (!impl->audio_idle) {
                                impl->audio_idle = g_idle_add([](gpointer data) -> gboolean {
                                    auto* impl = static_cast<Impl*>(data);
                                    impl->audio_idle = 0;
                                    auto batch = std::move(impl->audio_queue);
                                    auto* owner = impl->owner;
                                    if (!owner || !owner->on_audio_frame) return G_SOURCE_REMOVE;
                                    for (auto& [device_id, frame] : batch) {
                                        owner->on_audio_frame(device_id, frame);
                                    }
                                    return G_SOURCE_REMOVE;
                                }, impl);
                            }
                        }
                    } else if (payload[1] == BleProtocol::type_input) {
                        if (auto frame = BleProtocol::ParseInputStreamFrame(payload)) {
                            RunOnUiThread([impl, dev_id, f = *frame] {
                                if (impl->alive) impl->HandleInputFrame(dev_id, f);
                            });
                        }
                    }
                } else if (UuidMatch(uuid, kControlTxUuid)) {
                    if (payload.size() >= 4 && payload[0] == 1 && payload[1] == 0x10) {
                        if (auto legacy = BleProtocol::ParseLegacyDeviceInfo(payload)) {
                            std::string control_path;
                            {
                                std::lock_guard lock(impl->mutex);
                                auto it = impl->control_paths.find(dev_id);
                                if (it != impl->control_paths.end()) control_path = it->second;
                            }
                            if (!control_path.empty()) {
                                WriteFramesAsync(impl->connection, control_path,
                                                 {BleProtocol::LegacyUiStateReadyPayload()});
                            }
                            RunOnUiThread([owner, dev_id, info = std::move(*legacy)] {
                                if (owner->on_legacy_device_info) owner->on_legacy_device_info(dev_id, info);
                            });
                        }
                        return;
                    }
                    std::optional<std::string> json;
                    {
                        std::lock_guard lock(impl->mutex);
                        auto it = impl->proto_states.find(dev_id);
                        if (it != impl->proto_states.end()) {
                            json = it->second.control_reassembler.Push(payload);
                        }
                    }
                    if (!json) return;
                    if (auto message = BleProtocol::ParseControlMessage(*json)) {
                        RunOnUiThread([impl, dev_id, msg = std::move(*message)]() mutable {
                            if (impl->alive) impl->HandleControlMessage(dev_id, std::move(msg));
                        });
                    }
                } else if (UuidMatch(uuid, kOtaStateUuid)) {
                    if (auto event = BleProtocol::ParseFirmwareOtaStateEvent(payload)) {
                        impl->HandleOtaState(*event);
                    }
                }
            }), this);
        notify_subs[path] = sub;
        g_object_set_data_full(G_OBJECT(proxy), "voicestick-device",
                               g_strdup(device_id.c_str()), g_free);
        notify_proxies[device_id].push_back(proxy);
        Call(proxy, "StartNotify");
    }

    void WatchConnections() {
        if (!alive) return;
        std::vector<std::pair<std::string, std::string>> active;
        std::vector<std::string> missing;
        {
            std::lock_guard lock(mutex);
            for (const auto& [id, path] : sessions) active.emplace_back(id, path);
            for (const auto& id : paired_ids) {
                if (!sessions.contains(id) && !connecting.contains(id) && !cancelled.contains(id)) {
                    missing.push_back(id);
                }
            }
        }
        for (const auto& [id, path] : active) {
            auto* device = MakeProxy(connection, path.c_str(), kDeviceIface);
            const bool connected = device && PropertyBool(device, "Connected");
            if (device) g_object_unref(device);
            if (!connected) DropSession(id, "connection lost");
        }
        if (!missing.empty()) {
            EnsureDiscovery();
            RefreshKnownDevices();
        }
    }

    std::string DeviceIdForProxy(GDBusProxy* proxy) const {
        const char* id = static_cast<const char*>(g_object_get_data(G_OBJECT(proxy), "voicestick-device"));
        return id ? id : "";
    }

    std::function<void(FirmwareUpdateProgress)> ota_progress;
    std::function<void(bool, std::string)> ota_done;
    std::uint32_t ota_written = 0;

    void HandleOtaState(const FirmwareOtaStateEvent& event) {
        if (event.written) ota_written = *event.written;
        if (ota_progress) {
            FirmwareUpdateProgress progress;
            progress.written_bytes = static_cast<int>(ota_written);
            progress.is_device_confirmed = event.written.has_value();
            ota_progress(progress);
        }
        if (event.event == "done" || event.event == "end") {
            if (ota_done) ota_done(true, {});
            ota_done = {};
        } else if (event.event == "error" || event.event == "abort") {
            if (ota_done) ota_done(false, event.code.empty() ? "OTA failed" : event.code);
            ota_done = {};
        }
    }

    void RefreshKnownDevices() {
        GError* error = nullptr;
        auto* result = g_dbus_connection_call_sync(
            connection, kBluez, "/", "org.freedesktop.DBus.ObjectManager",
            "GetManagedObjects", nullptr, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
            G_DBUS_CALL_FLAGS_NONE, 8000, nullptr, &error);
        if (!result) {
            if (error) g_error_free(error);
            return;
        }
        GVariant* objects = nullptr;
        g_variant_get(result, "(@a{oa{sa{sv}}})", &objects);
        GVariantIter iter;
        g_variant_iter_init(&iter, objects);
        const gchar* path = nullptr;
        GVariant* ifaces = nullptr;
        while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
            if (g_variant_lookup(ifaces, kDeviceIface, "@a{sv}", nullptr)) {
                HandleDevice(path);
            }
            g_variant_unref(ifaces);
        }
        g_variant_unref(objects);
        g_variant_unref(result);
    }
};

BleCentralBluez::BleCentralBluez(std::vector<std::string> paired_device_ids)
    : impl_(std::make_unique<Impl>()) {
    impl_->owner = this;
    impl_->paired_ids.insert(paired_device_ids.begin(), paired_device_ids.end());
}

BleCentralBluez::~BleCentralBluez() {
    Shutdown();
}

void BleCentralBluez::Start() {
    GError* error = nullptr;
    impl_->connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (!impl_->connection) {
        LogBle(error ? error->message : "No system bus");
        if (error) g_error_free(error);
        if (on_scan_error) on_scan_error("Bluetooth is not available");
        return;
    }

    auto* result = g_dbus_connection_call_sync(
        impl_->connection, kBluez, "/", "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects", nullptr, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE, 8000, nullptr, &error);
    if (!result) {
        LogBle(error ? error->message : "BlueZ object manager failed");
        if (error) g_error_free(error);
        if (on_scan_error) on_scan_error("Turn on Bluetooth");
        return;
    }
    GVariant* objects = nullptr;
    g_variant_get(result, "(@a{oa{sa{sv}}})", &objects);
    GVariantIter iter;
    g_variant_iter_init(&iter, objects);
    const gchar* path = nullptr;
    GVariant* ifaces = nullptr;
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        if (!impl_->adapter && g_variant_lookup(ifaces, kAdapterIface, "@a{sv}", nullptr)) {
            impl_->adapter_path = path;
            impl_->adapter = MakeProxy(impl_->connection, path, kAdapterIface);
        }
        g_variant_unref(ifaces);
    }
    g_variant_unref(objects);
    g_variant_unref(result);

    if (!impl_->adapter) {
        if (on_scan_error) on_scan_error("Turn on Bluetooth");
        return;
    }
    impl_->object_signal = g_dbus_connection_signal_subscribe(
        impl_->connection, kBluez, "org.freedesktop.DBus.ObjectManager",
        "InterfacesAdded", "/", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
           GVariant* parameters, gpointer data) {
            auto* impl = static_cast<Impl*>(data);
            const gchar* path = nullptr;
            g_variant_get(parameters, "(&oa{sa{sv}})", &path, nullptr);
            if (path) impl->HandleDevice(path);
        },
        impl_.get(), nullptr);

    impl_->removed_signal = g_dbus_connection_signal_subscribe(
        impl_->connection, kBluez, "org.freedesktop.DBus.ObjectManager",
        "InterfacesRemoved", "/", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
           GVariant* parameters, gpointer data) {
            auto* impl = static_cast<Impl*>(data);
            const gchar* path = nullptr;
            GVariant* ifaces = nullptr;
            g_variant_get(parameters, "(&o@as)", &path, &ifaces);
            if (ifaces) {
                GVariantIter iter;
                g_variant_iter_init(&iter, ifaces);
                const gchar* iface = nullptr;
                while (g_variant_iter_next(&iter, "&s", &iface)) {
                    if (g_strcmp0(iface, kDeviceIface) == 0 && path) {
                        impl->DropSessionByPath(path, "device removed");
                    }
                }
                g_variant_unref(ifaces);
            }
        },
        impl_.get(), nullptr);

    impl_->props_signal = g_dbus_connection_signal_subscribe(
        impl_->connection, kBluez, "org.freedesktop.DBus.Properties",
        "PropertiesChanged", nullptr, kDeviceIface, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar* object_path, const gchar*, const gchar*,
           GVariant* parameters, gpointer data) {
            auto* impl = static_cast<Impl*>(data);
            if (!object_path) return;
            GVariant* changed = nullptr;
            g_variant_get(parameters, "(&s@a{sv}@as)", nullptr, &changed, nullptr);
            if (changed) {
                gboolean connected = FALSE;
                if (g_variant_lookup(changed, "Connected", "b", &connected) && !connected) {
                    impl->DropSessionByPath(object_path, "bluez disconnected");
                }
                g_variant_unref(changed);
            }
            impl->HandleDevice(object_path);
        },
        impl_.get(), nullptr);

    impl_->RefreshKnownDevices();
    impl_->EnsureDiscovery();
    impl_->watch_timeout = g_timeout_add_seconds(2, [](gpointer data) -> gboolean {
        auto* impl = static_cast<Impl*>(data);
        if (!impl->alive) return G_SOURCE_REMOVE;
        impl->WatchConnections();
        return G_SOURCE_CONTINUE;
    }, impl_.get());

    GError* login_error = nullptr;
    auto* login = g_dbus_proxy_new_sync(impl_->connection, G_DBUS_PROXY_FLAGS_NONE, nullptr,
                                        "org.freedesktop.login1", "/org/freedesktop/login1",
                                        "org.freedesktop.login1.Manager", nullptr, &login_error);
    if (login) {
        impl_->sleep_signal = g_signal_connect(login, "g-signal",
            reinterpret_cast<GCallback>(+[](GDBusProxy*, const gchar*, const gchar* signal, GVariant* params, gpointer data) {
                if (g_strcmp0(signal, "PrepareForSleep") != 0) return;
                gboolean sleeping = FALSE;
                g_variant_get(params, "(b)", &sleeping);
                auto* impl = static_cast<Impl*>(data);
                if (sleeping) {
                    std::vector<std::string> ids;
                    {
                        std::lock_guard lock(impl->mutex);
                        for (const auto& [id, _] : impl->sessions) ids.push_back(id);
                    }
                    for (const auto& id : ids) impl->DropSession(id, "system sleep");
                } else {
                    impl->EnsureDiscovery();
                    impl->RefreshKnownDevices();
                }
            }), impl_.get());
        g_object_set_data_full(G_OBJECT(impl_->adapter), "login1", login, g_object_unref);
    } else if (login_error) {
        g_error_free(login_error);
    }
}

void BleCentralBluez::UpdatePairedDeviceIds(const std::vector<std::string>& ids) {
    std::lock_guard lock(impl_->mutex);
    impl_->paired_ids = std::set<std::string>(ids.begin(), ids.end());
}

void BleCentralBluez::ConnectPairedDevice(const std::string& device_id,
                                          std::uint64_t bluetooth_address,
                                          BluetoothAddressKind,
                                          const std::string&) {
    std::string path;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->cancelled.erase(device_id);
        impl_->paired_ids.insert(device_id);
    }
    if (bluetooth_address != 0) {
        impl_->RefreshKnownDevices();
        {
            std::lock_guard lock(impl_->mutex);
            for (const auto& [id, device] : impl_->scanned) {
                if (id == device_id || device.bluetooth_address == bluetooth_address) {
                    path = device.object_path;
                    break;
                }
            }
        }
        if (!path.empty()) {
            impl_->ConnectPath(device_id, path);
            return;
        }
    }
    impl_->EnsureDiscovery();
}

void BleCentralBluez::SetDisplay(const std::string& scene,
                                 const std::string& text,
                                 const std::optional<std::string>& device_id) {
    const auto trimmed = TruncateUiText(text);
    std::vector<std::string> targets;
    {
        std::lock_guard lock(impl_->mutex);
        if (device_id) {
            auto it = impl_->proto_states.find(*device_id);
            if (it != impl_->proto_states.end() && it->second.initialized &&
                it->second.info && it->second.info->has_display) {
                targets.push_back(*device_id);
            }
        } else {
            for (const auto& [id, state] : impl_->proto_states) {
                if (state.initialized && state.info && state.info->has_display) {
                    targets.push_back(id);
                }
            }
        }
    }
    for (const auto& id : targets) {
        {
            std::lock_guard lock(impl_->mutex);
            auto it = impl_->proto_states.find(id);
            if (it == impl_->proto_states.end()) continue;
            if (it->second.last_display_scene == scene && it->second.last_display_text == trimmed) {
                continue;
            }
            it->second.last_display_scene = scene;
            it->second.last_display_text = trimmed;
        }
        impl_->SendRpc(id, [scene, trimmed](std::uint32_t rpc_id) {
            return BleProtocol::RpcDisplaySet(rpc_id, scene, trimmed);
        }, [impl = impl_.get(), id, scene, trimmed](std::optional<ControlRpcResponse> resp) {
            if (!impl->alive) return;
            if (!resp || resp->is_error) {
                std::lock_guard lock(impl->mutex);
                auto it = impl->proto_states.find(id);
                if (it != impl->proto_states.end() &&
                    it->second.last_display_scene == scene &&
                    it->second.last_display_text == trimmed) {
                    it->second.last_display_scene.reset();
                    it->second.last_display_text.reset();
                }
            }
        });
    }
}

void BleCentralBluez::StartAudioIn(const std::string& device_id, InteractionMode mode) {
    std::uint16_t previous_stream_id = 0;
    {
        std::lock_guard lock(impl_->mutex);
        auto it = impl_->proto_states.find(device_id);
        if (it == impl_->proto_states.end() || !it->second.initialized) return;
        previous_stream_id = it->second.audio_in_stream_id;
        it->second.audio_in_stream_id = 0;
    }
    const std::string trigger = mode == InteractionMode::kHoldToTalk ? "button_hold" : "button_toggle";
    auto start_new = [this, device_id, trigger] {
        impl_->SendRpc(device_id, [trigger](std::uint32_t rpc_id) {
            return BleProtocol::RpcStreamStartAudioIn(rpc_id, trigger);
        }, [this, device_id](std::optional<ControlRpcResponse> resp) {
            if (!impl_->alive || !resp || resp->is_error || !resp->stream_id) {
                LogBle("stream.start(audio_in) failed for " + device_id);
                return;
            }
            std::lock_guard lock(impl_->mutex);
            auto it = impl_->proto_states.find(device_id);
            if (it != impl_->proto_states.end()) {
                it->second.audio_in_stream_id = *resp->stream_id;
            }
        });
    };
    if (previous_stream_id != 0) {
        impl_->SendRpc(device_id, [previous_stream_id](std::uint32_t rpc_id) {
            return BleProtocol::RpcStreamStop(rpc_id, previous_stream_id);
        }, [start_new = std::move(start_new)](std::optional<ControlRpcResponse>) {
            start_new();
        });
    } else {
        start_new();
    }
}

void BleCentralBluez::StopAudioIn(const std::string& device_id) {
    std::uint16_t stream_id = 0;
    {
        std::lock_guard lock(impl_->mutex);
        auto it = impl_->proto_states.find(device_id);
        if (it == impl_->proto_states.end() || !it->second.initialized) return;
        stream_id = it->second.audio_in_stream_id;
        it->second.audio_in_stream_id = 0;
    }
    if (stream_id != 0) {
        impl_->SendRpc(device_id, [stream_id](std::uint32_t rpc_id) {
            return BleProtocol::RpcStreamStop(rpc_id, stream_id);
        });
    }
}

std::vector<std::string> BleCentralBluez::ConnectedReadyDeviceIds() const {
    std::vector<std::string> ids;
    std::lock_guard lock(impl_->mutex);
    for (const auto& [id, state] : impl_->proto_states) {
        if (state.initialized) ids.push_back(id);
    }
    return ids;
}

void BleCentralBluez::UpdateFirmware(ByteVector image,
                                     const std::string& device_id,
                                     std::function<void(FirmwareUpdateProgress)> progress,
                                     std::function<void(bool, std::string)> completion) {
    std::string ota_path;
    {
        std::lock_guard lock(impl_->mutex);
        auto it = impl_->ota_rx_paths.find(device_id);
        if (it == impl_->ota_rx_paths.end() || it->second.empty()) {
            completion(false, "Device does not support firmware update");
            return;
        }
        ota_path = it->second;
        impl_->firmware_cancel = false;
        impl_->ota_progress = std::move(progress);
        impl_->ota_done = std::move(completion);
        impl_->ota_written = 0;
    }
    std::thread([this, image = std::move(image), ota_path, device_id]() {
        const std::uint32_t transfer_id = 1;
        WriteCharacteristic(impl_->connection, ota_path,
                            BleProtocol::OtaBeginPayload(static_cast<std::uint32_t>(image.size()), transfer_id));
        constexpr std::size_t kChunk = 160;
        for (std::size_t offset = 0; offset < image.size(); offset += kChunk) {
            if (impl_->firmware_cancel) {
                WriteCharacteristic(impl_->connection, ota_path, BleProtocol::OtaAbortPayload(transfer_id));
                RunOnUiThread([this] {
                    if (impl_->ota_done) impl_->ota_done(false, "Cancelled");
                    impl_->ota_done = {};
                });
                return;
            }
            const auto end = std::min(offset + kChunk, image.size());
            WriteCharacteristic(impl_->connection, ota_path,
                                BleProtocol::OtaDataPayload(transfer_id, static_cast<std::uint32_t>(offset),
                                                            std::span(image.data() + offset, end - offset)));
            if (impl_->ota_progress) {
                FirmwareUpdateProgress progress;
                progress.written_bytes = static_cast<int>(end);
                progress.total_bytes = static_cast<int>(image.size());
                impl_->ota_progress(progress);
            }
        }
        WriteCharacteristic(impl_->connection, ota_path,
                            BleProtocol::OtaEndPayload(transfer_id, static_cast<std::uint32_t>(image.size())));
        (void)device_id;
    }).detach();
}

void BleCentralBluez::CancelFirmwareUpdate() {
    impl_->firmware_cancel = true;
}

bool BleCentralBluez::IsConnected(const std::string& device_id) const {
    std::lock_guard lock(impl_->mutex);
    return impl_->sessions.contains(device_id);
}

void BleCentralBluez::CancelPendingConnect(const std::string& device_id) {
    std::lock_guard lock(impl_->mutex);
    impl_->cancelled.insert(device_id);
}

void BleCentralBluez::Shutdown() {
    impl_->alive = false;
    if (impl_->watch_timeout) {
        g_source_remove(impl_->watch_timeout);
        impl_->watch_timeout = 0;
    }
    if (impl_->audio_idle) {
        g_source_remove(impl_->audio_idle);
        impl_->audio_idle = 0;
    }
    impl_->audio_queue.clear();
    if (!impl_->connection) return;
    if (impl_->discovering && impl_->adapter) {
        Call(impl_->adapter, "StopDiscovery");
        impl_->discovering = false;
    }
    if (impl_->object_signal) {
        g_dbus_connection_signal_unsubscribe(impl_->connection, impl_->object_signal);
        impl_->object_signal = 0;
    }
    if (impl_->removed_signal) {
        g_dbus_connection_signal_unsubscribe(impl_->connection, impl_->removed_signal);
        impl_->removed_signal = 0;
    }
    if (impl_->props_signal) {
        g_dbus_connection_signal_unsubscribe(impl_->connection, impl_->props_signal);
        impl_->props_signal = 0;
    }
    if (impl_->adapter) {
        g_object_unref(impl_->adapter);
        impl_->adapter = nullptr;
    }
    g_object_unref(impl_->connection);
    impl_->connection = nullptr;
}

void BleCentralBluez::StartPairingScan() {
    impl_->pairing_scan = true;
    impl_->EnsureDiscovery();
    impl_->RefreshKnownDevices();
    impl_->PublishScan();
}

void BleCentralBluez::StopPairingScan() {
    impl_->pairing_scan = false;
    impl_->MaybeStopDiscovery();
}

std::vector<ScannedBleDevice> BleCentralBluez::ScannedDevices() const {
    std::lock_guard lock(impl_->mutex);
    std::vector<ScannedBleDevice> devices;
    for (const auto& [_, device] : impl_->scanned) devices.push_back(device);
    return devices;
}

} // namespace voicestick
