#include "tools/impl/common.hpp"

#include "utils/fs.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace cc::tools {
namespace {

/// The maximum number of characters in a schema description before it is
/// truncated.
constexpr size_t kMaxDescriptionLength = 1024;

std::string truncate_description(std::string_view description) {
    if (description.size() <= kMaxDescriptionLength) return std::string(description);
    return std::string(description.substr(0, kMaxDescriptionLength)) + "…";
}

} // namespace

boost::json::object schema_object(
    std::initializer_list<std::pair<std::string, boost::json::object>> properties,
    std::initializer_list<std::string> required,
    bool closed) {
    boost::json::object node;
    node["type"] = "object";

    boost::json::object props;
    for (const auto& [name, schema] : properties) {
        props[name] = schema;
    }
    node["properties"] = std::move(props);

    if (!required.size()) {
        node["required"] = boost::json::array();
    } else {
        boost::json::array names;
        for (const auto& name : required) {
            names.emplace_back(name);
        }
        node["required"] = std::move(names);
    }

    if (closed) {
        node["additionalProperties"] = false;
    }
    return node;
}

boost::json::object schema_string(std::string_view description) {
    boost::json::object node;
    node["type"] = "string";
    node["description"] = truncate_description(description);
    return node;
}

boost::json::object schema_integer(std::string_view description) {
    boost::json::object node;
    node["type"] = "integer";
    node["description"] = truncate_description(description);
    return node;
}

boost::json::object schema_number(std::string_view description) {
    boost::json::object node;
    node["type"] = "number";
    node["description"] = truncate_description(description);
    return node;
}

boost::json::object schema_boolean(std::string_view description) {
    boost::json::object node;
    node["type"] = "boolean";
    node["description"] = truncate_description(description);
    return node;
}

boost::json::object schema_enum(std::string_view description,
                                std::initializer_list<std::string> values) {
    boost::json::object node;
    node["type"] = "string";
    node["description"] = truncate_description(description);
    boost::json::array options;
    for (const auto& value : values) {
        options.emplace_back(value);
    }
    node["enum"] = std::move(options);
    return node;
}

boost::json::object schema_array(std::string_view description, boost::json::object items) {
    boost::json::object node;
    node["type"] = "array";
    node["description"] = truncate_description(description);
    node["items"] = std::move(items);
    return node;
}

boost::json::object schema_any_object(std::string_view description) {
    boost::json::object node;
    node["type"] = "object";
    node["description"] = truncate_description(description);
    return node;
}

void FileStateTracker::record_read(const std::string& path, int64_t offset,
                                   std::optional<int64_t> limit) {
    const FileState state = file_state(path);
    Entry entry;
    entry.exists = state.exists;
    entry.size = state.size;
    entry.mtime_ms = state.mtime_ms;
    entry.from_read = true;
    entry.read_offset = offset;
    entry.read_limit = limit;
    // A read that starts away from the beginning of the file, or that stops
    // early, shows the model only part of it.
    entry.partial_view = limit.has_value() || offset > 1;

    store(path, entry);
}

void FileStateTracker::record_write(const std::string& path) {
    const FileState state = file_state(path);
    Entry entry;
    entry.exists = state.exists;
    entry.size = state.size;
    entry.mtime_ms = state.mtime_ms;

    store(path, entry);
}

void FileStateTracker::store(const std::string& path, Entry entry) {
    for (auto& [key, value] : entries_) {
        if (key == path) {
            value = std::move(entry);
            return;
        }
    }
    entries_.emplace_back(path, std::move(entry));
}

bool FileStateTracker::was_read(const std::string& path) const {
    for (const auto& [key, value] : entries_) {
        if (key == path) return true;
    }
    return false;
}

bool FileStateTracker::unchanged_since_read(const std::string& path) const {
    for (const auto& [key, value] : entries_) {
        if (key != path) continue;
        const FileState state = file_state(path);
        return state.exists == value.exists && state.size == value.size &&
               state.mtime_ms == value.mtime_ms;
    }
    return false;
}

bool FileStateTracker::range_unchanged_since_read(const std::string& path, int64_t offset,
                                                   std::optional<int64_t> limit) const {
    for (const auto& [key, value] : entries_) {
        if (key != path) continue;
        // Only a Read leaves a range behind; a write clears it.
        if (!value.from_read) return false;
        if (value.read_offset != offset || value.read_limit != limit) return false;
        const FileState state = file_state(path);
        return state.exists == value.exists && state.size == value.size &&
               state.mtime_ms == value.mtime_ms;
    }
    return false;
}

bool FileStateTracker::is_partial_view(const std::string& path) const {
    for (const auto& [key, value] : entries_) {
        if (key == path) return value.partial_view;
    }
    return false;
}

std::string format_file_size(int64_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    if (bytes < 1024) {
        return std::to_string(bytes) + "B";
    }
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.1f%s", value, units[unit]);
    return buffer;
}

