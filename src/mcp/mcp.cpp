#include "mcp/mcp.hpp"

#include "auth/listener.hpp"
#include "auth/oauth.hpp"
#include "tools/tool_use_context.hpp"
#include "utils/fs.hpp"
#include "utils/hash.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/process.hpp"
#include "utils/string.hpp"
#include "utils/time.hpp"

#include <ai/mcp/mcp_client.hpp>
#include <ai/mcp/transport.hpp>

#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>

#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cc::mcp {
namespace {

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

/// The prefix of the server names Claude Code manages itself.
constexpr std::string_view kClaudeAiServerPrefix = "claude.ai ";

/// The number of characters of a tool description that are sent to the model.
constexpr size_t kMaxDescriptionLength = 2048;

/// The marker appended to a truncated tool description.
constexpr std::string_view kTruncationMarker = "… [truncated]";

/// How long the handshake with a server may take before it is given up on.
constexpr std::chrono::seconds kConnectTimeout{60};

/// How much of the error of a server is kept.
constexpr size_t kMaxStderrLength = 1024 * 1024;

/// The largest tool result that is kept in the transcript.
constexpr size_t kMaxResultSizeChars = 100000;

/// Normalizes a server or tool name so that it matches the API pattern
/// `^[a-zA-Z0-9_-]{1,64}$`.
///
/// Every character outside that set becomes an underscore. The names of the
/// servers Claude Code manages itself additionally have their runs of
/// underscores collapsed and their leading and trailing underscores stripped,
/// so that they cannot interfere with the `__` delimiter.
std::string normalize_name(std::string_view name) {
    std::string normalized;
    normalized.reserve(name.size());
    for (uint32_t code_point : utf8_codepoints(name)) {
        const bool allowed = (code_point >= 'a' && code_point <= 'z') ||
                             (code_point >= 'A' && code_point <= 'Z') ||
                             (code_point >= '0' && code_point <= '9') || code_point == '_' ||
                             code_point == '-';
        if (allowed) {
            normalized += utf8_encode(code_point);
        } else {
            normalized.push_back('_');
        }
    }

    if (name.starts_with(kClaudeAiServerPrefix)) {
        // Collapse the runs of underscores, then drop one at each end.
        std::string collapsed;
        collapsed.reserve(normalized.size());
        for (char c : normalized) {
            if (c == '_' && !collapsed.empty() && collapsed.back() == '_') continue;
            collapsed.push_back(c);
        }
        if (!collapsed.empty() && collapsed.front() == '_') collapsed.erase(collapsed.begin());
        if (!collapsed.empty() && collapsed.back() == '_') collapsed.pop_back();
        normalized = std::move(collapsed);
    }

    return normalized;
}

/// Builds the name the model sees for a tool of a server.
std::string build_tool_name(std::string_view server_name, std::string_view tool_name) {
    return "mcp__" + normalize_name(server_name) + "__" + normalize_name(tool_name);
}

/// Builds the name a prompt of a server is known by.
///
/// Only the name of the server is normalized: the name of the prompt is the
/// one the server gave it, and the server is the one that answers to it.
std::string build_command_name(std::string_view server_name, std::string_view prompt_name) {
    return "mcp__" + normalize_name(server_name) + "__" + std::string(prompt_name);
}

/// Expands `${VAR}` and `${VAR:-default}` in a configuration value.
///
/// A variable that is neither set nor given a default is left as it is, so
/// that the error the server reports names it.
std::string expand_env_vars(std::string_view value) {
    std::string expanded;
    expanded.reserve(value.size());

    size_t position = 0;
    while (position < value.size()) {
        if (value[position] != '$' || position + 1 >= value.size() || value[position + 1] != '{') {
            expanded.push_back(value[position++]);
            continue;
        }

        const size_t end = value.find('}', position + 2);
        if (end == std::string_view::npos) {
            expanded.push_back(value[position++]);
            continue;
        }

        const std::string_view expression = value.substr(position + 2, end - position - 2);
        // Only the first ":-" separates the name from the default value.
        const size_t separator = expression.find(":-");
        const std::string_view name =
            separator == std::string_view::npos ? expression : expression.substr(0, separator);
        const std::string_view fallback =
            separator == std::string_view::npos ? std::string_view{} : expression.substr(separator + 2);

        if (const char* environment = std::getenv(std::string(name).c_str())) {
            expanded += environment;
        } else if (separator != std::string_view::npos) {
            expanded += fallback;
        } else {
            expanded.append(value.substr(position, end - position + 1));
        }
        position = end + 1;
    }

    return expanded;
}

/// Truncates a tool description at `kMaxDescriptionLength` characters.
std::string truncate_description(std::string_view description) {
    size_t length = 0;
    for (char c : description) {
        // Continuation bytes do not start a new character.
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++length;
    }
    if (length <= kMaxDescriptionLength) return std::string(description);

    std::string truncated;
    truncated.reserve(kMaxDescriptionLength + kTruncationMarker.size());
    size_t kept = 0;
    for (char c : description) {
        if (kept == kMaxDescriptionLength) break;
        truncated.push_back(c);
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++kept;
    }
    truncated += kTruncationMarker;
    return truncated;
}

// ---------------------------------------------------------------------------
// JSON-RPC plumbing
// ---------------------------------------------------------------------------

/// Parses a JSON-RPC message, returning null when it is malformed.
boost::json::value parse_message(const std::string& text) {
    boost::system::error_code error;
    boost::json::value value = boost::json::parse(text, error);
    if (error) return boost::json::value{nullptr};
    return value;
}

/// Points the standard error of the process at a pipe.
///
/// The reference starts its MCP servers with `stderr: 'pipe'`, so that the
/// log of a server never lands in the middle of the interface. The port does
/// the same: the error of the server is kept, and it becomes part of the
/// reason a server that could not be used carries.
class StderrCapture {
public:
    StderrCapture() {
        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0) return;
        saved_ = ::dup(STDERR_FILENO);
        if (saved_ < 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            return;
        }
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[1]);
        read_fd_ = fds[0];
    }

    ~StderrCapture() {
        restore();
        if (read_fd_ >= 0) ::close(read_fd_);
    }

    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;

    /// Gives the terminal its standard error back.
    void restore() {
        if (saved_ < 0) return;
        ::dup2(saved_, STDERR_FILENO);
        ::close(saved_);
        saved_ = -1;
    }

    /// Hands the reading end over to the caller.
    int release() {
        const int fd = read_fd_;
        read_fd_ = -1;
        return fd;
    }

private:
    int saved_ = -1;
    int read_fd_ = -1;
};

/// A transport that records the raw JSON-RPC traffic that passes through it.
///
/// The ai-sdk-cpp client reduces `tools/list` to a name, a description and a
/// schema, and collapses a `tools/call` result down to its text. Recording
/// the raw messages lets us recover the tool annotations and the structured
/// content that the server sent.
class RecordingTransport : public ai::mcp::Transport {
public:
    explicit RecordingTransport(std::unique_ptr<ai::mcp::Transport> inner, bool capture_stderr = false)
        : inner_(std::move(inner)), capture_stderr_(capture_stderr) {}

    ~RecordingTransport() override {
        if (drain_.joinable()) drain_.join();
    }

    void start() override {
        if (!capture_stderr_) {
            inner_->start();
            return;
        }
        // The child inherits the standard error of the process, so it is
        // pointed at a pipe for as long as the child is started.
        StderrCapture capture;
        inner_->start();
        capture.restore();
        const int fd = capture.release();
        if (fd >= 0) drain_ = std::thread([this, fd]() { drain(fd); });
    }

    void stop() override { inner_->stop(); }

    void send(const std::string& json_message) override {
        note_request(json_message);
        inner_->send(json_message);
    }

    std::string receive() override {
        std::string message = inner_->receive();
        note_response(message);
        return message;
    }

    /// The raw result of the last `tools/list` response.
    boost::json::value tools_list() const {
        std::lock_guard lock(mutex_);
        return tools_list_;
    }

    /// The raw result of the last `tools/call` response.
    boost::json::value tools_call() const {
        std::lock_guard lock(mutex_);
        return tools_call_;
    }

    /// Everything the server wrote to its standard error.
    std::string stderr_output() const {
        std::lock_guard lock(mutex_);
        return stderr_output_;
    }

private:
    /// Reads the error of the server until it stops talking.
    ///
    /// The pipe is drained as it fills, so that a server that talks a lot
    /// never blocks on a write.
    void drain(int fd) {
        std::array<char, 4096> buffer{};
        while (true) {
            const ssize_t got = ::read(fd, buffer.data(), buffer.size());
            if (got <= 0) break;
            std::lock_guard lock(mutex_);
            // The error is capped, so that a server that talks a lot does not
            // take the memory of the session.
            if (stderr_output_.size() < kMaxStderrLength) {
                stderr_output_.append(buffer.data(), static_cast<size_t>(got));
            }
        }
        ::close(fd);
    }

    /// Remembers which method each outstanding request id belongs to.
    void note_request(const std::string& text) {
        const boost::json::value message = parse_message(text);
        if (!message.is_object()) return;
        const auto& object = message.as_object();
        const auto id = object.find("id");
        const auto method = object.find("method");
        if (id == object.end() || method == object.end()) return;
        if (!id->value().is_int64() || !method->value().is_string()) return;

        std::lock_guard lock(mutex_);
        pending_[id->value().as_int64()] = std::string(method->value().as_string().c_str());
    }

