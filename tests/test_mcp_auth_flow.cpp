#include <catch2/catch_test_macros.hpp>

#include "mcp/mcp.hpp"
#include "utils/string.hpp"

#include <boost/asio.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

using namespace cc;

namespace {

/// The little server that stands in for the authorization server.
///
/// It answers the discovery of the protected resource, the discovery of the
/// authorization server, the registration of a client and the exchange of the
/// code for the tokens.
class FakeAuthServer {
public:
    FakeAuthServer() : acceptor_(io_context_) {
        boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::address_v4::loopback(), 0);
        acceptor_.open(endpoint.protocol(), error_);
        acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), error_);
        acceptor_.bind(endpoint, error_);
        acceptor_.listen(boost::asio::socket_base::max_listen_connections, error_);
        port_ = acceptor_.local_endpoint(error_).port();
    }

    ~FakeAuthServer() {
        boost::system::error_code error;
        acceptor_.close(error);
        if (worker_.joinable()) worker_.join();
    }

    FakeAuthServer(const FakeAuthServer&) = delete;
    FakeAuthServer& operator=(const FakeAuthServer&) = delete;

    /// The port the server listens on.
    int port() const { return port_; }

    /// The address of the server.
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

    /// The body of the last request the server got.
    const std::string& last_body() const { return last_body_; }

    /// The path of the last request the server got.
    const std::string& last_path() const { return last_path_; }

    /// Starts the server.
    void start() {
        worker_ = std::thread([this]() {
            while (true) {
                boost::asio::ip::tcp::socket socket(io_context_);
                boost::system::error_code error;
                acceptor_.accept(socket, error);
                if (error) return;
                answer(socket);
            }
        });
    }

private:
    /// Reads a request and answers it.
    void answer(boost::asio::ip::tcp::socket& socket) {
        std::string request;
        char buffer[4096];
        boost::system::error_code error;

        size_t header_end = std::string::npos;
        while (header_end == std::string::npos) {
            const size_t got = socket.read_some(boost::asio::buffer(buffer), error);
            if (error) return;
            request.append(buffer, got);
            header_end = request.find("\r\n\r\n");
        }

        // The body of the request is read as well, so that the answer comes
        // after the whole request.
        size_t length = 0;
        const size_t mark = request.find("Content-Length:");
        if (mark != std::string::npos) length = std::stoul(request.substr(mark + 15));
        while (request.size() < header_end + 4 + length) {
            const size_t got = socket.read_some(boost::asio::buffer(buffer), error);
            if (error) return;
            request.append(buffer, got);
        }

        const std::string head = request.substr(0, header_end);
        last_path_ = path_of(head);
        last_body_ = request.substr(header_end + 4);

        const std::string body = body_for(last_path_);
        const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                     "Content-Length: " +
                                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        boost::asio::write(socket, boost::asio::buffer(response), error);
    }

    /// The path of a request.
    static std::string path_of(const std::string& head) {
        const size_t first = head.find(' ');
        if (first == std::string::npos) return "/";
        const size_t second = head.find(' ', first + 1);
        if (second == std::string::npos) return "/";
        return head.substr(first + 1, second - first - 1);
    }

    /// The answer of the server for a path.
    std::string body_for(const std::string& path) const {
        if (cc::starts_with(path, "/.well-known/oauth-protected-resource")) {
            return "{\"resource\":\"http://127.0.0.1:" + std::to_string(port_) +
                   "\",\"authorization_servers\":[\"http://127.0.0.1:" + std::to_string(port_) +
                   "\"]}";
        }
        if (cc::starts_with(path, "/.well-known/oauth-authorization-server")) {
            return "{\"issuer\":\"http://127.0.0.1:" + std::to_string(port_) +
                   "\",\"authorization_endpoint\":\"http://127.0.0.1:" + std::to_string(port_) +
                   "/authorize\",\"token_endpoint\":\"http://127.0.0.1:" + std::to_string(port_) +
                   "/token\",\"registration_endpoint\":\"http://127.0.0.1:" + std::to_string(port_) +
                   "/register\",\"scopes_supported\":[\"read\"]}";
        }
        if (path == "/register") return "{\"client_id\":\"the-client\"}";
        if (path == "/token") {
            // A renewal of the tokens answers with another token and with no
            // refresh token of its own.
            if (last_body_.find("grant_type=refresh_token") != std::string::npos) {
                return "{\"access_token\":\"the-token-2\",\"token_type\":\"Bearer\","
                       "\"expires_in\":3600,\"scope\":\"read\"}";
            }
            return "{\"access_token\":\"the-token\",\"token_type\":\"Bearer\",\"expires_in\":3600,"
                   "\"refresh_token\":\"the-refresh\",\"scope\":\"read\"}";
        }
        return "{}";
    }

    boost::asio::io_context io_context_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::system::error_code error_;
    int port_ = 0;
    std::thread worker_;
    std::string last_path_;
    std::string last_body_;
};