std::string did_you_mean(const std::string& path, const std::string& cwd) {
    namespace fs = std::filesystem;

    const std::string directory = dirname(path);
    const std::string name = basename(path);

    std::error_code ec;
    std::vector<std::string> candidates;

    if (!directory.empty() && fs::is_directory(fs::path(directory), ec)) {
        for (const auto& entry : fs::directory_iterator(fs::path(directory), ec)) {
            if (entry.is_regular_file(ec)) {
                candidates.push_back(entry.path().string());
            }
        }
    }

    if (candidates.empty() && !cwd.empty()) {
        for (const auto& entry : fs::directory_iterator(fs::path(cwd), ec)) {
            if (entry.is_regular_file(ec)) {
                candidates.push_back(entry.path().string());
            }
        }
    }

    const std::optional<std::string> match = closest_match(name, candidates, 3);
    if (!match) return {};
    return "Did you mean " + basename(*match) + "?";
}

std::string expand_path(std::string_view path) {
    std::string trimmed = trim(path);
    std::string replaced = replace_all(trimmed, "\\", "/");
    return expand_home(replaced);
}

std::string tool_use_error(std::string_view message) {
    return "<tool_use_error>" + std::string(message) + "</tool_use_error>";
}

std::string system_reminder(std::string_view message) {
    return "<system-reminder>" + std::string(message) + "</system-reminder>";
}

std::string read_tool_prompt(bool include_max_size, int64_t max_size_bytes, bool targeted_offset,
                             bool pdf_supported) {
    std::string max_size_instruction;
    if (include_max_size) {
        max_size_instruction = ". Files larger than " + format_file_size(max_size_bytes) +
                               " will return an error; use offset and limit for larger files";
    }

    const std::string offset_instruction =
        targeted_offset
            ? "- When you already know which part of the file you need, only read that part. This "
              "can be important for larger files."
            : "- You can optionally specify a line offset and limit (especially handy for long "
              "files), but it's recommended to read the whole file by not providing these "
              "parameters";

    std::string prompt =
        "Reads a file from the local filesystem. You can access any file directly by using "
        "this tool.\n"
        "Assume this tool is able to read all files on the machine. If the User provides a path "
        "to a file assume that path is valid. It is okay to read a file that does not exist; an "
        "error will be returned.\n"
        "\n"
        "Usage:\n"
        "- The file_path parameter must be an absolute path, not a relative path\n"
        "- By default, it reads up to 2000 lines starting from the beginning of the file" +
        max_size_instruction + "\n" + offset_instruction +
        "\n"
        "- Results are returned using cat -n format, with line numbers starting at 1\n"
        "- This tool allows Claude Code to read images (eg PNG, JPG, etc). When reading an image "
        "file the contents are presented visually as Claude Code is a multimodal LLM.\n";

    // The PDF line is only in the prompt when the model of the session reads
    // the PDF files, the way the reference leaves it out for the models that
    // do not.
    if (pdf_supported) {
        prompt +=
            "- This tool can read PDF files (.pdf). For large PDFs (more than 10 pages), you MUST "
            "provide the pages parameter to read specific page ranges (e.g., pages: \"1-5\"). "
            "Reading a large PDF without the pages parameter will fail. Maximum 20 pages per "
            "request.\n";
    }

    prompt +=
        "- This tool can read Jupyter notebooks (.ipynb files) and returns all cells with their "
        "outputs, combining code, text, and visualizations.\n"
        "- This tool can only read files, not directories. To read a directory, use an ls "
        "command via the Bash tool.\n"
        "- You will regularly be asked to read screenshots. If the user provides a path to a "
        "screenshot, ALWAYS use this tool to view the file at the path. This tool will work "
        "with all temporary file paths.\n"
        "- If you read a file that exists but has empty contents you will receive a system "
        "reminder warning in place of file contents.";

    return prompt;
}

std::string write_tool_prompt() {
    return "Writes a file to the local filesystem.\n"
           "\n"
           "Usage:\n"
           "- This tool will overwrite the existing file if there is one at the provided path.\n"
           "- If this is an existing file, you MUST use the Read tool first to read the file's "
           "contents. This tool will fail if you did not read the file first.\n"
           "- Prefer the Edit tool for modifying existing files — it only sends the diff. Only use "
           "this tool to create new files or for complete rewrites.\n"
           "- NEVER create documentation files (*.md) or README files unless explicitly requested "
           "by the User.\n"
           "- Only use emojis if the user explicitly requests it. Avoid writing emojis to files "
           "unless asked.";
}

