#include "tools/impl/agent_tool.hpp"

#include "core/error.hpp"
#include "core/ids.hpp"
#include "tools/impl/common.hpp"
#include "tools/tool_names.hpp"
#include "tools/tool_use_context.hpp"
#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/utsname.h>
#endif

namespace cc::tools {
namespace {

/// The model aliases the tool accepts.
constexpr std::string_view kModelAliases[] = {"sonnet", "opus", "haiku"};

/// The isolation modes the tool accepts.
constexpr std::string_view kIsolationModes[] = {"worktree"};

/// The notes the reference appends to every sub-agent's system prompt.
constexpr std::string_view kAgentNotes =
    "Notes:\n"
    "- Agent threads always have their cwd reset between bash calls, as a result please only use "
    "absolute file paths.\n"
    "- In your final response, share file paths (always absolute, never relative) that are relevant "
    "to the task. Include code snippets only when the exact text is load-bearing (e.g., a bug you "
    "found, a function signature the caller asked for) — do not recap code you merely read.\n"
    "- For clear communication with the user the assistant MUST avoid using emojis.\n"
    "- Do not use a colon before tool calls. Text like \"Let me read the file:\" followed by a read "
    "tool call should just be \"Let me read the file.\" with a period.";

/// The text the tool puts in place of an agent that returned nothing.
constexpr std::string_view kNoOutputMarker = "(Subagent completed but returned no output.)";

/// The name of the platform the CLI runs on.
std::string platform_name() {
#if defined(_WIN32)
    return "win32";
#elif defined(__APPLE__)
    return "darwin";
#else
    return "linux";
#endif
}

/// The operating system version, in the form `uname -sr` prints it.
std::string os_version() {
#ifndef _WIN32
    struct utsname info {};
    if (uname(&info) != 0) return "";
    return std::string(info.sysname) + " " + info.release;
#else
    return "";
#endif
}

/// True when `alias` is one of the values in `choices`.
bool is_one_of(std::string_view value, const std::string_view* choices, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (value == choices[i]) return true;
    }
    return false;
}

/// The names of the agents the model may spawn.
std::string available_agents(const std::vector<agents::Definition>& definitions) {
    std::vector<std::string> names;
    names.reserve(definitions.size());
    for (const agents::Definition& definition : definitions) {
        names.push_back(definition.name);
    }
    return cc::join(names, ", ");
}

/// The tools an agent has access to, as the prompt lists them.
std::string tools_description(const agents::Definition& definition) {
    if (definition.tools.empty()) return "All tools";
    return cc::join(definition.tools, ", ");
}

/// The line the prompt lists an agent with.
std::string format_agent_line(const agents::Definition& definition) {
    return "- " + definition.name + ": " + definition.description + " (Tools: " +
           tools_description(definition) + ")";
}

/// True when a bare family alias names the same tier as the parent's model.
///
/// The sub-agent then keeps the parent's exact model, so that asking for
/// `sonnet` never silently downgrades an Opus session.
bool matches_parent_tier(const std::string& alias, const std::string& parent_model) {
    if (parent_model.empty()) return false;

    const std::string name = cc::to_lower(alias);
    if (name != "opus" && name != "sonnet" && name != "haiku") return false;

    return cc::contains(cc::to_lower(parent_model), name);
}

/// The model the agent runs on.
///
/// The model's own choice wins over the agent definition's frontmatter, and
/// the parent's model wins over both when neither names one.
std::string resolve_model(const agents::Definition& definition,
                          const std::optional<std::string>& requested,
                          const std::string& parent_model) {
    std::string spec;
    if (requested && !requested->empty()) {
        spec = *requested;
    } else if (!definition.model.empty()) {
        spec = definition.model;
    }

    if (spec.empty() || cc::to_lower(spec) == "inherit") return parent_model;
    if (matches_parent_tier(spec, parent_model)) return parent_model;
    return spec;
}

/// The section that tells the agent where it is running.
std::string environment_section(const std::string& cwd, const std::string& model) {
    std::string out = "Here is useful information about the environment you are running in:\n";
    out += "<env>\n";
    out += "Working directory: " + cwd + "\n";
    out += std::string("Is directory a git repo: ") +
           (cc::is_directory(cc::path_join(cwd, ".git")) ? "Yes" : "No") + "\n";
    out += "Platform: " + platform_name() + "\n";
    const std::string version = os_version();
    if (!version.empty()) out += "OS Version: " + version + "\n";
    out += "</env>\n";
    if (!model.empty()) out += "You are powered by the model " + model + ".";
    return out;
}

/// The system prompt the sub-agent runs with.
///
/// It is the agent definition's own instructions, followed by the notes the
/// reference gives every agent and the environment the parent session runs
/// in.
std::string build_system_prompt(const agents::Definition& definition, const std::string& model,
                                const ToolUseContext& context) {
    const std::string cwd = cc::resolve(context.options.cwd.empty() ? cc::cwd() : context.options.cwd);

    std::vector<std::string> sections;
    sections.push_back(cc::trim(definition.prompt));
    sections.push_back(std::string(kAgentNotes));
    sections.push_back(environment_section(cwd, model));
    return cc::join(sections, "\n\n");
}

/// The result the tool hands back to the model.
///
/// The agent's own text comes first, then the trailer that names the agent
/// and reports what the run cost.
std::string format_result(const std::string& text, const std::string& agent_id, int64_t total_tokens,
                          int64_t tool_uses, int64_t duration_ms) {
    const std::string body = text.empty() ? std::string(kNoOutputMarker) : text;

    std::string trailer = "agentId: " + agent_id + " (use SendMessage with to: '" + agent_id +
                          "' to continue this agent)\n";
    trailer += "<usage>total_tokens: " + std::to_string(total_tokens) + "\n";
    trailer += "tool_uses: " + std::to_string(tool_uses) + "\n";
    trailer += "duration_ms: " + std::to_string(duration_ms) + "</usage>";

    return body + "\n\n" + trailer;
}

} // namespace