    /// Keeps the result of the responses we care about.
    void note_response(const std::string& text) {
        const boost::json::value message = parse_message(text);
        if (!message.is_object()) return;
        const auto& object = message.as_object();
        const auto id = object.find("id");
        if (id == object.end() || !id->value().is_int64()) return;

        std::lock_guard lock(mutex_);
        const auto pending = pending_.find(id->value().as_int64());
        if (pending == pending_.end()) return;
        const std::string method = pending->second;
        pending_.erase(pending);

        if (auto error = object.find("error"); error != object.end() && !error->value().is_null()) {
            // The client turns an error response into an exception, so there
            // is no result to record.
            return;
        }
        const auto result = object.find("result");
        if (result == object.end()) return;
        if (method == "tools/list") {
            tools_list_ = result->value();
        } else if (method == "tools/call") {
            tools_call_ = result->value();
        }
    }

    std::unique_ptr<ai::mcp::Transport> inner_;
    mutable std::mutex mutex_;
    /// The method of every request still awaiting a response, by request id.
    std::map<int64_t, std::string> pending_;
    boost::json::value tools_list_ = boost::json::value{nullptr};
    boost::json::value tools_call_ = boost::json::value{nullptr};
    /// Everything the server wrote to its standard error.
    std::string stderr_output_;
    /// True when the error of the server is to be kept.
    bool capture_stderr_ = false;
    /// The thread that drains the error of the server.
    std::thread drain_;
};

/// Builds the transport a server is reached over.
std::unique_ptr<ai::mcp::Transport> make_transport(const ServerConfig& config) {
    if (config.transport == "http") {
        // A server that needs an authentication is reached with the token of
        // the user, the way the reference reaches it.
        std::unordered_map<std::string, std::string> headers;
        if (const auto auth = load_server_auth(server_key(config.name, config))) {
            if (!auth->tokens.access_token.empty()) {
                headers["Authorization"] = "Bearer " + auth->tokens.access_token;
            }
        }
        return std::make_unique<ai::mcp::StreamableHttpTransport>(config.url, std::move(headers));
    }

    std::unordered_map<std::string, std::string> env;
    env.reserve(config.env.size());
    for (const auto& [key, value] : config.env) {
        env.emplace(key, value);
    }
    return std::make_unique<ai::mcp::StdioTransport>(config.command, config.args, std::move(env));
}

/// Reads the annotations of a tool out of a raw `tools/list` result.
void apply_annotations(ServerTool& tool, const boost::json::value& tools_list,
                       const std::string& tool_name) {
    if (!tools_list.is_object()) return;
    const auto& object = tools_list.as_object();
    const auto tools = object.find("tools");
    if (tools == object.end() || !tools->value().is_array()) return;

    for (const auto& entry : tools->value().as_array()) {
        if (!entry.is_object()) continue;
        const auto& description = entry.as_object();
        if (jget_string(description, "name") != tool_name) continue;

        const boost::json::object annotations = jget_object(description, "annotations");
        tool.read_only = jget_bool(annotations, "readOnlyHint", false);
        tool.destructive = jget_bool(annotations, "destructiveHint", false);
        tool.open_world = jget_bool(annotations, "openWorldHint", false);
        return;
    }
}

/// The outcome of a connection attempt, shared with the worker thread.
struct ConnectState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    std::string error;
    std::vector<ServerTool> tools;
    std::vector<ServerResource> resources;
    std::vector<ServerPrompt> prompts;
};

} // namespace

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

struct Server::Impl {
    /// The session's event loop.
    boost::asio::io_context* io_context = nullptr;
    /// The transport the client talks over. The client owns it, so the
    /// pointer is only valid while the client is alive.
    RecordingTransport* transport = nullptr;
    /// The MCP client.
    ///
    /// It is shared so that the worker thread that performs the handshake can
    /// keep it alive after the attempt has been given up on.
    std::shared_ptr<ai::mcp::McpClient> client;
    /// True once the handshake has completed.
    std::atomic<bool> connected{false};
};

Server::Server(ServerConfig config, boost::asio::io_context& io_context)
    : config_(std::move(config)), impl_(std::make_unique<Impl>()) {
    impl_->io_context = &io_context;

    // Only a server that is a program of its own writes to the standard
    // error of the terminal.
    auto recorder = std::make_unique<RecordingTransport>(make_transport(config_),
                                                        config_.transport == "stdio");
    impl_->transport = recorder.get();
    impl_->client = std::make_shared<ai::mcp::McpClient>(std::move(recorder), config_.name);
}

Server::~Server() = default;

std::string Server::server_stderr() const {
    if (!impl_ || !impl_->transport) return {};
    const std::string output = impl_->transport->stderr_output();
    if (output.empty()) return {};
    return "\nServer stderr: " + output;
}

bool Server::is_connected() const {
    return impl_->connected.load(std::memory_order_acquire) && impl_->client &&
           impl_->client->is_connected();
}

std::vector<ServerTool> Server::tools() const {
    std::lock_guard lock(mutex_);
    return tools_;
}

std::vector<ServerResource> Server::resources() const {
    std::lock_guard lock(mutex_);
    return resources_;
}

std::vector<ServerPrompt> Server::prompts() const {
    std::lock_guard lock(mutex_);
    return prompts_;
}

bool Server::reconnect(std::string* error) {
    if (error) error->clear();

    // The transport of a server of the network carries the token of the
    // authentication, so it is built again: the token of the user is the one
    // that is known now.
    auto recorder = std::make_unique<RecordingTransport>(make_transport(config_),
                                                        config_.transport == "stdio");
    impl_->transport = recorder.get();
    impl_->client = std::make_shared<ai::mcp::McpClient>(std::move(recorder), config_.name);
    impl_->connected.store(false, std::memory_order_release);

    {
        std::lock_guard lock(mutex_);
        tools_.clear();
        resources_.clear();
        prompts_.clear();
    }

    return connect(error);
}

bool Server::connect(std::string* error) {
    if (is_connected()) return true;
    auto state = std::make_shared<ConnectState>();
    const std::shared_ptr<ai::mcp::McpClient> client = impl_->client;
    // The client owns the transport, so the pointer stays valid for as long as
    // the worker holds on to the client.
    RecordingTransport* transport = impl_->transport;

    // The transport blocks on the server, so the handshake runs on a worker
    // thread: a server that starts but never answers must not hang the
    // session.
    std::thread worker([state, client, transport]() {
        std::vector<ServerTool> tools;
        std::vector<ServerResource> resources;
        std::vector<ServerPrompt> prompts;
        std::string failure;
        bool ok = false;
        try {
            client->connect().get();
            const std::vector<ai::mcp::McpTool> listed = client->list_tools().get();
            tools.reserve(listed.size());
            for (const auto& tool : listed) {
                ServerTool entry;
                entry.server_name = client->server_name();
                entry.tool_name = tool.name;
                entry.description = tool.description;
                entry.input_schema = tool.input_schema.raw();
                apply_annotations(entry, transport->tools_list(), tool.name);
                tools.push_back(std::move(entry));
            }

            // The resources of the server are listed as well; a server
            // without them is not an error.
            try {
                const std::vector<ai::mcp::McpResource> listed_resources =
                    client->list_resources().get();
                resources.reserve(listed_resources.size());
                for (const auto& resource : listed_resources) {
                    ServerResource entry;
                    entry.server_name = client->server_name();
                    entry.uri = resource.uri;
                    entry.name = resource.name;
                    entry.description = resource.description;
                    entry.mime_type = resource.mime_type;
                    resources.push_back(std::move(entry));
                }
            } catch (const std::exception&) {
                // The server does not offer the resources.
            }

            // The prompts of the server become slash commands, the way the
            // reference turns them into commands.
            try {
                const std::vector<ai::mcp::McpPrompt> listed_prompts =
                    client->list_prompts().get();
                prompts.reserve(listed_prompts.size());
                for (const auto& prompt : listed_prompts) {
                    ServerPrompt entry;
                    entry.server_name = client->server_name();
                    entry.prompt_name = prompt.name;
                    entry.description = prompt.description;
                    for (const auto& argument : prompt.arguments) {
                        entry.argument_names.push_back(argument.name);
                    }
                    prompts.push_back(std::move(entry));
                }
            } catch (const std::exception&) {
                // The server does not offer the prompts.
            }

            ok = true;
        } catch (const std::exception& e) {
            failure = e.what();
        }

        {
            std::lock_guard lock(state->mutex);
            state->tools = std::move(tools);
            state->resources = std::move(resources);
            state->prompts = std::move(prompts);
            state->ok = ok;
            state->error = std::move(failure);
            state->done = true;
        }
        state->cv.notify_all();
    });

    bool timed_out = false;
    {
        std::unique_lock lock(state->mutex);
        const auto deadline = std::chrono::steady_clock::now() + kConnectTimeout;
        while (!state->done) {
            if (state->cv.wait_for(lock, std::chrono::milliseconds(20),
                                   [&state]() { return state->done; })) {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                timed_out = true;
                break;
            }
            lock.unlock();
            // The client of the SDK speaks to the server in the calling
            // thread, so the event loop of the session is left alone: the
            // handshake must not run on it, or the turn of the session would
            // be interrupted.
            lock.lock();
        }
    }

    if (timed_out) {
        // The worker keeps the client alive and finishes on its own.
        worker.detach();
        if (error) {
            *error = "the handshake timed out after " + std::to_string(kConnectTimeout.count()) +
                     " seconds";
            *error += server_stderr();
        }
        return false;
    }

    worker.join();

    if (!state->ok) {
        if (error) {
            *error = state->error.empty() ? "the server could not be reached" : state->error;
            *error += server_stderr();
        }
        return false;
    }

    {
        std::lock_guard lock(mutex_);
        tools_ = std::move(state->tools);
        resources_ = std::move(state->resources);
        prompts_ = std::move(state->prompts);
    }
    impl_->connected.store(true, std::memory_order_release);
    return true;
}

