#include "sandbox/profile.hpp"

#include "config/settings.hpp"
#include "utils/fs.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <cstdlib>
#include <sstream>
#include <unistd.h>

namespace cc::sandbox {
namespace {

/// A text of the shell, quoted the way the shell reads it.
std::string shell_quote(const std::string& text) {
    std::string out = "'";
    for (const char c : text) {
        if (c == '\'') {
            out += "'\\''";
            continue;
        }
        out += c;
    }
    out += "'";
    return out;
}

/// A path of the profile of the seatbelt.
///
/// The path is quoted the way the seatbelt wants it.
std::string literal(const std::string& path) {
    std::string out = "\"";
    for (const char c : path) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

/// The rules of the paths of the filesystem.
///
/// The seatbelt knows the paths that may be read and the paths that may be
/// written; the rules of the settings and the rules of the permissions of
/// the session are the ones that are added to them.
void filesystem_rules(std::ostringstream& profile, const Manager& manager,
                      const std::string& working_dir) {
    // Everything may be read, except the paths that are denied.
    profile << "(allow file-read*)\n";

    const FsReadConfig read = manager.get_fs_read_config();
    for (const std::string& path : read.deny_only) {
        profile << "(deny file-read* (subpath " << literal(cc::expand_home(path)) << "))\n";
    }
    for (const std::string& path : read.allow_within_deny) {
        profile << "(allow file-read* (subpath " << literal(cc::expand_home(path)) << "))\n";
    }

    // Only the working directory and the paths that are allowed may be
    // written.
    profile << "(deny file-write*)\n";
    profile << "(allow file-write* (subpath " << literal(working_dir) << "))\n";

    const FsWriteConfig write = manager.get_fs_write_config();
    for (const std::string& path : write.allow_only) {
        if (path == ".") continue;
        profile << "(allow file-write* (subpath " << literal(cc::expand_home(path)) << "))\n";
    }
    // The paths that may not be written come last, so that they win over the
    // paths that are allowed.
    for (const std::string& path : write.deny_within_allow) {
        profile << "(deny file-write* (subpath " << literal(cc::expand_home(path)) << "))\n";
    }
}

/// The rules of the network.
///
/// The seatbelt of macOS holds the network of the process with the rules of
/// the socket; the domains of the settings and the hosts of the rules of
/// the permissions are turned into the hosts they name.
void network_rules(std::ostringstream& profile, const Manager& manager) {
    // The network is closed, and the hosts that are allowed are opened
    // again.
    profile << "(deny network*)\n";

    if (manager.settings().network.allow_all_unix_sockets) {
        profile << "(allow network*)\n";
        return;
    }

    const NetworkRestrictionConfig hosts = manager.get_network_restriction_config();
    for (const std::string& host : hosts.allowed_hosts) {
        profile << "(allow network-outbound (remote host " << literal(host) << "))\n";
    }
    if (manager.settings().network.allow_local_binding) {
        profile << "(allow network-bind (local ip \"*:*\"))\n";
    }

    for (const std::string& socket : manager.get_allow_unix_sockets()) {
        profile << "(allow network-outbound (literal " << literal(cc::expand_home(socket))
                << "))\n";
    }

    // The hosts that are denied come last, so that they win over the hosts
    // that are allowed.
    for (const std::string& host : hosts.denied_hosts) {
        profile << "(deny network-outbound (remote host " << literal(host) << "))\n";
    }
}

} // namespace

std::string seatbelt_profile(const Manager& manager, const std::string& working_dir) {
    std::ostringstream profile;

    profile << "(version 1)\n";
    profile << "(deny default)\n";

    // The commands need to run the programs they use.
    profile << "(allow process-exec)\n";
    profile << "(allow process-fork)\n";
    profile << "(allow sysctl-read)\n";
    profile << "(allow mach-lookup)\n";

    // The shell needs a few things of the system to work at all.
    profile << "(allow file-read* (subpath \"/usr\") (subpath \"/bin\") (subpath \"/sbin\")\n";
    profile << "           (subpath \"/System\") (subpath \"/Library\") (subpath \"/private/etc\")\n";
    profile << "           (subpath \"/private/var/db\") (subpath \"/dev\") (subpath \"/opt/homebrew\")\n";
    profile << "           (subpath \"/usr/local\") (literal \"/\") (subpath \"/Users\"))\n";

    filesystem_rules(profile, manager, working_dir);
    network_rules(profile, manager);

    return profile.str();
}

std::string sandboxed_command(const Manager& manager, const std::string& command,
                              const std::string& working_dir) {
    // The seatbelt is a tool of macOS; on the other systems the command runs
    // as it is.
    if (!cc::file_exists("/usr/bin/sandbox-exec")) return {};

    // The profile is written to a file of the temporary directory, because
    // the seatbelt reads it from there.
    const char* tmpdir = std::getenv("TMPDIR");
    std::string directory = tmpdir != nullptr ? tmpdir : "/tmp";
    while (!directory.empty() && directory.back() == '/') directory.pop_back();

    const std::string path = directory + "/cc-sandbox-" + std::to_string(::getpid()) + ".sb";

    std::FILE* file = std::fopen(path.c_str(), "w");
    if (file == nullptr) return {};

    const std::string profile = seatbelt_profile(manager, working_dir);
    std::fwrite(profile.data(), 1, profile.size(), file);
    std::fclose(file);

    return "/usr/bin/sandbox-exec -f " + shell_quote(path) + " /bin/sh -c " +
           shell_quote(command);
}

} // namespace cc::sandbox