std::string edit_tool_prompt(bool compact_line_prefix) {
    const std::string prefix_format =
        compact_line_prefix ? "line number + tab" : "spaces + line number + arrow";
    return "Performs exact string replacements in files.\n"
           "\n"
           "Usage:\n"
           "- You must use your `Read` tool at least once in the conversation before editing. This "
           "tool will error if you attempt an edit without reading the file.\n"
           "- When editing text from Read tool output, ensure you preserve the exact indentation "
           "(tabs/spaces) as it appears AFTER the line number prefix. The line number prefix "
           "format is: {" +
           prefix_format +
           "}. Everything after that is the actual file content to match. Never include any part "
           "of the line number prefix in the old_string or new_string.\n"
           "- ALWAYS prefer editing existing files in the codebase. NEVER write new files unless "
           "explicitly required.\n"
           "- Only use emojis if the user explicitly requests it. Avoid adding emojis to files "
           "unless asked.\n"
           "- The edit will FAIL if `old_string` is not unique in the file. Either provide a "
           "larger string with more surrounding context to make it unique or use `replace_all` to "
           "change every instance of `old_string`.\n"
           "- Use `replace_all` for replacing and renaming strings across the file. This parameter "
           "is useful if you want to rename a variable for instance.";
}

std::string glob_tool_prompt() {
    return "- Fast file pattern matching tool that works with any codebase size\n"
           "- Supports glob patterns like \"**/*.js\" or \"src/**/*.ts\"\n"
           "- Returns matching file paths sorted by modification time\n"
           "- Use this tool when you need to find files by name patterns\n"
           "- When you are doing an open ended search that may require multiple rounds of globbing "
           "and grepping, use the Agent tool instead";
}

std::string grep_tool_prompt() {
    return "A powerful search tool built on ripgrep\n"
           "\n"
           "  Usage:\n"
           "  - ALWAYS use Grep for search tasks. NEVER invoke `grep` or `rg` as a Bash command. "
           "The Grep tool has been optimized for correct permissions and access.\n"
           "  - Supports full regex syntax (e.g., \"log.*Error\", \"function\\s+\\w+\")\n"
           "  - Filter files with glob parameter (e.g., \"*.js\", \"**/*.tsx\") or type parameter "
           "(e.g., \"js\", \"py\", \"rust\")\n"
           "  - Output modes: \"content\" shows matching lines, \"files_with_matches\" shows only "
           "file paths (default), \"count\" shows match counts\n"
           "  - Use Agent tool for open-ended searches requiring multiple rounds\n"
           "  - Pattern syntax: Uses ripgrep (not grep) - literal braces need escaping (use "
           "`interface\\{\\}` to find `interface{}` in Go code)\n"
           "  - Multiline matching: By default patterns match within single lines only. For "
           "cross-line patterns like `struct \\{[\\s\\S]*?field`, use `multiline: true`";
}

