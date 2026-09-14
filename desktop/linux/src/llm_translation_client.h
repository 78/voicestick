#pragma once

#include "app_config.h"

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

private:
    std::string TranslateSync(const std::string& text,
                              const std::string& target_language,
                              const std::vector<std::string>& hotwords,
                              std::string* error) const;
    std::string ChatCompletionsUrl(std::string* error) const;
    static std::string SystemPrompt(const std::string& target_language,
                                    const std::vector<std::string>& hotwords);

    AppConfig config_;
};

} // namespace voicestick
