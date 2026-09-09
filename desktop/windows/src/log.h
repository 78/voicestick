#pragma once

#include <string>
#include <string_view>

namespace voicestick {

void Log(std::string_view category, std::string_view message);

inline void LogApp(std::string_view message) { Log("APP", message); }
inline void LogBle(std::string_view message) { Log("BLE", message); }
inline void LogCoordinator(std::string_view message) { Log("CRD", message); }

} // namespace voicestick
