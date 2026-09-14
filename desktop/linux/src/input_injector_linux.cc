#include "input_injector_linux.h"

#include "app_config.h"
#include "log.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <atspi/atspi.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <gtk/gtk.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace voicestick {

namespace {

constexpr const char* kPortalName = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kRemoteDesktop = "org.freedesktop.portal.RemoteDesktop";
constexpr guint32 kDeviceKeyboard = 1;
constexpr guint32 kDevicePointer = 2;
constexpr guint32 kPersistUntilRevoked = 2;
constexpr guint kClipboardSettleMs = 120;
constexpr guint kPortalFocusMs = 180;
constexpr guint kEnterAfterPasteMs = 120;

AtspiAccessible* FindFocused(AtspiAccessible* node, int depth) {
    if (!node || depth > 16) return nullptr;
    AtspiStateSet* states = atspi_accessible_get_state_set(node);
    const bool focused = states && atspi_state_set_contains(states, ATSPI_STATE_FOCUSED);
    if (states) g_object_unref(states);
    if (focused) {
        g_object_ref(node);
        return node;
    }
    const int count = atspi_accessible_get_child_count(node, nullptr);
    for (int i = 0; i < count; ++i) {
        AtspiAccessible* child = atspi_accessible_get_child_at_index(node, i, nullptr);
        if (!child) continue;
        AtspiAccessible* found = FindFocused(child, depth + 1);
        g_object_unref(child);
        if (found) return found;
    }
    return nullptr;
}

AtspiAccessible* FindEditable(AtspiAccessible* node) {
    AtspiAccessible* current = node;
    while (current) {
        AtspiEditableText* editable = atspi_accessible_get_editable_text_iface(current);
        if (editable) {
            g_object_unref(editable);
            if (current != node) g_object_ref(current);
            return current;
        }
        AtspiAccessible* parent = atspi_accessible_get_parent(current, nullptr);
        if (current != node) g_object_unref(current);
        current = parent;
    }
    return nullptr;
}

bool InsertAt(AtspiAccessible* node, const std::string& text, bool press_enter) {
    AtspiEditableText* editable = atspi_accessible_get_editable_text_iface(node);
    AtspiText* iface = atspi_accessible_get_text_iface(node);
    bool ok = false;
    if (editable && iface) {
        gint caret = atspi_text_get_caret_offset(iface, nullptr);
        if (caret < 0) caret = 0;
        ok = atspi_editable_text_insert_text(editable, caret, text.c_str(),
                                             static_cast<gint>(text.size()), nullptr);
    }
    if (iface) g_object_unref(iface);
    if (editable) g_object_unref(editable);
    if (ok && press_enter) {
        atspi_generate_keyboard_event(XK_Return, nullptr, ATSPI_KEY_SYM, nullptr);
    }
    return ok;
}

bool InsertWithAtspi(const std::string& text, bool press_enter) {
    AtspiAccessible* desktop = atspi_get_desktop(0);
    if (!desktop) return false;
    AtspiAccessible* focused = FindFocused(desktop, 0);
    g_object_unref(desktop);
    if (!focused) return false;
    AtspiAccessible* editable = FindEditable(focused);
    bool ok = false;
    if (editable) {
        ok = InsertAt(editable, text, press_enter);
        if (editable != focused) g_object_unref(editable);
    }
    g_object_unref(focused);
    return ok;
}

std::string SenderToken(GDBusConnection* connection) {
    const char* unique = g_dbus_connection_get_unique_name(connection);
    std::string token = unique ? unique : "";
    // Portal request/session paths use the unique name with the leading ':'
    // removed, not replaced. ":1.79" becomes "1_79".
    if (!token.empty() && token.front() == ':') token.erase(token.begin());
    for (char& ch : token) {
        if (ch == '.') ch = '_';
    }
    return token;
}

std::string RandomToken() {
    char buf[48] = {};
    std::snprintf(buf, sizeof(buf), "vs%" G_GUINT64_FORMAT,
                  static_cast<guint64>(g_get_monotonic_time()) ^ g_random_int());
    return buf;
}

std::string RequestPath(GDBusConnection* connection, const std::string& token) {
    return "/org/freedesktop/portal/desktop/request/" + SenderToken(connection) + "/" + token;
}

std::string SessionPath(GDBusConnection* connection, const std::string& token) {
    return "/org/freedesktop/portal/desktop/session/" + SenderToken(connection) + "/" + token;
}

std::filesystem::path RestoreTokenPath() {
    return AppConfig::ConfigDirectory() / "remote-desktop.token";
}

std::string LoadRestoreToken() {
    std::ifstream in(RestoreTokenPath());
    std::string token;
    if (in) std::getline(in, token);
    return token;
}

void SaveRestoreToken(const std::string& token) {
    std::error_code error;
    std::filesystem::create_directories(RestoreTokenPath().parent_path(), error);
    std::ofstream out(RestoreTokenPath(), std::ios::trunc);
    if (out) out << token;
}

struct PortalWait {
    GDBusConnection* connection = nullptr;
    std::string request_path;
    guint sub = 0;
    int refs = 2;
    bool finished = false;
    std::function<void(guint32, GVariant*)> done;

