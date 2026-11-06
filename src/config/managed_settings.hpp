#pragma once

#include <boost/json.hpp>

#include <string>

namespace cc::config {

/// The directory the managed settings live in.
///
/// The reference reads them from the directory the platform keeps them in:
/// `/Library/Application Support/ClaudeCode` on macOS,
/// `C:\Program Files\ClaudeCode` on Windows and `/etc/claude-code` on Linux.
///
/// The variable `CLAUDE_CODE_MANAGED_SETTINGS_PATH` overrides the
/// directory; the reference only honours it in its own builds, and the port
/// honours it so that the tests do not need the machine of the user.
std::string managed_settings_dir();

/// The path of the managed settings file
/// (`<managed settings>/managed-settings.json`).
std::string managed_settings_path();

/// The drop-in directory of the managed settings
/// (`<managed settings>/managed-settings.d`).
std::string managed_settings_drop_in_dir();

/// The merged managed settings.
///
/// The base file is read first, and the files of the drop-in directory are
/// merged on top of it, in the order of their names — the later file wins.
/// An empty object is returned when there is no managed settings at all.
boost::json::object load_managed_settings();

} // namespace cc::config
