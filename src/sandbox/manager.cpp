#include "sandbox/manager.hpp"

#include "config/settings.hpp"
#include "permissions/rule_parser.hpp"
#include "permissions/shell_rules.hpp"
#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cstdlib>
#include <regex>
#include <set>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace cc::sandbox {
namespace {

/// True when the name of the variable is one of the ones that are never
/// stripped.
///
/// The reference keeps the variables that name a library out of the way of
/// the search, so that `LD_PRELOAD=…` is not mistaken for a command.
bool is_binary_hijack_var(std::string_view name) {
    return cc::starts_with(name, "LD_") || cc::starts_with(name, "DYLD_") || name == "PATH";
}

/// The directory of the temporary files of Claude.
///
/// The reference names it after the user, so that two users of the same
/// machine do not write over each other's files.
std::string claude_temp_dir() {
    const char* tmpdir = std::getenv("TMPDIR");
    std::string directory = tmpdir != nullptr && *tmpdir != '\0' ? tmpdir : "/tmp";
    while (!directory.empty() && directory.back() == '/') directory.pop_back();

#ifdef _WIN32
    const std::string name = "claude";
#else
    const std::string name = "claude-" + std::to_string(static_cast<long>(::getuid()));
#endif
    return directory + "/" + name;
}

/// The directory of the settings that the policy holds.
std::string managed_settings_drop_in_dir() {
#ifdef _WIN32
    return "C:\\Program Files\\ClaudeCode\\managed-settings.d";
#elif defined(__APPLE__)
    return "/Library/Application Support/ClaudeCode/managed-settings.d";
#else
    return "/etc/claude-code/managed-settings.d";
#endif
}

/// True when the machine has the program of the name in its path.
bool has_program(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (path == nullptr) return false;

    for (const std::string& directory : cc::split(path, ':')) {
        if (directory.empty()) continue;
        const std::string candidate = directory + "/" + name;
        if (cc::file_exists(candidate)) return true;
    }
    return false;
}

/// The path of a rule of the permissions, as the sandbox takes it.
std::string rule_path(const PermissionRuleValue& value) {
    if (!value.rule_content.has_value()) return {};
    std::string path = *value.rule_content;
    // A path that starts with a slash is relative to the folder of the
    // settings; the reference keeps the path as it is written.
    if (path.size() > 1 && path.front() == '/' && path[1] == '/') path.erase(0, 1);
    return path;
}

/// The names of the hosts a rule of the permissions names.
std::string rule_domain(const PermissionRuleValue& value) {
    if (!value.rule_content.has_value()) return {};
    const std::string& content = *value.rule_content;
    constexpr std::string_view kPrefix = "domain:";
    if (!cc::starts_with(content, kPrefix)) return {};
    return content.substr(kPrefix.size());
}

/// Adds the paths of a rule to a list, when the rule names one.
void add_rule_path(std::vector<std::string>& paths, const PermissionRuleValue& value) {
    const std::string path = rule_path(value);
    if (!path.empty()) paths.push_back(path);
}

/// The paths of the files of a bare repository, which may not be written.
const std::vector<std::string>& bare_git_repo_files() {
    static const std::vector<std::string> files = {"HEAD", "objects", "refs", "hooks", "config"};
    return files;
}

} // namespace

Manager::Manager(Settings settings) : settings_(std::move(settings)) {}

bool Manager::is_supported_platform() {
#ifdef _WIN32
    return false;
#else
    // The reference runs the sandbox on macOS, on Linux and on WSL2.
    return true;
#endif
}

std::string Manager::platform() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

bool Manager::is_platform_in_enabled_list() const {
    // The reference lets the sandbox run everywhere when the settings do not
    // name the platforms, and nowhere when they name an empty list.
    if (!settings_.enabled_platforms_set) return true;
    if (settings_.enabled_platforms.empty()) return false;

    const std::string current = platform();
    for (const std::string& name : settings_.enabled_platforms) {
        if (cc::to_lower(name) == current) return true;
    }
    return false;
}

bool Manager::is_sandboxing_enabled() const {
    if (!is_supported_platform()) return false;
    if (!dependency_errors().empty()) return false;
    if (!is_platform_in_enabled_list()) return false;
    return settings_.enabled;
}

std::vector<std::string> Manager::dependency_errors() const {
    std::vector<std::string> errors;

#ifndef _WIN32
    if (platform() != "macos") {
        // The seatbelt of macOS is part of the system; the other systems
        // need the tools of the runtime, and the sandbox cannot run
        // without them.
        if (!has_program("bwrap")) {
            errors.emplace_back("bubblewrap (bwrap) not installed");
        }
        if (!has_program("socat")) {
            errors.emplace_back("socat not installed");
        }
    }
#endif

    return errors;
}

