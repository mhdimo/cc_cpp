#pragma once

#include "mcp/command.hpp"
#include "tools/tool.hpp"

#include <boost/asio.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cc::mcp {

/// One configured MCP server.
struct ServerConfig {
    std::string name;
    /// The command to run, for a stdio server.
    std::string command;
    std::vector<std::string> args;
    /// The environment for the server.
    std::vector<std::pair<std::string, std::string>> env;
    /// The URL, for an http server.
    std::string url;
    /// The transport: "stdio" or "http".
    std::string transport = "stdio";

    /// The id of the client, when the configuration names one.
    ///
    /// The reference reads it from the `oauth` block of the configuration,
    /// under the key `clientId`.
    std::string oauth_client_id;

    /// The address of the metadata of the authorization server, when the
    /// configuration names one.
    ///
    /// The reference reads it from the `oauth` block of the configuration,
    /// under the key `authServerMetadataUrl`.
    std::string oauth_metadata_url;

    /// The scopes the authorization asks for, when the configuration names
    /// them.
    std::vector<std::string> oauth_scopes;
};

/// The tools an MCP server offers.
struct ServerTool {
    std::string server_name;
    std::string tool_name;
    std::string description;
    boost::json::object input_schema;
    bool read_only = false;
    bool destructive = false;
    bool open_world = false;
};

/// One resource an MCP server offers.
struct ServerResource {
    std::string server_name;
    std::string uri;
    std::string name;
    std::string description;
    std::string mime_type;
};

/// One prompt an MCP server offers.
struct ServerPrompt {
    std::string server_name;
    std::string prompt_name;
    std::string description;
    /// The names of the arguments the prompt takes.
    std::vector<std::string> argument_names;
};

/// A connected MCP server and the tools it offers.
class Server {
public:
    Server(ServerConfig config, boost::asio::io_context& io_context);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Connects to the server and lists its tools.
    bool connect(std::string* error);

    /// Builds the transport of the server again and connects it once more.
    ///
    /// The transport of a server of the network carries the token of the
    /// authentication, so it is built again after the user has signed in.
    bool reconnect(std::string* error);

    /// The tools the server offers.
    ///
    /// The servers connect in the background, so the list is read under a
    /// lock: it is filled while the session is already running.
    std::vector<ServerTool> tools() const;

    /// The resources the server offers.
    std::vector<ServerResource> resources() const;

    /// The prompts the server offers.
    std::vector<ServerPrompt> prompts() const;

    /// Runs one of the prompts of the server.
    ///
    /// The text of the prompt is returned.
    std::string run_prompt(const std::string& prompt_name,
                           const std::vector<std::string>& arguments, std::string* error);

    /// Reads a resource of the server.
    ///
    /// The result holds the contents of the resource; a binary one is
    /// reported as base64.
    boost::json::value read_resource(const std::string& uri, std::string* error);

    /// The name of the server.
    const std::string& name() const { return config_.name; }

    /// The configuration of the server.
    ///
    /// The authentication of the server needs it: the address of the server
    /// and the options of its authentication.
    const ServerConfig& config() const { return config_; }

    /// True when the server is connected and ready to be called.
    bool is_connected() const;

    /// The result of a tool call.
    struct ToolCall {
        /// The text of the result.
        std::string content;
        /// The structured content, when the server provided it.
        boost::json::value structured_content = boost::json::value{nullptr};
        /// The server's `_meta` passthrough.
        boost::json::value meta = boost::json::value{nullptr};
        /// True when the server reported the call as failed.
        bool is_error = false;
    };

    /// Calls a tool and returns its result.
    ///
    /// A text result is returned as a string; a structured result is returned
    /// as the JSON value the server sent.
    boost::json::value call_tool(const std::string& tool_name, const boost::json::object& input,
                                 std::string* error);

    /// Calls a tool and returns its result together with the structured
    /// content and the metadata the server sent.
    ToolCall call_tool_detailed(const std::string& tool_name, const boost::json::object& input,
                                std::string* error);

private:
    struct Impl;

    /// Everything the server wrote to its standard error.
    ///
    /// The reference keeps the error of a server that could not be used, and
    /// reports it next to the reason the connection failed.
    std::string server_stderr() const;

    ServerConfig config_;
    std::vector<ServerTool> tools_;
    std::vector<ServerResource> resources_;
    std::vector<ServerPrompt> prompts_;
    /// The lock of the lists of the server.
    mutable std::mutex mutex_;
    std::unique_ptr<Impl> impl_;
};

/// The state of one MCP server.
struct ServerStatus {
    /// The name of the server.
    std::string name;

    /// True when the server is connected and ready to be called.
    bool connected = false;

    /// True when the server is still being connected.
    bool connecting = false;

    /// True when the server asks for an authentication.
    ///
    /// The reference counts the servers that wait for an authentication
    /// apart from the ones that failed.
    bool needs_auth = false;

    /// The reason the server could not be used.
    std::string error;