AgentTool::AgentTool(std::vector<agents::Definition> definitions, Runner runner)
    : definitions_(std::move(definitions)), runner_(std::move(runner)) {}

AgentTool::AgentTool(std::vector<agents::Definition> definitions, DetailedRunner runner)
    : definitions_(std::move(definitions)), detailed_runner_(std::move(runner)) {}

boost::json::object AgentTool::input_schema() const {
    return schema_object(
        {
            {"description", schema_string("A short (3-5 word) description of the task")},
            {"prompt", schema_string("The task for the agent to perform")},
            {"subagent_type",
             schema_string("The type of specialized agent to use for this task")},
            {"model",
             schema_enum("Optional model override for this agent. Takes precedence over the agent "
                         "definition's model frontmatter. If omitted, uses the agent definition's "
                         "model, or inherits from the parent.",
                         {"sonnet", "opus", "haiku"})},
            {"run_in_background",
             schema_boolean("Set to true to run this agent in the background. You will be notified "
                            "when it completes.")},
            {"name",
             schema_string("Name for the spawned agent. Makes it addressable via "
                           "SendMessage({to: name}) while running.")},
            {"isolation",
             schema_enum("Isolation mode. \"worktree\" creates a temporary git worktree so the agent "
                         "works on an isolated copy of the repo.",
                         {"worktree"})},
        },
        {"description", "prompt"},
        /*closed=*/false);
}

