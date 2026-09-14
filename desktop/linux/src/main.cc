#include "gtk4_app.h"
#include "log.h"
#include "version.h"

#include <adwaita.h>

int main(int argc, char** argv) {
    adw_init();
    auto* application = adw_application_new("me.voicestick.VoiceStick", G_APPLICATION_DEFAULT_FLAGS);
    voicestick::LogApp(std::string("VoiceStick ") + VOICESTICK_VERSION_STR + " starting");
    voicestick::Gtk4App app(application);
    const int status = app.Run(argc, argv);
    g_object_unref(application);
    return status;
}
