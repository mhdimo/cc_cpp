#include <catch2/catch_test_macros.hpp>

#include "config/settings.hpp"
#include "sandbox/manager.hpp"
#include "sandbox/profile.hpp"
#include "sandbox/settings.hpp"
#include "tools/impl/bash_tool.hpp"
#include "tools/tool_use_context.hpp"

#include <boost/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace cc;

namespace {

/// A temporary directory that cleans itself up.
class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() / ("cc_sandbox_test_" + std::to_string(::getpid()) + "_" +
                                              std::to_string(counter_++));
        path_string_ = path_.string();
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const std::string& path() const { return path_string_; }

    /// Writes a file of the directory, making the folders it needs.
    void write(const std::string& name, const std::string& contents) const {
        const fs::path file = path_ / name;
        fs::create_directories(file.parent_path());
        std::ofstream out(file);
        out << contents;
        out.close();
    }

private:
    fs::path path_;
    std::string path_string_;
    static inline int counter_ = 0;
};

/// Puts the settings of the user in a folder of its own, so that the test
/// does not read the settings of the machine it runs on.
class ScopedConfigDir {
public:
    explicit ScopedConfigDir(const std::string& path) {
        const char* previous = std::getenv("CLAUDE_CONFIG_DIR");
        had_previous_ = previous != nullptr;
        if (had_previous_) previous_ = previous;
        ::setenv("CLAUDE_CONFIG_DIR", path.c_str(), 1);
    }

    ~ScopedConfigDir() {
        if (had_previous_) {
            ::setenv("CLAUDE_CONFIG_DIR", previous_.c_str(), 1);
        } else {
            ::unsetenv("CLAUDE_CONFIG_DIR");
        }
    }

private:
    bool had_previous_ = false;
    std::string previous_;
};

/// True when the Bash tool takes the sandbox as a pointer.
template <typename T>
concept HasSetSandboxPointer = requires(T& tool, sandbox::Manager* manager) {
    tool.set_sandbox(manager);
};

/// True when the Bash tool takes the sandbox as a reference.
template <typename T>
concept HasSetSandboxReference = requires(T& tool, sandbox::Manager& manager) {
    tool.set_sandbox(manager);
};

/// True when the Bash tool can be given the sandbox.
template <typename T>
concept HasSetSandbox = HasSetSandboxPointer<T> || HasSetSandboxReference<T>;

/// Runs a command through the Bash tool with the sandbox turned on.
template <typename Tool>
void run_with_sandbox(Tool& tool, sandbox::Manager& manager, const boost::json::object& input) {
    if constexpr (HasSetSandboxPointer<Tool>) {
        tool.set_sandbox(&manager);
    } else if constexpr (HasSetSandboxReference<Tool>) {
        tool.set_sandbox(manager);
    } else {
        return;
    }

    ToolUseContext context;
    context.options.cwd = "/tmp";
    tool.call(input, context);
}

} // namespace

TEST_CASE("The sandbox settings write their flags whatever they are") {
    sandbox::Settings settings;
    settings.enabled = false;
    settings.fail_if_unavailable = false;
    settings.auto_allow_bash_if_sandboxed = false;
    settings.allow_unsandboxed_commands = true;

    const auto written = sandbox::to_json(settings);
    REQUIRE(written.is_object());
    const auto& object = written.as_object();

    // The reference writes the flags whatever they are, so that a choice of
    // the panel is not inherited from the scope above.
    REQUIRE(object.find("enabled") != object.end());
    REQUIRE(object.find("failIfUnavailable") != object.end());
    REQUIRE(object.find("autoAllowBashIfSandboxed") != object.end());
    REQUIRE(object.find("allowUnsandboxedCommands") != object.end());

    REQUIRE(object.at("enabled").as_bool() == false);
    REQUIRE(object.at("failIfUnavailable").as_bool() == false);
    REQUIRE(object.at("autoAllowBashIfSandboxed").as_bool() == false);
    REQUIRE(object.at("allowUnsandboxedCommands").as_bool() == true);
}