ToolResult AgentTool::call(const boost::json::object& input, ToolUseContext& context) {
    SpawnOptions options;
    options.description = jget_string(input, "description");
    options.name = jget_string(input, "name");
    options.run_in_background = jget_bool(input, "run_in_background");
    options.isolation = jget_string(input, "isolation");

    const std::string task = jget_string(input, "prompt");
    const std::optional<std::string> subagent_type = jget_optional_string(input, "subagent_type");
    const std::optional<std::string> requested_model = jget_optional_string(input, "model");

    if (requested_model && !is_one_of(*requested_model, kModelAliases,
                                      sizeof(kModelAliases) / sizeof(kModelAliases[0]))) {
        throw ToolError("Invalid model '" + *requested_model +
                             "'. Expected one of: sonnet, opus, haiku.",
                         "Agent");
    }
    if (!options.isolation.empty() &&
        !is_one_of(options.isolation, kIsolationModes,
                   sizeof(kIsolationModes) / sizeof(kIsolationModes[0]))) {
        throw ToolError("Invalid isolation '" + options.isolation +
                             "'. Expected one of: worktree.",
                         "Agent");
    }

    // The named agent is used when the model names one, and the
    // general-purpose agent otherwise.
    const std::string wanted = (subagent_type && !subagent_type->empty())
                                   ? *subagent_type
                                   : std::string(kGeneralPurposeAgentType);

    const std::optional<agents::Definition> definition = agents::find(definitions_, wanted);
    if (!definition) {
        throw ToolError("Agent type '" + wanted + "' not found. Available agents: " +
                             available_agents(definitions_),
                         "Agent");
    }

    const std::string model = resolve_model(*definition, requested_model, context.options.main_loop_model);
    const std::string system_prompt = build_system_prompt(*definition, model, context);

    options.agent_id = cc::uuid_v4();

    const auto started = std::chrono::steady_clock::now();
    RunResult outcome;
    if (detailed_runner_) {
        outcome = detailed_runner_(options, system_prompt, task, definition->tools, model);
    } else if (runner_) {
        outcome.text = runner_(system_prompt, task, definition->tools, model);
    } else {
        throw ToolError("The Agent tool has no runner to launch the agent with.", "Agent");
    }
    const auto finished = std::chrono::steady_clock::now();

    if (outcome.duration_ms == 0) {
        outcome.duration_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count();
    }

    ToolResult result;
    result.content = format_result(outcome.text, options.agent_id, outcome.total_tokens,
                                    outcome.tool_uses, outcome.duration_ms);

    boost::json::array content;
    content.emplace_back(outcome.text.empty() ? std::string(kNoOutputMarker) : outcome.text);

    boost::json::object payload;
    payload["agentId"] = options.agent_id;
    payload["agentType"] = definition->name;
    payload["content"] = std::move(content);
    payload["totalToolUseCount"] = outcome.tool_uses;
    payload["totalDurationMs"] = outcome.duration_ms;
    payload["totalTokens"] = outcome.total_tokens;

    result.structured_content = boost::json::value(payload);
    result.tool_use_result = boost::json::value(std::move(payload));

    return result;
}

std::string AgentTool::prompt() const { return prompt_for(definitions_); }

