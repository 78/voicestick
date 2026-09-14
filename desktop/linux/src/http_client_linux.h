#pragma once

#include "byte_utils.h"

#include <map>
#include <span>
#include <string>
#include <string_view>

namespace voicestick {

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;

    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

HttpResponse HttpGet(const std::string& url,
                     const std::map<std::string, std::string>& headers = {});
HttpResponse HttpPostJson(const std::string& url,
                          const std::string& json_body,
                          const std::map<std::string, std::string>& headers = {});
ByteVector HttpGetBytes(const std::string& url, std::string& error);
std::string Sha256Hex(std::span<const std::uint8_t> data);
std::string JsonEscape(std::string_view text);
std::string TrimCopy(std::string value);
bool StartsWithScheme(std::string_view text, std::string_view scheme);

} // namespace voicestick