std::vector<std::string> Manager::dependency_warnings() const {
    std::vector<std::string> warnings;

    // The search of the files needs ripgrep; the sandbox runs without it.
    if (!has_program("rg")) {
        warnings.emplace_back("ripgrep (rg) not found");
    }

#ifndef _WIN32
    if (platform() != "macos" && !has_program("seccomp")) {
        warnings.emplace_back(
            "seccomp filter not installed (required to block unix domain sockets)");
    }
#endif

    return warnings;
}

bool Manager::cannot_block_unix_sockets() const {
    if (settings_.network.allow_all_unix_sockets) return false;

    for (const std::string& warning : dependency_warnings()) {
        if (warning.find("seccomp") != std::string::npos) return true;
    }
    return false;
}

void Manager::set_permission_rules(std::vector<std::string> allow_rules,
                                   std::vector<std::string> deny_rules) {
    allow_rules_ = std::move(allow_rules);
    deny_rules_ = std::move(deny_rules);
}

FsReadConfig Manager::get_fs_read_config() const {
    FsReadConfig config;

    for (const std::string& path : settings_.filesystem.deny_read) {
        config.deny_only.push_back(path);
    }
    for (const std::string& path : settings_.filesystem.allow_read) {
        config.allow_within_deny.push_back(path);
    }

    // The rules of the permissions that deny a read are paths that may not
    // be read inside the sandbox either.
    for (const std::string& rule : deny_rules_) {
        const PermissionRuleValue value = permissions::permission_rule_value_from_string(rule);
        if (value.tool_name != "Read") continue;
        add_rule_path(config.deny_only, value);
    }

    return config;
}

FsWriteConfig Manager::get_fs_write_config() const {
    FsWriteConfig config;

    // The working directory and the directory of the temporary files of
    // Claude may always be written, the way the reference allows them.
    config.allow_only.push_back(".");
    config.allow_only.push_back(claude_temp_dir());

    for (const std::string& path : settings_.filesystem.allow_write) {
        config.allow_only.push_back(path);
    }

    // The settings of the session may never be written, or a command in the
    // sandbox could change the rules of the session.
    config.deny_within_allow.push_back(config::user_settings_path());
    if (!working_dir_.empty()) {
        config.deny_within_allow.push_back(config::project_settings_path(working_dir_));
        config.deny_within_allow.push_back(config::local_settings_path(working_dir_));
        config.deny_within_allow.push_back(
            cc::path_join(cc::path_join(working_dir_, ".claude"), "skills"));
    }
    config.deny_within_allow.push_back(managed_settings_drop_in_dir());

    // The files of a bare repository let a command escape the sandbox.
    const std::string root = working_dir_.empty() ? cc::cwd() : working_dir_;
    for (const std::string& file : bare_git_repo_files()) {
        config.deny_within_allow.push_back(cc::path_join(root, file));
    }

    for (const std::string& path : settings_.filesystem.deny_write) {
        config.deny_within_allow.push_back(path);
    }

    // The rules of the permissions that allow a write are paths that may be
    // written inside the sandbox, and the ones that deny it are paths that
    // may not.
    for (const std::string& rule : allow_rules_) {
        const PermissionRuleValue value = permissions::permission_rule_value_from_string(rule);
        if (value.tool_name != "Edit") continue;
        add_rule_path(config.allow_only, value);
    }
    for (const std::string& rule : deny_rules_) {
        const PermissionRuleValue value = permissions::permission_rule_value_from_string(rule);
        if (value.tool_name != "Edit") continue;
        add_rule_path(config.deny_within_allow, value);
    }

    return config;
}

NetworkRestrictionConfig Manager::get_network_restriction_config() const {
    NetworkRestrictionConfig config;

    for (const std::string& domain : settings_.network.allowed_domains) {
        config.allowed_hosts.push_back(domain);
    }
    for (const std::string& domain : settings_.network.denied_domains) {
        config.denied_hosts.push_back(domain);
    }

    // The rules of the permissions that name a host are hosts of the
    // sandbox too.
    for (const std::string& rule : allow_rules_) {
        const PermissionRuleValue value = permissions::permission_rule_value_from_string(rule);
        if (value.tool_name != "WebFetch") continue;
        const std::string domain = rule_domain(value);
        if (!domain.empty()) config.allowed_hosts.push_back(domain);
    }
    for (const std::string& rule : deny_rules_) {
        const PermissionRuleValue value = permissions::permission_rule_value_from_string(rule);
        if (value.tool_name != "WebFetch") continue;
        const std::string domain = rule_domain(value);
        if (!domain.empty()) config.denied_hosts.push_back(domain);
    }

    return config;
}

std::vector<std::string> Manager::get_allow_unix_sockets() const {
    return settings_.network.allow_unix_sockets;
}

