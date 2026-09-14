#include "status_item_sni.h"

#include "log.h"
#include "translation_targets.h"

#include <gio/gio.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <mutex>
#include <utility>
#include <vector>

namespace voicestick {

namespace {

constexpr const char* kWatcherName = "org.kde.StatusNotifierWatcher";
constexpr const char* kWatcherPath = "/StatusNotifierWatcher";
constexpr const char* kWatcherIface = "org.kde.StatusNotifierWatcher";
constexpr const char* kItemIface = "org.kde.StatusNotifierItem";
constexpr const char* kMenuIface = "com.canonical.dbusmenu";

std::string IconForStatus(const std::string& status) {
    std::string lower = status;
    for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lower.find("pair") != std::string::npos) return "bluetooth-disconnected-symbolic";
    if (lower.find("listen") != std::string::npos) return "audio-input-microphone-symbolic";
    if (lower.find("error") != std::string::npos || lower.find("fail") != std::string::npos) {
        return "dialog-error-symbolic";
    }
    if (lower.find("process") != std::string::npos || lower.find("final") != std::string::npos) {
        return "content-loading-symbolic";
    }
    return "audio-input-microphone";
}

} // namespace

struct StatusItemSni::Impl {
    struct MenuEntry {
        int id = 0;
        std::string label;
        bool enabled = true;
        bool visible = true;
        bool separator = false;
        std::string action;
        std::string payload;
        std::string toggle_type;
        int toggle_state = -1;
        std::vector<int> children;
    };

    GDBusConnection* connection = nullptr;
    guint item_reg = 0;
    guint menu_reg = 0;
    guint name_owner = 0;
    std::string bus_name;
    std::string status = "Ready";
    std::string icon_name = "audio-input-microphone";
    bool has_recoverable = false;
    InteractionMode interaction_mode = InteractionMode::kHoldToTalk;
    bool auto_enter = true;
    OutputTarget output_target = OutputTarget::kFocusedApp;
    int confirmation_countdown_ms = 1000;
    std::vector<DeviceRow> devices;
    std::map<int, MenuEntry> entries;
    int next_id = 1;
    guint revision = 1;
    std::mutex mutex;
    StatusItemSni* owner = nullptr;

    void AddProperties(GVariantBuilder* props, const MenuEntry& entry,
                       const std::vector<std::string>& names) const {
        auto want = [&](const char* name) {
            return names.empty() ||
                   std::find(names.begin(), names.end(), name) != names.end();
        };
        if (entry.separator) {
            if (want("type")) {
                g_variant_builder_add(props, "{sv}", "type", g_variant_new_string("separator"));
            }
            return;
        }
        if (want("label")) {
            g_variant_builder_add(props, "{sv}", "label", g_variant_new_string(entry.label.c_str()));
        }
        if (want("enabled")) {
            g_variant_builder_add(props, "{sv}", "enabled", g_variant_new_boolean(entry.enabled));
        }
        if (want("visible")) {
            g_variant_builder_add(props, "{sv}", "visible", g_variant_new_boolean(entry.visible));
        }
        if (!entry.children.empty() && want("children-display")) {
            g_variant_builder_add(props, "{sv}", "children-display",
                                  g_variant_new_string("submenu"));
        }
        if (!entry.toggle_type.empty() && want("toggle-type")) {
            g_variant_builder_add(props, "{sv}", "toggle-type",
                                  g_variant_new_string(entry.toggle_type.c_str()));
        }
        if (entry.toggle_state >= 0 && want("toggle-state")) {
            g_variant_builder_add(props, "{sv}", "toggle-state",
                                  g_variant_new_int32(entry.toggle_state));
        }
    }

