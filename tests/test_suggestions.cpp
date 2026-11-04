#include <catch2/catch_test_macros.hpp>

#include "commands/custom.hpp"
#include "commands/registry.hpp"
#include "ui/ansi.hpp"
#include "ui/suggestions.hpp"
#include "ui/theme.hpp"
#include "utils/string.hpp"

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

TEST_CASE("The commands of the user name the place they come from") {
    commands::CustomCommand command;
    command.name = "ship-it";
    command.description = "Ships the branch";
    command.source = "userSettings";

    const auto items = ui::suggestions_for("/ship", "/tmp", {command});
    const auto* item = find(items, "ship-it");
    REQUIRE(item != nullptr);
    REQUIRE(item->description == "Ships the branch (user)");

    // The commands of the project name the project.
    command.source = "projectSettings";
    const auto project = ui::suggestions_for("/ship", "/tmp", {command});
    REQUIRE(find(project, "ship-it")->description == "Ships the branch (project)");

    // The commands of the policy of the machine name the policy.
    command.source = "policySettings";
    const auto managed = ui::suggestions_for("/ship", "/tmp", {command});
    REQUIRE(find(managed, "ship-it")->description == "Ships the branch (managed)");
}

TEST_CASE("The commands of a plugin carry the name of the plugin") {
    commands::CustomCommand command;
    command.name = "greeter:greet";
    command.description = "Greets the user";
    command.plugin_id = "greeter@official";

    const auto items = ui::suggestions_for("/greet", "/tmp", {command});
    const auto* item = find(items, "greeter:greet");
    REQUIRE(item != nullptr);
    REQUIRE(item->description == "(greeter) Greets the user");
}

TEST_CASE("The names of the arguments are written at the end of the description") {
    commands::CustomCommand command;
    command.name = "deploy-app";
    command.description = "Deploys the app";
    command.argument_names = {"environment", "version"};
    command.source = "userSettings";

    const auto items = ui::suggestions_for("/deploy", "/tmp", {command});
    const auto* item = find(items, "deploy-app");
    REQUIRE(item != nullptr);
    REQUIRE(item->description == "Deploys the app (user) (arguments: environment, version)");
}

TEST_CASE("The names of the arguments are read from the frontmatter") {
    const auto directory = std::filesystem::temp_directory_path() / "cc_argument_names_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory / ".claude" / "commands");

    {
        std::ofstream file(directory / ".claude" / "commands" / "list.md");
        file << "---\ndescription: Lists the things\narguments:\n  - one\n  - two\n---\nList them\n";
    }
    {
        std::ofstream file(directory / ".claude" / "commands" / "send.md");
        file << "---\ndescription: Sends the thing\narguments: target message\n---\nSend it\n";
    }
    {
        std::ofstream file(directory / ".claude" / "commands" / "show.md");
        file << "---\ndescription: Shows the thing\narguments: [one, two]\n---\nShow it\n";
    }

    const auto loaded = commands::load_custom_commands(directory.string());
    REQUIRE(loaded.size() == 3);

    // The list of the names is read, whichever way it is written.
    const auto names_of = [&loaded](const std::string& name) {
        for (const auto& command : loaded) {
            if (command.name == name) return command.argument_names;
        }
        return std::vector<std::string>();
    };

    REQUIRE(names_of("list") == std::vector<std::string>{"one", "two"});
    REQUIRE(names_of("send") == std::vector<std::string>{"target", "message"});
    REQUIRE(names_of("show") == std::vector<std::string>{"one", "two"});

    // The commands of the project come from the project.
    for (const auto& command : loaded) {
        REQUIRE(command.source == "projectSettings");
    }

    std::filesystem::remove_all(directory);
}