std::vector<std::string> Manager::get_linux_glob_pattern_warnings() const {
    std::vector<std::string> warnings;

    // The wildcards of the paths are only a problem on the systems whose
    // runtime does not know them.
    if (platform() != "linux") return warnings;
    if (!settings_.enabled) return warnings;

    const auto has_globs = [](const std::string& path) {
        std::string stripped = path;
        if (cc::ends_with(stripped, "/**")) stripped = stripped.substr(0, stripped.size() - 3);
        return stripped.find_first_of("*?[") != std::string::npos;
    };

    const auto collect = [&](const std::vector<std::string>& rules) {
        for (const std::string& rule : rules) {
            const PermissionRuleValue value = permissions::permission_rule_value_from_string(rule);
            if (value.tool_name != "Edit" && value.tool_name != "Read") continue;
            if (!value.rule_content.has_value()) continue;
            if (!has_globs(*value.rule_content)) continue;
            warnings.push_back(rule);
        }
    };
    collect(allow_rules_);
    collect(deny_rules_);

    return warnings;
}

bool Manager::contains_excluded_command(const std::string& command) const {
    if (settings_.excluded_commands.empty()) return false;

    for (const std::string& part : permissions::split_command(command)) {
        const std::string trimmed = cc::trim(part);
        if (trimmed.empty()) continue;

        // The variables of the environment and the wrappers are stripped
        // again and again, until nothing new comes out, so that
        // `FOO=bar bazel …` and `timeout 30 bazel …` match the pattern
        // `bazel:*`.
        std::vector<std::string> candidates = {trimmed};
        std::set<std::string> seen = {trimmed};
        size_t index = 0;
        while (index < candidates.size()) {
            const size_t end = candidates.size();
            for (size_t i = index; i < end; ++i) {
                // The candidate is copied, because the list may grow under
                // the feet of the loop.
                const std::string candidate = candidates[i];

                const std::string without_env =
                    permissions::strip_all_leading_env_vars(candidate, is_binary_hijack_var);
                if (seen.insert(without_env).second) candidates.push_back(without_env);

                const std::string without_wrapper = permissions::strip_wrappers_only(candidate);
                if (seen.insert(without_wrapper).second) candidates.push_back(without_wrapper);
            }
            index = end;
        }

        for (const std::string& pattern : settings_.excluded_commands) {
            const std::optional<std::string> prefix = permissions::extract_prefix(pattern);
            if (prefix.has_value()) {
                for (const std::string& candidate : candidates) {
                    if (candidate == *prefix ||
                        cc::starts_with(candidate, *prefix + " ")) {
                        return true;
                    }
                }
                continue;
            }
            if (permissions::has_wildcards(pattern)) {
                for (const std::string& candidate : candidates) {
                    if (permissions::matches_wildcard(pattern, candidate)) return true;
                }
                continue;
            }
            for (const std::string& candidate : candidates) {
                if (candidate == pattern) return true;
            }
        }
    }

    return false;
}

bool Manager::should_use_sandbox(const std::string& command, bool dangerously_disable) const {
    if (!is_sandboxing_enabled()) return false;

    // The user may ask for the command to run outside the sandbox, when the
    // policy allows it.
    if (dangerously_disable && are_unsandboxed_commands_allowed()) return false;

    if (command.empty()) return false;

    // The excluded commands run outside the sandbox.
    if (contains_excluded_command(command)) return false;

    return true;
}

