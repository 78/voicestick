#include "llm_translation_client.h"

#include "cJSON.h"
#include "http_client_linux.h"

#include <memory>
#include <thread>

namespace voicestick {

LLMTranslationClient::LLMTranslationClient(AppConfig config) : config_(std::move(config)) {}

void LLMTranslationClient::Translate(std::string text,
                                     std::string target_language,
                                     std::vector<std::string> hotwords,
                                     std::function<void(bool, std::string)> completion) const {
    auto config = config_;
    std::thread([client = LLMTranslationClient(std::move(config)),
                 text = std::move(text),
                 target_language = std::move(target_language),
                 hotwords = std::move(hotwords),
                 completion = std::move(completion)]() mutable {
        std::string error;
        auto translated = client.TranslateSync(text, target_language, hotwords, &error);
        if (!error.empty()) {
            completion(false, error);
        } else {
            completion(true, translated);
        }
    }).detach();
}

std::string LLMTranslationClient::TranslateSync(const std::string& text,
                                                const std::string& target_language,
                                                const std::vector<std::string>& hotwords,
                                                std::string* error) const {
    const auto api_key = TrimCopy(config_.llm_api_key);
    if (api_key.empty()) {
        *error = "Missing LLM API key";
        return {};
    }
    const auto url = ChatCompletionsUrl(error);
    if (!error->empty()) return {};

    const auto payload =
        "{\"model\":\"" + JsonEscape(config_.llm_model) + "\","
        "\"temperature\":0,"
        "\"messages\":["
        "{\"role\":\"system\",\"content\":\"" + JsonEscape(SystemPrompt(target_language, hotwords)) + "\"},"
        "{\"role\":\"user\",\"content\":\"" + JsonEscape(text) + "\"}"
        "]}";

    auto response = HttpPostJson(url, payload, {{"Authorization", "Bearer " + api_key}});
    if (!response.ok()) {
        *error = "LLM request failed: " + response.error;
        return {};
    }

    auto* root = cJSON_ParseWithLength(response.body.data(), response.body.size());
    if (!root) {
        *error = "Invalid LLM translation response";
        return {};
    }
    auto cleanup = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(root, cJSON_Delete);
    auto* choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    auto* first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
    auto* message = first ? cJSON_GetObjectItemCaseSensitive(first, "message") : nullptr;
    auto* content = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : nullptr;
    if (!cJSON_IsString(content) || content->valuestring == nullptr) {
        *error = "Invalid LLM translation response";
        return {};
    }
    return TrimCopy(content->valuestring);
}

std::string LLMTranslationClient::ChatCompletionsUrl(std::string* error) const {
    auto base = TrimCopy(config_.llm_base_url);
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.empty()) {
        *error = "Invalid LLM base URL";
        return {};
    }
    auto url = base.ends_with("/chat/completions") ? base : base + "/chat/completions";
    if (!StartsWithScheme(url, "http://") && !StartsWithScheme(url, "https://")) {
        url = "https://" + url;
    }
    return url;
}

std::string LLMTranslationClient::SystemPrompt(const std::string& target_language,
                                               const std::vector<std::string>& hotwords) {
    std::string prompt =
        "You are a real-time speech translator.\n"
        "Translate the user's text into " + target_language + ".\n"
        "Detect the source language automatically.\n"
        "Return only the translated text, with no explanations, quotes, prefixes, alternatives, or markdown.\n"
        "The text may come from live speech recognition and may contain minor recognition errors; infer the intended meaning when it is clear.";
    std::vector<std::string> terms;
    for (auto term : hotwords) {
        term = TrimCopy(std::move(term));
        if (!term.empty()) terms.push_back(std::move(term));
    }
    if (!terms.empty()) {
        prompt += "\n\nImportant terms that may appear:\n";
        for (const auto& term : terms) {
            prompt += "- " + term + "\n";
        }
    }
    return prompt;
}

} // namespace voicestick
