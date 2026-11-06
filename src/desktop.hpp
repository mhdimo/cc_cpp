#pragma once

#include <string>

namespace cc::desktop {

/// The state of the Claude Desktop application.
enum class InstallStatus {
    /// The application is not on this machine.
    NotInstalled,
    /// The application is there, but it is too old for the handoff.
    VersionTooOld,
    /// The application is there and it may be used.
    Ready,
};

/// The state of the Claude Desktop application.
struct Status {
    /// The state of the application.
    InstallStatus status = InstallStatus::NotInstalled;

    /// The version of the application, when it could be read.
    std::string version;
};

/// The version the application needs to be at for the handoff.
inline constexpr std::string_view kMinimumVersion = "1.1.2396";

/// Where the documentation of the application is.
inline constexpr std::string_view kDocsUrl = "https://clau.de/desktop";

/// The state of the Claude Desktop application on this machine.
Status install_status();

/// The address the application is downloaded from.
std::string download_url();

/// The address that carries a session to the application.
///
/// The address is `claude://resume?session=<id>&cwd=<folder>`, the way the
/// reference builds it.
std::string deep_link(const std::string& session_id, const std::string& cwd);

/// Opens the application with the session in it.
///
/// Returns false when the application could not be opened.
bool open_session(const std::string& session_id, const std::string& cwd);

} // namespace cc::desktop
