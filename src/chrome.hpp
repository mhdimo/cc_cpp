#pragma once

#include <string>
#include <string_view>

namespace cc::chrome {

/// The address of the extension of the browser.
inline constexpr std::string_view kExtensionUrl = "https://claude.ai/chrome";

/// The address of the permissions of the extension.
inline constexpr std::string_view kPermissionsUrl = "https://clau.de/chrome/permissions";

/// The address that reconnects the extension.
inline constexpr std::string_view kReconnectUrl = "https://clau.de/chrome/reconnect";

/// The name of the server of the browser.
inline constexpr std::string_view kServerName = "claude-in-chrome";

/// True when the extension of the browser is installed.
///
/// The extension is looked for in the folders of the profiles of every
/// browser, the way the reference looks for it.
bool extension_installed();

/// The name of the browser the extension was found in.
///
/// The name is empty when the extension was not found.
std::string extension_browser();

} // namespace cc::chrome
