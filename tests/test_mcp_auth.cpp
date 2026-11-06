#include <catch2/catch_test_macros.hpp>

#include "mcp/mcp.hpp"
#include "ui/ansi.hpp"
#include "ui/mcp_auth.hpp"
#include "ui/theme.hpp"
#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/time.hpp"

#include <boost/json.hpp>

#include <filesystem>
#include <string>

using namespace cc;

namespace {

/// The directory the tests keep their files in.
std::filesystem::path test_directory() {
    return std::filesystem::temp_directory_path() / "cc_mcp_auth_test";
}

/// The file the tests keep the tokens in.
std::string test_file() { return (test_directory() / ".credentials.json").string(); }

/// A server of MCP that is reached over the network.
mcp::ServerConfig http_server(const std::string& name, const std::string& url) {
    mcp::ServerConfig config;
    config.name = name;
    config.url = url;
    config.transport = "http";
    return config;
}

/// The document of the discovery of the protected resource.
boost::json::object protected_resource() {
    boost::json::array servers;
    servers.emplace_back(std::string("https://auth.example.com"));

    boost::json::object document;
    document["resource"] = std::string("https://mcp.example.com/mcp");
    document["authorization_servers"] = std::move(servers);
    return document;
}

/// The document of the discovery of the authorization server.
boost::json::object authorization_server() {
    boost::json::array scopes;
    scopes.emplace_back(std::string("read"));
    scopes.emplace_back(std::string("write"));

    boost::json::object document;
    document["issuer"] = std::string("https://auth.example.com");
    document["authorization_endpoint"] = std::string("https://auth.example.com/authorize");
    document["token_endpoint"] = std::string("https://auth.example.com/token");
    document["registration_endpoint"] = std::string("https://auth.example.com/register");
    document["scopes_supported"] = std::move(scopes);
    return document;
}

} // namespace

TEST_CASE("The discovery of the protected resource names the authorization servers") {
    const auto servers = mcp::parse_protected_resource(protected_resource());
    REQUIRE(servers.size() == 1);
    REQUIRE(servers.front() == "https://auth.example.com");

    // An answer that names none leaves the list empty.
    boost::json::object empty;
    REQUIRE(mcp::parse_protected_resource(empty).empty());
}

TEST_CASE("The discovery of the authorization server holds the endpoints") {
    const mcp::AuthorizationServer server = mcp::parse_authorization_server(authorization_server());

    REQUIRE(server.found);
    REQUIRE(server.issuer == "https://auth.example.com");
    REQUIRE(server.authorization_endpoint == "https://auth.example.com/authorize");
    REQUIRE(server.token_endpoint == "https://auth.example.com/token");
    REQUIRE(server.registration_endpoint == "https://auth.example.com/register");
    REQUIRE(server.scopes_supported.size() == 2);
    REQUIRE(server.scopes_supported.front() == "read");

    // The scope of the answer is read the way the servers write it: the key
    // `scope` first, then `default_scope`.
    boost::json::object with_scope = authorization_server();
    with_scope["scope"] = std::string("read write");
    REQUIRE(mcp::parse_authorization_server(with_scope).scope == "read write");

    boost::json::object with_default = authorization_server();
    with_default["default_scope"] = std::string("read");
    REQUIRE(mcp::parse_authorization_server(with_default).scope == "read");

    // An answer that holds no endpoint is no authorization server.
    boost::json::object nothing;
    REQUIRE(!mcp::parse_authorization_server(nothing).found);
}

TEST_CASE("The URL of the authorization holds what the provider needs") {
    const mcp::AuthorizationServer server = mcp::parse_authorization_server(authorization_server());

    const std::string url = mcp::build_authorization_url(
        server, "the-client", "http://localhost:1234/callback", "the-challenge", "the-state",
        "read write", "https://mcp.example.com/mcp");

    REQUIRE(url.find("https://auth.example.com/authorize?") == 0);
    REQUIRE(url.find("response_type=code") != std::string::npos);
    REQUIRE(url.find("client_id=the-client") != std::string::npos);
    REQUIRE(url.find("redirect_uri=http%3A%2F%2Flocalhost%3A1234%2Fcallback") != std::string::npos);
    REQUIRE(url.find("state=the-state") != std::string::npos);
    REQUIRE(url.find("code_challenge=the-challenge") != std::string::npos);
    REQUIRE(url.find("code_challenge_method=S256") != std::string::npos);
    REQUIRE(url.find("scope=read%20write") != std::string::npos);
    REQUIRE(url.find("resource=https%3A%2F%2Fmcp.example.com%2Fmcp") != std::string::npos);

    // A server that asks for no scope and names no resource leaves both out.
    const std::string bare = mcp::build_authorization_url(server, "c", "r", "ch", "st", "", "");
    REQUIRE(bare.find("scope=") == std::string::npos);
    REQUIRE(bare.find("resource=") == std::string::npos);
}