std::string bash_tool_prompt(int64_t max_timeout_ms, int64_t default_timeout_ms,
                             const std::vector<std::string>& sandbox_section,
                             bool include_git_instructions) {
    // The reference builds the prompt out of lines and joins them with a
    // newline; the bullets of a list carry one space in front of them and
    // the ones of a list inside a bullet carry two.
    std::vector<std::string> lines;

    lines.push_back("Executes a given bash command and returns its output.");
    lines.push_back("");
    lines.push_back(
        "The working directory persists between commands, but shell state does not. The shell "
        "environment is initialized from the user's profile (bash or zsh).");
    lines.push_back("");
    lines.push_back(
        "IMPORTANT: Avoid using this tool to run `find`, `grep`, `cat`, `head`, `tail`, `sed`, "
        "`awk`, or `echo` commands, unless explicitly instructed or after you have verified that a "
        "dedicated tool cannot accomplish your task. Instead, use the appropriate dedicated tool "
        "as this will provide a much better experience for the user:");
    lines.push_back("");
    lines.push_back(" - File search: Use Glob (NOT find or ls)");
    lines.push_back(" - Content search: Use Grep (NOT grep or rg)");
    lines.push_back(" - Read files: Use Read (NOT cat/head/tail)");
    lines.push_back(" - Edit files: Use Edit (NOT sed/awk)");
    lines.push_back(" - Write files: Use Write (NOT echo >/cat <<EOF)");
    lines.push_back(" - Communication: Output text directly (NOT echo/printf)");
    lines.push_back(
        "While the Bash tool can do similar things, it’s better to use the built-in tools as they "
        "provide a better user experience and make it easier to review tool calls and give "
        "permission.");
    lines.push_back("");
    lines.push_back("# Instructions");
    lines.push_back(
        " - If your command will create new directories or files, first use this tool to run `ls` "
        "to verify the parent directory exists and is the correct location.");
    lines.push_back(
        " - Always quote file paths that contain spaces with double quotes in your command (e.g., "
        "cd \"path with spaces/file.txt\")");
    lines.push_back(
        " - Try to maintain your current working directory throughout the session by using absolute "
        "paths and avoiding usage of `cd`. You may use `cd` if the User explicitly requests it.");
    lines.push_back(" - You may specify an optional timeout in milliseconds (up to " +
                    std::to_string(max_timeout_ms) + "ms / " +
                    std::to_string(max_timeout_ms / 60000) +
                    " minutes). By default, your command will timeout after " +
                    std::to_string(default_timeout_ms) + "ms (" +
                    std::to_string(default_timeout_ms / 60000) + " minutes).");
    lines.push_back(
        " - You can use the `run_in_background` parameter to run the command in the background. "
        "Only use this if you don't need the result immediately and are OK being notified when the "
        "command completes later. You do not need to check the output right away - you'll be "
        "notified when it finishes. You do not need to use '&' at the end of the command when "
        "using this parameter.");
    lines.push_back(" - When issuing multiple commands:");
    lines.push_back(
        "  - If the commands are independent and can run in parallel, make multiple Bash tool "
        "calls in a single message. Example: if you need to run \"git status\" and \"git diff\", "
        "send a single message with two Bash tool calls in parallel.");
    lines.push_back(
        "  - If the commands depend on each other and must run sequentially, use a single Bash "
        "call with '&&' to chain them together.");
    lines.push_back(
        "  - Use ';' only when you need to run commands sequentially but don't care if earlier "
        "commands fail.");
    lines.push_back("  - DO NOT use newlines to separate commands (newlines are ok in quoted "
                    "strings).");
    lines.push_back(" - For git commands:");
    lines.push_back("  - Prefer to create a new commit rather than amending an existing commit.");
    lines.push_back(
        "  - Before running destructive operations (e.g., git reset --hard, git push --force, git "
        "checkout --), consider whether there is a safer alternative that achieves the same goal. "
        "Only use destructive operations when they are truly the best approach.");
    lines.push_back(
        "  - Never skip hooks (--no-verify) or bypass signing (--no-gpg-sign, -c "
        "commit.gpgsign=false) unless the user has explicitly asked for it. If a hook fails, "
        "investigate and fix the underlying issue.");
    lines.push_back(" - Avoid unnecessary `sleep` commands:");
    lines.push_back("  - Do not sleep between commands that can run immediately — just run them.");
    lines.push_back(
        "  - If your command is long running and you would like to be notified when it finishes — "
        "use `run_in_background`. No sleep needed.");
    lines.push_back("  - Do not retry failing commands in a sleep loop — diagnose the root cause.");
    lines.push_back(
        "  - If waiting for a background task you started with `run_in_background`, you will be "
        "notified when it completes — do not poll.");
    lines.push_back(
        "  - If you must poll an external process, use a check command (e.g., `gh run view`) rather "
        "than sleeping first.");
    lines.push_back(
        "  - If you must sleep, keep the duration short (1-5 seconds) to avoid blocking the "
        "user.");

    // The section of the sandbox follows the bullets, with a line of its own
    // in front of it.
    if (!sandbox_section.empty()) {
        lines.push_back("");
        for (const std::string& line : sandbox_section) {
            lines.push_back(line);
        }
    }

    // The instructions about the commits and the pull requests come last,
    // with a line of their own in front of them.
    if (git_instructions_enabled(include_git_instructions)) {
        const std::string instructions = git_commit_and_pr_instructions();
        if (!instructions.empty()) {
            lines.push_back("");
            for (const std::string& line : split_lines(instructions)) {
                lines.push_back(line);
            }
        }
    }

    return join(lines, "\n");
}

bool git_instructions_enabled(bool setting) {
    const char* value = std::getenv("CLAUDE_CODE_DISABLE_GIT_INSTRUCTIONS");
    if (value == nullptr) return setting;

    const std::string normalized = cc::to_lower(cc::trim(value));
    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
        return false;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
        return true;
    }
    return setting;
}

