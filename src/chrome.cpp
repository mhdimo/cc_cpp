#include "chrome.hpp"

#include "utils/fs.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <cstdlib>
#include <string>
#include <vector>

namespace cc::chrome {
namespace {

/// The extension of the browser, and the ones of the builds of it that are
/// not the one of the users.
constexpr std::string_view kExtensionId = "fcoeoabgfenejglbffodgkkbkcdhcgfn";
constexpr std::string_view kDevExtensionId = "dihbgbndebgnbjfmelmegjepbnkhlgni";
constexpr std::string_view kAntExtensionId = "dngcpimnedloihjnnfngkgjoidhnaolf";

/// One browser and the folder its profiles are in.
struct Browser {
    /// The name of the browser.
    std::string name;

    /// The parts of the path of the folder, from the home of the user.
    std::vector<std::string> path;
};

/// The browsers that are looked at, in the order the reference looks at
/// them.
std::vector<Browser> browsers() {
#if defined(__APPLE__)
    return {
        {"Google Chrome", {"Library", "Application Support", "Google", "Chrome"}},
        {"Brave", {"Library", "Application Support", "BraveSoftware", "Brave-Browser"}},
        {"Arc", {"Library", "Application Support", "Arc", "User Data"}},
        {"Microsoft Edge", {"Library", "Application Support", "Microsoft Edge"}},
        {"Chromium", {"Library", "Application Support", "Chromium"}},
        {"Vivaldi", {"Library", "Application Support", "Vivaldi"}},
        {"Opera", {"Library", "Application Support", "com.operasoftware.Opera"}},
    };
#else
    return {
        {"Google Chrome", {".config", "google-chrome"}},
        {"Brave", {".config", "BraveSoftware", "Brave-Browser"}},
        {"Microsoft Edge", {".config", "microsoft-edge"}},
        {"Chromium", {".config", "chromium"}},
        {"Vivaldi", {".config", "vivaldi"}},
        {"Opera", {".config", "opera"}},
    };
#endif
}

/// The ids of the extension that are looked for.
std::vector<std::string> extension_ids() {
    std::vector<std::string> ids{std::string(kExtensionId)};
    const char* user_type = std::getenv("USER_TYPE");
    if (user_type != nullptr && std::string(user_type) == "ant") {
        ids.emplace_back(kDevExtensionId);
        ids.emplace_back(kAntExtensionId);
    }
    return ids;
}

} // namespace

bool extension_installed() { return !extension_browser().empty(); }

std::string extension_browser() {
    const std::string home = cc::home_dir();
    if (home.empty()) return {};

    for (const Browser& browser : browsers()) {
        std::string base = home;
        for (const std::string& part : browser.path) base = cc::path_join(base, part);

        for (const std::string& entry : cc::list_dir(base)) {
            const bool is_profile = entry == "Default" || cc::starts_with(entry, "Profile ");
            if (!is_profile) continue;

            for (const std::string& id : extension_ids()) {
                const std::string folder =
                    cc::path_join(cc::path_join(cc::path_join(base, entry), "Extensions"), id);
                if (cc::is_directory(folder)) return browser.name;
            }
        }
    }

    return {};
}

} // namespace cc::chrome