TEST_CASE("The names of the arguments of a command of a plugin are read from its file") {
    const auto directory = std::filesystem::temp_directory_path() / "cc_plugin_arguments_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    const auto path = directory / "greet.md";
    {
        std::ofstream file(path);
        file << "---\ndescription: Greets the user\narguments:\n  - who\n---\nGreet them\n";
    }

    // The commands of the plugins are read by the loader of the plugins,
    // which does not carry the names of the arguments over.
    commands::CustomCommand command;
    command.name = "greeter:greet";
    command.description = "Greets the user";
    command.plugin_id = "greeter@official";
    command.path = path.string();

    const auto items = ui::suggestions_for("/greet", "/tmp", {command});
    const auto* item = find(items, "greeter:greet");
    REQUIRE(item != nullptr);
    REQUIRE(item->description == "(greeter) Greets the user (arguments: who)");

    std::filesystem::remove_all(directory);
}

TEST_CASE("The hint of the arguments is not written in the description") {
    // The reference shows the names of the arguments, not the hint of them.
    commands::CustomCommand command;
    command.name = "deploy-app";
    command.description = "Deploys the app";
    command.argument_hint = "<environment>";
    command.source = "userSettings";

    const auto items = ui::suggestions_for("/deploy", "/tmp", {command});
    const auto* item = find(items, "deploy-app");
    REQUIRE(item != nullptr);
    REQUIRE(item->description == "Deploys the app (user)");
}

TEST_CASE("The search looks at the parts of the name of the command") {
    // `/add-dir` is offered for "dir", the way the reference offers it.
    const auto items = ui::suggestions_for("/dir", "/tmp");
    REQUIRE(offered(items, "add-dir"));
}

TEST_CASE("The search looks at the description of the command") {
    // The commands whose description holds the word are offered as well.
    const auto items = ui::suggestions_for("/conversation", "/tmp");
    REQUIRE(offered(items, "branch"));
    REQUIRE(offered(items, "btw"));
}

TEST_CASE("The exact name comes before the commands that only match the description") {
    const auto items = ui::suggestions_for("/config", "/tmp");
    REQUIRE(!items.empty());
    REQUIRE(items.front().display == "/config");
}

TEST_CASE("The commands are offered in the order the reference ranks them") {
    // The commands that were used recently stand at the top of the list, so
    // the test asks for a session that has used none of them.
    const EmptyConfig config;

    // With nothing typed the built-in commands are sorted by name.
    const auto all = ui::suggestions_for("/", "/tmp");
    REQUIRE(all.size() > 1);
    REQUIRE(all[0].display == "/add-dir");

    // The exact name comes first, then the names that start with it, the
    // shorter ones before the longer ones. The alias of `/resume` matches
    // as well, but its name does not start with what was typed, so it comes
    // after the names that do.
    const auto con = ui::suggestions_for("/con", "/tmp");
    REQUIRE(con.size() > 3);
    REQUIRE(con[0].display == "/config");
    REQUIRE(con[1].display == "/context");
    REQUIRE(con[2].display == "/resume (continue)");

    // The commands whose description holds the word come after the ones
    // whose name starts with it.
    bool seen_a_description = false;
    for (const auto& item : con) {
        if (item.display == "/branch") seen_a_description = true;
        if (item.display == "/config") REQUIRE_FALSE(seen_a_description);
    }
    REQUIRE(seen_a_description);

    // An alias matches as well, and the alias that was typed is shown in
    // brackets, the way the reference shows it. The commands whose
    // description holds the word are offered as well.
    const auto settings = ui::suggestions_for("/settings", "/tmp");
    REQUIRE(settings.size() > 1);
    REQUIRE(settings[0].display == "/config (settings)");
}

TEST_CASE("The commands that have arguments are not offered") {
    // The reference shows no suggestions when the command has arguments.
    REQUIRE(ui::suggestions_for("/config now", "/tmp").empty());
}

TEST_CASE("The entries of the overlay that are not chosen are dimmed") {
    const auto items = ui::suggestions_for("/cl", "/tmp");
    REQUIRE(items.size() > 1);
    REQUIRE(items[0].display == "/clear");

    // The second entry is the chosen one, so the first one is dimmed.
    const auto lines = ui::render_suggestions(items, 1, ui::Theme::dark(), 80);
    REQUIRE(lines.size() > 1);

    // The entry that is not chosen is dimmed with the attribute of the
    // terminal, and not painted with a colour of its own.
    REQUIRE(lines[0].find("\x1b[2m") != std::string::npos);
    REQUIRE(lines[0].find("153;153;153") == std::string::npos);
}

