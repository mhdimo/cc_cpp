#include <catch2/catch_test_macros.hpp>

#include "commands/custom.hpp"
#include "ui/suggestions.hpp"
#include "utils/fs.hpp"
#include "utils/path.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

using namespace cc;

namespace {

/// The command named `name`, or nothing.
const commands::CustomCommand* find(const std::vector<commands::CustomCommand>& commands,
                                    const std::string& name) {
    for (const auto& command : commands) {
        if (command.name == name) return &command;
    }
    return nullptr;
}

/// The suggestion whose name is `name`, or nothing.
const ui::Suggestion* find_suggestion(const std::vector<ui::Suggestion>& items,
                                      const std::string& name) {
    for (const auto& item : items) {
        if (item.display == "/" + name) return &item;
    }
    return nullptr;
}

/// The folders a test works in.
///
/// The variable `CLAUDE_CONFIG_DIR` points the configuration of Claude Code
/// at the folder, and `CLAUDE_CODE_MANAGED_SETTINGS_PATH` points the
/// managed settings at it as well — the tests never read the commands of
/// the machine they run on.
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const char* base = std::getenv("TMPDIR");
        std::string root = base != nullptr ? base : "/tmp";
        while (!root.empty() && root.back() == '/') root.pop_back();

        // The name of the folder holds the process and the object, so that
        // two tests that run at the same time do not share it.
        path_ = path_join(path_join(root, "cc-custom-commands-test-" + std::to_string(::getpid())),
                          "home-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        remove_dir_all(path_);
        mkdirp(path_);

        had_previous_ = std::getenv("CLAUDE_CONFIG_DIR") != nullptr;
        if (had_previous_) previous_ = std::getenv("CLAUDE_CONFIG_DIR");
        ::setenv("CLAUDE_CONFIG_DIR", path_.c_str(), 1);

        had_managed_ = std::getenv("CLAUDE_CODE_MANAGED_SETTINGS_PATH") != nullptr;
        if (had_managed_) managed_previous_ = std::getenv("CLAUDE_CODE_MANAGED_SETTINGS_PATH");
        ::setenv("CLAUDE_CODE_MANAGED_SETTINGS_PATH", path_join(path_, "managed").c_str(), 1);
    }

    ~TemporaryDirectory() {
        if (had_previous_) {
            ::setenv("CLAUDE_CONFIG_DIR", previous_.c_str(), 1);
        } else {
            ::unsetenv("CLAUDE_CONFIG_DIR");
        }
        if (had_managed_) {
            ::setenv("CLAUDE_CODE_MANAGED_SETTINGS_PATH", managed_previous_.c_str(), 1);
        } else {
            ::unsetenv("CLAUDE_CODE_MANAGED_SETTINGS_PATH");
        }
        remove_dir_all(path_);
    }

    /// The folder the configuration of the user lives in.
    std::string path() const { return path_; }

    /// A folder of its own for the test.
    std::string project(const std::string& name) const { return path_join(path_, name); }

private:
    std::string path_;
    std::string previous_;
    std::string managed_previous_;
    bool had_previous_ = false;
    bool had_managed_ = false;
};

/// Writes a file, making the folders it is in.
void write(const std::string& path, const std::string& text) {
    const std::filesystem::path file(path);
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file);
    out << text;
}

} // namespace

TEST_CASE("The skills of the project are commands as well") {
    TemporaryDirectory home;
    const std::string project = home.project("skills");

    write(path_join(project, ".claude/skills/deploy/SKILL.md"),
          "---\n"
          "name: deploy\n"
          "description: Deploys the app\n"
          "argument-hint: <environment>\n"
          "arguments: environment\n"
          "---\n"
          "Deploy the app to $ARGUMENTS\n");

    const auto commands = commands::load_custom_commands(project);

    const auto* deploy = find(commands, "deploy");
    REQUIRE(deploy != nullptr);
    REQUIRE(deploy->description == "Deploys the app");
    REQUIRE(deploy->argument_hint == "<environment>");
    REQUIRE(deploy->argument_names == std::vector<std::string>{"environment"});
    REQUIRE(deploy->source == "projectSettings");

    // The model is told where the skill lives.
    REQUIRE(deploy->content.find("Base directory for this skill: ") != std::string::npos);
    REQUIRE(deploy->content.find("Deploy the app to $ARGUMENTS") != std::string::npos);
}

