#pragma once

#include <string_view>

namespace cc::tools {

// Built-in tool names. These are the wire names the model sees and the names
// that appear in permission rules.
inline constexpr std::string_view AGENT = "Agent";
inline constexpr std::string_view LEGACY_AGENT = "Task";
inline constexpr std::string_view ASK_USER_QUESTION = "AskUserQuestion";
inline constexpr std::string_view BASH = "Bash";
inline constexpr std::string_view CONFIG = "Config";
inline constexpr std::string_view ENTER_PLAN_MODE = "EnterPlanMode";
inline constexpr std::string_view ENTER_WORKTREE = "EnterWorktree";
inline constexpr std::string_view EXIT_PLAN_MODE = "ExitPlanMode";
inline constexpr std::string_view EXIT_WORKTREE = "ExitWorktree";
inline constexpr std::string_view EDIT = "Edit";
inline constexpr std::string_view READ = "Read";
inline constexpr std::string_view WRITE = "Write";
inline constexpr std::string_view GLOB = "Glob";
inline constexpr std::string_view GREP = "Grep";
inline constexpr std::string_view LIST_MCP_RESOURCES = "ListMcpResourcesTool";
inline constexpr std::string_view READ_MCP_RESOURCE = "ReadMcpResource";
inline constexpr std::string_view LSP = "LSP";
inline constexpr std::string_view MCP_AUTH = "MCPAuth";
inline constexpr std::string_view NOTEBOOK_EDIT = "NotebookEdit";
inline constexpr std::string_view POWERSHELL = "PowerShell";
inline constexpr std::string_view REPL = "REPL";
inline constexpr std::string_view CRON_CREATE = "CronCreate";
inline constexpr std::string_view CRON_DELETE = "CronDelete";
inline constexpr std::string_view CRON_LIST = "CronList";
inline constexpr std::string_view SEND_MESSAGE = "SendMessage";
inline constexpr std::string_view SKILL = "Skill";
inline constexpr std::string_view SLEEP = "Sleep";
inline constexpr std::string_view STRUCTURED_OUTPUT = "StructuredOutput";
inline constexpr std::string_view TASK_CREATE = "TaskCreate";
inline constexpr std::string_view TASK_GET = "TaskGet";
inline constexpr std::string_view TASK_LIST = "TaskList";
inline constexpr std::string_view TASK_OUTPUT = "TaskOutput";
inline constexpr std::string_view TASK_STOP = "TaskStop";
inline constexpr std::string_view TASK_UPDATE = "TaskUpdate";
inline constexpr std::string_view TEAM_CREATE = "TeamCreate";
inline constexpr std::string_view TEAM_DELETE = "TeamDelete";
inline constexpr std::string_view TODO_WRITE = "TodoWrite";
inline constexpr std::string_view TOOL_SEARCH = "ToolSearch";
inline constexpr std::string_view WEB_FETCH = "WebFetch";
inline constexpr std::string_view WEB_SEARCH = "WebSearch";
inline constexpr std::string_view BRIEF = "SendUserMessage";
inline constexpr std::string_view LEGACY_BRIEF = "Brief";

} // namespace cc::tools