/// The value of a parameter of a query string.
std::string parameter_of(const std::string& url, const std::string& name) {
    const size_t mark = url.find('?');
    if (mark == std::string::npos) return {};

    for (const auto& field : split(url.substr(mark + 1), '&')) {
        const auto pair = split_once(field, '=');
        if (pair.first != name) continue;
        std::string value = pair.second;
        std::string out;
        for (size_t at = 0; at < value.size(); ++at) {
            if (value[at] == '%' && at + 2 < value.size()) {
                out.push_back(static_cast<char>(std::stoi(value.substr(at + 1, 2), nullptr, 16)));
                at += 2;
                continue;
            }
            out.push_back(value[at]);
        }
        return out;
    }
    return {};
}

/// The file the tokens of a test are kept in.
///
/// Every test has a file of its own, so that the tokens of the user are
/// never touched.
std::string storage_path(const std::string& name) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("cc_mcp_auth_flow_" + name);
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return (directory / ".credentials.json").string();
}

/// The port of the address of the callback of the browser.
int port_of_redirect(const std::string& redirect_uri) {
    const size_t scheme = redirect_uri.find("://");
    if (scheme == std::string::npos) return 0;

    const size_t colon = redirect_uri.find(':', scheme + 3);
    if (colon == std::string::npos) return 0;

    const size_t slash = redirect_uri.find('/', colon);
    if (slash == std::string::npos) return 0;
    return std::stoi(redirect_uri.substr(colon + 1, slash - colon - 1));
}

} // namespace

TEST_CASE("The flow of the authentication signs the user in") {
    const std::string path = storage_path("signs_in");

    FakeAuthServer server;
    server.start();

    mcp::ServerConfig config;
    config.name = "the-server";
    config.url = server.url() + "/mcp";
    config.transport = "http";

    mcp::AuthFlow flow(config);
    // The browser is not opened: the address of the authorization is read
    // out of the flow instead. The tokens go into a file of the test, so
    // that the tokens of the user are never touched.
    flow.set_skip_browser(true);
    flow.set_storage_path(path);
    flow.start();

    // The flow finds the authorization server and the URL of the browser.
    std::string url;
    for (int i = 0; i < 200 && url.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        url = flow.authorization_url().value_or("");
    }
    REQUIRE(!url.empty());
    REQUIRE(url.find(server.url() + "/authorize?") == 0);
    REQUIRE(url.find("client_id=the-client") != std::string::npos);
    REQUIRE(url.find("code_challenge_method=S256") != std::string::npos);

    // The client was registered with the name the reference uses.
    REQUIRE(server.last_path() == "/register");
    REQUIRE(server.last_body().find("Claude Code (the-server)") != std::string::npos);

    // The browser comes back to the local server with the code and the state.
    const std::string redirect = parameter_of(url, "redirect_uri");
    const std::string state = parameter_of(url, "state");
    REQUIRE(!redirect.empty());
    REQUIRE(!state.empty());

    const int port = port_of_redirect(redirect);
    REQUIRE(port > 0);

    {
        boost::asio::io_context io_context;
        boost::asio::ip::tcp::socket socket(io_context);
        socket.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));

        const std::string request = "GET /callback?code=the-code&state=" + state +
                                    " HTTP/1.1\r\nHost: localhost\r\n\r\n";
        boost::asio::write(socket, boost::asio::buffer(request));

        // The answer of the local server is read, so that the connection is
        // closed cleanly.
        char buffer[1024];
        boost::system::error_code error;
        socket.read_some(boost::asio::buffer(buffer), error);
    }

    // The flow exchanges the code for the tokens and keeps them.
    flow.wait();
    REQUIRE(flow.finished());
    const auto result = flow.result();
    REQUIRE(result);
    REQUIRE(result->ok);
    REQUIRE(result->tokens.access_token == "the-token");
    REQUIRE(result->tokens.refresh_token == "the-refresh");
    REQUIRE(result->tokens.scope == "read");
    REQUIRE(result->tokens.expires_at > 0);

    // The tokens were exchanged with the code and the challenge of the PKCE.
    REQUIRE(server.last_path() == "/token");
    REQUIRE(server.last_body().find("grant_type=authorization_code") != std::string::npos);
    REQUIRE(server.last_body().find("code=the-code") != std::string::npos);
    REQUIRE(server.last_body().find("code_verifier=") != std::string::npos);

    // The tokens are kept, under the key of the server.
    const std::string key = mcp::server_key("the-server", config);
    const auto stored = mcp::load_server_auth(key, path);
    REQUIRE(stored);
    REQUIRE(stored->tokens.access_token == "the-token");
    REQUIRE(stored->client_id == "the-client");

}