TEST_CASE("The name of a skill is the name of its folder") {
    TemporaryDirectory home;
    const std::string project = home.project("skill-name");

    // A skill that names another one in its frontmatter is called by the
    // name it names.
    write(path_join(project, ".claude/skills/folder/SKILL.md"),
          "---\nname: other\n---\nDo the thing\n");

    // A skill that names none is called after its folder.
    write(path_join(project, ".claude/skills/plain/SKILL.md"), "Do the thing\n");

    const auto commands = commands::load_custom_commands(project);

    const auto* other = find(commands, "other");
    REQUIRE(other != nullptr);
    REQUIRE(other->description == "Do the thing");

    const auto* plain = find(commands, "plain");
    REQUIRE(plain != nullptr);
}

TEST_CASE("A skill the user may not call is kept out of the list") {
    TemporaryDirectory home;
    const std::string project = home.project("hidden-skill");

    write(path_join(project, ".claude/skills/hidden/SKILL.md"),
          "---\nuser-invocable: false\ndescription: Only the model calls it\n---\nDo it\n");
    write(path_join(project, ".claude/skills/shown/SKILL.md"),
          "---\nuser-invocable: true\ndescription: The user calls it\n---\nDo it\n");

    const auto commands = commands::load_custom_commands(project);

    REQUIRE(find(commands, "hidden") == nullptr);
    REQUIRE(find(commands, "shown") != nullptr);
}

TEST_CASE("A command file that names no description is a custom command") {
    TemporaryDirectory home;
    const std::string project = home.project("description");

    // A file with no description and no text at all.
    write(path_join(project, ".claude/commands/empty.md"), "");
    // A file whose first line is a heading.
    write(path_join(project, ".claude/commands/heading.md"), "# The heading\n\nThe body\n");
    // A file whose first line is a heading without a space.
    write(path_join(project, ".claude/commands/nospace.md"), "#NoSpace\n\nThe body\n");
    // A file whose first line is a lone hash.
    write(path_join(project, ".claude/commands/hash.md"), "#\n\nThe body\n");

    const auto commands = commands::load_custom_commands(project);

    const auto* empty = find(commands, "empty");
    REQUIRE(empty != nullptr);
    REQUIRE(empty->description == "Custom command");

    // A heading is only stripped when it carries a space and some text.
    const auto* heading = find(commands, "heading");
    REQUIRE(heading != nullptr);
    REQUIRE(heading->description == "The heading");

    const auto* nospace = find(commands, "nospace");
    REQUIRE(nospace != nullptr);
    REQUIRE(nospace->description == "#NoSpace");

    const auto* hash = find(commands, "hash");
    REQUIRE(hash != nullptr);
    REQUIRE(hash->description == "#");
}

TEST_CASE("A skill that names no description is a skill") {
    TemporaryDirectory home;
    const std::string project = home.project("skill-description");

    write(path_join(project, ".claude/skills/empty/SKILL.md"), "");

    const auto commands = commands::load_custom_commands(project);

    const auto* empty = find(commands, "empty");
    REQUIRE(empty != nullptr);
    REQUIRE(empty->description == "Skill");
}

TEST_CASE("A skill is offered in the list of the commands") {
    TemporaryDirectory home;
    const std::string project = home.project("suggestion");

    write(path_join(project, ".claude/skills/deploy/SKILL.md"),
          "---\ndescription: Deploys the app\n---\nDeploy the app\n");

    const auto commands = commands::load_custom_commands(project);
    const auto items = ui::suggestions_for("/dep", project, commands);

    const auto* item = find_suggestion(items, "deploy");
    REQUIRE(item != nullptr);
    REQUIRE(item->description == "Deploys the app (project)");
}

TEST_CASE("The skills of the user and of the machine are commands as well") {
    TemporaryDirectory home;
    const std::string project = home.project("user-skills");

    write(path_join(home.path(), "skills/mine/SKILL.md"),
          "---\ndescription: A skill of the user\n---\nDo it\n");
    write(path_join(home.path(), "managed/.claude/skills/policy/SKILL.md"),
          "---\ndescription: A skill of the machine\n---\nDo it\n");

    const auto commands = commands::load_custom_commands(project);

    const auto* mine = find(commands, "mine");
    REQUIRE(mine != nullptr);
    REQUIRE(mine->source == "userSettings");

    const auto* policy = find(commands, "policy");
    REQUIRE(policy != nullptr);
    REQUIRE(policy->source == "policySettings");
}
