#include "desktop.hpp"

#include "utils/fs.hpp"
#include "utils/path.hpp"
#include "utils/process.hpp"
#include "utils/string.hpp"

namespace cc::desktop {
namespace {

/// The folder the application is in, on macOS.
constexpr std::string_view kMacApplicationPath = "/Applications/Claude.app";

/// The file the version of the application is in, on macOS.
constexpr std::string_view kMacVersionCommand =
    "defaults read /Applications/Claude.app/Contents/Info.plist CFBundleShortVersionString";

/// Compares two versions of the form `1.2.3`.
///
/// A version that cannot be read is not older than the one that is asked
/// for, the way the reference treats it.
bool version_is_at_least(const std::string& version, const std::string& wanted) {
    if (version.empty()) return true;

    const std::vector<std::string> parts = cc::split(version, '.');
    const std::vector<std::string> wanted_parts = cc::split(wanted, '.');

    for (size_t i = 0; i < wanted_parts.size(); ++i) {
        const int64_t left = i < parts.size() ? cc::parse_int(parts[i]).value_or(0) : 0;
        const int64_t right = cc::parse_int(wanted_parts[i]).value_or(0);
        if (left != right) return left > right;
    }
    return true;
}

/// True when the application is on this machine.
bool is_installed() {
#if defined(__APPLE__)
    return cc::is_directory(std::string(kMacApplicationPath));
#elif defined(__linux__)
    const cc::ProcessOutcome outcome =
        cc::run_process("xdg-mime query default x-scheme-handler/claude", cc::cwd(), "", 10);
    return outcome.exit_code == 0 && !cc::trim(outcome.out).empty();
#else
    return false;
#endif
}

/// The version of the application, when it can be read.
std::string version() {
#if defined(__APPLE__)
    const cc::ProcessOutcome outcome = cc::run_process(std::string(kMacVersionCommand), cc::cwd(),
                                                       "", 10);
    if (outcome.exit_code != 0) return {};
    return cc::trim(outcome.out);
#else
    return {};
#endif
}

/// Escapes a value so that it may be put in an address.
std::string escape(const std::string& value) {
    static const std::string_view safe = "abcdefghijklmnopqrstuvwxyz"
                                         "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                         "0123456789-_.~";
    std::string out;
    out.reserve(value.size());
    for (const unsigned char c : value) {
        if (safe.find(static_cast<char>(c)) != std::string_view::npos) {
            out += static_cast<char>(c);
        } else {
            out += "%";
            out += "0123456789ABCDEF"[(c >> 4) & 0x0F];
            out += "0123456789ABCDEF"[c & 0x0F];
        }
    }
    return out;
}

} // namespace

Status install_status() {
    Status status;
    if (!is_installed()) {
        status.status = InstallStatus::NotInstalled;
        return status;
    }

    const std::string found = version();
    if (found.empty()) {
        // The version cannot be read; the application is used anyway.
        status.status = InstallStatus::Ready;
        status.version = "unknown";
        return status;
    }

    status.version = found;
    if (!version_is_at_least(found, std::string(kMinimumVersion))) {
        status.status = InstallStatus::VersionTooOld;
        return status;
    }

    status.status = InstallStatus::Ready;
    return status;
}

std::string download_url() {
#if defined(__APPLE__)
    return "https://claude.ai/api/desktop/darwin/universal/dmg/latest/redirect";
#elif defined(_WIN32)
    return "https://claude.ai/api/desktop/win32/x64/exe/latest/redirect";
#else
    return "https://claude.ai/download";
#endif
}

std::string deep_link(const std::string& session_id, const std::string& cwd) {
    return "claude://resume?session=" + escape(session_id) + "&cwd=" + escape(cwd);
}

bool open_session(const std::string& session_id, const std::string& cwd) {
    const std::string url = deep_link(session_id, cwd);

#if defined(__APPLE__)
    const cc::ProcessOutcome outcome = cc::run_process("open \"" + url + "\"", cc::cwd(), "", 10);
    return outcome.exit_code == 0;
#elif defined(__linux__)
    const cc::ProcessOutcome outcome = cc::run_process("xdg-open \"" + url + "\"", cc::cwd(), "", 10);
    return outcome.exit_code == 0;
#elif defined(_WIN32)
    const cc::ProcessOutcome outcome =
        cc::run_process("cmd /c start \"\" \"" + url + "\"", cc::cwd(), "", 10);
    return outcome.exit_code == 0;
#else
    (void)url;
    return false;
#endif
}

} // namespace cc::desktop