TEST_CASE("The sandbox settings read the auto-allow mode as it is") {
    const auto parsed = sandbox::parse_settings(boost::json::parse(R"({"enabled": true})"));

    REQUIRE(parsed.enabled);
    // The reference reads the key with a fallback of true.
    REQUIRE(parsed.auto_allow_bash_if_sandboxed);
    REQUIRE(parsed.allow_unsandboxed_commands);
}

TEST_CASE("The commands of the excluded list are found behind their wrappers") {
    sandbox::Settings settings;
    settings.enabled = true;
    settings.excluded_commands = {"bazel:*"};

    const sandbox::Manager manager(settings);
    if (!manager.is_sandboxing_enabled()) {
        SKIP("the sandbox does not run on this machine");
    }

    // The command itself.
    REQUIRE_FALSE(manager.should_use_sandbox("bazel build //...", false));
    // Behind an environment variable.
    REQUIRE_FALSE(manager.should_use_sandbox("FOO=bar bazel build //...", false));
    // Behind a wrapper.
    REQUIRE_FALSE(manager.should_use_sandbox("timeout 30 bazel build //...", false));

    // A command that is not on the list still runs in the sandbox.
    REQUIRE(manager.should_use_sandbox("ls -la", false));
}

TEST_CASE("The wildcards of the excluded list are matched the way the reference matches them") {
    sandbox::Settings settings;
    settings.enabled = true;
    // The pattern carries the spaces the reference trims.
    settings.excluded_commands = {" bazel* "};

    const sandbox::Manager manager(settings);
    if (!manager.is_sandboxing_enabled()) {
        SKIP("the sandbox does not run on this machine");
    }

    REQUIRE_FALSE(manager.should_use_sandbox("bazel build //...", false));
}

TEST_CASE("The wildcards of the excluded list match the newlines of a command") {
    sandbox::Settings settings;
    settings.enabled = true;
    settings.excluded_commands = {"bazel*"};

    const sandbox::Manager manager(settings);
    if (!manager.is_sandboxing_enabled()) {
        SKIP("the sandbox does not run on this machine");
    }

    REQUIRE_FALSE(manager.should_use_sandbox("bazel\nbuild //...", false));
}

TEST_CASE("The seatbelt keeps the settings of the session out of the sandbox") {
    const TempDir home;
    const TempDir project;
    const ScopedConfigDir config(home.path());

    sandbox::Settings settings;
    settings.enabled = true;
    sandbox::Manager manager(settings);
    manager.set_working_directory(project.path());

    const std::string profile = sandbox::seatbelt_profile(manager, project.path());

    // The settings of the user and the settings of the project may not be
    // written by a command that runs in the sandbox.
    REQUIRE(profile.find(home.path()) != std::string::npos);
    REQUIRE(profile.find(".claude") != std::string::npos);
}

TEST_CASE("The seatbelt holds the network of the commands") {
    sandbox::Settings settings;
    settings.enabled = true;
    settings.network.allowed_domains = {"example.com"};
    const sandbox::Manager manager(settings);

    const std::string profile = sandbox::seatbelt_profile(manager, "/tmp");

    // The hosts that are allowed are named, and the rest of the world is
    // not reachable.
    REQUIRE(profile.find("example.com") != std::string::npos);
    REQUIRE(profile.find("(remote ip \"*:\"") == std::string::npos);
}

TEST_CASE("The seatbelt keeps the hosts that are denied out of the network") {
    sandbox::Settings settings;
    settings.enabled = true;
    settings.network.denied_domains = {"blocked.example"};
    const sandbox::Manager manager(settings);

    const std::string profile = sandbox::seatbelt_profile(manager, "/tmp");

    REQUIRE(profile.find("blocked.example") != std::string::npos);
}