Server::ToolCall Server::call_tool_detailed(const std::string& tool_name,
                                            const boost::json::object& input, std::string* error) {
    ToolCall call;
    if (error) error->clear();
    if (!is_connected()) {
        if (error) *error = "the server \"" + config_.name + "\" is not connected";
        return call;
    }

    boost::json::value result;
    try {
        result = impl_->client->call_tool(tool_name, boost::json::value(input)).get();
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return call;
    }

    const boost::json::value raw = impl_->transport->tools_call();
    if (raw.is_object()) {
        const auto& object = raw.as_object();
        call.content = json_tool_result_text(raw);
        if (auto structured = object.find("structuredContent"); structured != object.end() &&
                                                              !structured->value().is_null()) {
            call.structured_content = structured->value();
        }
        if (auto meta = object.find("_meta"); meta != object.end() && !meta->value().is_null()) {
            call.meta = meta->value();
        }
        call.is_error = jget_bool(object, "isError", false);
    } else if (result.is_string()) {
        call.content = std::string(result.as_string().c_str());
    } else {
        call.content = json_dump(result);
    }

    return call;
}

std::string Server::run_prompt(const std::string& prompt_name,
                               const std::vector<std::string>& arguments, std::string* error) {
    if (!is_connected()) {
        if (error) *error = "the server is not connected";
        return {};
    }

    // The arguments are given to the prompt by name.
    boost::json::object values;
    const ServerPrompt* prompt = nullptr;
    for (const auto& entry : prompts_) {
        if (entry.prompt_name == prompt_name) prompt = &entry;
    }
    if (prompt != nullptr) {
        for (size_t i = 0; i < prompt->argument_names.size(); ++i) {
            if (i < arguments.size()) values[prompt->argument_names[i]] = arguments[i];
        }
    }

    try {
        const std::vector<ai::mcp::McpPromptMessage> messages =
            impl_->client->get_prompt(prompt_name, std::move(values)).get();

        std::string text;
        for (const auto& message : messages) {
            if (!text.empty()) text += "\n";
            text += message.text;
        }
        return text;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return {};
    }
}

boost::json::value Server::read_resource(const std::string& uri, std::string* error) {
    if (!is_connected()) {
        if (error) *error = "the server is not connected";
        return boost::json::value{nullptr};
    }

    try {
        const std::vector<ai::mcp::McpResourceContent> contents =
            impl_->client->read_resource(uri).get();

        boost::json::array out;
        for (const auto& content : contents) {
            boost::json::object entry;
            entry["uri"] = content.uri;
            if (!content.mime_type.empty()) entry["mimeType"] = content.mime_type;
            if (content.blob_b64.has_value()) {
                entry["blob"] = *content.blob_b64;
            } else {
                entry["text"] = content.text;
            }
            out.emplace_back(std::move(entry));
        }
        return boost::json::value(std::move(out));
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return boost::json::value{nullptr};
    }
}

boost::json::value Server::call_tool(const std::string& tool_name, const boost::json::object& input,
                                     std::string* error) {
    const ToolCall call = call_tool_detailed(tool_name, input, error);
    if (error && !error->empty()) return boost::json::value{nullptr};
    if (!call.structured_content.is_null()) return call.structured_content;
    return boost::json::value(call.content);
}

namespace {

/// A tool of an MCP server, as the model sees it.
class McpTool : public Tool {
public:
    McpTool(std::shared_ptr<Server> server, ServerTool tool)
        : server_(std::move(server)), tool_(std::move(tool)) {
        name_ = build_tool_name(tool_.server_name, tool_.tool_name);
        description_ = truncate_description(tool_.description);
    }

    std::string_view name() const override { return name_; }

    std::string description() const override { return description_; }

    boost::json::object input_schema() const override { return tool_.input_schema; }

    ToolResult call(const boost::json::object& input, ToolUseContext& context) override;

    bool is_concurrency_safe(const boost::json::object& input) const override {
        return tool_.read_only;
    }

    bool is_read_only(const boost::json::object& input) const override { return tool_.read_only; }

    bool is_destructive(const boost::json::object& input) const override { return tool_.destructive; }

    bool is_open_world(const boost::json::object& input) const override { return tool_.open_world; }

    std::optional<McpInfo> mcp_info() const override {
        return McpInfo{tool_.server_name, tool_.tool_name};
    }

    size_t max_result_size_chars() const override { return kMaxResultSizeChars; }

private:
    std::shared_ptr<Server> server_;
    ServerTool tool_;
    std::string name_;
    std::string description_;
};

ToolResult McpTool::call(const boost::json::object& input, ToolUseContext& context) {
    std::string error;
    const Server::ToolCall call = server_->call_tool_detailed(tool_.tool_name, input, &error);

    ToolResult result;
    if (!error.empty()) {
        result.is_error = true;
        result.content = error;
        return result;
    }

    result.content = call.content;
    result.structured_content = call.structured_content;
    result.is_error = call.is_error;

    boost::json::object meta;
    if (!call.meta.is_null()) meta["_meta"] = call.meta;
    if (!call.structured_content.is_null()) meta["structuredContent"] = call.structured_content;
    result.mcp_meta = boost::json::value(std::move(meta));

    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

Registry::Registry(boost::asio::io_context& io_context) : io_context_(io_context) {}

Registry::~Registry() {
    if (connect_thread_.joinable()) connect_thread_.join();
}

void Registry::configure(const boost::json::object& settings) {
    servers_.clear();
    {
        std::lock_guard lock(mutex_);
        errors_.clear();
    }

    const auto servers = settings.find("mcpServers");
    if (servers == settings.end() || !servers->value().is_object()) return;

    for (const auto& [name, entry] : servers->value().as_object()) {
        if (!entry.is_object()) {
            errors_.emplace_back(name, "the server configuration is not an object");
            continue;
        }
        const boost::json::object& object = entry.as_object();

        ServerConfig config;
        config.name = name;
        config.command = expand_env_vars(jget_string(object, "command"));
        for (const auto& argument : jget_array(object, "args")) {
            if (argument.is_string()) {
                config.args.emplace_back(
                    expand_env_vars(std::string(argument.as_string().c_str())));
            }
        }
        for (const auto& [key, value] : jget_object(object, "env")) {
            if (value.is_string()) {
                config.env.emplace_back(key,
                                        expand_env_vars(std::string(value.as_string().c_str())));
            }
        }
        config.url = expand_env_vars(jget_string(object, "url"));

        // The block of the authentication of the server; the reference reads
        // the id of the client and the address of the metadata out of it.
        const boost::json::object oauth = jget_object(object, "oauth");
        config.oauth_client_id = expand_env_vars(jget_string(oauth, "clientId"));
        config.oauth_metadata_url = expand_env_vars(jget_string(oauth, "authServerMetadataUrl"));
        for (const auto& scope : jget_array(oauth, "scopes")) {
            if (scope.is_string()) config.oauth_scopes.emplace_back(scope.as_string().c_str());
        }

        const std::string type = jget_string(object, "type");
        if (type == "http" || type == "sse") {
            // The legacy SSE transport is spoken over the same endpoint as
            // streamable HTTP, which is the only HTTP transport available.
            config.transport = "http";
        } else if (type == "stdio") {
            config.transport = "stdio";
        } else if (type.empty()) {
            // The type is optional for backwards compatibility: the shape of
            // the configuration decides.
            config.transport = config.command.empty() && !config.url.empty() ? "http" : "stdio";
        } else {
            errors_.emplace_back(name, "unsupported transport \"" + type + "\"");
            continue;
        }

        if (config.transport == "http" && config.url.empty()) {
            errors_.emplace_back(name, "the server has no url");
            continue;
        }
        if (config.transport == "stdio" && config.command.empty()) {
            errors_.emplace_back(name, "the server has no command");
            continue;
        }

        servers_.push_back(std::make_shared<Server>(std::move(config), io_context_));
    }
}

void Registry::connect_all() {
    // The servers are connected in the background: the session has to be
    // ready to talk before a server that takes its time is up. The reference
    // does the same — the connections never hold up the first turn.
    std::lock_guard lock(mutex_);
    if (connect_thread_.joinable()) return;

    connect_thread_ = std::thread([this]() {
        for (auto& server : servers_) {
            std::string error;
            if (server->connect(&error)) continue;

            if (error.empty()) error = "the server could not be reached";
            std::lock_guard lock(mutex_);
            errors_.emplace_back(server->name(), error);
        }
        {
            std::lock_guard lock(mutex_);
            ready_ = true;
        }
        cv_.notify_all();
    });
}

bool Registry::connections_ready() const {
    std::lock_guard lock(mutex_);
    return ready_;
}

void Registry::wait_for_connections(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, timeout, [this]() { return ready_; });
}

std::vector<std::pair<std::string, std::string>> Registry::errors() const {
    std::lock_guard lock(mutex_);
    return errors_;
}

size_t Registry::connected_count() const {
    size_t count = 0;
    for (const auto& server : servers_) {
        if (server->is_connected()) ++count;
    }
    return count;
}

/// The tool that lists the resources of the MCP servers.
///
/// The reference adds it when the servers offer resources.
class ListMcpResourcesTool : public Tool {
public:
    explicit ListMcpResourcesTool(std::vector<std::shared_ptr<Server>> servers)
        : servers_(std::move(servers)) {}