    void Subscribe(const std::string& path) {
        if (sub) {
            g_dbus_connection_signal_unsubscribe(connection, sub);
            sub = 0;
        }
        request_path = path;
        sub = g_dbus_connection_signal_subscribe(
            connection, kPortalName, "org.freedesktop.portal.Request", "Response",
            request_path.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
               GVariant* parameters, gpointer data) {
                auto* wait = static_cast<PortalWait*>(data);
                guint32 response = 2;
                GVariant* results = nullptr;
                g_variant_get(parameters, "(u@a{sv})", &response, &results);
                wait->Complete(response, results);
                if (results) g_variant_unref(results);
                wait->Release();
            },
            this, nullptr);
    }

    void Complete(guint32 response, GVariant* results) {
        if (finished) return;
        finished = true;
        auto callback = std::move(done);
        if (sub) {
            g_dbus_connection_signal_unsubscribe(connection, sub);
            sub = 0;
        }
        if (callback) callback(response, results);
    }

    void Release() {
        if (--refs <= 0) delete this;
    }
};

void PortalCall(GDBusConnection* connection,
                const char* iface,
                const char* method,
                GVariant* args,
                const std::string& handle_token,
                std::function<void(guint32, GVariant*)> done) {
    auto* wait = new PortalWait{};
    wait->connection = connection;
    wait->done = std::move(done);
    wait->Subscribe(RequestPath(connection, handle_token));

    g_dbus_connection_call(
        connection, kPortalName, kPortalPath, iface, method, args,
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 120000, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            auto* wait = static_cast<PortalWait*>(data);
            GError* error = nullptr;
            auto* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
            if (error) {
                LogApp(std::string("RemoteDesktop portal call failed: ") + error->message);
                g_error_free(error);
                wait->Complete(2, nullptr);
                wait->Release();
            } else if (reply) {
                const gchar* handle = nullptr;
                g_variant_get(reply, "(&o)", &handle);
                if (!wait->finished && handle && wait->request_path != handle) {
                    LogApp(std::string("RemoteDesktop request handle is ") + handle);
                    wait->Subscribe(handle);
                }
                g_variant_unref(reply);
            }
            wait->Release();
        },
        wait);
}

bool PortalKeysym(GDBusConnection* connection, const std::string& session, gint32 keysym, bool press) {
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    GError* error = nullptr;
    auto* reply = g_dbus_connection_call_sync(
        connection, kPortalName, kPortalPath, kRemoteDesktop, "NotifyKeyboardKeysym",
        g_variant_new("(oa{sv}iu)", session.c_str(), &options, keysym, press ? 1u : 0u),
        nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error);
    if (error) {
        LogApp(std::string("RemoteDesktop key inject failed: ") + error->message);
        g_error_free(error);
        return false;
    }
    if (reply) g_variant_unref(reply);
    return true;
}

} // namespace

InputInjectorLinux::InputInjectorLinux(GtkApplication* application) : application_(application) {
    atspi_init();
    EnsureX11Clipboard();
}

