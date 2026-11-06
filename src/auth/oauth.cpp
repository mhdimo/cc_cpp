#include "auth/oauth.hpp"

#include "auth/storage.hpp"
#include "config/global_config.hpp"

#include "utils/hash.hpp"
#include "utils/json_util.hpp"
#include "utils/string.hpp"
#include "utils/time.hpp"

#include <ai/http/client.hpp>

#include <boost/json.hpp>

#include <chrono>
#include <cstdio>
#include <random>

namespace cc::auth {
namespace {

/// The characters that do not have to be escaped in a query string.
///
/// The reference escapes everything the `URL` class of the browser escapes.
bool needs_escape(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return false;
    switch (c) {
        case '-':
        case '_':
        case '.':
        case '!':
        case '~':
        case '*':
        case '\'':
        case '(':
        case ')':
            return false;
        default:
            return true;
    }
}

/// The text, with the characters a query string does not hold escaped.
std::string escape(const std::string& text) {
    static constexpr char kHex[] = "0123456789ABCDEF";

    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (!needs_escape(byte)) {
            out.push_back(c);
            continue;
        }
        out.push_back('%');
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0f]);
    }
    return out;
}

/// Sends a request to the token endpoint and reads the answer.
///
/// The exchange is done over the network; the failure is reported through
/// `error`.
bool post_token_request(boost::asio::io_context& io_context, const OauthConfig& config,
                        const boost::json::object& body, TokenResponse* out, std::string* error) {
    ai::http::HttpClient client(io_context);

    ai::http::Headers headers;
    headers["Content-Type"] = "application/json";
    headers["anthropic-beta"] = config.beta_header;

    auto task = client.post_json(config.token_url, boost::json::value(body), headers);

    task.start();
    // The request is driven by the event loop of the session, so the answer
    // is waited for here.
    while (!task.done()) {
        io_context.run_one_for(std::chrono::milliseconds(20));
    }

    ai::http::HttpResponse response;
    try {
        response = task.get();
    } catch (const std::exception& e) {
        if (error != nullptr) *error = e.what();
        return false;
    }

    if (response.status_code != 200) {
        if (error != nullptr) {
            *error = response.status_code == 401
                         ? "Authentication failed: Invalid authorization code"
                         : "Token exchange failed (" + std::to_string(response.status_code) + ")";
        }
        return false;
    }

    const boost::json::value parsed = json_parse(response.body, boost::json::value(nullptr));
    if (!parsed.is_object()) {
        if (error != nullptr) *error = "the token endpoint sent an answer the port cannot read";
        return false;
    }

    const auto& object = parsed.as_object();
    out->access_token = jget_string(object, "access_token");
    out->refresh_token = jget_string(object, "refresh_token");
    out->expires_in = jget_int(object, "expires_in", 0);
    out->scope = jget_string(object, "scope");

    if (out->access_token.empty()) {
        if (error != nullptr) *error = "the token endpoint sent no token";
        return false;
    }
    return true;
}

} // namespace

const std::vector<std::string>& all_scopes() {
    // The order is the one of the reference: the Console scopes come first,
    // so that a login can be moved from the Console to a Claude account.
    static const std::vector<std::string> kScopes = {
        "org:create_api_key",
        "user:profile",
        "user:inference",
        "user:sessions:claude_code",
        "user:mcp_servers",
        "user:file_upload",
    };
    return kScopes;
}

std::string random_token() {
    std::random_device device;
    std::vector<uint8_t> bytes(32);
    for (auto& byte : bytes) byte = static_cast<uint8_t>(device());

    return base64url(bytes);
}

std::string code_challenge(const std::string& verifier) { return base64url(sha256(verifier)); }

std::string authorization_url(const OauthConfig& config, const std::string& challenge,
                              const std::string& state, int port, bool manual,
                              bool claude_ai_account) {
    const std::string base = claude_ai_account ? config.claude_ai_authorize_url
                                                : config.console_authorize_url;

    std::string url = base;
    url += "?code=true";
    url += "&client_id=" + escape(config.client_id);
    url += "&response_type=code";
    url += "&redirect_uri=" +
           escape(manual ? config.manual_redirect_url
                         : "http://localhost:" + std::to_string(port) + "/callback");
    url += "&scope=" + escape(cc::join(all_scopes(), " "));
    url += "&code_challenge=" + escape(challenge);
    url += "&code_challenge_method=S256";
    url += "&state=" + escape(state);
    return url;
}