    std::string_view name() const override { return "ListMcpResourcesTool"; }

    std::string description() const override {
        return "Lists available resources from configured MCP servers.\n"
               "Each resource object includes a 'server' field indicating which server it's "
               "from.\n\n"
               "Usage examples:\n"
               "- List all resources from all servers: `listMcpResources`\n"
               "- List resources from a specific server: `listMcpResources({ server: "
               "\"myserver\" })`\n";
    }

    boost::json::object input_schema() const override {
        boost::json::object properties;
        boost::json::object server;
        server["type"] = "string";
        server["description"] = "Optional server name to filter resources by";
        properties["server"] = std::move(server);

        boost::json::object schema;
        schema["type"] = "object";
        schema["properties"] = std::move(properties);
        return schema;
    }

    ToolResult call(const boost::json::object& input, ToolUseContext& context) override;

    bool is_read_only(const boost::json::object& input) const override { return true; }
    bool is_concurrency_safe(const boost::json::object& input) const override { return true; }

    size_t max_result_size_chars() const override { return kMaxResultSizeChars; }

private:
    std::vector<std::shared_ptr<Server>> servers_;
};

/// The tool that reads one resource of an MCP server.
class ReadMcpResourceTool : public Tool {
public:
    explicit ReadMcpResourceTool(std::vector<std::shared_ptr<Server>> servers)
        : servers_(std::move(servers)) {}

    std::string_view name() const override { return "ReadMcpResourceTool"; }

    std::string description() const override {
        return "Reads a specific MCP resource by URI.\n\n"
               "Usage examples:\n"
               "- Read a resource: `readMcpResource({ server: \"myserver\", uri: "
               "\"file://myfile.txt\" })`\n";
    }

    boost::json::object input_schema() const override {
        boost::json::object properties;
        boost::json::object server;
        server["type"] = "string";
        server["description"] = "The MCP server name";
        properties["server"] = std::move(server);

        boost::json::object uri;
        uri["type"] = "string";
        uri["description"] = "The resource URI to read";
        properties["uri"] = std::move(uri);

        boost::json::object schema;
        schema["type"] = "object";
        schema["properties"] = std::move(properties);
        schema["required"] = boost::json::array{"server", "uri"};
        return schema;
    }

    ToolResult call(const boost::json::object& input, ToolUseContext& context) override;

    bool is_read_only(const boost::json::object& input) const override { return true; }
    bool is_concurrency_safe(const boost::json::object& input) const override { return true; }

    size_t max_result_size_chars() const override { return kMaxResultSizeChars; }

private:
    std::vector<std::shared_ptr<Server>> servers_;
};

Tools Registry::tools() const {
    Tools all;
    for (const auto& server : servers_) {
        for (const auto& tool : server->tools()) {
            all.push_back(std::make_shared<McpTool>(server, tool));
        }
    }

    // The reference adds the tools that list and read the resources of the
    // servers, when the servers offer any.
    bool has_resources = false;
    for (const auto& server : servers_) {
        if (!server->resources().empty()) has_resources = true;
    }
    if (has_resources) {
        all.push_back(std::make_shared<ListMcpResourcesTool>(servers_));
        all.push_back(std::make_shared<ReadMcpResourceTool>(servers_));
    }

    return all;
}

ToolResult ListMcpResourcesTool::call(const boost::json::object& input, ToolUseContext& context) {
    const std::string target = cc::jget_string(input, "server");

    ToolResult result;

    boost::json::array resources;
    for (const auto& server : servers_) {
        if (!server->is_connected()) continue;
        if (!target.empty() && server->name() != target) continue;

        for (const auto& resource : server->resources()) {
            boost::json::object entry;
            entry["uri"] = resource.uri;
            entry["name"] = resource.name;
            if (!resource.mime_type.empty()) entry["mimeType"] = resource.mime_type;
            if (!resource.description.empty()) entry["description"] = resource.description;
            entry["server"] = resource.server_name;
            resources.emplace_back(std::move(entry));
        }
    }

    if (!target.empty() && resources.empty()) {
        std::vector<std::string> names;
        for (const auto& server : servers_) {
            if (server->is_connected()) names.push_back(server->name());
        }
        result.is_error = true;
        result.content = "Server \"" + target + "\" not found. Available servers: " +
                         cc::join(names, ", ");
        return result;
    }

    result.structured_content = boost::json::value(std::move(resources));
    result.content = cc::json_dump(result.structured_content);
    return result;
}

ToolResult ReadMcpResourceTool::call(const boost::json::object& input, ToolUseContext& context) {
    const std::string target = cc::jget_string(input, "server");
    const std::string uri = cc::jget_string(input, "uri");

    ToolResult result;
    for (const auto& server : servers_) {
        if (!server->is_connected()) continue;
        if (server->name() != target) continue;

        std::string error;
        const boost::json::value contents = server->read_resource(uri, &error);
        if (!error.empty()) {
            result.is_error = true;
            result.content = error;
            return result;
        }

        result.structured_content = contents;
        result.content = cc::json_dump(contents);
        return result;
    }

    result.is_error = true;
    result.content = "Server \"" + target + "\" not found.";
    return result;
}

std::vector<std::string> Registry::server_names() const {
    std::vector<std::string> names;
    for (const auto& server : servers_) {
        if (server->is_connected()) names.push_back(server->name());
    }
    return names;
}

std::vector<McpCommand> Registry::commands() const {
    std::vector<McpCommand> commands;
    for (const auto& server : servers_) {
        for (const auto& prompt : server->prompts()) {
            McpCommand command;
            command.name = build_command_name(prompt.server_name, prompt.prompt_name);
            command.user_facing_name = prompt.server_name + ":" + prompt.prompt_name + " (MCP)";
            command.description = prompt.description;
            command.argument_names = prompt.argument_names;
            command.server_name = prompt.server_name;
            command.prompt_name = prompt.prompt_name;
            commands.push_back(std::move(command));
        }
    }
    return commands;
}

std::vector<ServerStatus> Registry::status() const {
    std::vector<ServerStatus> states;
    const auto failures = errors();
    const bool ready = connections_ready();

    for (const auto& server : servers_) {
        ServerStatus state;
        state.name = server->name();
        state.connected = server->is_connected();
        if (state.connected) {
            state.tool_count = server->tools().size();
            state.resource_count = server->resources().size();
            state.prompt_count = server->prompts().size();
        } else {
            for (const auto& [name, reason] : failures) {
                if (name == state.name) {
                    state.error = reason;
                    break;
                }
            }
            // A server that has not been given its chance yet is still on
            // its way, not a failure.
            if (state.error.empty() && !ready) state.connecting = true;
        }
        states.push_back(std::move(state));
    }

    return states;
}

boost::json::value Registry::call_tool(const std::string& server, const std::string& tool,
                                       const boost::json::object& input, std::string* error) {
    for (const auto& entry : servers_) {
        if (entry->name() != server) continue;
        if (!entry->is_connected()) {
            if (error) *error = "the server is not connected";
            return boost::json::value{nullptr};
        }
        return entry->call_tool(tool, input, error);
    }

    if (error) *error = "there is no server named " + server;
    return boost::json::value{nullptr};
}

std::shared_ptr<Server> Registry::server(const std::string& name) const {
    for (const auto& entry : servers_) {
        if (entry->name() == name) return entry;
    }
    return {};
}

std::string Registry::run_command(const McpCommand& command,
                                  const std::vector<std::string>& arguments, std::string* error) {
    if (error) error->clear();

    for (const auto& server : servers_) {
        if (server->name() != command.server_name) continue;
        return server->run_prompt(command.prompt_name, arguments, error);
    }

    if (error) *error = "the server \"" + command.server_name + "\" is not connected";
    return {};
}

// ---------------------------------------------------------------------------
// The authentication of the servers of MCP
// ---------------------------------------------------------------------------

namespace {

/// How long one request of the flow may take.
///
/// The reference gives every request of the flow thirty seconds.
constexpr std::chrono::seconds kRequestTimeout{30};

/// How long the flow waits for the user to sign in.
constexpr std::chrono::minutes kFlowTimeout{5};

/// The parts of an address.
struct UrlParts {
    /// The scheme of the address: `http` or `https`.
    std::string scheme = "https";
    /// The host of the address.
    std::string host;
    /// The port of the address.
    std::string port;
    /// The path of the address, with its query.
    std::string path = "/";
    /// True when the address could be read.
    bool valid = false;
};

/// Splits an address into its parts.
UrlParts split_url(const std::string& url) {
    UrlParts parts;

    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return parts;

    parts.scheme = url.substr(0, scheme_end);
    if (parts.scheme != "http" && parts.scheme != "https") return parts;

    const size_t authority_start = scheme_end + 3;
    const size_t path_start = url.find('/', authority_start);

    std::string authority;
    if (path_start == std::string::npos) {
        authority = url.substr(authority_start);
    } else {
        authority = url.substr(authority_start, path_start - authority_start);
        parts.path = url.substr(path_start);
    }
    if (parts.path.empty()) parts.path = "/";

    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        parts.host = authority.substr(0, colon);
        parts.port = authority.substr(colon + 1);
    } else {
        parts.host = authority;
    }