std::string AgentTool::prompt_for(const std::vector<agents::Definition>& definitions) {
    const std::string agent_tool = std::string(AGENT);
    const std::string read_tool = std::string(READ);
    const std::string glob_tool = std::string(GLOB);
    const std::string write_tool = std::string(WRITE);
    const std::string send_message_tool = std::string(SEND_MESSAGE);

    std::string out;
    out += "Launch a new agent to handle complex, multi-step tasks autonomously.\n\n";
    out += "The " + agent_tool +
           " tool launches specialized agents (subprocesses) that autonomously handle complex tasks. "
           "Each agent type has specific capabilities and tools available to it.\n\n";
    out += "Available agent types and the tools they have access to:\n";
    for (const agents::Definition& definition : definitions) {
        out += format_agent_line(definition) + "\n";
    }
    out += "\n";
    out += "When using the " + agent_tool +
           " tool, specify a subagent_type parameter to select which agent type to use. If omitted, "
           "the general-purpose agent is used.\n";

    out += "\nWhen NOT to use the " + agent_tool + " tool:\n";
    out += "- If you want to read a specific file path, use the " + read_tool + " tool or the " +
           glob_tool + " tool instead of the " + agent_tool +
           " tool, to find the match more quickly\n";
    out += "- If you are searching for a specific class definition like \"class Foo\", use the " +
           glob_tool + " tool instead, to find the match more quickly\n";
    out += "- If you are searching for code within a specific file or set of 2-3 files, use the " +
           read_tool + " tool instead of the " + agent_tool +
           " tool, to find the match more quickly\n";
    out += "- Other tasks that are not related to the agent descriptions above\n";

    out += "\nUsage notes:\n";
    out += "- Always include a short description (3-5 words) summarizing what the agent will do\n";
    out += "- Launch multiple agents concurrently whenever possible, to maximize performance; to do "
           "that, use a single message with multiple tool uses\n";
    out += "- When the agent is done, it will return a single message back to you. The result "
           "returned by the agent is not visible to the user. To show the user the result, you "
           "should send a text message back to the user with a concise summary of the result.\n";
    out += "- You can optionally run agents in the background using the run_in_background parameter. "
           "When an agent runs in the background, you will be automatically notified when it "
           "completes — do NOT sleep, poll, or proactively check on its progress. Continue with other "
           "work or respond to the user instead.\n";
    out += "- **Foreground vs background**: Use foreground (default) when you need the agent's results "
           "before you can proceed — e.g., research agents whose findings inform your next steps. "
           "Use background when you have genuinely independent work to do in parallel.\n";
    out += "- To continue a previously spawned agent, use " + send_message_tool +
           " with the agent's ID or name as the `to` field. The agent resumes with its full context "
           "preserved. Each " + agent_tool + " invocation starts fresh — provide a complete task "
           "description.\n";
    out += "- The agent's outputs should generally be trusted\n";
    out += "- Clearly tell the agent whether you expect it to write code or just to do research "
           "(search, file reads, web fetches, etc.), since it is not aware of the user's intent\n";
    out += "- If the agent description mentions that it should be used proactively, then you should "
           "try your best to use it without the user having to ask for it first. Use your "
           "judgement.\n";
    out += "- If the user specifies that they want you to run agents \"in parallel\", you MUST send a "
           "single message with multiple " + agent_tool +
           " tool use content blocks. For example, if you need to launch both a build-validator "
           "agent and a test-runner agent in parallel, send a single message with both tool calls.\n";
    out += "- You can optionally set `isolation: \"worktree\"` to run the agent in a temporary git "
           "worktree, giving it an isolated copy of the repository. The worktree is automatically "
           "cleaned up if the agent makes no changes; if changes are made, the worktree path and "
           "branch are returned in the result.\n";

    out += "\n";
    out += "## Writing the prompt\n";
    out += "\n";
    out += "Brief the agent like a smart colleague who just walked into the room — it hasn't "
           "seen this conversation, doesn't know what you've tried, doesn't understand why this "
           "task matters.\n";
    out += "- Explain what you're trying to accomplish and why.\n";
    out += "- Describe what you've already learned or ruled out.\n";
    out += "- Give enough context about the surrounding problem that the agent can make judgment "
           "calls rather than just following a narrow instruction.\n";
    out += "- If you need a short response, say so (\"report in under 200 words\").\n";
    out += "- Lookups: hand over the exact command. Investigations: hand over the question — "
           "prescribed steps become dead weight when the premise is wrong.\n";
    out += "\n";
    out += "Terse command-style prompts produce shallow, generic work.\n";
    out += "\n";
    out += "**Never delegate understanding.** Don't write \"based on your findings, fix the bug\" "
           "or \"based on the research, implement it.\" Those phrases push synthesis onto the "
           "agent instead of doing it yourself. Write prompts that prove you understood: include "
           "file paths, line numbers, what specifically to change.\n";

    out += "\nExample usage:\n\n";
    out += "<example_agent_descriptions>\n";
    out += "\"test-runner\": use this agent after you are done writing code to run tests\n";
    out += "\"greeting-responder\": use this agent to respond to user greetings with a friendly joke\n";
    out += "</example_agent_descriptions>\n\n";
    out += "<example>\n";
    out += "user: \"Please write a function that checks if a number is prime\"\n";
    out += "assistant: I'm going to use the " + write_tool + " tool to write the following code:\n";
    out += "<code>\n";
    out += "function isPrime(n) {\n";
    out += "  if (n <= 1) return false;\n";
    out += "  for (let i = 2; i * i <= n; i++) {\n";
    out += "    if (n % i === 0) return false;\n";
    out += "  }\n";
    out += "  return true;\n";
    out += "}\n";
    out += "</code>\n";
    out += "<commentary>\n";
    out += "Since a significant piece of code was written and the task was completed, now use the "
           "test-runner agent to run the tests\n";
    out += "</commentary>\n";
    out += "assistant: Uses the " + agent_tool + " tool to launch the test-runner agent\n";
    out += "</example>\n\n";
    out += "<example>\n";
    out += "user: \"Hello\"\n";
    out += "<commentary>\n";
    out += "Since the user is greeting, use the greeting-responder agent to respond with a friendly "
           "joke\n";
    out += "</commentary>\n";
    out += "assistant: \"I'm going to use the " + agent_tool +
           " tool to launch the greeting-responder agent\"\n";
    out += "</example>\n";

    return out;
}

} // namespace cc::tools
