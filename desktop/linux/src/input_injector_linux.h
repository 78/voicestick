#pragma once

#include "voice_stick_coordinator.h"

#include <gtk/gtk.h>

#include <functional>
#include <string>

namespace voicestick {

class InputInjectorLinux : public InputInjector {
public:
    explicit InputInjectorLinux(GtkApplication* application);
    ~InputInjectorLinux() override;

    void Paste(const std::string& text, bool press_enter) override;

private:
    void CopyToClipboard(const std::string& text);
    void NotifyCopied();
    void FinishPaste();
    void EnsureX11Clipboard();
    void OfferX11Clipboard(const std::string& text);
    void HandleX11Events();
    void ReplySelectionRequest(void* event);
    void EnsurePortalKeyboard(std::function<void(bool)> done);
    void BeginPortalSession();
    void ShowPortalParentWindow();
    void HidePortalParentWindow();
    bool InjectPortalCtrlV();
    bool InjectPortalEnter();
    void SchedulePortalEnter();
    void ClosePortalSession();

    GtkApplication* application_ = nullptr;
    std::string pending_text_;
    bool pending_enter_ = false;
    guint paste_timeout_ = 0;
    guint enter_timeout_ = 0;
    void* x11_display_ = nullptr;
    unsigned long x11_window_ = 0;
    guint x11_watch_ = 0;
    std::string x11_clipboard_text_;
    void* portal_connection_ = nullptr;
    std::string portal_session_path_;
    guint portal_closed_sub_ = 0;
    bool portal_ready_ = false;
    bool portal_requesting_ = false;
    guint portal_generation_ = 0;
    std::function<void(bool)> portal_pending_;
    GtkWidget* portal_window_ = nullptr;
};

} // namespace voicestick