    if (parts.host.empty()) return parts;
    if (parts.port.empty()) parts.port = parts.scheme == "https" ? "443" : "80";
    parts.valid = true;
    return parts;
}

/// The host of the header of a request.
///
/// The port is named when it is not the usual one of the scheme.
std::string host_header(const UrlParts& parts) {
    const bool default_port = (parts.scheme == "https" && parts.port == "443") ||
                              (parts.scheme == "http" && parts.port == "80");
    return default_port ? parts.host : parts.host + ":" + parts.port;
}

/// The address of the host of an address, without its path.
std::string origin_of(const std::string& url) {
    const UrlParts parts = split_url(url);
    if (!parts.valid) return {};

    const bool default_port = (parts.scheme == "https" && parts.port == "443") ||
                              (parts.scheme == "http" && parts.port == "80");
    return parts.scheme + "://" + parts.host + (default_port ? "" : ":" + parts.port);
}

/// The path of an address, without its query.
std::string path_of(const std::string& url) {
    const UrlParts parts = split_url(url);
    if (!parts.valid) return "/";

    std::string path = parts.path;
    const size_t mark = path.find('?');
    if (mark != std::string::npos) path = path.substr(0, mark);
    if (path.empty()) path = "/";
    return path;
}

/// The text of a value, with the characters that are not safe in an address
/// escaped.
///
/// The reference escapes everything the `URL` class of the browser escapes.
std::string url_encode(const std::string& text) {
    static constexpr char kHex[] = "0123456789ABCDEF";

    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9')) {
            out.push_back(c);
            continue;
        }
        switch (c) {
            case '-':
            case '_':
            case '.':
            case '~':
                out.push_back(c);
                continue;
            default:
                break;
        }
        out.push_back('%');
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0f]);
    }
    return out;
}

/// The text of a value, with the escapes of an address read back.
std::string url_decode(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    for (size_t at = 0; at < text.size(); ++at) {
        if (text[at] != '%' || at + 2 >= text.size()) {
            if (text[at] == '+') {
                out.push_back(' ');
                continue;
            }
            out.push_back(text[at]);
            continue;
        }

        const auto digit = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };

        const int high = digit(text[at + 1]);
        const int low = digit(text[at + 2]);
        if (high < 0 || low < 0) {
            out.push_back(text[at]);
            continue;
        }
        out.push_back(static_cast<char>(high * 16 + low));
        at += 2;
    }
    return out;
}

/// The parameters of a query string.
std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> out;

    for (const auto& field : cc::split(query, '&')) {
        if (field.empty()) continue;
        const auto pair = cc::split_once(field, '=');
        out[url_decode(pair.first)] = url_decode(pair.second);
    }
    return out;
}

/// The parameters of the query of an address.
std::map<std::string, std::string> parse_query_of(const std::string& url) {
    const size_t mark = url.find('?');
    if (mark == std::string::npos) return {};
    return parse_query(url.substr(mark + 1));
}

/// The answer of a request of the flow.
struct HttpAnswer {
    /// The status of the answer.
    int status = 0;

    /// The body of the answer.
    std::string body;

    /// The headers of the answer, with their names in lower case.
    std::map<std::string, std::string> headers;
};

/// Sets the time the next operation of a stream may take.
template <class Stream>
void set_timeout(Stream& stream, std::chrono::seconds timeout) {
    boost::beast::get_lowest_layer(stream).expires_after(timeout);
}

/// Reads the answer of a request that has been sent.
template <class Stream>
bool read_answer(Stream& stream, const std::string& method, const UrlParts& parts,
                  const std::vector<std::pair<std::string, std::string>>& headers,
                  const std::string& body, const std::string& content_type,
                  std::chrono::seconds timeout, HttpAnswer* out, std::string* error) {
    namespace beast = boost::beast;
    namespace http_ns = beast::http;

    http_ns::request<http_ns::string_body> request{http_ns::verb::unknown, parts.path, 11};
    request.method_string(method);
    request.set(http_ns::field::host, host_header(parts));
    request.set(http_ns::field::user_agent, "claude-code");
    request.set(http_ns::field::accept, "application/json");
    for (const auto& [name, value] : headers) request.set(name, value);

    if (!body.empty()) {
        request.set(http_ns::field::content_type, content_type);
        request.body() = body;
        request.prepare_payload();
    }

    beast::flat_buffer buffer;
    http_ns::response<http_ns::string_body> response;

    boost::system::error_code code;

    // The timeout is set again before every operation: it holds for the one
    // that follows it only.
    set_timeout(stream, timeout);
    http_ns::write(stream, request, code);
    if (code) {
        if (error) *error = "the server could not be reached";
        return false;
    }

    set_timeout(stream, timeout);
    http_ns::read(stream, buffer, response, code);
    if (code) {
        if (error) {
            *error = code == boost::asio::error::timed_out ? "the server did not answer in time"
                                                            : "the answer of the server could not be read";
        }
        return false;
    }

    out->status = response.result_int();
    out->body = std::move(response.body());
    for (const auto& field : response) {
        out->headers[cc::to_lower(std::string(field.name_string()))] = std::string(field.value());
    }
    return true;
}

/// Sends a request and reads the answer.
///
/// The call gives up after `timeout`; the failure is a message the user can
/// read, never a crash.
bool http_request(const std::string& method, const std::string& url,
                  const std::vector<std::pair<std::string, std::string>>& headers,
                  const std::string& body, const std::string& content_type,
                  std::chrono::seconds timeout, HttpAnswer* out, std::string* error) {
    namespace beast = boost::beast;
    namespace net = boost::asio;
    namespace ssl = net::ssl;
    using tcp = net::ip::tcp;

    const UrlParts parts = split_url(url);
    if (!parts.valid) {
        if (error) *error = "the address of the server is not a URL";
        return false;
    }

    net::io_context io_context;
    boost::system::error_code code;

    tcp::resolver resolver(io_context);
    const auto endpoints = resolver.resolve(parts.host, parts.port, code);
    if (code) {
        if (error) *error = "the address of the server could not be resolved";
        return false;
    }

    if (parts.scheme == "https") {
        ssl::context context(ssl::context::tls_client);
        context.set_default_verify_paths();
        context.set_verify_mode(ssl::verify_peer);

        ssl::stream<beast::basic_stream<tcp>> stream(io_context, context);
        if (!SSL_set_tlsext_host_name(stream.native_handle(), parts.host.c_str())) {
            if (error) *error = "the address of the server could not be reached";
            return false;
        }
        stream.set_verify_callback(ssl::host_name_verification(parts.host), code);

        beast::get_lowest_layer(stream).expires_after(timeout);
        beast::get_lowest_layer(stream).connect(endpoints, code);
        if (code) {
            if (error) *error = "the address of the server could not be reached";
            return false;
        }

        beast::get_lowest_layer(stream).expires_after(timeout);
        stream.handshake(ssl::stream_base::client, code);
        if (code) {
            if (error) *error = "the address of the server could not be reached";
            return false;
        }

        beast::get_lowest_layer(stream).expires_after(timeout);
        return read_answer(stream, method, parts, headers, body, content_type, timeout, out, error);
    }

    beast::basic_stream<tcp> socket(io_context);
    socket.expires_after(timeout);
    socket.connect(endpoints, code);
    if (code) {
        if (error) *error = "the address of the server could not be reached";
        return false;
    }
    return read_answer(socket, method, parts, headers, body, content_type, timeout, out, error);
}

/// Reads the document of a discovery.
bool http_get_json(const std::string& url, boost::json::value* out) {
    HttpAnswer answer;
    if (!http_request("GET", url, {}, "", "", kRequestTimeout, &answer, nullptr)) return false;
    if (answer.status < 200 || answer.status >= 300) return false;

    const boost::json::value parsed = json_parse(answer.body, boost::json::value(nullptr));
    if (!parsed.is_object()) return false;

    *out = parsed;
    return true;
}

/// The body of a form of a request.
std::string form_body(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out;
    for (const auto& [name, value] : fields) {
        if (!out.empty()) out.push_back('&');
        out += url_encode(name);
        out.push_back('=');
        out += url_encode(value);
    }
    return out;
}

/// The name of the entry of the keychain that holds the credentials.
///
/// The reference keeps the tokens of the servers of MCP in the entry of the
/// credentials of the account, under the key `mcpOAuth`; the port does the
/// same, so the two read each other's tokens.
std::string credentials_service_name() {
    const std::string config_dir = cc::claude_config_dir();
    const bool is_default = std::getenv("CLAUDE_CONFIG_DIR") == nullptr;

    std::string name = "Claude Code";
    if (!is_default) name += "-" + cc::sha256_hex(config_dir).substr(0, 8);
    name += "-credentials";
    return name;
}