namespace {

/// The values of the settings of the sandbox, as the prompt shows them.
boost::json::value restrictions_json(const Manager& manager) {
    boost::json::object filesystem;

    const FsReadConfig read = manager.get_fs_read_config();
    boost::json::object read_node;
    if (!read.deny_only.empty()) {
        boost::json::array denied;
        for (const std::string& path : read.deny_only) denied.emplace_back(path);
        read_node["denyOnly"] = std::move(denied);
    }
    if (!read.allow_within_deny.empty()) {
        boost::json::array allowed;
        for (const std::string& path : read.allow_within_deny) allowed.emplace_back(path);
        read_node["allowWithinDeny"] = std::move(allowed);
    }
    filesystem["read"] = std::move(read_node);

    const FsWriteConfig write = manager.get_fs_write_config();
    boost::json::object write_node;
    if (!write.allow_only.empty()) {
        // The directory of the temporary files is written as `$TMPDIR`, so
        // that the prompt is the same for every user.
        const std::string temp_dir = claude_temp_dir();
        boost::json::array allowed;
        for (const std::string& path : write.allow_only) {
            allowed.emplace_back(path == temp_dir ? "$TMPDIR" : path);
        }
        write_node["allowOnly"] = std::move(allowed);
    }
    if (!write.deny_within_allow.empty()) {
        boost::json::array denied;
        for (const std::string& path : write.deny_within_allow) denied.emplace_back(path);
        write_node["denyWithinAllow"] = std::move(denied);
    }
    filesystem["write"] = std::move(write_node);

    boost::json::object network;
    const NetworkRestrictionConfig hosts = manager.get_network_restriction_config();
    if (!hosts.allowed_hosts.empty()) {
        boost::json::array allowed;
        for (const std::string& host : hosts.allowed_hosts) allowed.emplace_back(host);
        network["allowedHosts"] = std::move(allowed);
    }
    if (!hosts.denied_hosts.empty()) {
        boost::json::array denied;
        for (const std::string& host : hosts.denied_hosts) denied.emplace_back(host);
        network["deniedHosts"] = std::move(denied);
    }
    const std::vector<std::string> sockets = manager.get_allow_unix_sockets();
    if (!sockets.empty()) {
        boost::json::array array;
        for (const std::string& socket : sockets) array.emplace_back(socket);
        network["allowUnixSockets"] = std::move(array);
    }

    boost::json::object root;
    root["filesystem"] = std::move(filesystem);
    if (!network.empty()) root["network"] = std::move(network);

    const auto& violations = manager.settings().ignore_violations;
    if (!violations.empty()) {
        boost::json::object ignored;
        for (const auto& [kind, entries] : violations) {
            boost::json::array array;
            for (const std::string& entry : entries) array.emplace_back(entry);
            ignored[kind] = std::move(array);
        }
        root["ignoreViolations"] = std::move(ignored);
    }

    return boost::json::value(std::move(root));
}

/// The bullets of the section of the sandbox.
///
/// The reference writes one space in front of a bullet of the first level
/// and two in front of a bullet of a list inside a bullet.
std::vector<std::string> sandbox_bullets(const Manager& manager) {
    std::vector<std::string> lines;

    const auto bullet = [&lines](const std::string& text) { lines.push_back(" - " + text); };
    const auto sub_bullet = [&lines](const std::string& text) { lines.push_back("  - " + text); };

    if (manager.are_unsandboxed_commands_allowed()) {
        bullet(
            "You should always default to running commands within the sandbox. Do NOT attempt to "
            "set `dangerouslyDisableSandbox: true` unless:");
        sub_bullet("The user *explicitly* asks you to bypass sandbox");
        sub_bullet(
            "A specific command just failed and you see evidence of sandbox restrictions causing "
            "the failure. Note that commands can fail for many reasons unrelated to the sandbox "
            "(missing files, wrong arguments, network issues, etc.).");
        bullet("Evidence of sandbox-caused failures includes:");
        sub_bullet("\"Operation not permitted\" errors for file/network operations");
        sub_bullet("Access denied to specific paths outside allowed directories");
        sub_bullet("Network connection failures to non-whitelisted hosts");
        sub_bullet("Unix socket connection errors");
        bullet("When you see evidence of sandbox-caused failure:");
        sub_bullet("Immediately retry with `dangerouslyDisableSandbox: true` (don't ask, just do it)");
        sub_bullet(
            "Briefly explain what sandbox restriction likely caused the failure. Be sure to mention "
            "that the user can use the `/sandbox` command to manage restrictions.");
        sub_bullet("This will prompt the user for permission");
        bullet(
            "Treat each command you execute with `dangerouslyDisableSandbox: true` individually. "
            "Even if you have recently run a command with this setting, you should default to "
            "running future commands within the sandbox.");
        bullet(
            "Do not suggest adding sensitive paths like ~/.bashrc, ~/.zshrc, ~/.ssh/*, or "
            "credential files to the sandbox allowlist.");
    } else {
        bullet(
            "All commands MUST run in sandbox mode - the `dangerouslyDisableSandbox` parameter is "
            "disabled by policy.");
        bullet("Commands cannot run outside the sandbox under any circumstances.");
        bullet(
            "If a command fails due to sandbox restrictions, work with the user to adjust sandbox "
            "settings instead.");
    }

    bullet(
        "For temporary files, always use the `$TMPDIR` environment variable. TMPDIR is "
        "automatically set to the correct sandbox-writable directory in sandbox mode. Do NOT use "
        "`/tmp` directly - use `$TMPDIR` instead.");

    return lines;
}

} // namespace

std::vector<std::string> prompt_section(const Manager& manager) {
    // The section is empty when the sandbox is off, the way the reference
    // leaves it out.
    if (!manager.is_sandboxing_enabled()) return {};

    std::vector<std::string> lines;
    lines.push_back("## Command sandbox");
    lines.push_back(
        "By default, your command will be run in a sandbox. This sandbox controls which "
        "directories and network hosts commands may access or modify without an explicit "
        "override.");
    lines.push_back("");
    lines.push_back("The sandbox has the following restrictions:");
    lines.push_back(cc::json_dump(restrictions_json(manager)));
    lines.push_back("");
    for (const std::string& line : sandbox_bullets(manager)) {
        lines.push_back(line);
    }

    return lines;
}

} // namespace cc::sandbox
