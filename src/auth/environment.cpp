#include "auth/environment.hpp"

#include "utils/string.hpp"

#include <cstdlib>

namespace cc::auth {
namespace {

/// The address of the API of Anthropic.
constexpr std::string_view kDefaultBaseUrl = "https://api.anthropic.com";

/// Reads a variable of the environment.
std::string environment(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) return {};
    return cc::trim(value);
}

} // namespace

std::string base_api_url() {
    const std::string url = environment("ANTHROPIC_BASE_URL");
    if (url.empty()) return std::string(kDefaultBaseUrl);
    // A trailing slash is taken away, so that the address of the API may be
    // joined with a path.
    std::string out = url;
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

std::vector<std::pair<std::string, std::string>> custom_headers() {
    std::vector<std::pair<std::string, std::string>> headers;

    const std::string raw = environment("ANTHROPIC_CUSTOM_HEADERS");
    if (raw.empty()) return headers;

    for (const std::string& line : cc::split_lines(raw)) {
        const std::string trimmed = cc::trim(line);
        if (trimmed.empty()) continue;

        // The header is written as `Name: Value`; the name is everything
        // before the first colon.
        const size_t colon = trimmed.find(':');
        if (colon == std::string::npos) continue;

        const std::string name = cc::trim(trimmed.substr(0, colon));
        const std::string value = cc::trim(trimmed.substr(colon + 1));
        if (name.empty()) continue;

        headers.emplace_back(name, value);
    }

    return headers;
}

std::string environment_model() {
    return environment("ANTHROPIC_MODEL");
}

std::string auth_token() {
    return environment("ANTHROPIC_AUTH_TOKEN");
}

std::string custom_api_key() {
    const std::string key = environment("ANTHROPIC_API_KEY");
    if (key.empty()) return {};

    // The last twenty characters of the key are kept, the way the reference
    // keeps them.
    constexpr size_t kKept = 20;
    if (key.size() <= kKept) return key;
    return key.substr(key.size() - kKept);
}

bool running_on_homespace() {
    // The reference looks at the kind of the user and at the variable that
    // names the cloud it runs in.
    if (environment("USER_TYPE") != "ant") return false;

    // The variable is true when it is one of the words the reference takes
    // for true.
    const std::string cloud = environment("COO_RUNNING_ON_HOMESPACE");
    if (cloud.empty()) return false;
    return cloud == "1" || cloud == "true" || cloud == "yes" || cloud == "on";
}

} // namespace cc::auth