    void RebuildMenu() {
        entries.clear();
        next_id = 1;
        // DBusMenu root id must be 0. GNOME AppIndicator attaches the popup to
        // item 0 and garbage-collects every other id if that node has no children.
        entries[0] = {.id = 0, .label = "root"};
        auto add = [&](MenuEntry entry) {
            entry.id = next_id++;
            entries[entry.id] = entry;
            return entry.id;
        };

        auto add_child = [&](int parent, MenuEntry entry) {
            const int id = add(std::move(entry));
            entries[parent].children.push_back(id);
            return id;
        };

        if (has_recoverable) {
            add_child(0, {.label = "Restore Last Input", .action = "restore"});
            add_child(0, {.separator = true});
        }

        std::vector<std::string> seen_ids;
        for (const auto& device : devices) {
            if (std::find(seen_ids.begin(), seen_ids.end(), device.id) != seen_ids.end()) continue;
            seen_ids.push_back(device.id);
            const int device_menu = add_child(0, {
                .label = device.name.empty() ? ("VS-" + device.id) : device.name,
            });
            add_child(device_menu, {
                .label = device.connected ? "Connected" : "Scanning",
                .enabled = false,
            });
            add_child(device_menu, {.separator = true});

            const int theme_menu = add_child(device_menu, {.label = "Theme Color"});
            static constexpr OverlayThemeColor kColors[] = {
                OverlayThemeColor::kWhite, OverlayThemeColor::kPink, OverlayThemeColor::kGreen,
                OverlayThemeColor::kYellow, OverlayThemeColor::kBlue, OverlayThemeColor::kPurple,
            };
            for (auto color : kColors) {
                add_child(theme_menu, {
                    .label = OverlayThemeColorDisplayName(color),
                    .action = "theme",
                    .payload = device.id + ":" + OverlayThemeColorName(color),
                    .toggle_type = "radio",
                    .toggle_state = device.theme == color ? 1 : 0,
                });
            }

            const int position_menu = add_child(device_menu, {.label = "Overlay Position"});
            static constexpr OverlayPosition kPositions[] = {
                OverlayPosition::kCenter, OverlayPosition::kTopLeft, OverlayPosition::kTopRight,
                OverlayPosition::kBottomLeft, OverlayPosition::kBottomRight,
            };
            for (auto position : kPositions) {
                add_child(position_menu, {
                    .label = OverlayPositionDisplayName(position),
                    .action = "position",
                    .payload = device.id + ":" + OverlayPositionName(position),
                    .toggle_type = "radio",
                    .toggle_state = device.position == position ? 1 : 0,
                });
            }

            const int translation_menu = add_child(device_menu, {.label = "Translation"});
            add_child(translation_menu, {
                .label = "Original",
                .action = "translation",
                .payload = device.id + ":original",
                .toggle_type = "radio",
                .toggle_state = device.output.transform == TextTransform::kOriginal ? 1 : 0,
            });
            add_child(translation_menu, {.separator = true});
            for (const auto& target : kTranslationTargets) {
                add_child(translation_menu, {
                    .label = "Translate to " + std::string(target.name),
                    .action = "translation",
                    .payload = device.id + ":" + std::string(target.code),
                    .toggle_type = "radio",
                    .toggle_state = device.output.transform == TextTransform::kTranslate &&
                                            device.output.translation_target == target.code
                                        ? 1
                                        : 0,
                });
            }

            add_child(device_menu, {.separator = true});
            add_child(device_menu, {
                .label = device.firmware.current_version.empty()
                             ? "Firmware Unknown"
                             : "Firmware " + device.firmware.current_version,
                .enabled = false,
            });
            if (device.firmware.is_checking) {
                add_child(device_menu, {.label = "Checking for Updates", .enabled = false});
            } else if (!device.firmware.error_message.empty()) {
                add_child(device_menu, {.label = "Update Check Failed", .enabled = false});
            } else if (device.firmware.update_available && !device.firmware.latest_version.empty()) {
                add_child(device_menu, {
                    .label = "Update to " + device.firmware.latest_version + "…",
                    .enabled = device.connected,
                    .action = "update",
                    .payload = device.id,
                });
            } else if (!device.firmware.latest_version.empty() &&
                       !device.firmware.current_version.empty()) {
                add_child(device_menu, {.label = "Firmware Up to Date", .enabled = false});
            }
            add_child(device_menu, {
                .label = "Forget This Device",
                .action = "forget",
                .payload = device.id,
            });
        }
        if (!seen_ids.empty()) add_child(0, {.separator = true});

        const int output_menu = add_child(0, {.label = "Output"});
        add_child(output_menu, {
            .label = "Focused App",
            .action = "output",
            .payload = "focused_app",
            .toggle_type = "radio",
            .toggle_state = output_target == OutputTarget::kFocusedApp ? 1 : 0,
        });
        add_child(output_menu, {
            .label = "Subtitle",
            .action = "output",
            .payload = "subtitle",
            .toggle_type = "radio",
            .toggle_state = output_target == OutputTarget::kSubtitle ? 1 : 0,
        });
        add_child(0, {
            .label = "Press Return After Paste",
            .action = "auto_enter",
            .toggle_type = "checkmark",
            .toggle_state = auto_enter ? 1 : 0,
        });
        const int interaction_menu = add_child(0, {.label = "Interaction"});
        add_child(interaction_menu, {
            .label = "Hold to Talk",
            .action = "interaction",
            .payload = "hold_to_talk",
            .toggle_type = "radio",
            .toggle_state = interaction_mode == InteractionMode::kHoldToTalk ? 1 : 0,
        });
        add_child(interaction_menu, {
            .label = "Click to Talk",
            .action = "interaction",
            .payload = "click_to_talk",
            .toggle_type = "radio",
            .toggle_state = interaction_mode == InteractionMode::kClickToTalk ? 1 : 0,
        });
        const int countdown_menu = add_child(0, {.label = "Countdown"});
        static const struct {
            const char* label;
            const char* payload;
            int ms;
        } countdown_options[] = {
            {"Off", "0", 0},
            {"0.5 s", "500", 500},
            {"1 s", "1000", 1000},
            {"2 s", "2000", 2000},
            {"3 s", "3000", 3000},
        };
        bool matched_countdown = false;
        for (const auto& option : countdown_options) {
            if (option.ms == confirmation_countdown_ms) matched_countdown = true;
        }
        for (const auto& option : countdown_options) {
            add_child(countdown_menu, {
                .label = option.label,
                .action = "countdown",
                .payload = option.payload,
                .toggle_type = "radio",
                .toggle_state = option.ms == confirmation_countdown_ms ? 1 : 0,
            });
        }
        if (!matched_countdown) {
            add_child(countdown_menu, {
                .label = std::to_string(confirmation_countdown_ms) + " ms",
                .action = "countdown",
                .payload = std::to_string(confirmation_countdown_ms),
                .toggle_type = "radio",
                .toggle_state = 1,
            });
        }
        add_child(0, {.separator = true});
        add_child(0, {.label = "Pair Device…", .action = "pair"});
        add_child(0, {.label = "Settings…", .action = "preferences"});
        add_child(0, {.separator = true});
        add_child(0, {.label = "Website", .action = "website"});
        add_child(0, {.label = "Quit", .action = "quit"});
        ++revision;
        if (connection) {
            g_dbus_connection_emit_signal(connection, nullptr, "/MenuBar", kMenuIface,
                                          "LayoutUpdated",
                                          g_variant_new("(ui)", revision, 0), nullptr);
        }
    }