InputInjectorLinux::~InputInjectorLinux() {
    if (paste_timeout_) g_source_remove(paste_timeout_);
    if (enter_timeout_) g_source_remove(enter_timeout_);
    if (x11_watch_) g_source_remove(x11_watch_);
    HidePortalParentWindow();
    ClosePortalSession();
    auto* display = static_cast<Display*>(x11_display_);
    if (display && x11_window_) XDestroyWindow(display, x11_window_);
    if (display) XCloseDisplay(display);
}

void InputInjectorLinux::EnsureX11Clipboard() {
    if (x11_display_) return;
    Display* display = XOpenDisplay(nullptr);
    if (!display) return;
    x11_display_ = display;
    XSetWindowAttributes attrs{};
    attrs.override_redirect = True;
    attrs.event_mask = PropertyChangeMask;
    x11_window_ = XCreateWindow(display, DefaultRootWindow(display), -32, -32, 1, 1, 0,
                                CopyFromParent, InputOutput, CopyFromParent,
                                CWOverrideRedirect | CWEventMask, &attrs);
    x11_watch_ = g_unix_fd_add(ConnectionNumber(display), G_IO_IN,
        +[](gint, GIOCondition, gpointer data) -> gboolean {
            static_cast<InputInjectorLinux*>(data)->HandleX11Events();
            return G_SOURCE_CONTINUE;
        }, this);
}

void InputInjectorLinux::OfferX11Clipboard(const std::string& text) {
    EnsureX11Clipboard();
    auto* display = static_cast<Display*>(x11_display_);
    if (!display || !x11_window_) return;
    x11_clipboard_text_ = text;
    const Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
    XSetSelectionOwner(display, clipboard, x11_window_, CurrentTime);
    XSetSelectionOwner(display, XA_PRIMARY, x11_window_, CurrentTime);
    XFlush(display);
}

void InputInjectorLinux::HandleX11Events() {
    auto* display = static_cast<Display*>(x11_display_);
    if (!display) return;
    while (XPending(display)) {
        XEvent event;
        XNextEvent(display, &event);
        if (event.type == SelectionRequest) ReplySelectionRequest(&event);
    }
}

void InputInjectorLinux::ReplySelectionRequest(void* raw_event) {
    auto* display = static_cast<Display*>(x11_display_);
    auto* event = static_cast<XEvent*>(raw_event);
    if (!display || !event) return;
    const XSelectionRequestEvent& request = event->xselectionrequest;
    const Atom targets = XInternAtom(display, "TARGETS", False);
    const Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    const Atom text_plain = XInternAtom(display, "text/plain;charset=utf-8", False);
    const Atom text_plain_utf8 = XInternAtom(display, "text/plain", False);
    XEvent reply{};
    reply.xselection.type = SelectionNotify;
    reply.xselection.display = request.display;
    reply.xselection.requestor = request.requestor;
    reply.xselection.selection = request.selection;
    reply.xselection.target = request.target;
    reply.xselection.property = request.property;
    reply.xselection.time = request.time;
    if (request.target == targets) {
        const Atom offered[] = {targets, utf8, XA_STRING, text_plain, text_plain_utf8};
        XChangeProperty(display, request.requestor, request.property, XA_ATOM, 32,
                        PropModeReplace, reinterpret_cast<const unsigned char*>(offered),
                        static_cast<int>(std::size(offered)));
    } else if (request.target == utf8 || request.target == XA_STRING ||
               request.target == text_plain || request.target == text_plain_utf8) {
        XChangeProperty(display, request.requestor, request.property, request.target, 8,
                        PropModeReplace,
                        reinterpret_cast<const unsigned char*>(x11_clipboard_text_.data()),
                        static_cast<int>(x11_clipboard_text_.size()));
    } else {
        reply.xselection.property = None;
    }
    XSendEvent(display, request.requestor, True, NoEventMask, &reply);
    XFlush(display);
}

void InputInjectorLinux::CopyToClipboard(const std::string& text) {
    OfferX11Clipboard(text);
    auto* display = gdk_display_get_default();
    if (!display) return;
    auto* clipboard = gdk_display_get_clipboard(display);
    gdk_clipboard_set_text(clipboard, text.c_str());
}

