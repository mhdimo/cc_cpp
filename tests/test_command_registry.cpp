#include <catch2/catch_test_macros.hpp>

#include "commands/registry.hpp"

#include <cstdlib>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace cc;

namespace cc::commands {

/// A probe that finds the setter of the model by argument-dependent lookup.
///
/// The port writes the model of the session into the description of the
/// command; the probe stands in for the model, so that the test can ask
/// whether the setter is there at all.
struct ModelProbe {
    std::string model;

    operator const std::string&() const { return model; }
};

} // namespace cc::commands

namespace {

/// True when the port can write the model of the session into the command.
template <typename T, typename = void>
struct HasSetModel : std::false_type {};

template <typename T>
struct HasSetModel<T, std::void_t<decltype(set_model(std::declval<T&>()))>> : std::true_type {};

/// Writes the model of the session into the command.
///
/// True when the port has a setter for it.
template <typename Probe>
bool write_model(const std::string& model) {
    if constexpr (HasSetModel<Probe>::value) {
        set_model(Probe{model});
        return true;
    } else {
        return false;
    }
}

/// True when the help lists the command.
///
/// The name is followed by a space, so that a command whose name is the
/// beginning of another one is not mistaken for it.
bool lists(const std::string& help, const std::string& name) {
    return help.find("\n  /" + name + " ") != std::string::npos;
}

} // namespace

TEST_CASE("The install command is not in the help") {
    // The reference reaches the native build through the command line, never
    // through a slash command.
    REQUIRE_FALSE(lists(commands::help_text(), "install"));
}

TEST_CASE("The install command is not a command of the session") {
    REQUIRE(commands::find_command("install") == nullptr);
}

TEST_CASE("The version command is kept out of the list") {
    // The reference offers the command to the users of its own build only,
    // so it is kept out of the list — but it is still there, and it still
    // answers when it is called by its name.
    REQUIRE(commands::find_command("version") != nullptr);
    REQUIRE_FALSE(lists(commands::help_text(), "version"));
}

TEST_CASE("The model command names the model of the session") {
    REQUIRE(write_model<commands::ModelProbe>("claude-sonnet-4-5"));

    const auto* command = commands::find_command("model");
    REQUIRE(command != nullptr);
    REQUIRE(command->description == "Set the AI model for Claude Code (currently Sonnet 4.5)");
}

TEST_CASE("The commands that name the machine carry the reference's wording") {
    // The reference picks the wording of these commands when the list is
    // built, so the port carries one of the two wordings of the reference.
    const auto* login = commands::find_command("login");
    REQUIRE(login != nullptr);
    REQUIRE((login->description == "Switch Anthropic accounts" ||
             login->description == "Sign in with your Anthropic account"));

    const auto* terminal_setup = commands::find_command("terminal-setup");
    REQUIRE(terminal_setup != nullptr);
    REQUIRE((terminal_setup->description ==
                 "Enable Option+Enter key binding for newlines and visual bell" ||
             terminal_setup->description == "Install Shift+Enter key binding for newlines"));

    const auto* init = commands::find_command("init");
    REQUIRE(init != nullptr);
    REQUIRE((init->description ==
                 "Initialize new CLAUDE.md file(s) and optional skills/hooks with codebase "
                 "documentation" ||
             init->description == "Initialize a new CLAUDE.md file with codebase documentation"));
}

TEST_CASE("The fast command carries the reference's wording") {
    const auto* command = commands::find_command("fast");
    REQUIRE(command != nullptr);
    // The fast mode is a research preview: the command says so.
    REQUIRE(command->description == "Toggle fast mode (Experimental)");
}

TEST_CASE("The commit command sends the context of the repository to the model") {
    commands::CommandContext context;
    context.cwd = ".";
    context.model = "claude-opus-4-6";

    std::string prompt;
    std::vector<std::string> allowed_tools;
    context.ask_model = [&prompt, &allowed_tools](const std::string& text,
                                                 const std::vector<std::string>& allowed) {
        prompt = text;
        allowed_tools = allowed;
        return "The commit was made.\n";
    };

    const auto result = commands::run_command("commit", "", context);
    REQUIRE(result.handled);

    // The context of the repository is read into the prompt, the way the
    // reference reads it: the commands are run and their output is put in
    // the place of the command.
    REQUIRE(prompt.find("Current git status:") != std::string::npos);
    REQUIRE(prompt.find("Current git diff (staged and unstaged changes):") != std::string::npos);
    REQUIRE(prompt.find("Current branch:") != std::string::npos);
    REQUIRE(prompt.find("Recent commits:") != std::string::npos);
    REQUIRE(prompt.find("Git Safety Protocol") != std::string::npos);

    // The attribution of the commit is the name of the model.
    REQUIRE(prompt.find("Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>") !=
            std::string::npos);

    // The model gets the git commands only, the way the reference gives
    // them to it.
    REQUIRE(allowed_tools.size() == 3);
    REQUIRE(allowed_tools[0] == "Bash(git add:*)");
    REQUIRE(allowed_tools[1] == "Bash(git status:*)");
    REQUIRE(allowed_tools[2] == "Bash(git commit:*)");
}