    GVariant* LayoutFor(int id, int depth) {
        auto it = entries.find(id);
        if (it == entries.end()) {
            return g_variant_new("(ia{sv}av)", 0, nullptr, nullptr);
        }
        const auto& entry = it->second;
        GVariantBuilder props;
        g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
        AddProperties(&props, entry, {});
        GVariantBuilder kids;
        g_variant_builder_init(&kids, G_VARIANT_TYPE("av"));
        if (depth != 0) {
            for (int child : entry.children) {
                g_variant_builder_add(&kids, "v", LayoutFor(child, depth > 0 ? depth - 1 : -1));
            }
        }
        return g_variant_new("(ia{sv}av)", entry.id, &props, &kids);
    }

    void HandleAction(const std::string& action, const std::string& payload) {
        if (!owner) return;
        if (action == "restore" && owner->on_restore) owner->on_restore();
        else if (action == "pair" && owner->on_pair) owner->on_pair();
        else if (action == "preferences" && owner->on_preferences) owner->on_preferences();
        else if (action == "website" && owner->on_website) owner->on_website();
        else if (action == "quit" && owner->on_quit) owner->on_quit();
        else if (action == "forget" && owner->on_forget) owner->on_forget(payload);
        else if (action == "update" && owner->on_update_firmware) owner->on_update_firmware(payload);
        else if (action == "output" && owner->on_output_target) {
            owner->on_output_target(OutputTargetFromName(payload));
        } else if (action == "auto_enter" && owner->on_auto_enter) {
            owner->on_auto_enter(!auto_enter);
        } else if (action == "interaction" && owner->on_interaction) {
            owner->on_interaction(InteractionModeFromName(payload));
        } else if (action == "countdown" && owner->on_countdown) {
            try {
                owner->on_countdown(std::stoi(payload));
            } catch (...) {
            }
        } else if (action == "theme" && owner->on_theme) {
            const auto colon = payload.find(':');
            if (colon != std::string::npos) {
                owner->on_theme(payload.substr(0, colon),
                                OverlayThemeColorFromName(payload.substr(colon + 1)));
            }
        } else if (action == "position" && owner->on_position) {
            const auto colon = payload.find(':');
            if (colon != std::string::npos) {
                owner->on_position(payload.substr(0, colon),
                                   OverlayPositionFromName(payload.substr(colon + 1)));
            }
        } else if (action == "translation" && owner->on_translation) {
            const auto colon = payload.find(':');
            if (colon == std::string::npos) return;
            const auto device_id = payload.substr(0, colon);
            const auto value = payload.substr(colon + 1);
            if (value == "original") {
                owner->on_translation(device_id, TextTransform::kOriginal, {});
            } else {
                owner->on_translation(device_id, TextTransform::kTranslate, value);
            }
        }
    }
};

