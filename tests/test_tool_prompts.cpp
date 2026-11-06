#include <catch2/catch_test_macros.hpp>

#include "tools/impl/agent_tool.hpp"
#include "tools/impl/bash_tool.hpp"
#include "tools/impl/common.hpp"
#include "tools/impl/file_tools.hpp"
#include "tools/impl/misc_tools.hpp"
#include "tools/impl/search_tools.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "tools/tool_names.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace cc;

namespace {

/// True when a tool carries a prompt of its own.
///
/// The reference sends the prompt of a tool to the model and keeps the
/// one-line description for itself, so the port gives every tool a
/// `prompt()`.
template <typename T>
concept HasPrompt = requires(const T& tool) { tool.prompt(); };

/// The prompt the model is sent for a tool.
///
/// The text is empty when the tool carries no prompt of its own, so that a
/// test reports the missing prompt instead of failing to build.
template <typename T>
std::string prompt_of(const T& tool) {
    if constexpr (HasPrompt<T>) {
        return tool.prompt();
    } else {
        return {};
    }
}

/// True when a tool says that it is deferred behind a search tool.
template <typename T>
concept HasShouldDefer = requires(const T& tool) { tool.should_defer(); };

/// True when the tool says that it is deferred.
///
/// False when the tool has no word about it, the way the tools of the port
/// do not once the deferral is gone.
template <typename T>
bool defers(const T& tool) {
    if constexpr (HasShouldDefer<T>) {
        return tool.should_defer();
    } else {
        return false;
    }
}

/// True when the text holds the needle.
bool holds(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

/// True when the schema refuses the properties it does not know.
bool closed(const boost::json::object& schema) {
    const auto* additional = schema.find("additionalProperties");
    if (additional == nullptr) return false;
    // Only `false` closes a schema; anything else leaves it open.
    return additional->value().is_bool() && additional->value().as_bool();
}

/// The one agent the Agent tool offers.
agents::Definition one_agent() {
    agents::Definition definition;
    definition.name = "test-runner";
    definition.description = "Runs the tests of the repository";
    definition.prompt = "You run the tests of the repository.";
    return definition;
}

/// The runner of the Agent tool, which never runs anything.
std::string run_no_agent(const std::string&, const std::string&, const std::vector<std::string>&,
                          const std::string&) {
    return "done";
}

/// The tracker the file tools share.
std::shared_ptr<tools::FileStateTracker> a_tracker() {
    return std::make_shared<tools::FileStateTracker>();
}

/// A tool that carries no prompt of its own.
///
/// The tools of an MCP server are like this: they have a description and
/// nothing else.
class PlainTool : public Tool {
public:
    std::string_view name() const override { return "Plain"; }
    std::string description() const override { return "A plain tool"; }
    boost::json::object input_schema() const override { return {}; }
    ToolResult call(const boost::json::object&, ToolUseContext&) override { return {}; }
};

} // namespace

TEST_CASE("A tool without a prompt of its own sends its description") {
    const PlainTool plain;

    REQUIRE(plain.prompt() == "A plain tool");
}

TEST_CASE("The Bash prompt carries the reference's git instructions") {
    const tools::BashTool bash;
    const std::string prompt = prompt_of(bash);

    REQUIRE(holds(prompt, "# Committing changes with git"));
    REQUIRE(holds(prompt, "# Creating pull requests"));
    REQUIRE(holds(prompt, "# Other common operations"));
    REQUIRE(holds(prompt, "Git Safety Protocol:"));
    REQUIRE(holds(prompt, "- NEVER update the git config"));
    REQUIRE(holds(prompt, "- NEVER skip hooks (--no-verify, --no-gpg-sign, etc) unless the user "
                          "explicitly requests it"));
    REQUIRE(holds(prompt, "- NEVER run force push to main/master, warn the user if they request it"));
    REQUIRE(holds(prompt, "- CRITICAL: Always create NEW commits rather than amending, unless the "
                          "user explicitly requests a git amend."));
    REQUIRE(holds(prompt, "- When staging files, prefer adding specific files by name rather than "
                          "using \"git add -A\" or \"git add .\""));
    REQUIRE(holds(prompt, "- NEVER commit changes unless the user explicitly asks you to."));

    // The steps of the commit, in the order the reference gives them.
    REQUIRE(holds(prompt, "1. Run the following bash commands in parallel, each using the Bash "
                          "tool:"));
    REQUIRE(holds(prompt, "- Run a git status command to see all untracked files. IMPORTANT: Never "
                          "use the -uall flag as it can cause memory issues on large repos."));
    REQUIRE(holds(prompt, "- Run a git diff command to see both staged and unstaged changes that "
                          "will be committed."));
    REQUIRE(holds(prompt, "- Run a git log command to see recent commit messages, so that you can "
                          "follow this repository's commit message style."));
    REQUIRE(holds(prompt, "2. Analyze all staged changes (both previously staged and newly added) "
                          "and draft a commit message:"));
    REQUIRE(holds(prompt, "- Draft a concise (1-2 sentences) commit message that focuses on the "
                          "\"why\" rather than the \"what\""));
    REQUIRE(holds(prompt, "3. Run the following commands in parallel:"));
    REQUIRE(holds(prompt, "- Add relevant untracked files to the staging area."));
    REQUIRE(holds(prompt, "- Run git status after the commit completes to verify success."));
    REQUIRE(holds(prompt, "4. If the commit fails due to pre-commit hook: fix the issue and create "
                          "a NEW commit"));

    // The notes that go with the commit.
    REQUIRE(holds(prompt, "- NEVER run additional commands to read or explore code, besides git "
                          "bash commands"));
    REQUIRE(holds(prompt, "- NEVER use the TodoWrite or Agent tools"));
    REQUIRE(holds(prompt, "- DO NOT push to the remote repository unless the user explicitly asks "
                          "you to do so"));
    REQUIRE(holds(prompt, "- IMPORTANT: Never use git commands with the -i flag (like git rebase -i "
                          "or git add -i) since they require interactive input which is not "
                          "supported."));
    REQUIRE(holds(prompt, "- IMPORTANT: Do not use --no-edit with git rebase commands, as the "
                          "--no-edit flag is not a valid option for git rebase."));
    REQUIRE(holds(prompt, "- If there are no changes to commit (i.e., no untracked files and no "
                          "modifications), do not create an empty commit"));
    REQUIRE(holds(prompt, "- In order to ensure good formatting, ALWAYS pass the commit message "
                          "via a HEREDOC, a la this example:"));
    REQUIRE(holds(prompt, "git commit -m \"$(cat <<'EOF'"));

    // The pull request.
    REQUIRE(holds(prompt, "Use the gh command via the Bash tool for ALL GitHub-related tasks "
                          "including working with issues, pull requests, checks, and releases."));
    REQUIRE(holds(prompt, "IMPORTANT: When the user asks you to create a pull request, follow these "
                          "steps carefully:"));
    REQUIRE(holds(prompt, "- Keep the PR title short (under 70 characters)"));
    REQUIRE(holds(prompt, "- Use the description/body for details, not the title"));
    REQUIRE(holds(prompt, "- Create PR using gh pr create with the format below. Use a HEREDOC to "
                          "pass the body to ensure correct formatting."));
    REQUIRE(holds(prompt, "- Return the PR URL when you're done, so the user can see it"));

    // The operations that are neither a commit nor a pull request.
    REQUIRE(holds(prompt, "- View comments on a Github PR: gh api repos/foo/bar/pulls/123/comments"));
}

TEST_CASE("The Bash prompt indents its bullets the way the reference does") {
    const tools::BashTool bash;
    const std::string prompt = prompt_of(bash);

    // The bullets of the top level carry one space in front of them.
    REQUIRE(holds(prompt, " - File search: Use Glob (NOT find or ls)"));
    REQUIRE(holds(prompt, " - Content search: Use Grep (NOT grep or rg)"));
    REQUIRE(holds(prompt, " - Read files: Use Read (NOT cat/head/tail)"));
    REQUIRE(holds(prompt, " - Edit files: Use Edit (NOT sed/awk)"));
    REQUIRE(holds(prompt, " - Write files: Use Write (NOT echo >/cat <<EOF)"));
    REQUIRE(holds(prompt, " - Communication: Output text directly (NOT echo/printf)"));
    REQUIRE(holds(prompt, " - If your command will create new directories or files, first use this "
                          "tool to run `ls` to verify the parent directory exists and is the "
                          "correct location."));
    REQUIRE(holds(prompt, " - Always quote file paths that contain spaces with double quotes in "
                          "your command (e.g., cd \"path with spaces/file.txt\")"));
    REQUIRE(holds(prompt, " - Try to maintain your current working directory throughout the "
                          "session by using absolute paths and avoiding usage of `cd`."));
    REQUIRE(holds(prompt, " - You may specify an optional timeout in milliseconds (up to "));
    REQUIRE(holds(prompt, " - You can use the `run_in_background` parameter to run the command in "
                          "the background."));
    REQUIRE(holds(prompt, " - When issuing multiple commands:"));
    REQUIRE(holds(prompt, " - For git commands:"));
    REQUIRE(holds(prompt, " - Avoid unnecessary `sleep` commands:"));

    // The bullets that belong to a bullet carry two spaces in front of them.
    REQUIRE(holds(prompt, "  - If the commands are independent and can run in parallel, make "
                          "multiple Bash tool calls in a single message."));
    REQUIRE(holds(prompt, "  - If the commands depend on each other and must run sequentially, use "
                          "a single Bash call with '&&' to chain them together."));
    REQUIRE(holds(prompt, "  - Prefer to create a new commit rather than amending an existing "
                          "commit."));
    REQUIRE(holds(prompt, "  - Do not sleep between commands that can run immediately — just run "
                          "them."));
}

TEST_CASE("The Read prompt mentions the notebooks") {
    const tools::ReadTool read(a_tracker());
    const std::string prompt = prompt_of(read);

    REQUIRE(holds(prompt, "- This tool can read Jupyter notebooks (.ipynb files) and returns all "
                          "cells with their outputs, combining code, text, and visualizations."));
    REQUIRE(holds(prompt, "- This tool can only read files, not directories. To read a directory, "
                          "use an ls command via the Bash tool."));
}

TEST_CASE("The Read prompt mentions the PDFs") {
    const tools::ReadTool read(a_tracker());
    const std::string prompt = prompt_of(read);

    REQUIRE(holds(prompt,
                  "- This tool can read PDF files (.pdf). For large PDFs (more than 10 pages), you "
                  "MUST provide the pages parameter to read specific page ranges (e.g., pages: "
                  "\"1-5\"). Reading a large PDF without the pages parameter will fail. Maximum 20 "
                  "pages per request."));
}

TEST_CASE("The TodoWrite prompt is the reference's long prompt") {
    const tools::TodoWriteTool todo;
    const std::string prompt = prompt_of(todo);

    REQUIRE(holds(prompt, "Use this tool to create and manage a structured task list for your "
                          "current coding session."));
    REQUIRE(holds(prompt, "## When to Use This Tool"));
    REQUIRE(holds(prompt, "## When NOT to Use This Tool"));
    REQUIRE(holds(prompt, "## Examples of When to Use the Todo List"));
    REQUIRE(holds(prompt, "## Examples of When NOT to Use the Todo List"));
    REQUIRE(holds(prompt, "## Task States and Management"));
    REQUIRE(holds(prompt, "When in doubt, use this tool. Being proactive with task management "
                          "demonstrates attentiveness and ensures you complete all requirements "
                          "successfully."));

    // The name of the Edit tool stands in for the one of the reference.
    REQUIRE(holds(prompt, "Uses the Edit tool to add a comment to the calculateTotal function"));
}

TEST_CASE("The ExitPlanMode prompt carries the reference's examples") {
    const tools::ExitPlanModeTool exit_plan;
    const std::string prompt = prompt_of(exit_plan);

    REQUIRE(holds(prompt, "## Examples"));
    REQUIRE(holds(prompt,
                  "1. Initial task: \"Search for and understand the implementation of vim mode in "
                  "the codebase\" - Do not use the exit plan mode tool because you are not planning "
                  "the implementation steps of a task."));
    REQUIRE(holds(prompt,
                  "2. Initial task: \"Help me implement yank mode for vim\" - Use the exit plan "
                  "mode tool after you have finished planning the implementation steps of the "
                  "task."));
    REQUIRE(holds(prompt,
                  "3. Initial task: \"Add a new feature to handle user authentication\" - If "
                  "unsure about auth method (OAuth, JWT, etc.), use AskUserQuestion first, then "
                  "use exit plan mode tool after clarifying the approach."));
}

TEST_CASE("The Agent prompt tells the model how to write the prompt") {
    const tools::AgentTool agent({one_agent()}, run_no_agent);
    const std::string prompt = prompt_of(agent);

    REQUIRE(holds(prompt, "## Writing the prompt"));
    REQUIRE(holds(prompt, "Brief the agent like a smart colleague who just walked into the room"));
    REQUIRE(holds(prompt, "- Explain what you're trying to accomplish and why."));
    REQUIRE(holds(prompt, "- Describe what you've already learned or ruled out."));
    REQUIRE(holds(prompt, "- Give enough context about the surrounding problem that the agent can "
                          "make judgment calls rather than just following a narrow instruction."));
    REQUIRE(holds(prompt, "- If you need a short response, say so (\"report in under 200 words\")."));
    REQUIRE(holds(prompt, "Lookups: hand over the exact command. Investigations: hand over the "
                          "question"));
    REQUIRE(holds(prompt, "Terse command-style prompts produce shallow, generic work."));
    REQUIRE(holds(prompt, "**Never delegate understanding.** Don't write \"based on your "
                          "findings, fix the bug\" or \"based on the research, implement it.\""));
    REQUIRE(holds(prompt, "Write prompts that prove you understood: include file paths, line "
                          "numbers, what specifically to change."));

    // The section stands between the notes and the examples.
    REQUIRE(prompt.find("## Writing the prompt") < prompt.find("Example usage:"));
}

TEST_CASE("The Agent prompt uses the em dash of the reference") {
    const tools::AgentTool agent({one_agent()}, run_no_agent);
    const std::string prompt = prompt_of(agent);

    REQUIRE(holds(prompt, "Brief the agent like a smart colleague who just walked into the room "
                          "— it hasn't seen this conversation"));
    REQUIRE(holds(prompt, "Lookups: hand over the exact command. Investigations: hand over the "
                          "question — prescribed steps become dead weight when the premise is "
                          "wrong."));
}

TEST_CASE("The prompts of the other tools are the reference's prompts") {
    const tools::WriteTool write(a_tracker());
    const tools::EditTool edit(a_tracker());
    const tools::GlobTool glob;
    const tools::GrepTool grep;
    const tools::AskUserQuestionTool ask;

    REQUIRE(holds(prompt_of(write),
                  "Writes a file to the local filesystem.\n\nUsage:\n- This tool will overwrite the "
                  "existing file if there is one at the provided path."));
    REQUIRE(holds(prompt_of(edit), "Performs exact string replacements in files.\n\nUsage:"));
    REQUIRE(holds(prompt_of(glob), "- Fast file pattern matching tool that works with any codebase "
                                   "size"));
    REQUIRE(holds(prompt_of(grep), "A powerful search tool built on ripgrep"));
    REQUIRE(holds(prompt_of(ask), "Use this tool when you need to ask the user questions during "
                                   "execution."));
}

TEST_CASE("The Bash schema has the flag that turns the sandbox off") {
    const tools::BashTool bash;
    const auto schema = bash.input_schema();

    const auto* properties = schema.find("properties");
    REQUIRE(properties != nullptr);
    REQUIRE(properties->value().is_object());

    const auto& fields = properties->value().as_object();
    const auto* flag = fields.find("dangerouslyDisableSandbox");
    REQUIRE(flag != nullptr);
    REQUIRE(flag->value().is_object());

    const auto& field = flag->value().as_object();
    const auto* type = field.find("type");
    REQUIRE(type != nullptr);
    REQUIRE(type->value().as_string() == "boolean");

    const auto* description = field.find("description");
    REQUIRE(description != nullptr);
    REQUIRE(description->value().as_string() ==
            "Set this to true to dangerously override sandbox mode and run commands without "
            "sandboxing.");
}

TEST_CASE("The AskUserQuestion schema has the annotations and the metadata") {
    const tools::AskUserQuestionTool ask;
    const auto schema = ask.input_schema();

    const auto* properties = schema.find("properties");
    REQUIRE(properties != nullptr);
    REQUIRE(properties->value().is_object());

    const auto& fields = properties->value().as_object();

    // The answers the permission component collected are still there.
    REQUIRE(fields.find("answers") != fields.end());

    const auto* annotations = fields.find("annotations");
    REQUIRE(annotations != nullptr);
    REQUIRE(annotations->value().is_object());
    const auto* annotations_description = annotations->value().as_object().find("description");
    REQUIRE(annotations_description != nullptr);
    REQUIRE(annotations_description->value().as_string() ==
            "Optional per-question annotations from the user (e.g., notes on preview "
            "selections). Keyed by question text.");

    const auto* metadata = fields.find("metadata");
    REQUIRE(metadata != nullptr);
    REQUIRE(metadata->value().is_object());
    const auto* metadata_description = metadata->value().as_object().find("description");
    REQUIRE(metadata_description != nullptr);
    REQUIRE(metadata_description->value().as_string() ==
            "Optional metadata for tracking and analytics purposes. Not displayed to user.");
}

TEST_CASE("The schemas the reference leaves open stay open") {
    const tools::AgentTool agent({one_agent()}, run_no_agent);
    const tools::AskUserQuestionTool ask;
    const tools::ExitPlanModeTool exit_plan;

    // The reference builds the schema of the Agent tool with a plain object,
    // so the model may add properties of its own.
    REQUIRE_FALSE(closed(agent.input_schema()));

    // The questions and the options of AskUserQuestion are plain objects too.
    const auto ask_schema = ask.input_schema();
    const auto* properties = ask_schema.find("properties");
    REQUIRE(properties != nullptr);
    REQUIRE(properties->value().is_object());
    const auto* questions = properties->value().as_object().find("questions");
    REQUIRE(questions != nullptr);
    REQUIRE(questions->value().is_object());
    const auto* items = questions->value().as_object().find("items");
    REQUIRE(items != nullptr);
    REQUIRE(items->value().is_object());
    REQUIRE_FALSE(closed(items->value().as_object()));

    const auto* question_properties = items->value().as_object().find("properties");
    REQUIRE(question_properties != nullptr);
    REQUIRE(question_properties->value().is_object());
    const auto* options = question_properties->value().as_object().find("options");
    REQUIRE(options != nullptr);
    REQUIRE(options->value().is_object());
    const auto* option_items = options->value().as_object().find("items");
    REQUIRE(option_items != nullptr);
    REQUIRE(option_items->value().is_object());
    REQUIRE_FALSE(closed(option_items->value().as_object()));

    // The plan mode tool takes the fields the SDK adds to it.
    REQUIRE_FALSE(closed(exit_plan.input_schema()));
}

TEST_CASE("The tools that are deferred are either honoured or not claimed") {
    // The reference defers a few of its tools behind a search tool, so that
    // the model only sees them when it looks for them.
    const tools::Registry registry;
    const bool has_search = find_tool_by_name(registry.all(), tools::TOOL_SEARCH) != nullptr;

    if (!has_search) {
        // The port ships without the search tool, so it must not claim that
        // the tools are deferred.
        const tools::TodoWriteTool todo;
        const tools::AskUserQuestionTool ask;
        const tools::ExitPlanModeTool exit_plan;

        REQUIRE_FALSE(defers(todo));
        REQUIRE_FALSE(defers(ask));
        REQUIRE_FALSE(defers(exit_plan));
    }
}
