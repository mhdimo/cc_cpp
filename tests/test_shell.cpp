#include <catch2/catch_test_macros.hpp>

#include "tools/impl/bash_tool.hpp"
#include "tools/tool_use_context.hpp"

#include <chrono>

using namespace cc;

TEST_CASE("The shell runner reports the output as it arrives") {
    std::string streamed;

    const auto result = tools::run_shell_command_streaming(
        "printf 'first\\n'; sleep 0.2; printf 'second\\n'", "/tmp", 5000,
        [&streamed](const std::string& chunk) { streamed += chunk; });

    REQUIRE(result.exit_code == 0);
    REQUIRE(result.output.find("first") != std::string::npos);
    REQUIRE(result.output.find("second") != std::string::npos);

    // The callback saw the output while the command was still running.
    REQUIRE(streamed.find("first") != std::string::npos);
    REQUIRE(streamed.find("second") != std::string::npos);
}

TEST_CASE("The shell runner keeps stderr apart from stdout") {
    const auto result = tools::run_shell_command("echo out; echo err 1>&2", "/tmp", 5000);

    REQUIRE(result.output.find("out") != std::string::npos);
    REQUIRE(result.output.find("err") == std::string::npos);
    REQUIRE(result.error_output.find("err") != std::string::npos);
}

TEST_CASE("The shell runner stops a command that runs too long") {
    const auto start = std::chrono::steady_clock::now();
    const auto result = tools::run_shell_command("sleep 30", "/tmp", 300);

    REQUIRE(result.timed_out);
    REQUIRE(result.exit_code != 0);

    // The command was killed rather than waited for.
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    REQUIRE(elapsed < 10000);
}

TEST_CASE("The Bash tool forwards the output of the command to the context") {
    tools::BashTool tool;

    ToolUseContext context;
    context.options.cwd = "/tmp";

    std::string progress;
    context.on_progress = [&progress](const std::string& chunk) { progress += chunk; };

    boost::json::object input;
    input["command"] = "echo streamed";

    const auto result = tool.call(input, context);

    REQUIRE(result.content == "streamed");
    REQUIRE(progress.find("streamed") != std::string::npos);
}
