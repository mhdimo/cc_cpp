#include "query/system_prompt.hpp"

#include "utils/fs.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace cc::query {
namespace {

/// The paragraph that tells the model who it is.
constexpr std::string_view kIdentityParagraph =
    "You are Claude Code, Anthropic's official CLI for Claude. You are an interactive agent that "
    "helps users with software engineering tasks. Use the instructions below and the tools available "
    "to you to assist the user.";

/// The instruction used when the session runs in the default permission mode.
constexpr std::string_view kDefaultPermissionMode =
    "Tools are executed in a user-selected permission mode. When you attempt to call a tool that is "
    "not automatically allowed by the user's permission mode or permission settings, the user will be "
    "prompted so that they can approve or deny the execution. If the user denies a tool you call, do "
    "not re-attempt the exact same tool call. Instead, think about why the user has denied the tool "
    "call and adjust your approach.";

/// The instruction used when the session accepts edits without asking.
constexpr std::string_view kAcceptEditsPermissionMode =
    "Edits to files are accepted automatically and the user is not prompted for them. All other tools "
    "are executed in a user-selected permission mode. When you attempt to call a tool that is not "
    "automatically allowed by the user's permission mode or permission settings, the user will be "
    "prompted so that they can approve or deny the execution. If the user denies a tool you call, do "
    "not re-attempt the exact same tool call. Instead, think about why the user has denied the tool "
    "call and adjust your approach.";

/// The instruction used when the session runs in plan mode.
///
/// The instruction names the file the plan is written to, the way the
/// reference does; the file is the only one the model may change.
constexpr std::string_view kPlanPermissionMode =
    "Plan mode is active. The user indicated that they do not want you to execute yet -- you MUST NOT "
    "make any edits (with the exception of the plan file mentioned below), run any non-readonly tools "
    "(including changing configs or making commits), or otherwise make any changes to the system. This "
    "supersedes any other instructions you have received.";

/// The number of the agents that may explore the codebase in the plan mode.
constexpr int kPlanExploreAgentCount = 3;

/// The number of the agents that may design the plan in the plan mode.
constexpr int kPlanAgentCount = 1;

/// The instruction used when the session skips the permission prompts.
constexpr std::string_view kBypassPermissionsPermissionMode =
    "Permission prompts are disabled: tool calls are executed without asking the user to approve them "
    "first. The user chose this mode, so stay inside the scope of the request and be careful with "
    "actions that are hard to undo.";

/// The instruction used when the session cannot ask the user for permission.
constexpr std::string_view kDontAskPermissionMode =
    "The user cannot be asked for permission. A tool call that is not automatically allowed is denied "
    "instead of being put to the user, so plan your tool calls around the permission rules and explain "
    "what you need when a call is denied.";

/// The instruction used when a classifier decides the permission questions.
constexpr std::string_view kAutoPermissionMode =
    "Tool calls that would otherwise ask the user for permission are decided automatically instead of "
    "prompting. If a tool call is denied, do not re-attempt the exact same tool call. Instead, think "
    "about why it was denied and adjust your approach.";

/// The instruction that introduces the memory files.
constexpr std::string_view kMemoryInstruction =
    "Codebase and user instructions are shown below. Be sure to adhere to these instructions. "
    "IMPORTANT: These instructions OVERRIDE any default behavior and you MUST follow them exactly as "
    "written.";

/// The description of the user memory file.
constexpr std::string_view kGlobalMemoryDescription =
    " (user's private global instructions for all projects)";

/// The description of a `CLAUDE.local.md` file.
constexpr std::string_view kLocalMemoryDescription =
    " (user's private project instructions, not checked in)";

/// The description of a checked-in memory file.
constexpr std::string_view kProjectMemoryDescription =
    " (project instructions, checked into the codebase)";

/// The section that lists the tools the session has.
std::string tools_section(const std::vector<std::string>& tool_names) {
    if (tool_names.empty()) return "";

    std::string out = "# Tools\n\nThe following tools are available to you: ";
    for (size_t i = 0; i < tool_names.size(); ++i) {
        if (i > 0) out += ", ";
        out += tool_names[i];
    }
    out += ".";
    return out;
}

/// The section that states the date, the working directory and the model.
std::string environment_section(const SystemPromptOptions& options) {
    std::vector<std::string> items;
    items.push_back("Today's date is " + current_date() + ".");
    items.push_back("Primary working directory: " + cc::resolve(options.cwd));
    if (!options.model.empty()) {
        items.push_back("You are powered by the model " + options.model + ".");
    }

    std::string out = "# Environment\n\nYou have been invoked in the following environment:";
    for (const auto& item : items) out += "\n - " + item;
    return out;
}

/// The section that states how the session handles permissions.
std::string permission_mode_section(PermissionMode mode, const std::string& plan_file_path) {
    std::string_view instruction = kDefaultPermissionMode;
    switch (mode) {
    case PermissionMode::AcceptEdits:
        instruction = kAcceptEditsPermissionMode;
        break;
    case PermissionMode::Plan:
        instruction = kPlanPermissionMode;
        break;
    case PermissionMode::BypassPermissions:
        instruction = kBypassPermissionsPermissionMode;
        break;
    case PermissionMode::DontAsk:
        instruction = kDontAskPermissionMode;
        break;
    case PermissionMode::Auto:
        instruction = kAutoPermissionMode;
        break;
    case PermissionMode::Default:
    case PermissionMode::Bubble:
        break;
    }

    std::string out = "# Permission Mode\n\n" + std::string(instruction);

    // The plan mode names the file the plan is written to, and the way the
    // plan is worked out.
    if (mode != PermissionMode::Plan || plan_file_path.empty()) return out;

    const std::string plan_info =
        "A plan file already exists at " + plan_file_path +
        ". You can read it and make incremental edits using the Edit tool.";
    const std::string no_plan_info =
        "No plan file exists yet. You should create your plan at " + plan_file_path +
        " using the Write tool.";

    out += "\n\n## Plan File Info:\n";
    out += cc::file_exists(plan_file_path) ? plan_info : no_plan_info;
    out +=
        "\nYou should build your plan incrementally by writing to or editing this file. NOTE that this "
        "is the only file you are allowed to edit - other than this you are only allowed to take "
        "READ-ONLY actions."
        "\n"
        "\n"
        "## Plan Workflow"
        "\n"
        "\n"
        "### Phase 1: Initial Understanding\n"
        "Goal: Gain a comprehensive understanding of the user's request by reading through code and "
        "asking them questions. Critical: In this phase you should only use the Explore subagent "
        "type."
        "\n"
        "1. Focus on understanding the user's request and the code associated with their request. "
        "Actively search for existing functions, utilities, and patterns that can be reused — avoid "
        "proposing new code when suitable implementations already exist."
        "\n"
        "2. **Launch up to " +
        std::to_string(kPlanExploreAgentCount) +
        " Explore agents IN PARALLEL** (single message, multiple tool calls) to efficiently explore "
        "the codebase."
        "\n"
        "   - Use 1 agent when the task is isolated to known files, the user provided specific file "
        "paths, or you're making a small targeted change."
        "\n"
        "   - Use multiple agents when: the scope is uncertain, multiple areas of the codebase are "
        "involved, or you need to understand existing code before planning."
        "\n"
        "   - Quality over quantity - " +
        std::to_string(kPlanExploreAgentCount) +
        " agents maximum, but you should try to use the minimum number of agents necessary (usually "
        "just 1)"
        "\n"
        "   - If using multiple agents: Provide each agent with a specific search focus or area to "
        "explore. Example: One agent searches for existing implementations, another explores related "
        "components, a third investigating test patterns"
        "\n"
        "\n"
        "### Phase 2: Design\n"
        "Goal: Design an implementation approach."
        "\n"
        "Launch " +
        std::to_string(kPlanAgentCount) +
        " Plan agent(s) to design the implementation based on the user's intent and your exploration "
        "results from Phase 1."
        "\n"
        "Guidelines:"
        "\n"
        "- **Default**: Launch at least 1 Plan agent for most tasks - it helps validate your "
        "understanding and consider alternatives"
        "\n"
        "- **Skip agents**: Only for truly trivial tasks (typo fixes, single-line changes, simple "
        "renames)"
        "\n"
        "In the agent prompt:"
        "\n"
        "- Provide comprehensive background context from Phase 1 exploration including filenames and "
        "code path traces"
        "\n"
        "- Describe requirements and constraints"
        "\n"
        "- Request a detailed implementation plan"
        "\n"
        "\n"
        "### Phase 3: Review\n"
        "Goal: Review the plan(s) from Phase 2 and ensure alignment with the user's intentions."
        "\n"
        "1. Read the critical files identified by agents to deepen your understanding"
        "\n"
        "2. Ensure that the plans align with the user's original request"
        "\n"
        "3. Use AskUserQuestion to clarify any remaining questions with the user"
        "\n"
        "\n"
        "### Phase 4: Final Plan\n"
        "Goal: Write your final plan to the plan file (the only file you can edit)."
        "\n"
        "- Ensure that the plan file is concise enough to scan quickly, but detailed enough to execute "
        "effectively"
        "\n"
        "- Structure it with clear sections"
        "\n"
        "\n"
        "### Phase 5: Call ExitPlanMode\n"
        "At the very end of your turn, once you have asked the user questions and are happy with your "
        "final plan file - you should always call ExitPlanMode to indicate to the user that you are "
        "done planning."
        "This is critical - your turn should only end with either using the AskUserQuestion tool OR "
        "calling the ExitPlanMode tool. Do not stop unless it's for these 2 reasons"
        "\n"
        "\n"
        "**Important:** Use AskUserQuestion ONLY to clarify requirements or choose between "
        "approaches. Use ExitPlanMode to request plan approval. Do NOT ask about plan approval in "
        "any other way - no text questions, no AskUserQuestion. Phrases like \"Is this plan okay?\", "
        "\"Should I proceed?\", \"How does this plan look?\", \"Any changes before we start?\", or "
        "similar MUST use ExitPlanMode."
        "\n"
        "NOTE: At any point in time through this workflow you should feel free to ask the user "
        "questions or clarifications using the AskUserQuestion tool. Don't make large assumptions "
        "about user intent. The goal is to present a well researched plan to the user, and tie any "
        "loose ends before implementation begins.";

    return out;
}

/// The description of a memory file, as the reference labels it.
std::string_view memory_description(const config::MemoryFile& file) {
    if (file.is_global) return kGlobalMemoryDescription;
    if (cc::basename(file.path) == "CLAUDE.local.md") return kLocalMemoryDescription;
    return kProjectMemoryDescription;
}

/// The section that holds the memory files.
std::string claude_md_section(const std::vector<config::MemoryFile>& memory_files) {
    if (memory_files.empty()) return "";

    std::string out = "# claudeMd\n\n";
    out += kMemoryInstruction;
    for (const auto& file : memory_files) {
        out += "\n\nContents of " + file.path + std::string(memory_description(file)) + ":\n\n";
        out += cc::trim(file.contents);
    }
    return out;
}

/// The section that applies the output style.
std::string output_style_section(const std::string& style) {
    const size_t newline = style.find('\n');
    const std::string name = newline == std::string::npos ? style : style.substr(0, newline);
    const std::string body =
        newline == std::string::npos ? std::string() : cc::trim(style.substr(newline));

    std::string out = "# Output Style: " + name;
    if (!body.empty()) out += "\n\n" + body;
    return out;
}

} // namespace

std::string build_system_prompt(const SystemPromptOptions& options) {
    // A custom prompt replaces everything else.
    if (options.custom_prompt) return *options.custom_prompt;

    std::vector<std::string> sections;
    sections.push_back(std::string(kIdentityParagraph));

    if (const std::string tools = tools_section(options.tool_names); !tools.empty()) {
        sections.push_back(tools);
    }

    sections.push_back(environment_section(options));
    sections.push_back(permission_mode_section(options.permission_mode, options.plan_file_path));

    if (const std::string memory = claude_md_section(options.memory_files); !memory.empty()) {
        sections.push_back(memory);
    }

    if (options.output_style) {
        sections.push_back(output_style_section(*options.output_style));
    }

    std::string prompt = cc::join(sections, "\n\n");

    if (options.append_prompt) {
        prompt += "\n\n" + *options.append_prompt;
    }

    return prompt;
}

std::string current_date() {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif

    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday);
    return std::string(buffer);
}

} // namespace cc::query
