#include "query/model.hpp"

#include <cctype>
#include <regex>

namespace cc::query {
namespace {

/// The tag of the context window, the way the reference reads it.
const std::regex& context_tag() {
    static const std::regex tag(R"(\[([12])m\])", std::regex::icase);
    return tag;
}

/// The tag of the window of a million tokens.
const std::regex& one_million_tag() {
    static const std::regex tag(R"(\[1m\])", std::regex::icase);
    return tag;
}

} // namespace

std::string model_for_api(const std::string& model) {
    // The tag may sit anywhere in the name, and the reference takes every
    // one of them off.
    return std::regex_replace(model, context_tag(), "");
}

bool has_1m_context(const std::string& model) {
    return std::regex_search(model, one_million_tag());
}

OutputTokens output_tokens_for(const std::string& model) {
    const std::string name = model_for_api(model);

    // The version of a family, so that a model that is newer than the ones
    // the reference knows is given the number of the newest one: the family
    // of Opus is 4 for `claude-opus-4-6` and 5 for `claude-opus-5-5`.
    const auto major_version = [&name](const std::string& family) {
        const size_t at = name.find(family);
        if (at == std::string::npos) return -1;
        size_t i = at + family.size();
        int number = 0;
        while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i])) != 0) {
            number = number * 10 + (name[i] - '0');
            ++i;
        }
        return number;
    };

    // The version that follows the version of the family, e.g. the 6 of
    // `claude-opus-4-6`.
    const auto minor_version = [&name](const std::string& family, int major) {
        const size_t at = name.find(family);
        if (at == std::string::npos) return -1;
        size_t i = at + family.size();
        int number = 0;
        while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i])) != 0) {
            number = number * 10 + (name[i] - '0');
            ++i;
        }
        if (number != major) return -1;
        if (i < name.size() && name[i] == '-') ++i;
        number = 0;
        while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i])) != 0) {
            number = number * 10 + (name[i] - '0');
            ++i;
        }
        return number;
    };

    const int opus = major_version("opus-");
    const int sonnet = major_version("sonnet-");
    const int haiku = major_version("haiku-");

    // The family of the fifth generation answers with the most.
    if (opus >= 5) return {64000, 128000};
    if (sonnet >= 5) return {32000, 128000};
    if (haiku >= 5) return {32000, 64000};

    // The family of the fourth generation answers with less, and the newer
    // models of it with more than the older ones.
    if (opus == 4) {
        const int minor = minor_version("opus-", 4);
        if (minor >= 6) return {64000, 128000};
        if (minor >= 5) return {32000, 64000};
        return {32000, 32000};
    }
    if (sonnet == 4) {
        const int minor = minor_version("sonnet-", 4);
        if (minor >= 6) return {32000, 128000};
        return {32000, 64000};
    }
    if (haiku == 4) return {32000, 64000};

    // The families that are older answer with less.
    if (name.find("claude-3-opus") != std::string::npos) return {4096, 4096};
    if (name.find("claude-3-sonnet") != std::string::npos) return {8192, 8192};
    if (name.find("claude-3-haiku") != std::string::npos) return {4096, 4096};
    if (name.find("3-5-sonnet") != std::string::npos || name.find("3-5-haiku") != std::string::npos) {
        return {8192, 8192};
    }
    if (name.find("3-7-sonnet") != std::string::npos) return {32000, 64000};

    // A model of a gateway the reference knows nothing about answers with
    // the usual number.
    return {32000, 64000};
}

bool is_anthropic_model(const std::string& model) {
    return model.rfind("claude", 0) == 0;
}

bool supports_adaptive_thinking(const std::string& model) {
    const std::string name = model_for_api(model);

    // The reference asks the models of the 4.6 generation for the adaptive
    // thinking, and says that every model that is newer is trained on it.
    if (name.find("opus-4-6") != std::string::npos || name.find("sonnet-4-6") != std::string::npos) {
        return true;
    }
    // The fifth generation is newer than the one the reference knows.
    if (name.find("opus-5") != std::string::npos || name.find("sonnet-5") != std::string::npos) {
        return true;
    }
    // The other models of Anthropic take a budget of tokens.
    if (name.find("opus") != std::string::npos || name.find("sonnet") != std::string::npos ||
        name.find("haiku") != std::string::npos) {
        return false;
    }
    // A model of a gateway is asked for the adaptive thinking.
    return true;
}

} // namespace cc::query