std::string git_commit_and_pr_instructions() {
    return "# Committing changes with git\n"
           "\n"
           "Only create commits when requested by the user. If unclear, ask first. When the user "
           "asks you to create a new git commit, follow these steps carefully:\n"
           "\n"
           "You can call multiple tools in a single response. When multiple independent pieces of "
           "information are requested and all commands are likely to succeed, run multiple tool "
           "calls in parallel for optimal performance. The numbered steps below indicate which "
           "commands should be batched in parallel.\n"
           "\n"
           "Git Safety Protocol:\n"
           "- NEVER update the git config\n"
           "- NEVER run destructive git commands (push --force, reset --hard, checkout ., restore "
           "., clean -f, branch -D) unless the user explicitly requests these actions. Taking "
           "unauthorized destructive actions is unhelpful and can result in lost work, so it's best "
           "to ONLY run these commands when given direct instructions \n"
           "- NEVER skip hooks (--no-verify, --no-gpg-sign, etc) unless the user explicitly requests "
           "it\n"
           "- NEVER run force push to main/master, warn the user if they request it\n"
           "- CRITICAL: Always create NEW commits rather than amending, unless the user explicitly "
           "requests a git amend. When a pre-commit hook fails, the commit did NOT happen — so "
           "--amend would modify the PREVIOUS commit, which may result in destroying work or "
           "losing previous changes. Instead, after hook failure, fix the issue, re-stage, and "
           "create a NEW commit\n"
           "- When staging files, prefer adding specific files by name rather than using \"git "
           "add -A\" or \"git add .\", which can accidentally include sensitive files (.env, "
           "credentials) or large binaries\n"
           "- NEVER commit changes unless the user explicitly asks you to. It is VERY IMPORTANT to "
           "only commit when explicitly asked, otherwise the user will feel that you are being too "
           "proactive\n"
           "\n"
           "1. Run the following bash commands in parallel, each using the Bash tool:\n"
           "  - Run a git status command to see all untracked files. IMPORTANT: Never use the -uall "
           "flag as it can cause memory issues on large repos.\n"
           "  - Run a git diff command to see both staged and unstaged changes that will be "
           "committed.\n"
           "  - Run a git log command to see recent commit messages, so that you can follow this "
           "repository's commit message style.\n"
           "2. Analyze all staged changes (both previously staged and newly added) and draft a "
           "commit message:\n"
           "  - Summarize the nature of the changes (eg. new feature, enhancement to an existing "
           "feature, bug fix, refactoring, test, docs, etc.). Ensure the message accurately "
           "reflects the changes and their purpose (i.e. \"add\" means a wholly new feature, "
           "\"update\" means an enhancement to an existing feature, \"fix\" means a bug fix, etc.).\n"
           "  - Do not commit files that likely contain secrets (.env, credentials.json, etc). Warn "
           "the user if they specifically request to commit those files\n"
           "  - Draft a concise (1-2 sentences) commit message that focuses on the \"why\" rather "
           "than the \"what\"\n"
           "  - Ensure it accurately reflects the changes and their purpose\n"
           "3. Run the following commands in parallel:\n"
           "   - Add relevant untracked files to the staging area.\n"
           "   - Create the commit with a message.\n"
           "   - Run git status after the commit completes to verify success.\n"
           "   Note: git status depends on the commit completing, so run it sequentially after the "
           "commit.\n"
           "4. If the commit fails due to pre-commit hook: fix the issue and create a NEW commit\n"
           "\n"
           "Important notes:\n"
           "- NEVER run additional commands to read or explore code, besides git bash commands\n"
           "- NEVER use the TodoWrite or Agent tools\n"
           "- DO NOT push to the remote repository unless the user explicitly asks you to do so\n"
           "- IMPORTANT: Never use git commands with the -i flag (like git rebase -i or git add -i) "
           "since they require interactive input which is not supported.\n"
           "- IMPORTANT: Do not use --no-edit with git rebase commands, as the --no-edit flag is not "
           "a valid option for git rebase.\n"
           "- If there are no changes to commit (i.e., no untracked files and no modifications), do "
           "not create an empty commit\n"
           "- In order to ensure good formatting, ALWAYS pass the commit message via a HEREDOC, a "
           "la this example:\n"
           "<example>\n"
           "git commit -m \"$(cat <<'EOF'\n"
           "   Commit message here.\n"
           "   EOF\n"
           ")\"\n"
           "</example>\n"
           "\n"
           "# Creating pull requests\n"
           "Use the gh command via the Bash tool for ALL GitHub-related tasks including working "
           "with issues, pull requests, checks, and releases. If given a Github URL use the gh "
           "command to get the information needed.\n"
           "\n"
           "IMPORTANT: When the user asks you to create a pull request, follow these steps "
           "carefully:\n"
           "\n"
           "1. Run the following bash commands in parallel using the Bash tool, in order to "
           "understand the current state of the branch since it diverged from the main branch:\n"
           "   - Run a git status command to see all untracked files (never use -uall flag)\n"
           "   - Run a git diff command to see both staged and unstaged changes that will be "
           "committed\n"
           "   - Check if the current branch tracks a remote branch and is up to date with the "
           "remote, so you know if you need to push to the remote\n"
           "   - Run a git log command and `git diff [base-branch]...HEAD` to understand the full "
           "commit history for the current branch (from the time it diverged from the base branch)\n"
           "2. Analyze all changes that will be included in the pull request, making sure to look "
           "at all relevant commits (NOT just the latest commit, but ALL commits that will be "
           "included in the pull request!!!), and draft a pull request title and summary:\n"
           "   - Keep the PR title short (under 70 characters)\n"
           "   - Use the description/body for details, not the title\n"
           "3. Run the following commands in parallel:\n"
           "   - Create new branch if needed\n"
           "   - Push to remote with -u flag if needed\n"
           "   - Create PR using gh pr create with the format below. Use a HEREDOC to pass the body "
           "to ensure correct formatting.\n"
           "<example>\n"
           "gh pr create --title \"the pr title\" --body \"$(cat <<'EOF'\n"
           "## Summary\n"
           "<1-3 bullet points>\n"
           "\n"
           "## Test plan\n"
           "[Bulleted markdown checklist of TODOs for testing the pull request...]\n"
           "EOF\n"
           ")\"\n"
           "</example>\n"
           "\n"
           "Important:\n"
           "- DO NOT use the TodoWrite or Agent tools\n"
           "- Return the PR URL when you're done, so the user can see it\n"
           "\n"
           "# Other common operations\n"
           "- View comments on a Github PR: gh api repos/foo/bar/pulls/123/comments";
}

