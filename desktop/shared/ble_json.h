#pragma once

#include "cJSON.h"

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>

namespace voicestick::ble_json {

using Object = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

inline Object ParseObject(const std::string& json) {
    Object object(nullptr, cJSON_Delete);
    if (json.find('\0') == std::string::npos) {
        object.reset(cJSON_ParseWithLengthOpts(json.c_str(), json.size() + 1, nullptr, true));
    }
    if (!cJSON_IsObject(object.get())) object.reset();
    return object;
}

inline std::string String(const cJSON* object, const char* name) {
    const auto* value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
    return value ? std::string(value) : std::string{};
}

template <typename UInt>
std::optional<UInt> Unsigned(const cJSON* object, const char* name) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item)) return std::nullopt;

    // Keep JSON integers exact for both cJSON and browser clients.
    constexpr double max_exact_integer = 9007199254740991.0;
    const double value = item->valuedouble;
    if (!(value >= 0 && value <= max_exact_integer && value <= std::numeric_limits<UInt>::max()) ||
        std::trunc(value) != value) {
        return std::nullopt;
    }
    return static_cast<UInt>(value);
}

inline std::optional<bool> Bool(const cJSON* object, const char* name) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsBool(item)) return std::nullopt;
    return cJSON_IsTrue(item) != 0;
}

inline std::optional<double> Double(const cJSON* object, const char* name) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble)) return std::nullopt;
    return item->valuedouble;
}

inline const cJSON* ChildObject(const cJSON* object, const char* name) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsObject(item) ? item : nullptr;
}

inline const cJSON* ChildArray(const cJSON* object, const char* name) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsArray(item) ? item : nullptr;
}

} // namespace voicestick::ble_json
