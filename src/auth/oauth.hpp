#pragma once

#include <boost/asio.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cc::auth {

/// The endpoints of the OAuth flow.
///
/// The values are the ones of the reference's `constants/oauth.ts`.
struct OauthConfig {
    /// The base of the API.
    std::string base_api_url = "https://api.anthropic.com";

    /// Where the user signs in with a Claude account.
    std::string claude_ai_authorize_url = "https://claude.com/cai/oauth/authorize";

    /// Where the user signs in with an Anthropic Console account.
    std::string console_authorize_url = "https://platform.claude.com/oauth/authorize";

    /// Where the authorization code is exchanged for the tokens.
    std::string token_url = "https://platform.claude.com/v1/oauth/token";

    /// Where the browser is sent when the user has to paste the code.
    std::string manual_redirect_url = "https://platform.claude.com/oauth/code/callback";

    /// The id of the client.
    std::string client_id = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";

    /// The beta the API asks for when the tokens are used.
    std::string beta_header = "oauth-2025-04-20";
};

/// The scopes the flow asks for.
///
/// The order is the one of the reference: the Console scopes come first, so
/// that a login can be moved from the Console to a Claude account.
const std::vector<std::string>& all_scopes();

/// The tokens of a login.
struct Tokens {
    /// The token the API is called with.
    std::string access_token;

    /// The token that gets a new access token.
    std::string refresh_token;

    /// When the access token stops working, in milliseconds since the epoch.
    int64_t expires_at = 0;

    /// The scopes the tokens carry.
    std::vector<std::string> scopes;

    /// The kind of subscription the account has.
    std::string subscription_type;
};

/// A random string of thirty-two bytes, in the base64url of the standard.
std::string random_token();

/// The challenge of a verifier: the base64url of its SHA-256.
std::string code_challenge(const std::string& verifier);

/// The URL the browser is sent to.
///
/// The `manual` flow asks the user to paste the code, so its redirect goes
/// to the page of the provider instead of to the local server.
std::string authorization_url(const OauthConfig& config, const std::string& challenge,
                              const std::string& state, int port, bool manual,
                              bool claude_ai_account);

/// The answer of the token endpoint.
struct TokenResponse {
    /// The token the API is called with.
    std::string access_token;

    /// The token that gets a new access token.
    std::string refresh_token;

    /// How long the access token lives, in seconds.
    int64_t expires_in = 0;

    /// The scopes the tokens carry, separated by spaces.
    std::string scope;
};

/// Exchanges an authorization code for the tokens.
///
/// The exchange is done over the network, on the event loop of the session;
/// the failure is reported through `error`.
bool exchange_code(boost::asio::io_context& io_context, const OauthConfig& config,
                   const std::string& code, const std::string& state, const std::string& verifier,
                   int port, bool manual, TokenResponse* out, std::string* error);

/// Gets a new access token with the refresh token.
bool refresh_tokens(boost::asio::io_context& io_context, const OauthConfig& config,
                    const std::string& refresh_token, TokenResponse* out, std::string* error);

/// The scopes of a scope string.
std::vector<std::string> parse_scopes(const std::string& scopes);

/// One limit of the plan of the user.
struct RateLimit {
    /// The share of the limit that is used, in percent.
    double utilization = 0;

    /// True when the share of the limit is known.
    bool has_utilization = false;

    /// When the limit starts again, in milliseconds since the epoch.
    int64_t resets_at = 0;
};

/// The use of the plan of the user.
struct Utilization {
    /// The limit of the session.
    RateLimit five_hour;

    /// The limit of the week.
    RateLimit seven_day;

    /// The limit of the week, for the Sonnet model alone.
    RateLimit seven_day_sonnet;

    /// The extra usage of the plan.
    ///
    /// The reference names it `extra_usage`; it is the spending of the month
    /// beyond the limits of the plan.
    struct ExtraUsage {
        /// True when the user turned the extra usage on.
        bool is_enabled = false;

        /// The limit of the month, in cents; null when there is none.
        std::optional<double> monthly_limit;

        /// What was spent of the month, in cents.
        std::optional<double> used_credits;

        /// The share of the limit that is used, from 0 to 100.
        std::optional<double> utilization;
    };

    /// The extra usage, when the API reports one.
    std::optional<ExtraUsage> extra_usage;

    /// True when the answer of the API held the limits.
    bool known = false;
};

/// Asks the API how much of the plan of the user is used.
///
/// The answer is asked over the network; the failure is reported through
/// `error`.
bool fetch_utilization(boost::asio::io_context& io_context, const OauthConfig& config,
                      const std::string& access_token, Utilization* out, std::string* error);

/// True when the account of the user may spend beyond the limits of the plan.
///
/// The reference asks the kind of the subscription and the way the account
/// is billed: only the Stripe and the mobile subscriptions may buy the
/// extra usage. The billing is read from `oauthAccount.billingType` in the
/// global configuration file.
bool overage_provisioning_allowed();

} // namespace cc::auth