/// The name of the user the credentials belong to.
std::string credentials_user_name() {
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

/// Reads the text of the entry of the keychain, when there is one.
std::optional<std::string> read_keychain_entry() {
    const std::string command = "security find-generic-password -a \"" + credentials_user_name() +
                                "\" -w -s \"" + credentials_service_name() + "\"";
    const cc::ProcessOutcome outcome = cc::run_process(command, cc::cwd(), "", 10);
    if (outcome.exit_code != 0) return std::nullopt;

    const std::string text = cc::trim(outcome.out);
    if (text.empty()) return std::nullopt;
    return text;
}

/// Writes the text of the entry of the keychain.
bool write_keychain_entry(const std::string& text, std::string* error) {
    const std::string command = "add-generic-password -U -a \"" + credentials_user_name() +
                                "\" -s \"" + credentials_service_name() + "\" -X \"" + hex_of(text) +
                                "\"\n";

    // The command is given on the standard input, so that the credentials do
    // not show up in the arguments of the process.
    const cc::ProcessOutcome outcome = cc::run_process("security -i", cc::cwd(), command, 10);
    if (outcome.exit_code != 0) {
        if (error) *error = "the keychain refused the tokens";
        return false;
    }
    return true;
}

/// The document of the credentials of the account, as it was read the last
/// time.
///
/// The keychain of the machine is slow to read, so the document is kept for
/// a while, the way the reference keeps it.
struct CredentialsCache {
    std::mutex mutex;
    /// The place the document was read from.
    std::string path;
    /// The document itself.
    boost::json::object document;
    /// When the document was read, in milliseconds since the epoch.
    int64_t read_at = 0;
    /// True once the document has been read.
    bool known = false;
};

/// How long the document of the credentials is kept.
constexpr int64_t kCredentialsCacheMs = 30000;

/// The document of the credentials of the account, as it was read.
CredentialsCache& credentials_cache() {
    static CredentialsCache cache;
    return cache;
}

/// Reads the document of the credentials of the account.
///
/// The document holds the credentials of the login and the tokens of the
/// servers of MCP; the two are kept together, the way the reference keeps
/// them.
///
/// An empty path reads the entry of the keychain of the machine; any other
/// path reads the file of that path.
///
/// The document is kept for a while, so that the keychain is not read for
/// every server of the session. A read that is asked to be fresh goes to the
/// keychain anyway: the tokens are written into the document of the
/// credentials, and the credentials of the login must not be lost.
boost::json::object read_credentials_document(const std::string& path, bool fresh = false) {
    CredentialsCache& cache = credentials_cache();
    {
        std::lock_guard lock(cache.mutex);
        if (!fresh && cache.known && cache.path == path &&
            cc::now_ms() - cache.read_at < kCredentialsCacheMs) {
            return cache.document;
        }
    }

    std::optional<std::string> text;
    if (path.empty()) {
        if (!has_keychain()) text = cc::read_file(credentials_path());
        if (!text) text = read_keychain_entry();
    } else {
        text = cc::read_file(path);
    }

    boost::json::object document;
    if (text) {
        const boost::json::value parsed = json_parse(*text, boost::json::value(nullptr));
        if (parsed.is_object()) document = parsed.as_object();
    }

    {
        std::lock_guard lock(cache.mutex);
        cache.path = path;
        cache.document = document;
        cache.read_at = cc::now_ms();
        cache.known = true;
    }
    return document;
}

/// Writes the document of the credentials of the account.
///
/// An empty path writes the entry of the keychain of the machine; any other
/// path writes the file of that path.
bool write_credentials_document(const std::string& path, const boost::json::object& document,
                                std::string* error) {
    const std::string text = json_dump(document);

    bool written = false;
    if (path.empty()) {
        if (!has_keychain()) {
            written = cc::write_file(credentials_path(), text + "\n", error);

            // The file holds the credentials, so only the user may read it.
            std::error_code code;
            std::filesystem::permissions(credentials_path(),
                                         std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_write,
                                         std::filesystem::perm_options::replace, code);
        } else {
            written = write_keychain_entry(text, error);
        }
    } else {
        written = cc::write_file(path, text + "\n", error);

        // The file holds the credentials, so only the user may read it.
        std::error_code code;
        std::filesystem::permissions(path,
                                     std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, code);
    }

    if (!written) return false;

    CredentialsCache& cache = credentials_cache();
    {
        std::lock_guard lock(cache.mutex);
        cache.path = path;
        cache.document = document;
        cache.read_at = cc::now_ms();
        cache.known = true;
    }
    return true;
}

/// The place the tokens of the servers of MCP are kept.
///
/// An empty path means the keychain of the machine; the systems that have no
/// keychain keep them in the file of the credentials of the configuration
/// folder.
std::string auth_storage_path(const std::string& path) {
    if (!path.empty()) return path;
    if (has_keychain()) return {};
    return credentials_path();
}

/// The addresses the discovery of the authorization server looks at.
///
/// The reference looks at the discovery of the protected resource (RFC 9728)
/// first and at the discovery of the authorization server (RFC 8414) after
/// it; the path of the server is folded into the address of the document,
/// the way the standards say.
std::vector<std::string> discovery_addresses(const std::string& server_url) {
    const std::string origin = origin_of(server_url);
    if (origin.empty()) return {};

    std::string path = path_of(server_url);
    if (path == "/") path.clear();

    std::vector<std::string> addresses;
    addresses.push_back(origin + "/.well-known/oauth-protected-resource" + path);
    addresses.push_back(origin + "/.well-known/oauth-authorization-server" + path);
    addresses.push_back(origin + "/.well-known/openid-configuration" + path);
    return addresses;
}

/// The addresses the metadata of an authorization server may be found at.
std::vector<std::string> authorization_server_addresses(const std::string& authorization_server_url) {
    const std::string origin = origin_of(authorization_server_url);
    if (origin.empty()) return {};

    std::string path = path_of(authorization_server_url);
    if (path == "/") path.clear();

    std::vector<std::string> addresses;
    addresses.push_back(origin + "/.well-known/oauth-authorization-server" + path);
    addresses.push_back(origin + "/.well-known/openid-configuration" + path);
    return addresses;
}

/// Reads the metadata of an authorization server out of an address.
bool read_authorization_server(const std::string& url, AuthorizationServer* out) {
    boost::json::value document;
    if (!http_get_json(url, &document)) return false;

    *out = parse_authorization_server(document.as_object());
    return out->found;
}

/// Looks for the authorization server of a server of MCP.
AuthorizationServer discover_authorization_server(const ServerConfig& config, std::string* error) {
    // The configured address of the metadata is used as it is; the reference
    // asks for it over https, the way the standards say.
    if (!config.oauth_metadata_url.empty()) {
        AuthorizationServer server;
        if (!read_authorization_server(config.oauth_metadata_url, &server)) {
            if (error) *error = "the metadata of the authorization server could not be read";
            return {};
        }
        return server;
    }

    for (const std::string& address : discovery_addresses(config.url)) {
        boost::json::value document;
        if (!http_get_json(address, &document)) continue;

        // The answer of the discovery of the protected resource names the
        // authorization servers of the resource.
        for (const std::string& candidate : parse_protected_resource(document.as_object())) {
            for (const std::string& as_address : authorization_server_addresses(candidate)) {
                AuthorizationServer server;
                if (read_authorization_server(as_address, &server)) return server;
            }
        }

        // The answer of the discovery of the authorization server holds the
        // endpoints themselves.
        AuthorizationServer server = parse_authorization_server(document.as_object());
        if (server.found) return server;
    }

    if (error) *error = "the server does not offer an authentication";
    return {};
}

/// Registers a client of a server of MCP.
///
/// The reference registers itself as `Claude Code (<server>)` and asks for
/// the codes of the flow of the browser.
bool register_client(const ServerConfig& config, const AuthorizationServer& server,
                     const std::string& redirect_uri, const std::string& scope, std::string* client_id,
                     std::string* client_secret, std::string* error) {
    boost::json::array redirect_uris;
    redirect_uris.emplace_back(redirect_uri);

    boost::json::array grant_types;
    grant_types.emplace_back(std::string("authorization_code"));
    grant_types.emplace_back(std::string("refresh_token"));

    boost::json::array response_types;
    response_types.emplace_back(std::string("code"));

    boost::json::object body;
    body["client_name"] = "Claude Code (" + config.name + ")";
    body["redirect_uris"] = std::move(redirect_uris);
    body["grant_types"] = std::move(grant_types);
    body["response_types"] = std::move(response_types);
    body["token_endpoint_auth_method"] = std::string("none");
    if (!scope.empty()) body["scope"] = scope;

    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"},
    };

    HttpAnswer answer;
    if (!http_request("POST", server.registration_endpoint, headers, json_dump(body),
                      "application/json", kRequestTimeout, &answer, error)) {
        return false;
    }

    if (answer.status < 200 || answer.status >= 300) {
        if (error) *error = "the server refused the registration of the client";
        return false;
    }

    const boost::json::value parsed = json_parse(answer.body, boost::json::value(nullptr));
    if (!parsed.is_object()) {
        if (error) *error = "the server sent an answer the port cannot read";
        return false;
    }

    const boost::json::object& object = parsed.as_object();
    *client_id = jget_string(object, "client_id");
    *client_secret = jget_string(object, "client_secret");
    if (client_id->empty()) {
        if (error) *error = "the server registered no client";
        return false;
    }
    return true;
}

