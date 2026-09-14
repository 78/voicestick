#pragma once

#include <array>
#include <string_view>

namespace voicestick {

struct TranslationTarget {
    std::string_view code;
    std::string_view name;
};

inline constexpr std::array<TranslationTarget, 20> kTranslationTargets{{
    {"en", "English"},
    {"zh-Hans", "Chinese (Simplified)"},
    {"zh-Hant", "Chinese (Traditional)"},
    {"ja", "Japanese"},
    {"ko", "Korean"},
    {"ru", "Russian"},
    {"fr", "French"},
    {"de", "German"},
    {"es", "Spanish"},
    {"it", "Italian"},
    {"pt", "Portuguese"},
    {"nl", "Dutch"},
    {"sv", "Swedish"},
    {"pl", "Polish"},
    {"tr", "Turkish"},
    {"ar", "Arabic"},
    {"hi", "Hindi"},
    {"id", "Indonesian"},
    {"vi", "Vietnamese"},
    {"th", "Thai"},
}};

inline constexpr const char* kWebsiteUrl = "https://78.github.io/voicestick/";

} // namespace voicestick