namespace {

void ItemMethod(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method,
                GVariant*, GDBusMethodInvocation* invocation, gpointer user_data) {
    auto* impl = static_cast<StatusItemSni::Impl*>(user_data);
    if (g_strcmp0(method, "ContextMenu") == 0 ||
        g_strcmp0(method, "SecondaryActivate") == 0 || g_strcmp0(method, "Scroll") == 0) {
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                          "Unknown method %s", method);
    (void)impl;
}

GVariant* ItemGetProperty(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                          const gchar* property, GError**, gpointer user_data) {
    auto* impl = static_cast<StatusItemSni::Impl*>(user_data);
    std::lock_guard lock(impl->mutex);
    if (g_strcmp0(property, "Category") == 0) return g_variant_new_string("ApplicationStatus");
    if (g_strcmp0(property, "Id") == 0) return g_variant_new_string("voicestick");
    if (g_strcmp0(property, "Title") == 0) return g_variant_new_string("VoiceStick");
    if (g_strcmp0(property, "Status") == 0) return g_variant_new_string("Active");
    if (g_strcmp0(property, "WindowId") == 0) return g_variant_new_int32(0);
    if (g_strcmp0(property, "IconName") == 0) return g_variant_new_string(impl->icon_name.c_str());
    if (g_strcmp0(property, "OverlayIconName") == 0) return g_variant_new_string("");
    if (g_strcmp0(property, "AttentionIconName") == 0) return g_variant_new_string("");
    if (g_strcmp0(property, "AttentionMovieName") == 0) return g_variant_new_string("");
    if (g_strcmp0(property, "ItemIsMenu") == 0) return g_variant_new_boolean(TRUE);
    if (g_strcmp0(property, "Menu") == 0) return g_variant_new_object_path("/MenuBar");
    if (g_strcmp0(property, "IconThemePath") == 0) return g_variant_new_string("");
    if (g_strcmp0(property, "ToolTip") == 0) {
        return g_variant_new("(sa(iiay)ss)", "", nullptr, "VoiceStick", impl->status.c_str());
    }
    return nullptr;
}

void MenuMethod(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method,
                GVariant* parameters, GDBusMethodInvocation* invocation, gpointer user_data) {
    auto* impl = static_cast<StatusItemSni::Impl*>(user_data);
    std::unique_lock lock(impl->mutex);
    if (g_strcmp0(method, "GetLayout") == 0) {
        gint parent = 0;
        gint depth = -1;
        g_variant_get(parameters, "(iias)", &parent, &depth, nullptr);
        GVariant* layout = impl->LayoutFor(parent, depth);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u@(ia{sv}av))", impl->revision, layout));
        return;
    }
    if (g_strcmp0(method, "GetGroupProperties") == 0) {
        GVariantIter* id_iter = nullptr;
        GVariantIter* name_iter = nullptr;
        g_variant_get(parameters, "(aias)", &id_iter, &name_iter);
        std::vector<int> ids;
        gint32 id = 0;
        while (g_variant_iter_next(id_iter, "i", &id)) ids.push_back(id);
        std::vector<std::string> names;
        const gchar* name = nullptr;
        while (g_variant_iter_next(name_iter, "&s", &name)) {
            if (name) names.emplace_back(name);
        }
        g_variant_iter_free(id_iter);
        g_variant_iter_free(name_iter);
        GVariantBuilder result;
        g_variant_builder_init(&result, G_VARIANT_TYPE("a(ia{sv})"));
        auto append = [&](const StatusItemSni::Impl::MenuEntry& entry) {
            GVariantBuilder props;
            g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
            impl->AddProperties(&props, entry, names);
            g_variant_builder_add(&result, "(ia{sv})", entry.id, &props);
        };
        if (ids.empty()) {
            for (const auto& [_, entry] : impl->entries) append(entry);
        } else {
            for (int wanted : ids) {
                auto it = impl->entries.find(wanted);
                if (it != impl->entries.end()) append(it->second);
            }
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(ia{sv}))", &result));
        return;
    }
    if (g_strcmp0(method, "GetProperty") == 0) {
        gint32 id = 0;
        const gchar* name = nullptr;
        g_variant_get(parameters, "(is)", &id, &name);
        auto it = impl->entries.find(id);
        GVariantBuilder props;
        g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
        if (it != impl->entries.end()) {
            impl->AddProperties(&props, it->second, name ? std::vector<std::string>{name} : std::vector<std::string>{});
        }
        GVariant* dict = g_variant_builder_end(&props);
        GVariant* value = nullptr;
        if (name) g_variant_lookup(dict, name, "*", &value);
        g_variant_unref(dict);
        if (!value) value = g_variant_new_string("");
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
        return;
    }
    if (g_strcmp0(method, "Event") == 0) {
        gint id = 0;
        const gchar* event_id = nullptr;
        g_variant_get(parameters, "(isvu)", &id, &event_id, nullptr, nullptr);
        if (event_id && g_strcmp0(event_id, "clicked") == 0) {
            auto it = impl->entries.find(id);
            if (it != impl->entries.end() && !it->second.action.empty()) {
                auto action = it->second.action;
                auto payload = it->second.payload;
                lock.unlock();
                impl->HandleAction(action, payload);
                g_dbus_method_invocation_return_value(invocation, nullptr);
                return;
            }
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }
    if (g_strcmp0(method, "EventGroup") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(ai)", nullptr));
        return;
    }
    if (g_strcmp0(method, "AboutToShow") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
        return;
    }
    if (g_strcmp0(method, "AboutToShowGroup") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(aiai)", nullptr, nullptr));
        return;
    }
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                          "Unknown method %s", method);
}

