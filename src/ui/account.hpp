#pragma once

#include <string>

namespace cc::ui {

/// The name of the account of the user.
///
/// The name is read from `oauthAccount.displayName` in the global
/// configuration file (`~/.claude.json`), the way the reference reads it
/// for the greeting of the startup banner. It is empty when the user has
/// no name there.
std::string account_display_name();

/// The organization the account of the user belongs to.
///
/// The name is read from `oauthAccount.organizationName` in the global
/// configuration file. It is empty when the user belongs to none.
std::string account_organization_name();

} // namespace cc::ui
