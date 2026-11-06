#include <catch2/catch_test_macros.hpp>

#include "commands/custom.hpp"
#include "commands/registry.hpp"
#include "ui/suggestions.hpp"
#include "utils/time.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace cc;

namespace {

/// The suggestion whose name is `name`, or nothing.
const ui::Suggestion* find(const std::vector<ui::Suggestion>& items, const std::string& name) {
    for (const auto& item : items) {
        if (item.display == "/" + name) return &item;
    }
    return nullptr;
}

/// True when one of the suggestions is the command named `name`.
bool offered(const std::vector<ui::Suggestion>& items, const std::string& name) {
    return find(items, name) != nullptr;
}

/// The place of the suggestion named `name` in the list, or the size of the
/// list when it is not there.
size_t place(const std::vector<ui::Suggestion>& items, const std::string& name) {
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].display == "/" + name) return i;
    }
    return items.size();
}

/// Writes a global configuration file in a folder of its own.
std::string write_config(const std::string& contents) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "cc_command_list_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    std::ofstream file(directory / ".claude.json");
    file << contents;

    return directory.string();
}

/// A configuration of its own, so that the tests do not depend on the
/// commands the user has used.
///
/// The commands that were used recently stand at the top of the list, and
/// the use is kept in the global configuration file of the user.
class EmptyConfig {
public:
    EmptyConfig() {
        const char* previous = std::getenv("CLAUDE_CONFIG_DIR");
        if (previous != nullptr) previous_ = previous;

        directory_ = std::filesystem::temp_directory_path() / "cc_empty_config_test";
        std::filesystem::create_directories(directory_);
        std::ofstream file(directory_ / ".claude.json");
        file << "{}";

        setenv("CLAUDE_CONFIG_DIR", directory_.c_str(), 1);
    }

    ~EmptyConfig() {
        if (previous_.empty()) {
            unsetenv("CLAUDE_CONFIG_DIR");
        } else {
            setenv("CLAUDE_CONFIG_DIR", previous_.c_str(), 1);
        }
    }

private:
    std::string previous_;
    std::filesystem::path directory_;
};

} // namespace

TEST_CASE("The commands that are prompts stand at the end of the list") {
    // The commands that were used recently stand at the top of the list, so
    // the test asks for a session that has used none of them.
    const EmptyConfig config;

    // The reference puts the commands that are prompts after the commands
    // that run inside the CLI.
    const auto items = ui::suggestions_for("/", "/tmp");
    REQUIRE(items.size() > 1);

    const size_t first_prompt = place(items, "init");
    REQUIRE(first_prompt < items.size());

    for (const auto& command : commands::all()) {
        if (!commands::is_available(command)) continue;
        if (command.type == "prompt") continue;
        const size_t at = place(items, command.name);
        REQUIRE(at < items.size());
        REQUIRE(at < first_prompt);
    }

    for (const char* name : {"init", "insights", "pr-comments", "review", "security-review",
                             "statusline"}) {
        const size_t at = place(items, name);
        REQUIRE(at < items.size());
        REQUIRE(at >= first_prompt);
    }
}

TEST_CASE("The commands of the internal build are kept out of the listing") {
    // The reference offers /version to the users of its own build only, so
    // it is kept out of the list — but it is still there, and it still
    // answers when it is called by its name.
    const auto items = ui::suggestions_for("/", "/tmp");
    REQUIRE_FALSE(offered(items, "version"));
    REQUIRE(commands::find_command("version") != nullptr);

    // The commands of the commit are offered to everybody.
    REQUIRE(offered(items, "commit"));
    REQUIRE(offered(items, "commit-push-pr"));
}

TEST_CASE("The list is empty when the name of a command is followed by a space") {
    // The reference draws the hint of the arguments alone.
    REQUIRE(ui::suggestions_for("/config ", "/tmp").empty());
    REQUIRE(ui::suggestions_for("/config  ", "/tmp").empty());

    // A name that no command answers to is searched as it is.
    REQUIRE_FALSE(ui::suggestions_for("/configx ", "/tmp").empty());
}

TEST_CASE("The command that is kept out of the listing is found by its name") {
    // The reference puts the command at the top of the results when its name
    // is what was typed.
    const auto items = ui::suggestions_for("/heapdump", "/tmp");
    REQUIRE(items.size() >= 1);
    REQUIRE(items.front().display == "/heapdump");
}

TEST_CASE("The name of a command that is typed in the middle is completed") {
    // The reference completes the name of the command that is typed in the
    // middle of the input with the first command whose name begins with it.
    const auto items = ui::suggestions_for("/rev", "/tmp");
    REQUIRE(items.size() >= 1);
    REQUIRE(items.front().display == "/review");
}

TEST_CASE("The commands that were used recently stand at the top of the list") {
    commands::CustomCommand command;
    command.name = "ship-it";
    command.description = "Ships the branch";
    command.source = "userSettings";

    // The use of the command is written in the global configuration file.
    const std::string directory = write_config(
        "{\"skillUsage\": {\"ship-it\": {\"usageCount\": 5, \"lastUsedAt\": " +
        std::to_string(cc::now_ms()) + "}}}");
    const char* previous = std::getenv("CLAUDE_CONFIG_DIR");
    setenv("CLAUDE_CONFIG_DIR", directory.c_str(), 1);

    const auto items = ui::suggestions_for("/", "/tmp", {command});
    REQUIRE(items.size() > 1);
    REQUIRE(items.front().display == "/ship-it");

    if (previous != nullptr) {
        setenv("CLAUDE_CONFIG_DIR", previous, 1);
    } else {
        unsetenv("CLAUDE_CONFIG_DIR");
    }
}

TEST_CASE("The mark of the sandbox is the tick of the reference") {
    commands::SandboxState state;
    state.enabled = true;
    commands::set_sandbox_state(state);

    const commands::Command* command = commands::find_command("sandbox");
    REQUIRE(command != nullptr);
    REQUIRE(command->description.rfind("✔ sandbox enabled", 0) == 0);

    // The state of the sandbox is put back, so that the other tests find it
    // the way they expect it.
    commands::set_sandbox_state(commands::SandboxState{});
}

TEST_CASE("The commands that are prompts tell the model which command was run") {
    commands::CommandContext context;
    std::string asked;
    context.ask_model = [&asked](const std::string& prompt,
                                 const std::vector<std::string>&) {
        asked = prompt;
        return std::string();
    };

    const commands::CommandResult result = commands::run_command("review", "", context);
    REQUIRE(result.handled);
    REQUIRE(asked.rfind("<command-message>review</command-message>", 0) == 0);
    REQUIRE(asked.find("<command-name>/review</command-name>") != std::string::npos);
}