std::string todo_write_tool_description() {
    return "Update the todo list for the current session. To be used proactively and often to "
           "track progress and pending tasks. Make sure that at least one task is in_progress at "
           "all times. Always provide both content (imperative) and activeForm (present "
           "continuous) for each task.";
}

std::string todo_write_tool_prompt() {
    return "Use this tool to create and manage a structured task list for your current coding "
           "session. This helps you track progress, organize complex tasks, and demonstrate "
           "thoroughness to the user.\n"
           "It also helps the user understand the progress of the task and overall progress of "
           "their requests.\n"
           "\n"
           "## When to Use This Tool\n"
           "Use this tool proactively in these scenarios:\n"
           "\n"
           "1. Complex multi-step tasks - When a task requires 3 or more distinct steps or actions\n"
           "2. Non-trivial and complex tasks - Tasks that require careful planning or multiple "
           "operations\n"
           "3. User explicitly requests todo list - The user directly asks you to use the todo list\n"
           "4. User provides multiple tasks - User provides a list of things to be done (numbered or "
           "comma-separated)\n"
           "5. After receiving new instructions - Immediately capture user requirements as todos\n"
           "6. When you start working on a task - Mark it as in_progress BEFORE beginning work. "
           "Ideally you should only have one todo as in_progress at a time\n"
           "7. After completing a task - Mark it as completed and add any new follow-up tasks "
           "discovered during implementation\n"
           "\n"
           "## When NOT to Use This Tool\n"
           "\n"
           "Skip using this tool when:\n"
           "1. There is only a single, straightforward task\n"
           "2. The task is trivial and tracking it provides no organizational benefit\n"
           "3. The task can be completed in less than 3 trivial steps\n"
           "4. The task is purely conversational or informational\n"
           "\n"
           "NOTE that you should not use this tool if there is only one trivial task to do. In this "
           "case you are better off just doing the task directly.\n"
           "\n"
           "## Examples of When to Use the Todo List\n"
           "\n"
           "<example>\n"
           "User: I want to add a dark mode toggle to the application settings. Make sure you run "
           "the tests and build when you're done!\n"
           "Assistant: *Creates todo list with the following items:*\n"
           "1. Creating dark mode toggle component in Settings page\n"
           "2. Adding dark mode state management (context/store)\n"
           "3. Implementing CSS-in-JS styles for dark theme\n"
           "4. Updating existing components to support theme switching\n"
           "5. Running tests and build process, addressing any failures or errors that occur\n"
           "*Begins working on the first task*\n"
           "\n"
           "<reasoning>\n"
           "The assistant used the todo list because:\n"
           "1. Adding dark mode is a multi-step feature requiring UI, state management, and styling "
           "changes\n"
           "2. The user explicitly requested tests and build be run afterward\n"
           "3. The assistant inferred that tests and build need to pass by adding \"Ensure tests "
           "and build succeed\" as the final task\n"
           "</reasoning>\n"
           "</example>\n"
           "\n"
           "<example>\n"
           "User: Help me rename the function getCwd to getCurrentWorkingDirectory across my "
           "project\n"
           "Assistant: *Uses grep or search tools to locate all instances of getCwd across the "
           "codebase*\n"
           "I've found 15 instances of 'getCwd' across 8 different files.\n"
           "*Creates todo list with specific items for each file to update*\n"
           "\n"
           "<reasoning>\n"
           "The assistant used the todo list because:\n"
           "1. First, the assistant searched to understand the scope of the task\n"
           "2. Upon finding multiple occurrences across different files, it determined this was a "
           "complex multi-step task\n"
           "3. The todo list helps ensure every instance is tracked and updated systematically\n"
           "4. This approach prevents missing any occurrences and maintains code consistency\n"
           "</reasoning>\n"
           "</example>\n"
           "\n"
           "\n"
           "<example>\n"
           "User: I need to implement these features for my e-commerce site: user registration, "
           "product catalog, shopping cart, and checkout flow.\n"
           "Assistant: *Creates a todo list breaking down these features into specific tasks based "
           "on the project architecture*\n"
           "Let's start with user registration. This will involve creating database models, "
           "API endpoints, and frontend components.\n"
           "\n"
           "<reasoning>\n"
           "The assistant used the todo list because:\n"
           "1. The user provided multiple complex features to implement in a comma separated list\n"
           "2. The todo list helps organize these large features into manageable steps\n"
           "3. The todo list allows for tracking progress and ensuring all features are completed "
           "successfully\n"
           "</reasoning>\n"
           "</example>\n"
           "\n"
           "<example>\n"
           "User: Can you help optimize my React application? It's rendering slowly and has "
           "performance issues.\n"
           "Assistant: *Reviews component structure, render logic, and data fetching patterns*\n"
           "After analyzing the codebase, I've identified several performance bottlenecks.\n"
           "*Identifies items: 1) Implementing code splitting for route components, 2) Memoizing "
           "expensive calculations, 3) Optimizing image loading, 4) Reviewing bundle size and "
           "implementing dynamic imports*\n"
           "Let's start with the code splitting optimization since it will have the most impact on "
           "initial load performance.\n"
           "\n"
           "<reasoning>\n"
           "The assistant used the todo list because:\n"
           "1. First, the assistant examined the codebase to identify specific performance issues\n"
           "2. Based on this analysis, it identified multiple optimization opportunities\n"
           "3. Performance optimization is a non-trivial task requiring multiple steps\n"
           "4. The todo list helps methodically track improvements across different components\n"
           "5. This systematic approach ensures all performance bottlenecks are addressed\n"
           "</reasoning>\n"
           "</example>\n"
           "\n"
           "## Examples of When NOT to Use the Todo List\n"
           "\n"
           "<example>\n"
           "User: How do I print 'Hello World' in Python?\n"
           "Assistant: In Python, you can print \"Hello World\" with this simple code:\n"
           "\n"
           "python\n"
           "print(\"Hello World\")\n"
           "\n"
           "This will output the text \"Hello World\" to the console when executed.\n"
           "\n"
           "<reasoning>\n"
           "The assistant did not use the todo list because this is a single, trivial task that can "
           "be completed in one step. There's no need to track multiple tasks or steps for such a "
           "straightforward request.\n"
           "</reasoning>\n"
           "</example>\n"
           "\n"
           "<example>\n"
           "User: What does the git status command do?\n"
           "Assistant: The git status command shows the current state of your working directory and "
           "staging area. It displays which changes have been staged, which haven't, and which "
           "files aren't being tracked by Git. This is a fundamental command for understanding the "
           "current state of your repository before making commits.\n"
           "\n"
           "<reasoning>\n"
           "The assistant did not use the todo list because this is an informational request with "
           "no actual coding task to complete. The user is simply asking for an explanation, not "
           "for the assistant to perform multiple steps or tasks.\n"
           "</reasoning>\n"
           "</example>\n"
           "\n"
           "<example>\n"
           "User: Can you add a comment to the calculateTotal function to explain what it does?\n"
           "Assistant: Sure, let me add a comment to the calculateTotal function to explain what it "
           "does.\n"
           "* Uses the Edit tool to add a comment to the calculateTotal function *\n"
           "\n"
           "<reasoning>\n"
           "The assistant did not use the todo list because this is a single, straightforward task "
           "confined to one location in the code. Adding a comment doesn't require tracking "
           "multiple steps or tasks.\n"
           "</reasoning>\n"
           "</example>\n"
           "\n"
           "<example>\n"
           "User: Run npm install for me and tell me what happens.\n"
           "Assistant: I'll run the npm install command for you.\n"
           "\n"
           "*Executes: npm install*\n"
           "\n"
           "The command completed successfully. Here's the output:\n"
           "[Output of npm install command]\n"
           "\n"
           "All dependencies have been installed according to your package.json file.\n"
           "\n"
           "<reasoning>\n"
           "The assistant did not use the todo list because this is a single command execution "
           "with immediate results. The user will know when the command completes and see the "
           "output directly.\n"
           "</reasoning>\n"
           "</example>\n"
           "\n"
           "## Task States and Management\n"
           "\n"
           "1. **Task States**: Use these states to track progress:\n"
           "   - pending: Task not yet started\n"
           "   - in_progress: Currently working on (limit to ONE task at a time)\n"
           "   - completed: Task finished successfully\n"
           "\n"
           "   **IMPORTANT**: Task descriptions must have two forms:\n"
           "   - content: The imperative form describing what needs to be done (e.g., \"Run tests\", "
           "\"Build the project\")\n"
           "   - activeForm: The present continuous form shown during execution (e.g., \"Running "
           "tests\", \"Building the project\")\n"
           "\n"
           "2. **Task Management**:\n"
           "   - Update task status in real-time as you work\n"
           "   - Mark tasks complete IMMEDIATELY after finishing (don't batch completions)\n"
           "   - Exactly ONE task must be in_progress at any time (not less, not more)\n"
           "   - Complete current tasks before starting new ones\n"
           "   - Remove tasks that are no longer relevant from the list entirely\n"
           "\n"
           "3. **Task Completion Requirements**:\n"
           "   - ONLY mark a task as completed when you have FULLY accomplished it\n"
           "   - If you encounter errors, blockers, or cannot finish, keep the task as in_progress\n"
           "   - When blocked, create a new task describing what needs to be resolved\n"
           "   - Never mark a task as completed if:\n"
           "     - Tests are failing\n"
           "     - Implementation is partial\n"
           "     - You encountered unresolved errors\n"
           "     - You couldn't find necessary files or dependencies\n"
           "\n"
           "4. **Task Breakdown**:\n"
           "   - Create specific, actionable items\n"
           "   - Break complex tasks into smaller, manageable steps\n"
           "   - Use clear, descriptive task names\n"
           "   - Always provide both forms:\n"
           "     - content: \"Fix authentication bug\"\n"
           "     - activeForm: \"Fixing authentication bug\"\n"
           "\n"
           "When in doubt, use this tool. Being proactive with task management demonstrates "
           "attentiveness and ensures you complete all requirements successfully.\n";
}