void InputInjectorLinux::NotifyCopied() {
    if (!application_) return;
    auto* notification = g_notification_new("VoiceStick");
    g_notification_set_body(notification, "Text copied. Press Ctrl+V to paste.");
    g_application_send_notification(G_APPLICATION(application_), "voicestick-copied", notification);
    g_object_unref(notification);
}

void InputInjectorLinux::ClosePortalSession() {
    auto* connection = static_cast<GDBusConnection*>(portal_connection_);
    if (portal_closed_sub_ && connection) {
        g_dbus_connection_signal_unsubscribe(connection, portal_closed_sub_);
        portal_closed_sub_ = 0;
    }
    if (connection && !portal_session_path_.empty()) {
        g_dbus_connection_call(connection, kPortalName, portal_session_path_.c_str(),
                               "org.freedesktop.portal.Session", "Close", nullptr,
                               nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr, nullptr);
    }
    portal_session_path_.clear();
    portal_ready_ = false;
    portal_requesting_ = false;
    if (portal_connection_) {
        g_object_unref(static_cast<GDBusConnection*>(portal_connection_));
        portal_connection_ = nullptr;
    }
}

void InputInjectorLinux::HidePortalParentWindow() {
    if (!portal_window_) return;
    gtk_window_destroy(GTK_WINDOW(portal_window_));
    portal_window_ = nullptr;
}

void InputInjectorLinux::ShowPortalParentWindow() {
    if (portal_window_) return;
    portal_window_ = gtk_application_window_new(application_);
    gtk_window_set_title(GTK_WINDOW(portal_window_), "VoiceStick");
    gtk_window_set_default_size(GTK_WINDOW(portal_window_), 480, 240);
    gtk_window_set_resizable(GTK_WINDOW(portal_window_), FALSE);

    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 20);
    gtk_widget_set_margin_bottom(box, 20);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);

    auto* hint = gtk_label_new(
        "识别结果已复制。GNOME 马上会弹出远程控制授权框，请打开「允许远程交互」并允许。");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), hint);

    auto* text = gtk_label_new(pending_text_.c_str());
    gtk_label_set_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_selectable(GTK_LABEL(text), TRUE);
    gtk_widget_set_halign(text, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), text);

    auto* allow = gtk_button_new_with_label("没看到系统授权框，重新请求");
    gtk_widget_add_css_class(allow, "suggested-action");
    gtk_box_append(GTK_BOX(box), allow);
    g_signal_connect(allow, "clicked", reinterpret_cast<GCallback>(+[](GtkButton*, gpointer data) {
        auto* self = static_cast<InputInjectorLinux*>(data);
        auto pending = std::move(self->portal_pending_);
        self->ClosePortalSession();
        self->portal_requesting_ = true;
        self->portal_pending_ = std::move(pending);
        self->BeginPortalSession();
    }), this);

    auto* later = gtk_button_new_with_label("关闭，我自己按 Ctrl+V");
    gtk_box_append(GTK_BOX(box), later);
    g_signal_connect(later, "clicked", reinterpret_cast<GCallback>(+[](GtkButton*, gpointer data) {
        auto* self = static_cast<InputInjectorLinux*>(data);
        self->portal_requesting_ = false;
        auto callback = std::move(self->portal_pending_);
        self->HidePortalParentWindow();
        if (callback) callback(false);
    }), this);

    g_signal_connect(portal_window_, "close-request",
        reinterpret_cast<GCallback>(+[](GtkWindow*, gpointer data) -> gboolean {
            auto* self = static_cast<InputInjectorLinux*>(data);
            self->portal_window_ = nullptr;
            if (self->portal_requesting_ && !self->portal_ready_) {
                self->ClosePortalSession();
                self->portal_requesting_ = false;
                auto callback = std::move(self->portal_pending_);
                if (callback) callback(false);
            }
            return FALSE;
        }), this);

    gtk_window_set_child(GTK_WINDOW(portal_window_), box);
    gtk_window_present(GTK_WINDOW(portal_window_));
    BeginPortalSession();
}