GVariant* MenuGetProperty(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                          const gchar* property, GError**, gpointer) {
    if (g_strcmp0(property, "Version") == 0) return g_variant_new_uint32(3);
    if (g_strcmp0(property, "TextDirection") == 0) return g_variant_new_string("ltr");
    if (g_strcmp0(property, "Status") == 0) return g_variant_new_string("normal");
    if (g_strcmp0(property, "IconThemePath") == 0) {
        return g_variant_new_strv(nullptr, 0);
    }
    return nullptr;
}

const GDBusInterfaceVTable kItemVtable{ItemMethod, ItemGetProperty, nullptr, {}};
const GDBusInterfaceVTable kMenuVtable{MenuMethod, MenuGetProperty, nullptr, {}};

// Omit Activate so GNOME AppIndicator does not wait for a double-click
// before showing the menu (see indicatorStatusIcon.js supportsActivation).
const gchar kItemXml[] =
    "<node>"
    "  <interface name='org.kde.StatusNotifierItem'>"
    "    <method name='ContextMenu'><arg type='i' name='x' direction='in'/>"
    "      <arg type='i' name='y' direction='in'/></method>"
    "    <method name='SecondaryActivate'><arg type='i' name='x' direction='in'/>"
    "      <arg type='i' name='y' direction='in'/></method>"
    "    <method name='Scroll'><arg type='i' name='delta' direction='in'/>"
    "      <arg type='s' name='orientation' direction='in'/></method>"
    "    <property name='Category' type='s' access='read'/>"
    "    <property name='Id' type='s' access='read'/>"
    "    <property name='Title' type='s' access='read'/>"
    "    <property name='Status' type='s' access='read'/>"
    "    <property name='WindowId' type='i' access='read'/>"
    "    <property name='IconName' type='s' access='read'/>"
    "    <property name='OverlayIconName' type='s' access='read'/>"
    "    <property name='AttentionIconName' type='s' access='read'/>"
    "    <property name='AttentionMovieName' type='s' access='read'/>"
    "    <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "    <property name='ItemIsMenu' type='b' access='read'/>"
    "    <property name='Menu' type='o' access='read'/>"
    "    <property name='IconThemePath' type='s' access='read'/>"
    "  </interface>"
    "</node>";