bool exchange_code(boost::asio::io_context& io_context, const OauthConfig& config,
                   const std::string& code, const std::string& state, const std::string& verifier,
                   int port, bool manual, TokenResponse* out, std::string* error) {
    boost::json::object body;
    body["grant_type"] = std::string("authorization_code");
    body["code"] = code;
    body["redirect_uri"] = manual ? config.manual_redirect_url
                                  : "http://localhost:" + std::to_string(port) + "/callback";
    body["client_id"] = config.client_id;
    body["code_verifier"] = verifier;
    body["state"] = state;

    return post_token_request(io_context, config, body, out, error);
}

bool refresh_tokens(boost::asio::io_context& io_context, const OauthConfig& config,
                    const std::string& refresh_token, TokenResponse* out, std::string* error) {
    boost::json::object body;
    body["grant_type"] = std::string("refresh_token");
    body["refresh_token"] = refresh_token;
    body["client_id"] = config.client_id;
    body["scope"] = cc::join(all_scopes(), " ");

    return post_token_request(io_context, config, body, out, error);
}

std::vector<std::string> parse_scopes(const std::string& scopes) {
    std::vector<std::string> out;
    for (const auto& scope : cc::split(scopes, ' ')) {
        if (!scope.empty()) out.push_back(scope);
    }
    return out;
}

namespace {

/// Reads one limit of the answer of the API.
RateLimit read_limit(const boost::json::object& parent, const std::string& name) {
    RateLimit limit;

    const boost::json::object object = jget_object(parent, name);
    if (object.empty()) return limit;

    const double utilization = jget_double(object, "utilization", -1.0);
    if (utilization >= 0) {
        limit.utilization = utilization;
        limit.has_utilization = true;
    }

    const std::string resets = jget_string(object, "resets_at");
    if (!resets.empty()) limit.resets_at = parse_timestamp(resets);

    return limit;
}

/// Reads the extra usage of the answer of the API.
///
/// The reference reads it as `extra_usage`, with the spending of the month in
/// cents.
Utilization::ExtraUsage read_extra_usage(const boost::json::object& parent) {
    Utilization::ExtraUsage extra;

    const boost::json::object object = jget_object(parent, "extra_usage");
    if (object.empty()) return extra;

    extra.is_enabled = jget_bool(object, "is_enabled", false);

    const auto number = [&object](const char* key) -> std::optional<double> {
        const auto it = object.find(key);
        if (it == object.end() || it->value().is_null()) return std::nullopt;
        if (!it->value().is_double() && !it->value().is_int64()) return std::nullopt;
        return it->value().as_double();
    };

    extra.monthly_limit = number("monthly_limit");
    extra.used_credits = number("used_credits");
    extra.utilization = number("utilization");

    return extra;
}

} // namespace

bool fetch_utilization(boost::asio::io_context& io_context, const OauthConfig& config,
                      const std::string& access_token, Utilization* out, std::string* error) {
    ai::http::HttpClient client(io_context);

    ai::http::Headers headers;
    headers["Authorization"] = "Bearer " + access_token;
    headers["anthropic-beta"] = config.beta_header;
    headers["Content-Type"] = "application/json";

    // The address of the API is the one of the environment, the way the
    // reference reads it.
    const std::string base = config.base_api_url;
    auto task = client.get(base + "/api/oauth/usage", headers);

    task.start();
    while (!task.done()) {
        io_context.run_one_for(std::chrono::milliseconds(20));
    }

    ai::http::HttpResponse response;
    try {
        response = task.get();
    } catch (const std::exception& e) {
        if (error != nullptr) *error = e.what();
        return false;
    }

    if (response.status_code != 200) {
        if (error != nullptr) {
            *error = "Failed to load usage data (" + std::to_string(response.status_code) + ")";
        }
        return false;
    }

    const boost::json::value parsed = json_parse(response.body, boost::json::value(nullptr));
    if (!parsed.is_object()) {
        if (error != nullptr) *error = "Failed to load usage data";
        return false;
    }

    const auto& object = parsed.as_object();
    out->five_hour = read_limit(object, "five_hour");
    out->seven_day = read_limit(object, "seven_day");
    out->seven_day_sonnet = read_limit(object, "seven_day_sonnet");

    const Utilization::ExtraUsage extra = read_extra_usage(object);
    if (!jget_object(object, "extra_usage").empty()) out->extra_usage = extra;

    out->known = true;
    return true;
}

bool overage_provisioning_allowed() {
    // The account has to be a subscriber of Claude, not a customer of the
    // API.
    const auto tokens = current_tokens();
    if (!tokens || tokens->subscription_type.empty()) return false;

    // Only the Stripe and the mobile subscriptions may buy the extra usage.
    const std::string billing = cc::config::global_account_billing_type();
    return billing == "stripe_subscription" || billing == "stripe_subscription_contracted" ||
           billing == "apple_subscription" || billing == "google_play_subscription";
}

} // namespace cc::auth