TEST_CASE("The settings of the sandbox are merged scope by scope") {
    const TempDir home;
    const TempDir project;
    const ScopedConfigDir config(home.path());

    home.write("settings.json",
               R"({"sandbox": {"enabled": true, "network": {"allowedDomains": ["example.com"]}}})");
    project.write(".claude/settings.json", R"({"sandbox": {"enabled": false}})");

    const auto settings = config::load_settings(project.path());
    REQUIRE(settings.sandbox.is_object());
    const auto& sandbox = settings.sandbox.as_object();

    // The scope of the project wins for the key it names.
    const auto* enabled = sandbox.find("enabled");
    REQUIRE(enabled != nullptr);
    REQUIRE(enabled->value().as_bool() == false);

    // The keys the scope of the project does not name are kept.
    const auto* network = sandbox.find("network");
    REQUIRE(network != nullptr);
    REQUIRE(network->value().is_object());
    const auto* domains = network->value().as_object().find("allowedDomains");
    REQUIRE(domains != nullptr);
    REQUIRE(domains->value().is_array());
    REQUIRE(domains->value().as_array().size() == 1);
    REQUIRE(domains->value().as_array()[0].as_string() == "example.com");
}

TEST_CASE("The prompt of the Bash tool describes the sandbox") {
    sandbox::Settings settings;
    settings.enabled = true;

    const sandbox::Manager manager(settings);
    if (!manager.is_sandboxing_enabled()) {
        SKIP("the sandbox does not run on this machine");
    }

    const auto section = sandbox::prompt_section(manager);
    REQUIRE(!section.empty());

    // The wording of the reference.
    std::string text;
    for (const std::string& line : section) {
        text += line;
        text += "\n";
    }

    REQUIRE(text.find("## Command sandbox") != std::string::npos);
    REQUIRE(text.find("By default, your command will be run in a sandbox. This sandbox controls "
                      "which directories and network hosts commands may access or modify without "
                      "an explicit override.") != std::string::npos);
    REQUIRE(text.find("The sandbox has the following restrictions:") != std::string::npos);
    REQUIRE(text.find("For temporary files, always use the `$TMPDIR` environment variable.") !=
            std::string::npos);
}

TEST_CASE("The prompt of the Bash tool leaves the sandbox out when it is off") {
    sandbox::Settings settings;
    settings.enabled = false;

    const sandbox::Manager manager(settings);
    REQUIRE(sandbox::prompt_section(manager).empty());
}

TEST_CASE("The Bash tool runs the command inside the sandbox") {
    if (!fs::exists("/usr/bin/sandbox-exec")) {
        SKIP("the seatbelt of macOS is not on this machine");
    }

    std::string ran;
    tools::BashTool bash([&ran](const std::string& command, const std::string&, int64_t) {
        ran = command;
        return tools::ShellResult{};
    });

    if constexpr (!HasSetSandbox<tools::BashTool>) {
        FAIL("the Bash tool cannot be given the sandbox");
        return;
    }

    sandbox::Settings settings;
    settings.enabled = true;
    sandbox::Manager manager(settings);

    boost::json::object input;
    input["command"] = "echo hello";
    run_with_sandbox(bash, manager, input);

    // The command that ran is the one the seatbelt holds, not the command
    // the model wrote.
    REQUIRE(ran.find("echo hello") != std::string::npos);
    REQUIRE(ran != "echo hello");
}

TEST_CASE("The Bash tool honours the flag that turns the sandbox off") {
    if (!fs::exists("/usr/bin/sandbox-exec")) {
        SKIP("the seatbelt of macOS is not on this machine");
    }

    std::string ran;
    tools::BashTool bash([&ran](const std::string& command, const std::string&, int64_t) {
        ran = command;
        return tools::ShellResult{};
    });

    if constexpr (!HasSetSandbox<tools::BashTool>) {
        FAIL("the Bash tool cannot be given the sandbox");
        return;
    }

    sandbox::Settings settings;
    settings.enabled = true;
    sandbox::Manager manager(settings);

    boost::json::object input;
    input["command"] = "echo hello";
    input["dangerouslyDisableSandbox"] = true;
    run_with_sandbox(bash, manager, input);

    // The user asked for the command to run outside the sandbox, so it runs
    // as it was written.
    REQUIRE(ran == "echo hello");
}