const gchar kMenuXml[] =
    "<node>"
    "  <interface name='com.canonical.dbusmenu'>"
    "    <method name='GetLayout'>"
    "      <arg type='i' name='parentId' direction='in'/>"
    "      <arg type='i' name='recursionDepth' direction='in'/>"
    "      <arg type='as' name='propertyNames' direction='in'/>"
    "      <arg type='u' name='revision' direction='out'/>"
    "      <arg type='(ia{sv}av)' name='layout' direction='out'/>"
    "    </method>"
    "    <method name='GetGroupProperties'>"
    "      <arg type='ai' name='ids' direction='in'/>"
    "      <arg type='as' name='propertyNames' direction='in'/>"
    "      <arg type='a(ia{sv})' name='properties' direction='out'/>"
    "    </method>"
    "    <method name='GetProperty'>"
    "      <arg type='i' name='id' direction='in'/>"
    "      <arg type='s' name='name' direction='in'/>"
    "      <arg type='v' name='value' direction='out'/>"
    "    </method>"
    "    <method name='Event'>"
    "      <arg type='i' name='id' direction='in'/>"
    "      <arg type='s' name='eventId' direction='in'/>"
    "      <arg type='v' name='data' direction='in'/>"
    "      <arg type='u' name='timestamp' direction='in'/>"
    "    </method>"
    "    <method name='EventGroup'>"
    "      <arg type='a(isvu)' name='events' direction='in'/>"
    "      <arg type='ai' name='idErrors' direction='out'/>"
    "    </method>"
    "    <method name='AboutToShow'><arg type='i' name='id' direction='in'/>"
    "      <arg type='b' name='needUpdate' direction='out'/></method>"
    "    <method name='AboutToShowGroup'>"
    "      <arg type='ai' name='ids' direction='in'/>"
    "      <arg type='ai' name='updatesNeeded' direction='out'/>"
    "      <arg type='ai' name='idErrors' direction='out'/>"
    "    </method>"
    "    <signal name='ItemsPropertiesUpdated'>"
    "      <arg type='a(ia{sv})' name='updatedProps'/>"
    "      <arg type='a(ias)' name='removedProps'/>"
    "    </signal>"
    "    <signal name='LayoutUpdated'>"
    "      <arg type='u' name='revision'/><arg type='i' name='parent'/>"
    "    </signal>"
    "    <property name='Version' type='u' access='read'/>"
    "    <property name='TextDirection' type='s' access='read'/>"
    "    <property name='Status' type='s' access='read'/>"
    "    <property name='IconThemePath' type='as' access='read'/>"
    "  </interface>"
    "</node>";

} // namespace

StatusItemSni::StatusItemSni() : impl_(new Impl) {
    impl_->owner = this;
}

StatusItemSni::~StatusItemSni() {
    Shutdown();
    delete impl_;
}

