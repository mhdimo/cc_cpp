#pragma once

#include <boost/json.hpp>

#include <map>
#include <string>
#include <vector>

namespace cc::sandbox {

/// The network rules of the sandbox.
struct Network {
    /// The hosts the commands may talk to.
    std::vector<std::string> allowed_domains;

    /// The hosts the commands may not talk to.
    std::vector<std::string> denied_domains;

    /// The sockets the commands may use; macOS only.
    std::vector<std::string> allow_unix_sockets;

    /// True when every socket may be used.
    bool allow_all_unix_sockets = false;

    /// True when the commands may listen on a port of their own.
    bool allow_local_binding = false;

    /// The port of the proxy the HTTP traffic goes through.
    int http_proxy_port = 0;

    /// The port of the proxy the SOCKS traffic goes through.
    int socks_proxy_port = 0;

    /// True when only the domains of the managed settings are respected.
    ///
    /// The reference reads the key from the settings of the policy; the port
    /// has no scope of its own, so the key is only kept.
    bool allow_managed_domains_only = false;
};

/// The rules of the filesystem the sandbox holds.
struct Filesystem {
    /// The paths the commands may write to.
    std::vector<std::string> allow_write;

    /// The paths the commands may not write to.
    std::vector<std::string> deny_write;

    /// The paths the commands may not read.
    std::vector<std::string> deny_read;

    /// The paths that may be read even though a rule denies them.
    std::vector<std::string> allow_read;

    /// True when only the paths of the managed settings may be read.
    ///
    /// The reference reads the key from the settings of the policy; the port
    /// has no scope of its own, so the key is only kept.
    bool allow_managed_read_paths_only = false;
};

/// The settings of the sandbox.
///
/// The reference keeps them in the `sandbox` key of the settings; the
/// fields are the ones of its schema.
struct Settings {
    /// True when the commands run in the sandbox.
    bool enabled = false;

    /// True when the session stops when the sandbox cannot be started.
    bool fail_if_unavailable = false;

    /// True when the commands that run in the sandbox need no permission.
    ///
    /// The reference reads the key with a default of true.
    bool auto_allow_bash_if_sandboxed = true;

    /// True when a command may ask to run outside the sandbox.
    bool allow_unsandboxed_commands = true;

    /// The commands that run outside the sandbox.
    std::vector<std::string> excluded_commands;

    /// The platforms the sandbox runs on.
    ///
    /// The key is not in the schema of the reference; it restricts the
    /// sandbox to the platforms it names. An empty list means that the
    /// sandbox runs on none of them.
    std::vector<std::string> enabled_platforms;

    /// True when the settings name the platforms at all.
    ///
    /// The reference tells a list that is not there from a list that is empty:
    /// the first lets the sandbox run everywhere, the second nowhere.
    bool enabled_platforms_set = false;

    /// The command of ripgrep, when the settings name one.
    std::string ripgrep_command;

    /// The arguments of the command of ripgrep.
    std::vector<std::string> ripgrep_args;

    /// The network rules.
    Network network;

    /// The rules of the filesystem.
    Filesystem filesystem;

    /// The violations that are not reported, by the kind of violation.
    std::map<std::string, std::vector<std::string>> ignore_violations;

    /// True when the trust of the machine is allowed inside the sandbox.
    ///
    /// The reference needs it for the tools of Go, which check the
    /// certificates of the servers they talk to.
    bool enable_weaker_nested_sandbox = false;

    /// True when the network is not isolated as much.
    bool enable_weaker_network_isolation = false;
};

/// Reads the settings of the sandbox out of the `sandbox` key.
///
/// The key is read as it is written in the settings of the user, the way
/// the reference reads it.
Settings parse_settings(const boost::json::value& value);

/// The settings of the sandbox, as they are written in the settings file.
boost::json::value to_json(const Settings& settings);

/// Applies the changes of the panel to the settings that are already there.
///
/// The reference writes the keys the panel changed into the object that is
/// in the file and leaves every other key alone; the object that comes back
/// is the one that is written.
boost::json::value apply_changes(const boost::json::value& existing, const Settings& changed);

/// Adds a command to the excluded commands of the settings that are there.
///
/// The reference adds the command to the list of the local settings and
/// leaves the list alone when the command is already in it.
boost::json::value add_excluded_command(const boost::json::value& existing,
                                         const std::string& pattern);

} // namespace cc::sandbox