std::string exit_plan_mode_tool_prompt() {
    return "Use this tool when you are in plan mode and have finished writing your plan to the "
           "plan file and are ready for user approval.\n"
           "\n"
           "## How This Tool Works\n"
           "- You should have already written your plan to the plan file specified in the plan "
           "mode system message\n"
           "- This tool does NOT take the plan content as a parameter - it will read the plan from "
           "the file you wrote\n"
           "- This tool simply signals that you're done planning and ready for the user to review "
           "and approve\n"
           "- The user will see the contents of your plan file when they review it\n"
           "\n"
           "## When to Use This Tool\n"
           "IMPORTANT: Only use this tool when the task requires planning the implementation steps "
           "of a task that requires writing code. For research tasks where you're gathering "
           "information, searching files, reading files or in general trying to understand the "
           "codebase - do NOT use this tool.\n"
           "\n"
           "## Before Using This Tool\n"
           "Ensure your plan is complete and unambiguous:\n"
           "- If you have unresolved questions about requirements or approach, use AskUserQuestion "
           "first (in earlier phases)\n"
           "- Once your plan is finalized, use THIS tool to request approval\n"
           "\n"
           "**Important:** Do NOT use AskUserQuestion to ask \"Is this plan okay?\" or \"Should I "
           "proceed?\" - that's exactly what THIS tool does. ExitPlanMode inherently requests user "
           "approval of your plan.\n"
           "\n"
           "## Examples\n"
           "\n"
           "1. Initial task: \"Search for and understand the implementation of vim mode in the "
           "codebase\" - Do not use the exit plan mode tool because you are not planning the "
           "implementation steps of a task.\n"
           "2. Initial task: \"Help me implement yank mode for vim\" - Use the exit plan mode tool "
           "after you have finished planning the implementation steps of the task.\n"
           "3. Initial task: \"Add a new feature to handle user authentication\" - If unsure about "
           "auth method (OAuth, JWT, etc.), use AskUserQuestion first, then use exit plan mode tool "
           "after clarifying the approach.\n";
}