TEST_CASE("The key of a server is the one the reference uses") {
    const mcp::ServerConfig config = http_server("the-server", "https://mcp.example.com/mcp");
    const std::string key = mcp::server_key("the-server", config);

    // The name of the server and the hash of its configuration.
    REQUIRE(key.find("the-server|") == 0);
    REQUIRE(key.size() == std::string("the-server|").size() + 16);

    // The same server has the same key, another one has another.
    REQUIRE(mcp::server_key("the-server", config) == key);
    REQUIRE(mcp::server_key("another-server", config) != key);

    const mcp::ServerConfig moved = http_server("the-server", "https://mcp.example.com/other");
    REQUIRE(mcp::server_key("the-server", moved) != key);
}

TEST_CASE("The tokens of a server are kept and read back") {
    std::filesystem::remove_all(test_directory());
    std::filesystem::create_directories(test_directory());
    const std::string path = test_file();

    const std::string key = mcp::server_key("the-server", http_server("the-server", "https://x/"));

    mcp::StoredServerAuth auth;
    auth.server_name = "the-server";
    auth.server_url = "https://mcp.example.com/mcp";
    auth.client_id = "the-client";
    auth.client_secret = "the-secret";
    auth.authorization_server_url = "https://auth.example.com";
    auth.tokens.access_token = "the-token";
    auth.tokens.refresh_token = "the-refresh";
    auth.tokens.scope = "read write";
    auth.tokens.expires_at = 1234567890;

    std::string error;
    REQUIRE(mcp::save_server_auth(key, auth, &error, path));
    REQUIRE(error.empty());

    const auto loaded = mcp::load_server_auth(key, path);
    REQUIRE(loaded);
    REQUIRE(loaded->server_name == "the-server");
    REQUIRE(loaded->server_url == "https://mcp.example.com/mcp");
    REQUIRE(loaded->client_id == "the-client");
    REQUIRE(loaded->client_secret == "the-secret");
    REQUIRE(loaded->authorization_server_url == "https://auth.example.com");
    REQUIRE(loaded->tokens.access_token == "the-token");
    REQUIRE(loaded->tokens.refresh_token == "the-refresh");
    REQUIRE(loaded->tokens.scope == "read write");
    REQUIRE(loaded->tokens.expires_at == 1234567890);

    // The document holds the tokens under the key the reference uses.
    const boost::json::value document = json_parse(*cc::read_file(path), boost::json::value(nullptr));
    REQUIRE(document.is_object());
    const boost::json::object servers = jget_object(document.as_object(), "mcpOAuth");
    REQUIRE(servers.find(key) != servers.end());

    // A server that is not there has no tokens.
    REQUIRE(!mcp::load_server_auth("another|key", path));

    // The tokens are forgotten.
    REQUIRE(mcp::clear_server_auth(key, path));
    REQUIRE(!mcp::load_server_auth(key, path));

    std::filesystem::remove_all(test_directory());
}

TEST_CASE("The tokens of a server do not disturb the credentials of the account") {
    std::filesystem::remove_all(test_directory());
    std::filesystem::create_directories(test_directory());
    const std::string path = test_file();

    // The document of the credentials of the account.
    {
        boost::json::object document;
        document["access_token"] = std::string("the-token-of-the-account");
        document["refresh_token"] = std::string("the-refresh-of-the-account");
        REQUIRE(cc::write_file(path, json_dump(document)));
    }

    const std::string key = mcp::server_key("the-server", http_server("the-server", "https://x/"));
    mcp::StoredServerAuth auth;
    auth.server_name = "the-server";
    auth.tokens.access_token = "the-token";
    REQUIRE(mcp::save_server_auth(key, auth, nullptr, path));

    // The credentials of the account are still there.
    const boost::json::value document = json_parse(*cc::read_file(path), boost::json::value(nullptr));
    REQUIRE(document.is_object());
    REQUIRE(jget_string(document.as_object(), "access_token") == "the-token-of-the-account");
    REQUIRE(jget_string(document.as_object(), "refresh_token") == "the-refresh-of-the-account");

    // The tokens of the server are there as well.
    REQUIRE(mcp::load_server_auth(key, path));

    std::filesystem::remove_all(test_directory());
}

TEST_CASE("The tokens of a server are the ones that are still good") {
    mcp::OAuthTokens tokens;
    REQUIRE(!mcp::tokens_usable(tokens));

    tokens.access_token = "the-token";
    REQUIRE(mcp::tokens_usable(tokens));

    // A token that is about to expire is not good.
    tokens.expires_at = cc::now_ms() + 1000;
    REQUIRE(!mcp::tokens_usable(tokens));

    tokens.expires_at = cc::now_ms() + 600000;
    REQUIRE(mcp::tokens_usable(tokens));
}

TEST_CASE("The dialog of the authentication says what is happening") {
    const ui::Theme theme = ui::Theme::dark();
    const ui::McpAuthDialog dialog(theme, "the-server",
                                   http_server("the-server", "https://mcp.example.com/mcp"));

    const auto lines = dialog.render(80);
    REQUIRE_FALSE(lines.empty());

    std::string joined;
    for (const auto& line : lines) {
        joined += ui::strip(line);
        joined += "\n";
    }

    // The name of the server, the news that a browser is opening, and the
    // way out of the dialog.
    REQUIRE(joined.find("Authenticating with the-server…") != std::string::npos);
    REQUIRE(joined.find("A browser window will open for authentication") != std::string::npos);
    REQUIRE(joined.find("Return here after authenticating in your browser. Press Esc to go back.") !=
            std::string::npos);
}