TEST_CASE("The window of the list keeps every row when the list fits") {
    std::vector<ui::Suggestion> items;
    for (int i = 0; i < 5; ++i) {
        ui::Suggestion item;
        item.kind = ui::Suggestion::Kind::File;
        item.display = "file" + std::to_string(i);
        items.push_back(std::move(item));
    }

    // The list is no longer than the window, so every row stays on the
    // screen and only the pointer moves.
    const auto lines = ui::render_suggestions(items, 4, ui::Theme::dark(), 80, 5);
    REQUIRE(lines.size() == 5);

    // The window of a list that is longer than the window still follows
    // the pointer.
    const auto scrolled = ui::render_suggestions(items, 4, ui::Theme::dark(), 80, 3);
    REQUIRE(scrolled.size() == 3);
}

TEST_CASE("The list of the `@` holds the entries whose name begins with a dot") {
    const auto directory = std::filesystem::temp_directory_path() / "cc_at_dot_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    {
        std::ofstream file(directory / ".gitignore");
        file << "build\n";
    }
    {
        std::ofstream file(directory / "notes.txt");
        file << "hello\n";
    }

    // The reference lists every entry of the folder.
    const auto items = ui::suggestions_for("@", directory.string());

    bool found = false;
    for (const auto& item : items) {
        if (item.display == ".gitignore") found = true;
    }
    REQUIRE(found);

    std::filesystem::remove_all(directory);
}

TEST_CASE("The search of the files looks at the path and not only at the name") {
    const auto directory = std::filesystem::temp_directory_path() / "cc_at_path_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory / "src");
    {
        std::ofstream file(directory / "src" / "main.cpp");
        file << "int main() { return 0; }\n";
    }

    // The index of the reference holds the paths, so 'src/ma' finds the
    // file.
    const auto items = ui::suggestions_for("@src/ma", directory.string());

    bool found = false;
    for (const auto& item : items) {
        if (item.display == "src/main.cpp") found = true;
    }
    REQUIRE(found);

    std::filesystem::remove_all(directory);
}

TEST_CASE("The file suggestion replaces only the token it completes") {
    const auto directory = std::filesystem::temp_directory_path() / "cc_at_token_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    {
        std::ofstream file(directory / "hello.txt");
        file << "hello\n";
    }

    // The text the user wrote before the `@` is kept, so only the `@` and
    // the text that follows it are replaced.
    const auto items = ui::suggestions_for("hello @he", directory.string());
    REQUIRE(items.size() == 1);
    REQUIRE(items[0].display == "hello.txt");
    REQUIRE(items[0].insert_at == 6);
    REQUIRE(items[0].insert_length == 3);
    REQUIRE(items[0].insert == "@hello.txt ");

    std::filesystem::remove_all(directory);
}

TEST_CASE("The list of the `@` offers the agents under the files") {
    const auto directory = std::filesystem::temp_directory_path() / "cc_at_agents_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    const std::vector<ui::AgentEntry> agents = {{"Explore", "Looks for things in the code base"}};
    const auto items = ui::suggestions_for("@", directory.string(), {}, {}, agents);

    bool found = false;
    for (const auto& item : items) {
        if (item.display != "Explore (agent)") continue;
        found = true;
        // The agent is marked with a star, the way the reference marks it.
        REQUIRE(item.kind == ui::Suggestion::Kind::Agent);
        REQUIRE(item.description == "Looks for things in the code base");
    }
    REQUIRE(found);

    // The agent that is asked for by name is offered as well.
    const auto named = ui::suggestions_for("@exp", directory.string(), {}, {}, agents);
    REQUIRE(named.size() == 1);
    REQUIRE(named[0].display == "Explore (agent)");

    std::filesystem::remove_all(directory);
}
