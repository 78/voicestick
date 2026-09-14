#pragma once

#include <functional>

namespace voicestick {

void RunOnUiThread(std::function<void()> action);
bool IsUiThread();

} // namespace voicestick