/// Asks the token endpoint for tokens.
bool token_request(const std::string& token_endpoint,
                   const std::vector<std::pair<std::string, std::string>>& fields, OAuthTokens* out,
                   std::string* error) {
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/x-www-form-urlencoded"},
        {"Accept", "application/json"},
    };

    HttpAnswer answer;
    if (!http_request("POST", token_endpoint, headers, form_body(fields),
                      "application/x-www-form-urlencoded", kRequestTimeout, &answer, error)) {
        return false;
    }

    const boost::json::value parsed = json_parse(answer.body, boost::json::value(nullptr));
    const boost::json::object object = parsed.is_object() ? parsed.as_object() : boost::json::object{};

    if (answer.status < 200 || answer.status >= 300) {
        // The answer of a server that refuses the code names the reason.
        const std::string reason = jget_string(object, "error");
        const std::string description = jget_string(object, "error_description");
        if (error) {
            if (reason == "invalid_grant") {
                *error = "the server refused the code";
            } else if (!reason.empty()) {
                *error = "the server refused the code: " + reason;
            } else {
                *error = "the server refused the code (" + std::to_string(answer.status) + ")";
            }
            if (!description.empty()) *error += " — " + description;
        }
        return false;
    }

    out->access_token = jget_string(object, "access_token");
    out->refresh_token = jget_string(object, "refresh_token");
    out->scope = jget_string(object, "scope");

    const int64_t expires_in = jget_int(object, "expires_in", 0);
    out->expires_at = expires_in > 0 ? cc::now_ms() + expires_in * 1000 : 0;

    if (out->access_token.empty()) {
        if (error) *error = "the server sent no token";
        return false;
    }
    return true;
}

/// Exchanges the code of the browser for the tokens.
bool exchange_code(const std::string& token_endpoint, const std::string& client_id,
                   const std::string& code, const std::string& redirect_uri,
                   const std::string& verifier, const std::string& resource, OAuthTokens* out,
                   std::string* error) {
    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back("grant_type", "authorization_code");
    fields.emplace_back("code", code);
    fields.emplace_back("redirect_uri", redirect_uri);
    fields.emplace_back("client_id", client_id);
    fields.emplace_back("code_verifier", verifier);
    if (!resource.empty()) fields.emplace_back("resource", resource);

    return token_request(token_endpoint, fields, out, error);
}

/// Opens a page of the web browser.
///
/// Returns false when the browser could not be opened, so that the caller
/// can tell the user where to go instead.
bool open_browser(const std::string& url) {
#if defined(__APPLE__)
    const cc::ProcessOutcome outcome = cc::run_process("open \"" + url + "\"", cc::cwd(), "", 10);
    return !outcome.failed && outcome.exit_code == 0;
#elif defined(__linux__)
    const cc::ProcessOutcome outcome = cc::run_process("xdg-open \"" + url + "\"", cc::cwd(), "", 10);
    return !outcome.failed && outcome.exit_code == 0;
#else
    (void)url;
    return false;
#endif
}

} // namespace

std::string server_key(const std::string& name, const ServerConfig& config) {
    // The reference names the entry after the name of the server and the
    // hash of its configuration, so that two servers of the same name do
    // not share their tokens.
    boost::json::object hash_of;
    hash_of["type"] = config.transport;
    hash_of["url"] = config.url;
    hash_of["headers"] = boost::json::object{};

    const std::string hash = cc::sha256_hex(json_dump(hash_of)).substr(0, 16);
    return name + "|" + hash;
}

std::vector<std::string> parse_protected_resource(const boost::json::object& metadata) {
    std::vector<std::string> out;
    for (const auto& value : jget_array(metadata, "authorization_servers")) {
        if (value.is_string()) out.emplace_back(value.as_string().c_str());
    }
    return out;
}

AuthorizationServer parse_authorization_server(const boost::json::object& metadata) {
    AuthorizationServer server;
    server.issuer = jget_string(metadata, "issuer");
    server.authorization_endpoint = jget_string(metadata, "authorization_endpoint");
    server.token_endpoint = jget_string(metadata, "token_endpoint");
    server.registration_endpoint = jget_string(metadata, "registration_endpoint");

    for (const auto& value : jget_array(metadata, "scopes_supported")) {
        if (value.is_string()) server.scopes_supported.emplace_back(value.as_string().c_str());
    }

    // The reference reads the scope of the answer the way the servers write
    // it: the key `scope` first, then `default_scope`.
    server.scope = jget_string(metadata, "scope");
    if (server.scope.empty()) server.scope = jget_string(metadata, "default_scope");

    server.found = !server.authorization_endpoint.empty();
    return server;
}

std::string build_authorization_url(const AuthorizationServer& server, const std::string& client_id,
                                    const std::string& redirect_uri, const std::string& challenge,
                                    const std::string& state, const std::string& scope,
                                    const std::string& resource) {
    std::string url = server.authorization_endpoint;
    url += "?response_type=code";
    url += "&client_id=" + url_encode(client_id);
    url += "&redirect_uri=" + url_encode(redirect_uri);
    url += "&state=" + url_encode(state);
    url += "&code_challenge=" + url_encode(challenge);
    url += "&code_challenge_method=S256";
    if (!scope.empty()) url += "&scope=" + url_encode(scope);
    if (!resource.empty()) url += "&resource=" + url_encode(resource);
    return url;
}

bool save_server_auth(const std::string& key, const StoredServerAuth& auth, std::string* error,
                      const std::string& path) {
    const std::string where = auth_storage_path(path);

    // The document is read afresh: the credentials of the login are in it as
    // well, and they must not be lost by the tokens of a server.
    boost::json::object document = read_credentials_document(where, /*fresh=*/true);
    boost::json::object servers = jget_object(document, "mcpOAuth");

    boost::json::object entry;
    entry["serverName"] = auth.server_name;
    entry["serverUrl"] = auth.server_url;
    entry["clientId"] = auth.client_id;
    entry["clientSecret"] = auth.client_secret;
    entry["authorizationServerUrl"] = auth.authorization_server_url;
    entry["accessToken"] = auth.tokens.access_token;
    entry["refreshToken"] = auth.tokens.refresh_token;
    entry["scope"] = auth.tokens.scope;
    entry["expiresAt"] = auth.tokens.expires_at;

    servers[key] = std::move(entry);
    document["mcpOAuth"] = std::move(servers);
    return write_credentials_document(where, document, error);
}

std::optional<StoredServerAuth> load_server_auth(const std::string& key, const std::string& path) {
    const boost::json::object document = read_credentials_document(auth_storage_path(path));
    const boost::json::object servers = jget_object(document, "mcpOAuth");

    const auto entry = servers.find(key);
    if (entry == servers.end() || !entry->value().is_object()) return std::nullopt;

    const boost::json::object& object = entry->value().as_object();
    StoredServerAuth auth;
    auth.server_name = jget_string(object, "serverName");
    auth.server_url = jget_string(object, "serverUrl");
    auth.client_id = jget_string(object, "clientId");
    auth.client_secret = jget_string(object, "clientSecret");
    auth.authorization_server_url = jget_string(object, "authorizationServerUrl");
    auth.tokens.access_token = jget_string(object, "accessToken");
    auth.tokens.refresh_token = jget_string(object, "refreshToken");
    auth.tokens.scope = jget_string(object, "scope");
    auth.tokens.expires_at = jget_int(object, "expiresAt", 0);
    return auth;
}

bool clear_server_auth(const std::string& key, const std::string& path) {
    const std::string where = auth_storage_path(path);

    boost::json::object document = read_credentials_document(where, /*fresh=*/true);
    boost::json::object servers = jget_object(document, "mcpOAuth");

    const auto entry = servers.find(key);
    if (entry == servers.end()) return true;

    servers.erase(key);
    document["mcpOAuth"] = std::move(servers);
    return write_credentials_document(where, document, nullptr);
}

bool tokens_usable(const OAuthTokens& tokens) {
    if (tokens.access_token.empty()) return false;
    if (tokens.expires_at == 0) return true;
    return tokens.expires_at > cc::now_ms() + 60000;
}

bool refresh_server_auth(const ServerConfig& config, std::string* error, const std::string& path) {
    if (error != nullptr) error->clear();

    const std::string key = server_key(config.name, config);
    const auto stored = load_server_auth(key, path);
    if (!stored) {
        if (error != nullptr) *error = "the server has no tokens";
        return false;
    }
    if (stored->tokens.refresh_token.empty()) {
        if (error != nullptr) *error = "the tokens of the server cannot be renewed";
        return false;
    }

    // The endpoint of the tokens is found again, the way the reference finds
    // it when it renews the tokens of a server.
    std::string discovery_error;
    const AuthorizationServer server = discover_authorization_server(config, &discovery_error);
    if (!server.found || server.token_endpoint.empty()) {
        if (error != nullptr) {
            *error = discovery_error.empty() ? "the authorization server could not be found"
                                             : discovery_error;
        }
        return false;
    }

    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back("grant_type", "refresh_token");
    fields.emplace_back("refresh_token", stored->tokens.refresh_token);
    if (!stored->client_id.empty()) fields.emplace_back("client_id", stored->client_id);
    if (!config.url.empty()) fields.emplace_back("resource", config.url);

    OAuthTokens tokens;
    if (!token_request(server.token_endpoint, fields, &tokens, error)) return false;

    // The server may not give a new refresh token; the old one is kept.
    if (tokens.refresh_token.empty()) tokens.refresh_token = stored->tokens.refresh_token;
    if (tokens.scope.empty()) tokens.scope = stored->tokens.scope;

    StoredServerAuth auth = *stored;
    auth.tokens = tokens;
    return save_server_auth(key, auth, error, path);
}

// ---------------------------------------------------------------------------
// The flow of the authentication
// ---------------------------------------------------------------------------

struct AuthFlow::Impl {
    /// The configuration of the server.
    ServerConfig config;