bool StatusItemSni::Start() {
    GError* error = nullptr;
    impl_->connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!impl_->connection) {
        LogApp(std::string("D-Bus session failed: ") + (error ? error->message : ""));
        if (error) g_error_free(error);
        return false;
    }

    impl_->bus_name = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
    impl_->name_owner = g_bus_own_name_on_connection(
        impl_->connection, impl_->bus_name.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE,
        nullptr, nullptr, nullptr, nullptr);

    auto* item_info = g_dbus_node_info_new_for_xml(kItemXml, &error);
    if (!item_info) {
        LogApp(std::string("SNI XML error: ") + (error ? error->message : ""));
        if (error) g_error_free(error);
        return false;
    }
    impl_->item_reg = g_dbus_connection_register_object(
        impl_->connection, "/StatusNotifierItem", item_info->interfaces[0],
        &kItemVtable, impl_, nullptr, &error);
    g_dbus_node_info_unref(item_info);
    if (!impl_->item_reg) {
        LogApp(std::string("SNI register failed: ") + (error ? error->message : ""));
        if (error) g_error_free(error);
        return false;
    }

    error = nullptr;
    auto* menu_info = g_dbus_node_info_new_for_xml(kMenuXml, &error);
    if (!menu_info) {
        LogApp(std::string("DBusMenu XML error: ") + (error ? error->message : ""));
        if (error) g_error_free(error);
        return false;
    }
    impl_->menu_reg = g_dbus_connection_register_object(
        impl_->connection, "/MenuBar", menu_info->interfaces[0],
        &kMenuVtable, impl_, nullptr, &error);
    g_dbus_node_info_unref(menu_info);
    if (!impl_->menu_reg) {
        LogApp(std::string("DBusMenu register failed: ") + (error ? error->message : ""));
        if (error) g_error_free(error);
        return false;
    }

    {
        std::lock_guard lock(impl_->mutex);
        impl_->RebuildMenu();
    }

    error = nullptr;
    g_dbus_connection_call_sync(
        impl_->connection, kWatcherName, kWatcherPath, kWatcherIface,
        "RegisterStatusNotifierItem", g_variant_new("(s)", impl_->bus_name.c_str()),
        nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &error);
    if (error) {
        LogApp(std::string("StatusNotifierWatcher register failed: ") + error->message);
        g_error_free(error);
        return false;
    }
    LogApp("StatusNotifierItem registered");
    return true;
}

void StatusItemSni::Shutdown() {
    if (!impl_->connection) return;
    if (impl_->item_reg) g_dbus_connection_unregister_object(impl_->connection, impl_->item_reg);
    if (impl_->menu_reg) g_dbus_connection_unregister_object(impl_->connection, impl_->menu_reg);
    if (impl_->name_owner) g_bus_unown_name(impl_->name_owner);
    g_object_unref(impl_->connection);
    impl_->connection = nullptr;
    impl_->item_reg = 0;
    impl_->menu_reg = 0;
    impl_->name_owner = 0;
}

void StatusItemSni::SetStatus(const std::string& status) {
    std::lock_guard lock(impl_->mutex);
    impl_->status = status;
    impl_->icon_name = IconForStatus(status);
    impl_->RebuildMenu();
    if (impl_->connection) {
        g_dbus_connection_emit_signal(impl_->connection, nullptr, "/StatusNotifierItem",
                                      kItemIface, "NewIcon", nullptr, nullptr);
        g_dbus_connection_emit_signal(impl_->connection, nullptr, "/StatusNotifierItem",
                                      kItemIface, "NewToolTip", nullptr, nullptr);
    }
}

void StatusItemSni::SetHasRecoverableInput(bool value) {
    std::lock_guard lock(impl_->mutex);
    impl_->has_recoverable = value;
    impl_->RebuildMenu();
}

void StatusItemSni::SetInputOptions(InteractionMode mode,
                                    bool auto_enter,
                                    OutputTarget output,
                                    int countdown_ms) {
    std::lock_guard lock(impl_->mutex);
    impl_->interaction_mode = mode;
    impl_->auto_enter = auto_enter;
    impl_->output_target = output;
    impl_->confirmation_countdown_ms = countdown_ms;
    impl_->RebuildMenu();
}

void StatusItemSni::SetDevices(const std::vector<DeviceRow>& devices) {
    std::lock_guard lock(impl_->mutex);
    impl_->devices = devices;
    impl_->RebuildMenu();
}

} // namespace voicestick