TEST_CASE("The flow of the authentication takes the address the user pasted") {
    FakeAuthServer server;
    server.start();

    mcp::ServerConfig config;
    config.name = "the-server";
    config.url = server.url() + "/mcp";
    config.transport = "http";

    mcp::AuthFlow flow(config);
    flow.set_skip_browser(true);
    flow.set_storage_path(storage_path("pasted"));
    flow.start();

    std::string url;
    for (int i = 0; i < 200 && url.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        url = flow.authorization_url().value_or("");
    }
    REQUIRE(!url.empty());

    // The user pastes the address the browser shows, because the browser
    // could not reach the local server.
    const std::string state = parameter_of(url, "state");
    flow.submit_callback_url("http://localhost:1234/callback?code=the-code&state=" + state);

    flow.wait();
    REQUIRE(flow.finished());
    const auto result = flow.result();
    REQUIRE(result);
    REQUIRE(result->ok);
    REQUIRE(result->tokens.access_token == "the-token");
}

TEST_CASE("The flow of the authentication refuses a state that does not match") {
    FakeAuthServer server;
    server.start();

    mcp::ServerConfig config;
    config.name = "the-server";
    config.url = server.url() + "/mcp";
    config.transport = "http";

    mcp::AuthFlow flow(config);
    flow.set_skip_browser(true);
    flow.set_storage_path(storage_path("state"));
    flow.start();

    std::string url;
    for (int i = 0; i < 200 && url.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        url = flow.authorization_url().value_or("");
    }
    REQUIRE(!url.empty());

    flow.submit_callback_url("http://localhost:1234/callback?code=the-code&state=another-state");

    flow.wait();
    REQUIRE(flow.finished());
    const auto result = flow.result();
    REQUIRE(result);
    REQUIRE(!result->ok);
    REQUIRE(result->error.find("state") != std::string::npos);
}

TEST_CASE("The tokens of a server are renewed when they have expired") {
    const std::string path = storage_path("renewed");

    FakeAuthServer server;
    server.start();

    mcp::ServerConfig config;
    config.name = "the-server";
    config.url = server.url() + "/mcp";
    config.transport = "http";

    // The server is signed in first.
    mcp::AuthFlow flow(config);
    flow.set_skip_browser(true);
    flow.set_storage_path(path);
    flow.start();

    std::string url;
    for (int i = 0; i < 200 && url.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        url = flow.authorization_url().value_or("");
    }
    REQUIRE(!url.empty());

    const std::string state = parameter_of(url, "state");
    flow.submit_callback_url("http://localhost:1234/callback?code=the-code&state=" + state);
    flow.wait();
    REQUIRE(flow.result());
    REQUIRE(flow.result()->ok);

    const std::string key = mcp::server_key("the-server", config);
    auto stored = mcp::load_server_auth(key, path);
    REQUIRE(stored);
    REQUIRE(stored->tokens.access_token == "the-token");
    REQUIRE(stored->tokens.refresh_token == "the-refresh");

    // The tokens are renewed with the refresh token.
    std::string error;
    REQUIRE(mcp::refresh_server_auth(config, &error, path));
    REQUIRE(error.empty());
    REQUIRE(server.last_path() == "/token");
    REQUIRE(server.last_body().find("grant_type=refresh_token") != std::string::npos);
    REQUIRE(server.last_body().find("refresh_token=the-refresh") != std::string::npos);

    // The new tokens are kept, and the old refresh token is not lost.
    stored = mcp::load_server_auth(key, path);
    REQUIRE(stored);
    REQUIRE(stored->tokens.access_token == "the-token-2");
    REQUIRE(stored->tokens.refresh_token == "the-refresh");
}
