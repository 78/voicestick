#pragma once

#include "app_config.h"

#include <Windows.h>
#include <Winhttp.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace voicestick {

class LLMTranslationClient {
public:
    explicit LLMTranslationClient(AppConfig config);

    void Translate(std::string text,
                   std::string target_language,
                   std::vector<std::string> hotwords,
                   std::function<void(bool, std::string)> completion) const;
    static std::wstring Utf16FromUtf8(std::string_view text);
    static std::string Utf8FromUtf16(std::wstring_view text);

private:
    std::string TranslateSync(const std::string& text,
                              const std::string& target_language,
                              const std::vector<std::string>& hotwords,
                              std::string* error) const;
    std::string ChatCompletionsPathAndQuery(std::wstring* host, INTERNET_PORT* port, bool* secure,
                                            std::string* error) const;
    static std::string SystemPrompt(const std::string& target_language,
                                    const std::vector<std::string>& hotwords);
    static std::string JsonEscape(std::string_view text);
    static std::string LastErrorText();

    AppConfig config_;
};

} // namespace voicestick
