#include "config/managed_settings.hpp"

#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cstdlib>

namespace cc::config {
namespace {

/// Merges the settings of `from` into `into`.
///
/// The reference merges the settings key by key: the more specific scope
/// wins for the key it names, and the arrays of the scopes are put
/// together.
void merge_settings(boost::json::object& into, const boost::json::object& from) {
    for (const auto& [key, value] : from) {
        if (!value.is_array()) {
            into[key] = value;
            continue;
        }

        boost::json::array merged;
        const auto existing = into.find(key);
        if (existing != into.end() && existing->value().is_array()) {
            for (const auto& entry : existing->value().as_array()) merged.emplace_back(entry);
        }
        for (const auto& entry : value.as_array()) {
            bool known = false;
            for (const auto& item : merged) {
                if (item == entry) known = true;
            }
            if (!known) merged.emplace_back(entry);
        }
        into[key] = boost::json::value(std::move(merged));
    }
}

} // namespace

std::string managed_settings_dir() {
    if (const char* path = std::getenv("CLAUDE_CODE_MANAGED_SETTINGS_PATH"); path && *path) {
        return path;
    }

#if defined(__APPLE__)
    return "/Library/Application Support/ClaudeCode";
#elif defined(_WIN32)
    return "C:\\Program Files\\ClaudeCode";
#else
    return "/etc/claude-code";
#endif
}

std::string managed_settings_path() {
    return cc::path_join(managed_settings_dir(), "managed-settings.json");
}

std::string managed_settings_drop_in_dir() {
    return cc::path_join(managed_settings_dir(), "managed-settings.d");
}

boost::json::object load_managed_settings() {
    boost::json::object merged;

    // The base file comes first.
    if (const auto contents = cc::read_file(managed_settings_path())) {
        const boost::json::value parsed = cc::json_parse(*contents, boost::json::value(nullptr));
        if (parsed.is_object()) merge_settings(merged, parsed.as_object());
    }

    // The drop-ins are merged on top of it, in the order of their names.
    std::vector<std::string> names;
    for (const auto& name : cc::list_dir(managed_settings_drop_in_dir())) {
        if (name.empty() || name[0] == '.') continue;
        if (!cc::ends_with(name, ".json")) continue;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());

    for (const auto& name : names) {
        const auto contents = cc::read_file(cc::path_join(managed_settings_drop_in_dir(), name));
        if (!contents) continue;
        const boost::json::value parsed = cc::json_parse(*contents, boost::json::value(nullptr));
        if (parsed.is_object()) merge_settings(merged, parsed.as_object());
    }

    return merged;
}

} // namespace cc::config
