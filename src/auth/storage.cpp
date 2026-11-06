#include "auth/storage.hpp"

#include "utils/fs.hpp"
#include "utils/hash.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/process.hpp"
#include "utils/string.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace cc::auth {
namespace {

/// The suffix of the entry of the keychain that holds the credentials.
constexpr std::string_view kCredentialsSuffix = "-credentials";

/// The name of the entry of the keychain.
///
/// The name of the configuration folder is folded into it when the folder is
/// not the default one, so that two of them do not share their login.
std::string service_name() {
    const std::string config_dir = cc::claude_config_dir();
    const bool is_default = std::getenv("CLAUDE_CONFIG_DIR") == nullptr;

    std::string name = "Claude Code";
    if (!is_default) {
        name += "-" + cc::sha256_hex(config_dir).substr(0, 8);
    }
    name += std::string(kCredentialsSuffix);
    return name;
}

/// The name of the user the credentials belong to.
std::string user_name() {
    if (const char* user = std::getenv("USER"); user != nullptr && *user != '\0') return user;
    return "claude-code-user";
}

/// The path of the file the credentials go into when there is no keychain.
std::string credentials_path() { return cc::path_join(cc::claude_config_dir(), ".credentials.json"); }

/// The hex of the bytes.
std::string hex_of(const std::string& text) {
    static constexpr char kHex[] = "0123456789abcdef";

    std::string out;
    out.reserve(text.size() * 2);
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0f]);
    }
    return out;
}

/// True when the machine has a keychain the credentials can go into.
bool has_keychain() {
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

/// Reads the credentials from the keychain.
std::optional<StoredCredentials> read_keychain() {
    const std::string command = "security find-generic-password -a \"" + user_name() +
                                "\" -w -s \"" + service_name() + "\"";
    const cc::ProcessOutcome outcome = cc::run_process(command, cc::cwd(), "", 10);
    if (outcome.exit_code != 0) return std::nullopt;

    const std::string text = cc::trim(outcome.out);
    if (text.empty()) return std::nullopt;

    const boost::json::value parsed = json_parse(text, boost::json::value(nullptr));
    if (!parsed.is_object()) return std::nullopt;

    const auto& object = parsed.as_object();
    StoredCredentials credentials;
    credentials.access_token = jget_string(object, "access_token");
    credentials.refresh_token = jget_string(object, "refresh_token");
    credentials.expires_at = jget_int(object, "expires_at", 0);
    credentials.scope = jget_string(object, "scope");
    credentials.subscription_type = jget_string(object, "subscription_type");
    return credentials;
}

/// Writes the credentials into the keychain.
bool write_keychain(const StoredCredentials& credentials, std::string* error) {
    boost::json::object object;
    object["access_token"] = credentials.access_token;
    object["refresh_token"] = credentials.refresh_token;
    object["expires_at"] = credentials.expires_at;
    object["scope"] = credentials.scope;
    object["subscription_type"] = credentials.subscription_type;

    const std::string json = json_dump(object);
    const std::string command = "add-generic-password -U -a \"" + user_name() + "\" -s \"" +
                                service_name() + "\" -X \"" + hex_of(json) + "\"\n";

    // The command is given on the standard input, so that the credentials
    // do not show up in the arguments of the process.
    const cc::ProcessOutcome outcome = cc::run_process("security -i", cc::cwd(), command, 10);
    if (outcome.exit_code != 0) {
        if (error != nullptr) *error = "the keychain refused the credentials";
        return false;
    }
    return true;
}

/// Removes the credentials from the keychain.
bool clear_keychain() {
    const std::string command = "security delete-generic-password -a \"" + user_name() +
                                "\" -s \"" + service_name() + "\"";
    const cc::ProcessOutcome outcome = cc::run_process(command, cc::cwd(), "", 10);
    return outcome.exit_code == 0;
}

/// Reads the credentials from the file.
std::optional<StoredCredentials> read_file() {
    const auto text = cc::read_file(credentials_path());
    if (!text) return std::nullopt;

    const boost::json::value parsed = json_parse(*text, boost::json::value(nullptr));
    if (!parsed.is_object()) return std::nullopt;

    const auto& object = parsed.as_object();
    StoredCredentials credentials;
    credentials.access_token = jget_string(object, "access_token");
    credentials.refresh_token = jget_string(object, "refresh_token");
    credentials.expires_at = jget_int(object, "expires_at", 0);
    credentials.scope = jget_string(object, "scope");
    credentials.subscription_type = jget_string(object, "subscription_type");
    return credentials;
}

/// Writes the credentials into the file.
bool write_file(const StoredCredentials& credentials, std::string* error) {
    boost::json::object object;
    object["access_token"] = credentials.access_token;
    object["refresh_token"] = credentials.refresh_token;
    object["expires_at"] = credentials.expires_at;
    object["scope"] = credentials.scope;
    object["subscription_type"] = credentials.subscription_type;

    if (!cc::write_file(credentials_path(), json_dump_pretty(object) + "\n", error)) return false;

    // The file holds the credentials, so only the user may read it.
    std::error_code code;
    std::filesystem::permissions(credentials_path(),
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, code);
    return true;
}

} // namespace

bool save_credentials(const StoredCredentials& credentials, std::string* error) {
    forget_tokens();
    if (has_keychain()) return write_keychain(credentials, error);
    return write_file(credentials, error);
}

std::optional<StoredCredentials> load_credentials() {
    if (has_keychain()) {
        if (auto credentials = read_keychain()) return credentials;
    }
    return read_file();
}

bool clear_credentials(std::string* error) {
    forget_tokens();
    bool ok = true;

    if (has_keychain()) ok = clear_keychain();

    if (cc::file_exists(credentials_path()) && !cc::remove_file(credentials_path())) {
        if (error != nullptr) *error = "the credentials could not be removed";
        ok = false;
    }
    return ok;
}

bool has_valid_credentials() {
    const auto credentials = load_credentials();
    if (!credentials) return false;
    if (credentials->access_token.empty()) return false;

    // A token that is about to expire is refreshed instead of used.
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    return credentials->expires_at > now + 60000;
}

namespace {

/// The tokens of the account, as they were read the last time.
///
/// The keychain is read for every call, and the list of the commands is
/// built again on every keystroke, so the tokens are kept until they are
/// written again.
std::optional<Tokens> g_tokens;
bool g_tokens_known = false;

} // namespace

void forget_tokens() {
    g_tokens.reset();
    g_tokens_known = false;
}

std::optional<Tokens> current_tokens() {
    if (g_tokens_known) return g_tokens;

    g_tokens_known = true;
    const auto credentials = load_credentials();
    if (!credentials) {
        g_tokens.reset();
        return std::nullopt;
    }

    Tokens tokens;
    tokens.access_token = credentials->access_token;
    tokens.refresh_token = credentials->refresh_token;
    tokens.expires_at = credentials->expires_at;
    tokens.scopes = parse_scopes(credentials->scope);
    tokens.subscription_type = credentials->subscription_type;
    g_tokens = tokens;
    return g_tokens;
}

} // namespace cc::auth