void InputInjectorLinux::BeginPortalSession() {
    GError* error = nullptr;
    auto* connection = static_cast<GDBusConnection*>(portal_connection_);
    if (!connection) {
        connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (!connection) {
            if (error) {
                LogApp(std::string("No session bus for RemoteDesktop portal: ") + error->message);
                g_error_free(error);
            }
            portal_requesting_ = false;
            auto callback = std::move(portal_pending_);
            HidePortalParentWindow();
            if (callback) callback(false);
            return;
        }
        portal_connection_ = connection;
    }

    const guint generation = ++portal_generation_;
    const auto finish = [this, generation](bool ok) {
        if (generation != portal_generation_) return;
        portal_requesting_ = false;
        HidePortalParentWindow();
        auto callback = std::move(portal_pending_);
        if (callback) callback(ok);
    };

    const auto session_token = RandomToken();
    const auto create_token = RandomToken();
    portal_session_path_ = SessionPath(connection, session_token);
    LogApp("RemoteDesktop CreateSession " + portal_session_path_);

    GVariantBuilder create_opts;
    g_variant_builder_init(&create_opts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&create_opts, "{sv}", "handle_token", g_variant_new_string(create_token.c_str()));
    g_variant_builder_add(&create_opts, "{sv}", "session_handle_token",
                          g_variant_new_string(session_token.c_str()));
    PortalCall(connection, kRemoteDesktop, "CreateSession", g_variant_new("(a{sv})", &create_opts), create_token,
        [this, connection, finish](guint32 response, GVariant* results) {
            if (response != 0) {
                LogApp("RemoteDesktop CreateSession denied");
                portal_session_path_.clear();
                finish(false);
                return;
            }
            const gchar* session = nullptr;
            if (results && g_variant_lookup(results, "session_handle", "&s", &session) && session) {
                portal_session_path_ = session;
            }
            LogApp("RemoteDesktop SelectDevices keyboard+pointer");
            const auto select_token = RandomToken();
            GVariantBuilder select_opts;
            g_variant_builder_init(&select_opts, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&select_opts, "{sv}", "handle_token",
                                  g_variant_new_string(select_token.c_str()));
            g_variant_builder_add(&select_opts, "{sv}", "types",
                                  g_variant_new_uint32(kDeviceKeyboard | kDevicePointer));
            g_variant_builder_add(&select_opts, "{sv}", "persist_mode",
                                  g_variant_new_uint32(kPersistUntilRevoked));
            if (const auto restore = LoadRestoreToken(); !restore.empty()) {
                g_variant_builder_add(&select_opts, "{sv}", "restore_token",
                                      g_variant_new_string(restore.c_str()));
            }
            PortalCall(connection, kRemoteDesktop, "SelectDevices",
                       g_variant_new("(oa{sv})", portal_session_path_.c_str(), &select_opts),
                       select_token,
                [this, connection, finish](guint32 response, GVariant*) {
                    if (response != 0) {
                        LogApp("RemoteDesktop SelectDevices denied");
                        ClosePortalSession();
                        finish(false);
                        return;
                    }
                    LogApp("RemoteDesktop Start; waiting for GNOME dialog");
                    const auto start_token = RandomToken();
                    GVariantBuilder start_opts;
                    g_variant_builder_init(&start_opts, G_VARIANT_TYPE("a{sv}"));
                    g_variant_builder_add(&start_opts, "{sv}", "handle_token",
                                          g_variant_new_string(start_token.c_str()));
                    PortalCall(connection, kRemoteDesktop, "Start",
                               g_variant_new("(osa{sv})", portal_session_path_.c_str(), "",
                                             &start_opts),
                               start_token,
                        [this, connection, finish](guint32 response, GVariant* results) {
                            if (response != 0) {
                                LogApp("RemoteDesktop Start denied; stay with Ctrl+V");
                                ClosePortalSession();
                                finish(false);
                                return;
                            }
                            const gchar* restore = nullptr;
                            if (results && g_variant_lookup(results, "restore_token", "&s", &restore) && restore) {
                                SaveRestoreToken(restore);
                            }
                            portal_ready_ = true;
                            portal_closed_sub_ = g_dbus_connection_signal_subscribe(
                                connection, kPortalName, "org.freedesktop.portal.Session", "Closed",
                                portal_session_path_.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                [](GDBusConnection*, const gchar*, const gchar*, const gchar*,
                                   const gchar*, GVariant*, gpointer data) {
                                    auto* self = static_cast<InputInjectorLinux*>(data);
                                    LogApp("RemoteDesktop session closed");
                                    self->portal_ready_ = false;
                                    self->portal_session_path_.clear();
                                    self->portal_closed_sub_ = 0;
                                },
                                this, nullptr);
                            LogApp("RemoteDesktop keyboard share granted");
                            finish(true);
                        });
                });
        });
}