std::string ask_user_question_tool_prompt() {    return "Use this tool when you need to ask the user questions during execution. This allows "
           "you to:\n"
           "1. Gather user preferences or requirements\n"
           "2. Clarify ambiguous instructions\n"
           "3. Get decisions on implementation choices as you work\n"
           "4. Offer choices to the user about what direction to take.\n"
           "\n"
           "Usage notes:\n"
           "- Users will always be able to select \"Other\" to provide custom text input\n"
           "- Use multiSelect: true to allow multiple answers to be selected for a question\n"
           "- If you recommend a specific option, make that the first option in the list and add "
           "\"(Recommended)\" at the end of the label\n"
           "\n"
           "Plan mode note: In plan mode, use this tool to clarify requirements or choose between "
           "approaches BEFORE finalizing your plan. Do NOT use this tool to ask \"Is my plan "
           "ready?\" or \"Should I proceed?\" - use ExitPlanMode for plan approval. IMPORTANT: Do "
           "not reference \"the plan\" in your questions (e.g., \"Do you have feedback about the "
           "plan?\", \"Does the plan look good?\") because the user cannot see the plan in the UI "
           "until you call ExitPlanMode. If you need plan approval, use ExitPlanMode instead.";
}

std::string ask_user_question_preview_prompt() {
    return "\n"
           "Preview feature:\n"
           "Use the optional `preview` field on options when presenting concrete artifacts that "
           "users need to visually compare:\n"
           "- ASCII mockups of UI layouts or components\n"
           "- Code snippets showing different implementations\n"
           "- Diagram variations\n"
           "- Configuration examples\n"
           "\n"
           "Preview content is rendered as markdown in a monospace box. Multi-line text with "
           "newlines is supported. When any option has a preview, the UI switches to a side-by-side "
           "layout with a vertical option list on the left and preview on the right. Do not use "
           "previews for simple preference questions where labels and descriptions suffice. Note "
           "that previews are only supported for single-select questions (not multiSelect).\n";
}

} // namespace cc::tools
