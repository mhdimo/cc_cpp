#include "sandbox/settings.hpp"

#include "utils/json_util.hpp"

namespace cc::sandbox {
namespace {

/// The strings of an array, ignoring the entries that are not strings.
std::vector<std::string> strings_of(const boost::json::value& value) {
    std::vector<std::string> out;
    if (!value.is_array()) return out;
    for (const auto& item : value.as_array()) {
        if (item.is_string()) out.emplace_back(item.as_string().c_str());
    }
    return out;
}

} // namespace

Settings parse_settings(const boost::json::value& value) {
    Settings settings;
    if (!value.is_object()) return settings;

    const boost::json::object& object = value.as_object();

    settings.enabled = jget_bool(object, "enabled", false);
    settings.fail_if_unavailable = jget_bool(object, "failIfUnavailable", false);
    settings.auto_allow_bash_if_sandboxed =
        jget_bool(object, "autoAllowBashIfSandboxed", true);
    settings.allow_unsandboxed_commands = jget_bool(object, "allowUnsandboxedCommands", true);
    settings.enable_weaker_nested_sandbox = jget_bool(object, "enableWeakerNestedSandbox", false);
    settings.enable_weaker_network_isolation =
        jget_bool(object, "enableWeakerNetworkIsolation", false);
    settings.excluded_commands = strings_of(boost::json::value(jget_array(object, "excludedCommands")));
    settings.enabled_platforms = strings_of(boost::json::value(jget_array(object, "enabledPlatforms")));
    settings.enabled_platforms_set = object.find("enabledPlatforms") != object.end();

    const auto ripgrep = object.find("ripgrep");
    if (ripgrep != object.end() && ripgrep->value().is_object()) {
        const boost::json::object& inner = ripgrep->value().as_object();
        settings.ripgrep_command = jget_string(inner, "command");
        settings.ripgrep_args = strings_of(boost::json::value(jget_array(inner, "args")));
    }

    // The iterator of a member is compared with the end of the object: the
    // end is a pointer of its own, so it is never null.
    const auto network = object.find("network");
    if (network != object.end() && network->value().is_object()) {
        const boost::json::object& inner = network->value().as_object();
        settings.network.allowed_domains = strings_of(boost::json::value(jget_array(inner, "allowedDomains")));
        settings.network.denied_domains = strings_of(boost::json::value(jget_array(inner, "deniedDomains")));
        settings.network.allow_unix_sockets = strings_of(boost::json::value(jget_array(inner, "allowUnixSockets")));
        settings.network.allow_all_unix_sockets = jget_bool(inner, "allowAllUnixSockets", false);
        settings.network.allow_local_binding = jget_bool(inner, "allowLocalBinding", false);
        settings.network.http_proxy_port =
            static_cast<int>(jget_int(inner, "httpProxyPort", 0));
        settings.network.socks_proxy_port =
            static_cast<int>(jget_int(inner, "socksProxyPort", 0));
        settings.network.allow_managed_domains_only =
            jget_bool(inner, "allowManagedDomainsOnly", false);
    }

    const auto filesystem = object.find("filesystem");
    if (filesystem != object.end() && filesystem->value().is_object()) {
        const boost::json::object& inner = filesystem->value().as_object();
        settings.filesystem.allow_write = strings_of(boost::json::value(jget_array(inner, "allowWrite")));
        settings.filesystem.deny_write = strings_of(boost::json::value(jget_array(inner, "denyWrite")));
        settings.filesystem.deny_read = strings_of(boost::json::value(jget_array(inner, "denyRead")));
        settings.filesystem.allow_read = strings_of(boost::json::value(jget_array(inner, "allowRead")));
        settings.filesystem.allow_managed_read_paths_only =
            jget_bool(inner, "allowManagedReadPathsOnly", false);
    }

    const auto violations = object.find("ignoreViolations");
    if (violations != object.end() && violations->value().is_object()) {
        for (const auto& entry : violations->value().as_object()) {
            settings.ignore_violations[entry.key()] =
                strings_of(boost::json::value(entry.value()));
        }
    }

    return settings;
}

boost::json::value to_json(const Settings& settings) {
    boost::json::object object;

    // The reference writes the flags whatever their value is, so that a
    // choice of `false` is not inherited from the scope above.
    object["enabled"] = settings.enabled;
    object["failIfUnavailable"] = settings.fail_if_unavailable;
    object["autoAllowBashIfSandboxed"] = settings.auto_allow_bash_if_sandboxed;
    object["allowUnsandboxedCommands"] = settings.allow_unsandboxed_commands;
    if (settings.enable_weaker_nested_sandbox) object["enableWeakerNestedSandbox"] = true;
    if (settings.enable_weaker_network_isolation) {
        object["enableWeakerNetworkIsolation"] = true;
    }

    if (!settings.excluded_commands.empty()) {
        boost::json::array commands;
        for (const std::string& command : settings.excluded_commands) {
            commands.emplace_back(command);
        }
        object["excludedCommands"] = std::move(commands);
    }

    // The list of the platforms is written even when it is empty, because an
    // empty list means that the sandbox runs nowhere.
    if (settings.enabled_platforms_set || !settings.enabled_platforms.empty()) {
        boost::json::array platforms;
        for (const std::string& platform : settings.enabled_platforms) {
            platforms.emplace_back(platform);
        }
        object["enabledPlatforms"] = std::move(platforms);
    }

    if (!settings.ripgrep_command.empty()) {
        boost::json::object ripgrep;
        ripgrep["command"] = settings.ripgrep_command;
        if (!settings.ripgrep_args.empty()) {
            boost::json::array args;
            for (const std::string& arg : settings.ripgrep_args) {
                args.emplace_back(arg);
            }
            ripgrep["args"] = std::move(args);
        }
        object["ripgrep"] = std::move(ripgrep);
    }

    if (!settings.network.allowed_domains.empty() || !settings.network.denied_domains.empty() ||
        !settings.network.allow_unix_sockets.empty() || settings.network.allow_all_unix_sockets ||
        settings.network.allow_local_binding || settings.network.http_proxy_port > 0 ||
        settings.network.socks_proxy_port > 0 || settings.network.allow_managed_domains_only) {
        boost::json::object network;

        if (!settings.network.allowed_domains.empty()) {
            boost::json::array domains;
            for (const std::string& domain : settings.network.allowed_domains) {
                domains.emplace_back(domain);
            }
            network["allowedDomains"] = std::move(domains);
        }
        if (!settings.network.denied_domains.empty()) {
            boost::json::array domains;
            for (const std::string& domain : settings.network.denied_domains) {
                domains.emplace_back(domain);
            }
            network["deniedDomains"] = std::move(domains);
        }
        if (!settings.network.allow_unix_sockets.empty()) {
            boost::json::array sockets;
            for (const std::string& socket : settings.network.allow_unix_sockets) {
                sockets.emplace_back(socket);
            }
            network["allowUnixSockets"] = std::move(sockets);
        }
        if (settings.network.allow_all_unix_sockets) network["allowAllUnixSockets"] = true;
        if (settings.network.allow_local_binding) network["allowLocalBinding"] = true;
        if (settings.network.http_proxy_port > 0) {
            network["httpProxyPort"] = static_cast<int64_t>(settings.network.http_proxy_port);
        }
        if (settings.network.socks_proxy_port > 0) {
            network["socksProxyPort"] = static_cast<int64_t>(settings.network.socks_proxy_port);
        }
        if (settings.network.allow_managed_domains_only) {
            network["allowManagedDomainsOnly"] = true;
        }

        object["network"] = std::move(network);
    }

    if (!settings.filesystem.allow_write.empty() || !settings.filesystem.deny_write.empty() ||
        !settings.filesystem.deny_read.empty() || !settings.filesystem.allow_read.empty() ||
        settings.filesystem.allow_managed_read_paths_only) {
        boost::json::object filesystem;

        const auto write = [&filesystem](const std::vector<std::string>& paths, const char* key) {
            if (paths.empty()) return;
            boost::json::array array;
            for (const std::string& path : paths) array.emplace_back(path);
            filesystem[key] = std::move(array);
        };

        write(settings.filesystem.allow_write, "allowWrite");
        write(settings.filesystem.deny_write, "denyWrite");
        write(settings.filesystem.deny_read, "denyRead");
        write(settings.filesystem.allow_read, "allowRead");
        if (settings.filesystem.allow_managed_read_paths_only) {
            filesystem["allowManagedReadPathsOnly"] = true;
        }

        object["filesystem"] = std::move(filesystem);
    }

    if (!settings.ignore_violations.empty()) {
        boost::json::object violations;
        for (const auto& [kind, entries] : settings.ignore_violations) {
            boost::json::array array;
            for (const std::string& entry : entries) array.emplace_back(entry);
            violations[kind] = std::move(array);
        }
        object["ignoreViolations"] = std::move(violations);
    }

    return boost::json::value(std::move(object));
}

boost::json::value apply_changes(const boost::json::value& existing, const Settings& changed) {
    // The object that is already in the file is the base; every key the
    // panel does not touch stays as it is.
    boost::json::object object;
    if (existing.is_object()) object = existing.as_object();

    object["enabled"] = changed.enabled;
    object["autoAllowBashIfSandboxed"] = changed.auto_allow_bash_if_sandboxed;
    object["allowUnsandboxedCommands"] = changed.allow_unsandboxed_commands;

    return boost::json::value(std::move(object));
}

boost::json::value add_excluded_command(const boost::json::value& existing,
                                         const std::string& pattern) {
    boost::json::object object;
    if (existing.is_object()) object = existing.as_object();

    boost::json::array commands;
    if (const auto* excluded = object.find("excludedCommands");
        excluded != object.end() && excluded->value().is_array()) {
        for (const auto& entry : excluded->value().as_array()) {
            if (entry.is_string()) commands.emplace_back(entry.as_string().c_str());
        }
    }

    // The command is not added twice.
    bool known = false;
    for (const auto& entry : commands) {
        if (entry.is_string() && entry.as_string() == pattern) known = true;
    }
    if (!known) commands.emplace_back(pattern);

    object["excludedCommands"] = std::move(commands);
    return boost::json::value(std::move(object));
}

} // namespace cc::sandbox
