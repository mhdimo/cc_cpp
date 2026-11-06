#include "ide.hpp"

#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/process.hpp"
#include "utils/string.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <string_view>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cc {
namespace {

/// The directory the IDEs leave their lock files in.
std::string lock_directory() { return cc::path_join(cc::claude_config_dir(), "ide"); }

/// True when the folder of the session is the folder of the IDE, or is in it.
bool folder_holds(const std::string& folder, const std::string& cwd) {
    if (folder.empty()) return false;

    const std::string resolved = cc::resolve(folder);
    return cwd == resolved || starts_with(cwd, resolved + "/");
}

/// True when something answers on the port.
bool port_answers(int port) {
    if (port <= 0) return false;

    // The port is only asked about; the connection is closed right away.
    const int handle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (handle < 0) return false;

    struct sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const bool connected =
        ::connect(handle, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0;
    ::close(handle);
    return connected;
}

} // namespace

std::vector<DetectedIde> detect_ides(const std::string& cwd) {
    std::vector<DetectedIde> ides;

    const std::string directory = lock_directory();
    if (!cc::is_directory(directory)) return ides;

    for (const auto& entry : cc::list_dir(directory)) {
        if (!ends_with(entry, ".lock")) continue;

        const std::string path = cc::path_join(directory, entry);
        const auto contents = cc::read_file(path);
        if (!contents) continue;

        DetectedIde ide;

        // The name of the file is the port the IDE listens on.
        const std::string port = entry.substr(0, entry.size() - 5);
        ide.port = std::atoi(port.c_str());
        if (ide.port <= 0) continue;

        const boost::json::value parsed = json_parse(*contents, boost::json::value(nullptr));
        if (parsed.is_object()) {
            const auto& object = parsed.as_object();
            ide.name = jget_string(object, "ideName");
            ide.url = jget_string(object, "url");
            ide.auth_token = jget_string(object, "authToken");
            for (const auto& folder : jget_array(object, "workspaceFolders")) {
                if (folder.is_string()) {
                    ide.workspace_folders.emplace_back(folder.as_string().c_str());
                }
            }
        } else {
            // The older format is a list of the folders of the IDE.
            for (const auto& line : cc::split_lines(*contents)) {
                const std::string folder = cc::trim(line);
                if (!folder.empty()) ide.workspace_folders.push_back(folder);
            }
        }

        if (ide.name.empty()) ide.name = "IDE";

        for (const auto& folder : ide.workspace_folders) {
            if (folder_holds(folder, cwd)) ide.matches_cwd = true;
        }

        ide.is_running = port_answers(ide.port);
        ides.push_back(std::move(ide));
    }

    // The IDEs of the folder of the session come first.
    std::sort(ides.begin(), ides.end(), [](const DetectedIde& a, const DetectedIde& b) {
        if (a.matches_cwd != b.matches_cwd) return a.matches_cwd;
        return a.port < b.port;
    });
    return ides;
}

namespace {

/// The IDEs of the family of VS Code that the reference knows.
constexpr std::string_view kVsCodeIdes[] = {"vscode", "cursor", "windsurf"};

/// The IDEs of JetBrains that the reference knows.
constexpr std::string_view kJetBrainsIdes[] = {
    "intellij", "pycharm",     "webstorm", "phpstorm", "rubymine", "clion",
    "goland",   "rider",       "datagrip", "appcode",  "dataspell", "aqua",
    "gateway",  "fleet",       "androidstudio",
};

/// The bundles of the IDEs of JetBrains, and the name that goes with them.
struct JetBrainsBundle {
    /// The name of the IDE.
    std::string_view name;
    /// The word the bundle of the application carries.
    std::string_view keyword;
};

constexpr JetBrainsBundle kJetBrainsBundles[] = {
    {"intellij", "com.jetbrains.intellij"},   {"pycharm", "com.jetbrains.pycharm"},
    {"webstorm", "com.jetbrains.webstorm"},   {"phpstorm", "com.jetbrains.phpstorm"},
    {"rubymine", "com.jetbrains.rubymine"},   {"clion", "com.jetbrains.clion"},
    {"goland", "com.jetbrains.goland"},       {"rider", "com.jetbrains.rider"},
    {"datagrip", "com.jetbrains.datagrip"},   {"appcode", "com.jetbrains.appcode"},
    {"dataspell", "com.jetbrains.dataspell"},
};

/// Reads a variable of the environment.
std::string environment(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) return {};
    return cc::trim(value);
}

/// The kind of the terminal the session runs in.
///
/// The reference reads it from what the IDE leaves in the environment.
std::string detect_terminal() {
    // The trace of Cursor.
    if (std::getenv("CURSOR_TRACE_ID") != nullptr) return "cursor";

    // The askpass of the family of VS Code tells which IDE it is. The name
    // of the IDE is written with a capital letter in the path of it, so the
    // path is read in small letters.
    const std::string askpass = cc::to_lower(environment("VSCODE_GIT_ASKPASS_MAIN"));
    if (askpass.find("cursor") != std::string::npos) return "cursor";
    if (askpass.find("windsurf") != std::string::npos) return "windsurf";
    if (askpass.find("antigravity") != std::string::npos) return "antigravity";

    // The bundle of the application, on macOS.
    const std::string bundle = cc::to_lower(environment("__CFBundleIdentifier"));
    if (!bundle.empty()) {
        if (bundle.find("vscodium") != std::string::npos) return "codium";
        if (bundle.find("windsurf") != std::string::npos) return "windsurf";
        if (bundle.find("com.google.android.studio") != std::string::npos) return "androidstudio";
        for (const auto& entry : kJetBrainsBundles) {
            if (bundle.find(entry.keyword) != std::string::npos) return std::string(entry.name);
        }
    }

    // The name of the program of the terminal.
    const std::string program = cc::to_lower(environment("TERM_PROGRAM"));
    if (program.find("vscode") != std::string::npos) return "vscode";
    if (program.find("cursor") != std::string::npos) return "cursor";
    if (program.find("windsurf") != std::string::npos) return "windsurf";
    if (program.find("antigravity") != std::string::npos) return "antigravity";

    // The terminal of the IDEs of JetBrains.
    if (environment("TERMINAL_EMULATOR") == "JetBrains-JediTerm") return "pycharm";

    return {};
}

/// The numbers of a version.
///
/// The version of the extension of the IDE is three numbers, and whatever
/// follows them.
std::vector<int> version_numbers(const std::string& version) {
    std::vector<int> numbers;
    std::string current;
    for (const char c : version) {
        if (c == '.' || c == '-' || c == '+') {
            if (!current.empty()) numbers.push_back(std::atoi(current.c_str()));
            current.clear();
            continue;
        }
        if (c < '0' || c > '9') break;
        current += c;
    }
    if (!current.empty()) numbers.push_back(std::atoi(current.c_str()));
    return numbers;
}

} // namespace

