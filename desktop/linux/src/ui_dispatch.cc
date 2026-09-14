#include "ui_dispatch.h"

#include <glib.h>

#include <utility>

namespace voicestick {

namespace {
GMainContext* UiContext() {
    return g_main_context_default();
}
} // namespace

void RunOnUiThread(std::function<void()> action) {
    if (!action) return;
    if (g_main_context_is_owner(UiContext())) {
        action();
        return;
    }
    auto* heap_action = new std::function<void()>(std::move(action));
    g_main_context_invoke(UiContext(), [](gpointer data) -> gboolean {
        auto* fn = static_cast<std::function<void()>*>(data);
        (*fn)();
        delete fn;
        return G_SOURCE_REMOVE;
    }, heap_action);
}

bool IsUiThread() {
    return g_main_context_is_owner(UiContext()) != FALSE;
}

} // namespace voicestick
