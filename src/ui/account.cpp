#include "ui/account.hpp"

#include "config/global_config.hpp"
#include "utils/json_util.hpp"

namespace cc::ui {
namespace {

/// The account of the user, as it is written in the global configuration
/// file.
///
/// The account is empty when the user has never signed in.
boost::json::object oauth_account() {
    return cc::jget_object(cc::config::load_global_config(), "oauthAccount");
}

} // namespace

std::string account_display_name() {
    return cc::jget_string(oauth_account(), "displayName");
}

std::string account_organization_name() {
    return cc::jget_string(oauth_account(), "organizationName");
}

} // namespace cc::ui