void InputInjectorLinux::EnsurePortalKeyboard(std::function<void(bool)> done) {
    if (portal_ready_ && !portal_session_path_.empty()) {
        done(true);
        return;
    }
    if (portal_requesting_) {
        portal_pending_ = std::move(done);
        return;
    }
    portal_requesting_ = true;
    portal_pending_ = std::move(done);
    ShowPortalParentWindow();
}

bool InputInjectorLinux::InjectPortalCtrlV() {
    auto* connection = static_cast<GDBusConnection*>(portal_connection_);
    if (!connection || portal_session_path_.empty()) return false;
    if (!PortalKeysym(connection, portal_session_path_, XK_Control_L, true)) return false;
    if (!PortalKeysym(connection, portal_session_path_, XK_v, true)) return false;
    if (!PortalKeysym(connection, portal_session_path_, XK_v, false)) return false;
    if (!PortalKeysym(connection, portal_session_path_, XK_Control_L, false)) return false;
    PortalKeysym(connection, portal_session_path_, XK_Control_L, false);
    return true;
}

bool InputInjectorLinux::InjectPortalEnter() {
    auto* connection = static_cast<GDBusConnection*>(portal_connection_);
    if (!connection || portal_session_path_.empty()) return false;
    PortalKeysym(connection, portal_session_path_, XK_Control_L, false);
    if (!PortalKeysym(connection, portal_session_path_, XK_Return, true)) return false;
    return PortalKeysym(connection, portal_session_path_, XK_Return, false);
}

void InputInjectorLinux::SchedulePortalEnter() {
    if (enter_timeout_) g_source_remove(enter_timeout_);
    enter_timeout_ = g_timeout_add(kEnterAfterPasteMs, [](gpointer data) -> gboolean {
        auto* self = static_cast<InputInjectorLinux*>(data);
        self->enter_timeout_ = 0;
        if (self->InjectPortalEnter()) {
            LogApp("RemoteDesktop Enter sent");
        } else {
            LogApp("RemoteDesktop Enter failed");
        }
        return G_SOURCE_REMOVE;
    }, this);
}

void InputInjectorLinux::Paste(const std::string& text, bool press_enter) {
    pending_text_ = text;
    pending_enter_ = press_enter;
    CopyToClipboard(text);
    if (enter_timeout_) {
        g_source_remove(enter_timeout_);
        enter_timeout_ = 0;
    }
    if (paste_timeout_) g_source_remove(paste_timeout_);
    paste_timeout_ = g_timeout_add(kClipboardSettleMs, [](gpointer data) -> gboolean {
        auto* self = static_cast<InputInjectorLinux*>(data);
        self->paste_timeout_ = 0;
        self->FinishPaste();
        return G_SOURCE_REMOVE;
    }, this);
}

void InputInjectorLinux::FinishPaste() {
    CopyToClipboard(pending_text_);
    if (InsertWithAtspi(pending_text_, pending_enter_)) {
        LogApp("Inserted text through AT-SPI");
        return;
    }
    EnsurePortalKeyboard([this](bool granted) {
        if (!granted) {
            LogApp("Automatic paste is not available; use Ctrl+V");
            NotifyCopied();
            return;
        }
        g_timeout_add(kPortalFocusMs, [](gpointer data) -> gboolean {
            auto* self = static_cast<InputInjectorLinux*>(data);
            self->CopyToClipboard(self->pending_text_);
            if (self->InjectPortalCtrlV()) {
                LogApp("Pasted through RemoteDesktop portal");
                if (self->pending_enter_) self->SchedulePortalEnter();
            } else {
                LogApp("RemoteDesktop inject failed; use Ctrl+V");
                self->NotifyCopied();
            }
            return G_SOURCE_REMOVE;
        }, this);
    });
}

} // namespace voicestick
