#pragma once

#include "auth/oauth.hpp"

#include <optional>
#include <string>

namespace cc::auth {

/// The credentials of a login, as they are kept.
struct StoredCredentials {
    /// The token the API is called with.
    std::string access_token;

    /// The token that gets a new access token.
    std::string refresh_token;

    /// When the access token stops working, in milliseconds since the epoch.
    int64_t expires_at = 0;

    /// The scopes the tokens carry.
    std::string scope;

    /// The kind of subscription the account has.
    std::string subscription_type;
};

/// Keeps the credentials of the login.
///
/// On macOS they go into the keychain, the way the reference keeps them;
/// elsewhere they go into a file of the configuration folder.
bool save_credentials(const StoredCredentials& credentials, std::string* error);

/// The credentials of the login, when there are any.
std::optional<StoredCredentials> load_credentials();

/// Forgets the credentials of the login.
bool clear_credentials(std::string* error);

/// True when the credentials of a login are there and still good.
///
/// A token that is about to expire is not good; the session refreshes it
/// then.
bool has_valid_credentials();

/// The tokens of the login, refreshed when the old ones have expired.
std::optional<Tokens> current_tokens();

/// Forgets the tokens that were read, so that they are read again.
///
/// The tokens are read from the keychain of the machine, which is slow;
/// they are kept until they are written again.
void forget_tokens();

} // namespace cc::auth