std::string terminal_ide_type() {
    const std::string type = detect_terminal();

    for (const auto& ide : kVsCodeIdes) {
        if (type == ide) return type;
    }
    for (const auto& ide : kJetBrainsIdes) {
        if (type == ide) return type;
    }

    return {};
}

bool is_vscode_ide(const std::string& ide_type) {
    for (const auto& ide : kVsCodeIdes) {
        if (ide_type == ide) return true;
    }
    return false;
}

std::string vscode_command(const std::string& ide_type) {
    if (ide_type == "vscode") return "code";
    if (ide_type == "cursor") return "cursor";
    if (ide_type == "windsurf") return "windsurf";
    return {};
}

std::optional<std::string> installed_extension_version(const std::string& command) {
    if (command.empty()) return std::nullopt;

    const ProcessOutcome outcome =
        run_process(command + " --list-extensions --show-versions", {}, {}, 30);
    if (outcome.failed) return std::nullopt;

    // The answer is a line per extension: the name of the extension, an `@`
    // and the version of it.
    for (const std::string& line : cc::split_lines(outcome.out)) {
        const size_t at = line.find('@');
        if (at == std::string::npos) continue;
        if (line.substr(0, at) != kExtensionId) continue;

        const std::string version = cc::trim(line.substr(at + 1));
        if (version.empty()) return std::nullopt;
        return version;
    }

    return std::nullopt;
}

bool install_extension(const std::string& command) {
    if (command.empty()) return false;

    const ProcessOutcome outcome = run_process(
        command + " --force --install-extension " + std::string(kExtensionId), {}, {}, 300);
    return !outcome.failed && outcome.exit_code == 0;
}

bool older_version(const std::string& older, const std::string& newer) {
    const std::vector<int> left = version_numbers(older);
    const std::vector<int> right = version_numbers(newer);

    for (size_t i = 0; i < std::max(left.size(), right.size()); ++i) {
        const int a = i < left.size() ? left[i] : 0;
        const int b = i < right.size() ? right[i] : 0;
        if (a != b) return a < b;
    }

    return false;
}

} // namespace cc
