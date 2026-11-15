#include "llm_translation_client.h"

#include "cJSON.h"
#include "http_client_linux.h"

#define LLM_JSON_IMPLEMENTATION
#include "llm_json.hpp"
#define LLM_RETRY_IMPLEMENTATION
#include "llm_retry.hpp"

#include <memory>
#include <thread>

namespace voicestick {

namespace {

// Live translation is latency sensitive: one quick retry for dropped
// connections, 408/429 and 5xx, and none for auth or bad request errors.
llm::RetryConfig TranslationRetryConfig() {
    llm::RetryConfig config;
    config.max_attempts = 2;
    config.base_delay_ms = 300.0;
    return config;
}

} // namespace

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

    llm::json::Value messages = llm::json::array();
    messages.push_back(llm::json::make_object({
        {"role", "system"},
        {"content", SystemPrompt(target_language, hotwords)},
    }));
    messages.push_back(llm::json::make_object({
        {"role", "user"},
        {"content", text},
    }));
    const auto payload = llm::json::make_object({
        {"model", config_.llm_model},
        {"temperature", 0},
        {"messages", messages},
    }).dump();

    HttpResponse response;
    try {
        response = llm::with_retry<HttpResponse>([&] {
            auto attempt = HttpPostJson(url, payload, {{"Authorization", "Bearer " + api_key}});
            if (!attempt.ok()) {
                throw llm::LLMError{attempt.status, "LLM request failed: " + attempt.error, true};
            }
            return attempt;
        }, TranslationRetryConfig()).value;
    } catch (const llm::LLMError& failure) {
        *error = failure.message;
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