TEST_CASE("The commit-push-pr command sends the context of the repository to the model") {
    commands::CommandContext context;
    context.cwd = ".";
    context.model = "claude-sonnet-4-5";

    std::string prompt;
    std::vector<std::string> allowed_tools;
    context.ask_model = [&prompt, &allowed_tools](const std::string& text,
                                                 const std::vector<std::string>& allowed) {
        prompt = text;
        allowed_tools = allowed;
        return "The pull request was opened.\n";
    };

    const auto result = commands::run_command("commit-push-pr", "", context);
    REQUIRE(result.handled);
    REQUIRE(prompt.find("gh pr create") != std::string::npos);
    REQUIRE(prompt.find("Co-Authored-By: Claude Sonnet 4.5 <noreply@anthropic.com>") !=
            std::string::npos);
}

TEST_CASE("The upgrade command opens the page of the plan") {
    commands::CommandContext context;

    std::string opened;
    context.open_browser = [&opened](const std::string& url) {
        opened = url;
        return true;
    };

    const auto result = commands::run_command("upgrade", "", context);
    REQUIRE(result.handled);
    REQUIRE(opened == "https://claude.ai/upgrade/max");
    REQUIRE(result.output.find("https://claude.ai/upgrade/max") != std::string::npos);
}

TEST_CASE("The commands the port does not have are kept out of the list") {
    // The reference keeps most of these behind a feature flag or an
    // account, and the port keeps them out of its list as well, so that
    // every command that is offered works.
    for (const char* name : {"voice", "tag", "passes", "brief", "heapdump", "remote-control",
                              "remote-env", "think-back", "web-setup", "ultraplan",
                              "ultrareview"}) {
        const auto* command = commands::find_command(name);
        REQUIRE(command != nullptr);
        REQUIRE_FALSE(commands::is_available(*command));
    }
}

TEST_CASE("The command that installs the GitHub application is offered") {
    // The reference offers the command to the subscribers of Claude and to
    // the customers of the API that talk to Anthropic themselves.
    const auto* command = commands::find_command("install-github-app");
    REQUIRE(command != nullptr);
    REQUIRE(command->description == "Set up Claude GitHub Actions for a repository");

    // The session talks to Anthropic itself, so the command is offered.
    const char* base = std::getenv("ANTHROPIC_BASE_URL");
    const char* disabled = std::getenv("DISABLE_INSTALL_GITHUB_APP_COMMAND");
    setenv("ANTHROPIC_BASE_URL", "https://api.anthropic.com", 1);
    unsetenv("DISABLE_INSTALL_GITHUB_APP_COMMAND");
    REQUIRE(commands::is_available(*command));

    // The kill switch of the reference keeps it out of the list.
    setenv("DISABLE_INSTALL_GITHUB_APP_COMMAND", "1", 1);
    REQUIRE_FALSE(commands::is_available(*command));

    if (base != nullptr) {
        setenv("ANTHROPIC_BASE_URL", base, 1);
    } else {
        unsetenv("ANTHROPIC_BASE_URL");
    }
    if (disabled != nullptr) {
        setenv("DISABLE_INSTALL_GITHUB_APP_COMMAND", disabled, 1);
    }
}

TEST_CASE("The commit commands are offered to everybody") {
    // The reference offers the commands of the commit to everybody; only
    // the commands of the internal build are kept out of the list.
    REQUIRE(commands::find_command("commit") != nullptr);
    REQUIRE(commands::is_available(*commands::find_command("commit")));
    REQUIRE(commands::find_command("commit-push-pr") != nullptr);
    REQUIRE(commands::is_available(*commands::find_command("commit-push-pr")));

    // The command of the version is for the internal build only.
    REQUIRE(commands::find_command("version") != nullptr);
    REQUIRE_FALSE(commands::is_available(*commands::find_command("version")));

    // The command of the plan of the user is offered to everybody.
    REQUIRE(commands::is_available(*commands::find_command("upgrade")));
}

TEST_CASE("The hint of the arguments advances with the arguments that are typed") {
    const std::vector<std::string> names = {"file", "description"};

    // The reference names the arguments that are still to be filled in: the
    // arguments that were typed already are the ones that are filled in.
    REQUIRE(commands::progressive_argument_hint(names, "") == "[file] [description]");
    REQUIRE(commands::progressive_argument_hint(names, "foo") == "[description]");
    REQUIRE(commands::progressive_argument_hint(names, "foo ") == "[description]");
    REQUIRE(commands::progressive_argument_hint(names, "foo bar") == "");

    // Every argument is filled in: there is nothing left to say.
    REQUIRE(commands::progressive_argument_hint(names, "foo bar ").empty());
}