    /// The number of tools the server offers.
    size_t tool_count = 0;

    /// The number of resources the server offers.
    size_t resource_count = 0;

    /// The number of prompts the server offers.
    size_t prompt_count = 0;
};

/// The MCP servers of a session.
class Registry {
public:
    Registry(boost::asio::io_context& io_context);

    ~Registry();

    /// Reads the server configurations from a settings object.
    ///
    /// The servers are read from the `mcpServers` key. A configuration that
    /// names neither a command nor a url, or that asks for a transport this
    /// build cannot speak, is rejected and reported through `errors()`.
    void configure(const boost::json::object& settings);

    /// Connects to every configured server.
    ///
    /// The connections are made in the background: the session must be ready
    /// to talk before a server that takes its time is up. The reference does
    /// the same — a server never holds up the first turn.
    ///
    /// A server that cannot be reached does not stop the session: the failure
    /// is collected and the remaining servers are connected.
    void connect_all();

    /// True once every server has been given its chance.
    bool connections_ready() const;

    /// Waits until every server has been given its chance.
    ///
    /// The wait is given up on after the timeout.
    void wait_for_connections(std::chrono::milliseconds timeout);

    /// The tools of every connected server, as Claude Code tools.
    ///
    /// Each tool is named `mcp__<server>__<tool>`; its description is the
    /// server's description for that tool, truncated at 1000 characters with
    /// `… [truncated]` appended.
    Tools tools() const;

    /// The names of the connected servers.
    std::vector<std::string> server_names() const;

    /// The commands the servers offer as prompts.
    ///
    /// Each command is named `mcp__<server>__<prompt>` and is shown to the
    /// user as `<server>:<prompt> (MCP)`.
    std::vector<McpCommand> commands() const;

    /// Runs one of the commands of the servers.
    ///
    /// The arguments are given to the prompt by name. The text of the prompt
    /// is returned.
    std::string run_command(const McpCommand& command, const std::vector<std::string>& arguments,
                            std::string* error);

    /// The servers that could not be used, with the reason.
    std::vector<std::pair<std::string, std::string>> errors() const;

    /// The number of servers that are connected.
    size_t connected_count() const;

    /// The state of every configured server.
    ///
    /// A server that is not connected carries the reason it failed.
    std::vector<ServerStatus> status() const;

    /// Calls a tool of a server and returns its result.
    ///
    /// The result holds the contents of the answer; the reason the call
    /// failed is given back in `error`. The value is null when the server is
    /// not there, or when it is not connected.
    ///
    /// The call waits for the answer of the server, so the caller has to
    /// keep the context of the session going while it waits.
    boost::json::value call_tool(const std::string& server, const std::string& tool,
                                 const boost::json::object& input, std::string* error);

    /// One of the servers of the session.
    ///
    /// The caller keeps the server alive, so that a call may be made to it
    /// after the session has ended. The server is empty when there is none
    /// of that name.
    std::shared_ptr<Server> server(const std::string& name) const;

private:
    boost::asio::io_context& io_context_;
    std::vector<std::shared_ptr<Server>> servers_;
    std::vector<std::pair<std::string, std::string>> errors_;
    /// The thread the servers are connected on.
    std::thread connect_thread_;
    /// The lock of the errors and of the state of the connections.
    mutable std::mutex mutex_;
    /// Woken when the connections are done.
    std::condition_variable cv_;
    /// True once every server has been given its chance.
    bool ready_ = false;
};

// ---------------------------------------------------------------------------
// The authentication of the servers of MCP
// ---------------------------------------------------------------------------

/// The tokens of the authentication of a server of MCP.
struct OAuthTokens {
    /// The token the server is called with.
    std::string access_token;

    /// The token that gets a new access token.
    std::string refresh_token;

    /// The scopes the tokens carry, separated by spaces.
    std::string scope;

    /// When the access token stops working, in milliseconds since the epoch.
    int64_t expires_at = 0;
};

/// The endpoints of the authorization server of a server of MCP.
struct AuthorizationServer {
    /// The issuer of the authorization server.
    std::string issuer;

    /// Where the user is sent to sign in.
    std::string authorization_endpoint;

    /// Where the code is exchanged for the tokens.
    std::string token_endpoint;

    /// Where a client is registered.
    std::string registration_endpoint;

    /// The scopes the server offers.
    std::vector<std::string> scopes_supported;

    /// The scope the authorization asks for, when the server names one.
    ///
    /// The reference reads it out of the answer of the discovery: the key
    /// `scope` first, then `default_scope`.
    std::string scope;

    /// True when the metadata of the authorization server was found.
    bool found = false;
};

/// What is kept about one server of MCP.
struct StoredServerAuth {
    /// The name of the server.
    std::string server_name;

    /// The address of the server.
    std::string server_url;

    /// The id of the client the server was registered with.
    std::string client_id;

    /// The secret of the client, when the server gave one.
    std::string client_secret;

    /// The address of the authorization server.
    std::string authorization_server_url;

