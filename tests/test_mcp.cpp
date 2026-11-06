#include <catch2/catch_test_macros.hpp>

#include "config/settings.hpp"
#include "mcp/mcp.hpp"
#include "utils/json_util.hpp"
#include "utils/string.hpp"

#include <boost/asio.hpp>
#include <boost/json.hpp>

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace cc;

namespace {

/// True when the machine can run the little MCP server of the tests.
bool has_python() {
    const char* path = std::getenv("PATH");
    if (path == nullptr) return false;

    for (const auto& directory : split(path, ':')) {
        if (directory.empty()) continue;
        if (std::filesystem::exists(std::filesystem::path(directory) / "python3")) return true;
    }
    return false;
}

/// The configuration of the little MCP server of the tests.
boost::json::object server_settings() {
    boost::json::array args;
    args.emplace_back(std::string(CC_TEST_SOURCE_DIR) + "/mcp_server.py");

    boost::json::object server;
    server["command"] = std::string("python3");
    server["args"] = std::move(args);

    boost::json::object servers;
    servers["test-server"] = std::move(server);

    boost::json::object settings;
    settings["mcpServers"] = std::move(servers);
    return settings;
}

} // namespace

TEST_CASE("The servers of the folder are only used when it is trusted") {
    const auto directory = std::filesystem::temp_directory_path() / "cc_mcp_settings_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    {
        std::ofstream out(directory / ".mcp.json");
        out << R"({"mcpServers": {"test-server": {"command": "python3"}}})";
    }

    // A folder that is not trusted does not get to run its own servers.
    const auto untrusted = config::load_settings(directory.string(), false);
    bool found = false;
    for (const auto& [name, server] : untrusted.mcp_servers) {
        if (name == "test-server") found = true;
    }
    REQUIRE(!found);

    // A folder that is trusted does.
    const auto trusted = config::load_settings(directory.string(), true);
    const auto entry = trusted.mcp_servers.find("test-server");
    REQUIRE(entry != trusted.mcp_servers.end());
    REQUIRE(entry->second.scope == "project");
    REQUIRE(jget_string(entry->second.config, "command") == "python3");

    std::filesystem::remove_all(directory);
}

TEST_CASE("The prompts of an MCP server are commands") {
    if (!has_python()) SKIP("python3 is not on the path");

    boost::asio::io_context io_context;
    mcp::Registry registry(io_context);
    registry.configure(server_settings());
    // The servers are connected in the background, so that the session is
    // ready to talk before a server that takes its time is up.
    registry.connect_all();
    registry.wait_for_connections(std::chrono::seconds(30));
    REQUIRE(registry.connections_ready());

    const auto commands = registry.commands();
    REQUIRE(commands.size() == 1);

    // The name of the command is the one the model would see, and the user
    // sees the name of the server and of the prompt. Only the characters
    // outside the set the API allows are replaced.
    REQUIRE(commands[0].name == "mcp__test-server__greet");
    REQUIRE(commands[0].user_facing_name == "test-server:greet (MCP)");
    REQUIRE(commands[0].description == "Greets someone.");
    REQUIRE(commands[0].argument_names == std::vector<std::string>{"who"});

    // The server offers a tool and a resource as well.
    REQUIRE(registry.status().size() == 1);
    REQUIRE(registry.status()[0].connected);
    REQUIRE(registry.status()[0].tool_count == 1);
    REQUIRE(registry.status()[0].resource_count == 1);
    REQUIRE(registry.status()[0].prompt_count == 1);

    // The prompt is fetched from the server, with the arguments by name.
    std::string error;
    const std::string text = registry.run_command(commands[0], {"world"}, &error);
    REQUIRE(error.empty());
    REQUIRE(text.find("Hello, world") != std::string::npos);
}