    /// The key the tokens of the server are kept under.
    std::string key;

    /// The event loop of the server that waits for the code of the browser.
    boost::asio::io_context io_context;

    /// The lock of the state of the flow.
    mutable std::mutex mutex;

    /// Woken when the state of the flow changes.
    std::condition_variable cv;

    /// The URL the browser is sent to, once the flow has found it.
    std::optional<std::string> authorization_url;

    /// The state the flow is in.
    Stage stage = Stage::Working;

    /// The result of the flow, once it has ended.
    std::optional<AuthResult> result;

    /// True when the user gave the flow up.
    bool cancelled = false;

    /// True when the browser must not be opened.
    bool skip_browser = false;

    /// The place the tokens of the server are kept.
    std::string storage_path;

    /// The address the browser showed, when the user pasted it.
    std::optional<std::string> manual_url;

    /// The thread the flow runs on.
    std::thread worker;

    /// Records the URL the browser is sent to.
    void set_authorization_url(std::string url) {
        {
            std::lock_guard lock(mutex);
            authorization_url = std::move(url);
        }
        cv.notify_all();
    }

    /// Records the state of the flow.
    void set_stage(Stage value) {
        {
            std::lock_guard lock(mutex);
            stage = value;
        }
        cv.notify_all();
    }

    /// Ends the flow with its result.
    void finish(AuthResult outcome) {
        {
            std::lock_guard lock(mutex);
            result = std::move(outcome);
            stage = Stage::Finished;
        }
        cv.notify_all();
    }

    /// Ends the flow with the reason it failed.
    void fail(std::string reason) {
        AuthResult outcome;
        outcome.ok = false;
        outcome.error = std::move(reason);
        finish(std::move(outcome));
    }

    /// True when the user gave the flow up.
    bool was_cancelled() {
        std::lock_guard lock(mutex);
        return cancelled;
    }

    /// True when the browser must not be opened.
    bool browser_skipped() {
        std::lock_guard lock(mutex);
        return skip_browser;
    }

    /// The place the tokens of the server are kept.
    std::string tokens_path() {
        std::lock_guard lock(mutex);
        return storage_path;
    }

    /// The address the browser showed, when the user pasted one.
    std::optional<std::string> take_manual_url() {
        std::lock_guard lock(mutex);
        auto url = manual_url;
        manual_url.reset();
        return url;
    }

    /// The scope the authorization asks for.
    std::string wanted_scope(const AuthorizationServer& server) const {
        if (!config.oauth_scopes.empty()) return cc::join(config.oauth_scopes, " ");
        return server.scope;
    }

    /// The flow of the authentication.
    void run();
};

void AuthFlow::Impl::run() {
    // Only a server that is reached over the network may be authenticated;
    // the reference refuses the others as well.
    if (config.transport != "http") {
        fail("the server is not reached over the network, so it cannot be authenticated");
        return;
    }

    // The authorization server of the server.
    std::string error;
    const AuthorizationServer server = discover_authorization_server(config, &error);
    if (!server.found) {
        fail(error.empty() ? "the server does not offer an authentication" : error);
        return;
    }
    if (server.token_endpoint.empty()) {
        fail("the authorization server has no token endpoint");
        return;
    }
    if (was_cancelled()) {
        fail("the authentication was cancelled");
        return;
    }

    // The client the flow signs in with.
    std::string client_id = config.oauth_client_id;
    std::string client_secret;
    if (const auto stored = load_server_auth(key, tokens_path())) {
        if (!stored->client_id.empty()) {
            client_id = stored->client_id;
            client_secret = stored->client_secret;
        }
    }

    // The local server the browser comes back to.
    auth::AuthCodeListener listener(io_context);
    const int port = listener.start();
    if (port <= 0) {
        fail("the local server of the callback could not be started");
        return;
    }
    const std::string redirect_uri = "http://localhost:" + std::to_string(port) + "/callback";

    const std::string scope = wanted_scope(server);

    if (client_id.empty()) {
        if (server.registration_endpoint.empty()) {
            fail("the server does not offer a registration of clients");
            return;
        }
        if (!register_client(config, server, redirect_uri, scope, &client_id, &client_secret, &error)) {
            fail(error.empty() ? "the client could not be registered" : error);
            return;
        }
    }

    if (was_cancelled()) {
        fail("the authentication was cancelled");
        return;
    }

    // The challenge of the PKCE and the state of the flow.
    const std::string verifier = auth::random_token();
    const std::string challenge = auth::code_challenge(verifier);
    const std::string state = auth::random_token();

    const std::string url =
        build_authorization_url(server, client_id, redirect_uri, challenge, state, scope, config.url);
    set_authorization_url(url);
    set_stage(Stage::Waiting);

    // The browser is opened in the background: the flow does not wait for it.
    if (!browser_skipped()) {
        std::thread browser([url]() { open_browser(url); });
        browser.detach();
    }

    // The code of the browser.
    std::string code;
    std::string returned_state;
    const auto deadline = std::chrono::steady_clock::now() + kFlowTimeout;

    while (true) {
        if (was_cancelled()) {
            fail("the authentication was cancelled");
            return;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            fail("the authentication timed out");
            return;
        }

        // The local server may have got the code.
        if (listener.code()) {
            code = *listener.code();
            returned_state = listener.state().value_or("");
            break;
        }

        // The user may have pasted the address of the callback.
        if (const auto manual = take_manual_url()) {
            const auto parameters = parse_query_of(*manual);
            const auto refusal = parameters.find("error");
            if (refusal != parameters.end() && !refusal->second.empty()) {
                const auto description = parameters.find("error_description");
                fail("the server refused the authentication: " + refusal->second +
                     (description != parameters.end() && !description->second.empty()
                          ? " — " + description->second
                          : ""));
                return;
            }

            const auto pasted = parameters.find("code");
            if (pasted == parameters.end() || pasted->second.empty()) {
                // The address is not one of the callback: the user may try
                // again.
                continue;
            }

            code = pasted->second;
            const auto returned = parameters.find("state");
            returned_state = returned == parameters.end() ? "" : returned->second;
            break;
        }

        // The local server waits for the browser.
        io_context.run_one_for(std::chrono::milliseconds(50));
    }

    if (returned_state != state) {
        fail("the state of the authentication did not match");
        return;
    }

    // The code is exchanged for the tokens.
    OAuthTokens tokens;
    if (!exchange_code(server.token_endpoint, client_id, code, redirect_uri, verifier, config.url,
                       &tokens, &error)) {
        fail(error.empty() ? "the code of the browser was refused" : error);
        return;
    }

    // The tokens are kept.
    StoredServerAuth auth;
    auth.server_name = config.name;
    auth.server_url = config.url;
    auth.client_id = client_id;
    auth.client_secret = client_secret;
    auth.authorization_server_url = server.issuer;
    auth.tokens = tokens;

    std::string save_error;
    save_server_auth(key, auth, &save_error, tokens_path());

    AuthResult outcome;
    outcome.ok = true;
    outcome.tokens = tokens;
    outcome.authorization_url = url;
    finish(std::move(outcome));
}

AuthFlow::AuthFlow(ServerConfig config) : impl_(std::make_shared<Impl>()) {
    impl_->config = std::move(config);
    impl_->key = server_key(impl_->config.name, impl_->config);
}

AuthFlow::~AuthFlow() {
    cancel();
    if (!impl_->worker.joinable()) return;

    // The flow may be waiting for the network, which may take a while. The
    // thread is left to finish on its own when it takes too long, so that
    // the interface is not frozen; it keeps the state of the flow alive.
    {
        std::unique_lock lock(impl_->mutex);
        impl_->cv.wait_for(lock, std::chrono::seconds(2),
                           [this]() { return impl_->stage == Stage::Finished; });
    }

    if (impl_->stage == Stage::Finished) {
        impl_->worker.join();
    } else {
        impl_->worker.detach();
    }
}

void AuthFlow::start() {
    if (impl_->worker.joinable()) return;
    impl_->worker = std::thread([impl = impl_]() { impl->run(); });
}

void AuthFlow::set_skip_browser(bool skip) {
    std::lock_guard lock(impl_->mutex);
    impl_->skip_browser = skip;
}

void AuthFlow::set_storage_path(std::string path) {
    std::lock_guard lock(impl_->mutex);
    impl_->storage_path = std::move(path);
}

std::optional<std::string> AuthFlow::authorization_url() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->authorization_url;
}

AuthFlow::Stage AuthFlow::stage() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->stage;
}

void AuthFlow::submit_callback_url(std::string url) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->manual_url = std::move(url);
    }
    impl_->cv.notify_all();
}

void AuthFlow::cancel() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->cancelled = true;
    }
    impl_->cv.notify_all();
}

bool AuthFlow::finished() const { return stage() == Stage::Finished; }

std::optional<AuthResult> AuthFlow::result() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->result;
}

void AuthFlow::wait() {
    std::unique_lock lock(impl_->mutex);
    impl_->cv.wait(lock, [this]() { return impl_->stage == Stage::Finished; });
}

AuthResult authenticate_server(ServerConfig config) {
    AuthFlow flow(std::move(config));
    flow.start();
    flow.wait();

    auto result = flow.result();
    if (!result) {
        AuthResult outcome;
        outcome.ok = false;
        outcome.error = "the authentication did not finish";
        return outcome;
    }
    return *result;
}

} // namespace cc::mcp
