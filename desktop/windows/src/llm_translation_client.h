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

    void Transform(std::string text,
                   OutputProfile profile,
                   std::vector<std::string> hotwords,
                   std::function<void(bool, std::string)> completion) const;
    static std::wstring Utf16FromUtf8(std::string_view text);
    static std::string Utf8FromUtf16(std::wstring_view text);

private:
    std::string TransformSync(const std::string& text,
                              const OutputProfile& profile,
                              const std::vector<std::string>& hotwords,
                              std::string* error) const;
    std::string ChatCompletionsPathAndQuery(std::wstring* host, INTERNET_PORT* port, bool* secure,
                                            std::string* error) const;
    static std::string SystemPrompt(const OutputProfile& profile,
                                    const std::vector<std::string>& hotwords);
    static std::string JsonEscape(std::string_view text);
    static std::string LastErrorText();

    AppConfig config_;
};

} // namespace voicestick
