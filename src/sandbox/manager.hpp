#pragma once

#include "sandbox/settings.hpp"

#include <string>
#include <vector>

namespace cc::sandbox {

/// The paths the sandbox may not read.
struct FsReadConfig {
    /// The paths that may not be read.
    std::vector<std::string> deny_only;

    /// The paths that may be read even though a rule denies them.
    std::vector<std::string> allow_within_deny;
};

/// The paths the sandbox may write.
struct FsWriteConfig {
    /// The paths that may be written.
    std::vector<std::string> allow_only;

    /// The paths that may not be written even though a rule allows them.
    std::vector<std::string> deny_within_allow;
};

/// The hosts the sandbox may talk to.
struct NetworkRestrictionConfig {
    /// The hosts that may be reached.
    std::vector<std::string> allowed_hosts;

    /// The hosts that may not be reached.
    std::vector<std::string> denied_hosts;
};

/// The state of the sandbox of the session.
///
/// The reference keeps it in a manager of its own; the port keeps the
/// settings and the decisions about them in one place.
class Manager {
public:
    /// The manager of the sandbox, with the settings that are read from the
    /// settings of the user.
    explicit Manager(Settings settings = {});

    /// The settings of the sandbox.
    const Settings& settings() const { return settings_; }

    /// Replaces the settings of the sandbox.
    void set_settings(Settings settings) { settings_ = std::move(settings); }

    /// True when the commands run in the sandbox.
    ///
    /// The platform of the machine, the dependencies it needs and the list of
    /// the platforms the sandbox may run on all have a say in it, the way
    /// they have in the reference.
    bool is_sandboxing_enabled() const;

    /// True when the platform of the machine is one the sandbox may run on.
    ///
    /// The list is empty when the settings do not name it, which lets the
    /// sandbox run everywhere; a list that is there but empty lets it run
    /// nowhere.
    bool is_platform_in_enabled_list() const;

    /// True when the commands that run in the sandbox need no permission.
    bool is_auto_allow_bash_if_sandboxed_enabled() const {
        return settings_.auto_allow_bash_if_sandboxed;
    }

    /// True when a command may ask to run outside the sandbox.
    bool are_unsandboxed_commands_allowed() const {
        return settings_.allow_unsandboxed_commands;
    }

    /// The commands that run outside the sandbox.
    const std::vector<std::string>& excluded_commands() const {
        return settings_.excluded_commands;
    }

    /// True when the platform of the machine can run the sandbox.
    ///
    /// The reference runs it on macOS, on Linux and on WSL2.
    static bool is_supported_platform();

    /// The name of the platform of the machine, e.g. `macos`.
    static std::string platform();

    /// The errors of the dependencies of the sandbox.
    ///
    /// The reference checks for the runtime of Anthropic, which the port does
    /// not have; the seatbelt of macOS is used instead, so the check is for
    /// the tools that the sandbox needs.
    std::vector<std::string> dependency_errors() const;

    /// The warnings of the dependencies of the sandbox.
    std::vector<std::string> dependency_warnings() const;

    /// True when the sandbox cannot block the unix sockets.
    bool cannot_block_unix_sockets() const;

    /// Gives the manager the rules of the permissions of the session.
    ///
    /// The paths and the hosts they name are part of the sandbox, the way
    /// the reference merges them with the rules of the `sandbox` key.
    void set_permission_rules(std::vector<std::string> allow_rules,
                              std::vector<std::string> deny_rules);

    /// Tells the manager the folder the session runs in.
    void set_working_directory(std::string cwd) { working_dir_ = std::move(cwd); }

    /// The paths the sandbox may not read.
    FsReadConfig get_fs_read_config() const;

    /// The paths the sandbox may write.
    FsWriteConfig get_fs_write_config() const;

    /// The hosts the sandbox may talk to.
    NetworkRestrictionConfig get_network_restriction_config() const;

    /// The sockets the sandbox may use.
    std::vector<std::string> get_allow_unix_sockets() const;

    /// The patterns of the permissions that the sandbox cannot hold.
    ///
    /// The runtime of the reference does not know the wildcards of the
    /// paths on Linux, so the patterns are ignored there.
    std::vector<std::string> get_linux_glob_pattern_warnings() const;

    /// True when the settings of the sandbox are held by a policy.
    ///
    /// The port has no scope of its own for the policy, so the answer is
    /// always false.
    bool are_settings_locked_by_policy() const { return false; }

    /// True when the command runs in the sandbox.
    ///
    /// The command runs outside the sandbox when the sandbox is off, when the
    /// user asks for it and the policy allows it, or when the command is one
    /// of the excluded ones.
    bool should_use_sandbox(const std::string& command, bool dangerously_disable) const;

private:
    /// True when the command holds one of the excluded commands.
    bool contains_excluded_command(const std::string& command) const;

    Settings settings_;
    std::vector<std::string> allow_rules_;
    std::vector<std::string> deny_rules_;
    std::string working_dir_;
};

/// The section of the prompt of the `Bash` tool that describes the sandbox.
///
/// The section is empty when the sandbox is off, the way the reference
/// leaves it out.
std::vector<std::string> prompt_section(const Manager& manager);

} // namespace cc::sandbox