    /// The tokens of the server.
    OAuthTokens tokens;
};

/// The result of the authentication of a server of MCP.
struct AuthResult {
    /// True when the server was authenticated.
    bool ok = false;

    /// The tokens of the server, when there are any.
    OAuthTokens tokens;

    /// The reason the authentication failed.
    std::string error;

    /// The URL the browser was sent to.
    std::string authorization_url;
};

/// The key the tokens of a server are kept under.
///
/// The reference names the entry after the name of the server and the hash
/// of its configuration, so that two servers of the same name do not share
/// their tokens.
std::string server_key(const std::string& name, const ServerConfig& config);

/// The authorization servers of the answer of the discovery of the protected
/// resource (RFC 9728).
///
/// The answer names them under the key `authorization_servers`; the list is
/// empty when the answer names none.
std::vector<std::string> parse_protected_resource(const boost::json::object& metadata);

/// The endpoints of the authorization server, out of the answer of the
/// discovery of the authorization server (RFC 8414).
///
/// The reference reads the scope of the answer the way the servers write it:
/// the key `scope` first, then `default_scope`, then the scopes the server
/// offers.
AuthorizationServer parse_authorization_server(const boost::json::object& metadata);

/// The URL the browser is sent to.
///
/// The challenge is the one of the PKCE, the state the one that comes back
/// with the code, and the resource the address of the server, the way the
/// reference asks for it.
std::string build_authorization_url(const AuthorizationServer& server, const std::string& client_id,
                                    const std::string& redirect_uri, const std::string& challenge,
                                    const std::string& state, const std::string& scope,
                                    const std::string& resource);

/// Keeps what is known about a server of MCP.
///
/// The tokens go into the keychain of the machine, under the entry of the
/// credentials of the account and under the key `mcpOAuth`, the way the
/// reference keeps them; elsewhere they go into the file of the credentials
/// of the configuration folder.
///
/// A path of its own keeps them in that file instead, which is what the
/// tests do, so that the tokens of the user are never touched by a test.
bool save_server_auth(const std::string& key, const StoredServerAuth& auth, std::string* error = nullptr,
                      const std::string& path = "");

/// What is known about a server of MCP, when there is something.
std::optional<StoredServerAuth> load_server_auth(const std::string& key,
                                                  const std::string& path = "");

/// Forgets what is known about a server of MCP.
bool clear_server_auth(const std::string& key, const std::string& path = "");

/// True when the tokens of a server are there and have not expired.
bool tokens_usable(const OAuthTokens& tokens);

/// Gets a new access token with the refresh token of a server.
///
/// The tokens are kept when the server gave new ones; the failure is
/// reported through `error`.
bool refresh_server_auth(const ServerConfig& config, std::string* error = nullptr,
                         const std::string& path = "");

/// The authentication of one server of MCP.
///
/// The flow looks for the authorization server, registers a client, opens the
/// browser and waits for the code the browser brings; the code is exchanged
/// for the tokens, which are kept.
///
/// The flow runs on a thread of its own, so that the interface may be drawn
/// while it waits for the user.
class AuthFlow {
public:
    explicit AuthFlow(ServerConfig config);
    ~AuthFlow();

    AuthFlow(const AuthFlow&) = delete;
    AuthFlow& operator=(const AuthFlow&) = delete;

    /// The state the flow is in.
    enum class Stage {
        /// The flow looks for the authorization server.
        Working,
        /// The flow waits for the user to sign in.
        Waiting,
        /// The flow has ended.
        Finished,
    };

    /// Starts the flow on a thread of its own.
    void start();

    /// Says that the browser must not be opened.
    ///
    /// The reference uses it when the user is given the address of the
    /// authorization instead of being sent to the browser.
    void set_skip_browser(bool skip);

    /// The place the tokens of the server are kept.
    ///
    /// The tokens go into the keychain of the machine by default; a path of
    /// its own keeps them in that file instead, which is what the tests do,
    /// so that the tokens of the user are never touched by a test.
    void set_storage_path(std::string path);

    /// The URL the browser is sent to, once the flow has found it.
    std::optional<std::string> authorization_url() const;

    /// The state the flow is in.
    Stage stage() const;

    /// Hands the address the browser shows back to the flow.
    ///
    /// The flow waits for it when the local server did not get the code; the
    /// address is the one of the callback, with the code and the state in it.
    void submit_callback_url(std::string url);

    /// Gives the flow up.
    ///
    /// The result of the flow carries the reason it was given up on.
    void cancel();

    /// True once the flow has ended.
    bool finished() const;

    /// The result of the flow, once it has ended.
    std::optional<AuthResult> result() const;

    /// Waits for the flow to end.
    void wait();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

/// Authenticates one server of MCP and returns the tokens, or the reason the
/// authentication failed.
///
/// The browser is opened in the background and the flow waits for the code
/// the browser brings; it gives up after five minutes.
AuthResult authenticate_server(ServerConfig config);

} // namespace cc::mcp
