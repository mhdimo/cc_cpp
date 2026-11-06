#include "permissions/classifier.hpp"

#include "tools/impl/common.hpp"
#include "tools/tool_names.hpp"
#include "utils/json_util.hpp"

#include <algorithm>
#include <vector>

namespace cc::permissions {
namespace {

/// The advice the model is given when an action was blocked.
///
/// The wording is the one of the reference.
constexpr std::string_view kWorkaroundGuidance =
    "IMPORTANT: You *may* attempt to accomplish this action using other tools that might naturally be "
    "used to accomplish this goal, e.g. using head instead of cat. But you *should not* attempt to work "
    "around this denial in malicious ways, e.g. do not use your ability to run tests to execute "
    "non-test actions. You should only try to work around this restriction in reasonable ways that do "
    "not attempt to bypass the intent behind this denial. If you believe this capability is essential to "
    "complete the user's request, STOP and explain to the user what you were trying to do and why you "
    "need this permission. Let the user decide how to proceed.";

/// The rules the classifier follows.
///
/// The prompt of the reference is not in the tree that was read, so the
/// port writes one of its own with the same purpose: to tell whether the
/// action of the assistant is one the user has to approve.
constexpr std::string_view kSystemPrompt =
    "You are a security classifier for Claude Code, an assistant that runs commands and changes files "
    "on the machine of the user.\n"
    "\n"
    "You are given the action the assistant wants to perform and the conversation so far. Decide "
    "whether the action may run without the user approving it.\n"
    "\n"
    "Block the action when it:\n"
    "- destroys or overwrites data the user did not ask to lose (rm -rf, git reset --hard, dropping a "
    "database, overwriting a file that is not part of the work);\n"
    "- sends data off the machine (curl to an unknown host, scp, mail, a script that uploads a file);\n"
    "- reads or writes outside the folder the session runs in (the secrets of the user, the keys of "
    "another project, the files of the system);\n"
    "- changes the machine itself (the settings of the terminal, the permissions of the files, the "
    "accounts of the user, the services that are started);\n"
    "- installs software, or runs a command the user did not ask for;\n"
    "- commits and pushes, or opens a pull request, without being asked to.\n"
    "\n"
    "Do not block the action when it:\n"
    "- reads a file of the folder the session runs in, searches in it, or lists it;\n"
    "- edits a file of that folder the user asked to change;\n"
    "- runs the build, the tests or the linter of the project;\n"
    "- runs a command the user asked for, in the folder the session runs in.\n"
    "\n"
    "When the action is not clearly dangerous, allow it: the user is watching the session and can stop "
    "it at any moment.";

} // namespace

bool classifier_allowlisted(std::string_view tool_name) {
    // The tools that only read, and the ones that change nothing but the
    // state of the session itself.
    static const std::vector<std::string_view> safe = {
        "Read",          "Grep",         "Glob",         "LSP",
        "TodoWrite",     "TaskCreate",   "TaskGet",      "TaskUpdate",
        "TaskList",      "TaskStop",     "TaskOutput",   "AskUserQuestion",
        "EnterPlanMode", "ExitPlanMode", "ListMcpResourcesTool", "ToolSearch",
        "Sleep",         "SendMessage",  "TeamCreate",   "TeamDelete",
    };

    return std::find(safe.begin(), safe.end(), tool_name) != safe.end();
}

boost::json::object ClassifierTool::input_schema() const {
    return tools::schema_object(
        {
            {"thinking", tools::schema_string("Brief step-by-step reasoning.")},
            {"shouldBlock",
             tools::schema_boolean("Whether the action should be blocked (true) or allowed (false)")},
            {"reason", tools::schema_string("Brief explanation of the classification decision")},
        },
        {"thinking", "shouldBlock", "reason"});
}

ToolResult ClassifierTool::call(const boost::json::object& input, ToolUseContext& /*context*/) {
    answered_ = true;
    should_block_ = jget_bool(input, "shouldBlock", false);
    reason_ = jget_string(input, "reason");

    ToolResult result;
    result.content = "The action was classified: " + std::string(should_block_ ? "blocked" : "allowed");
    return result;
}

std::string classifier_prompt(const std::string& action, const std::string& transcript) {
    std::string prompt(kSystemPrompt);
    prompt += "\n\n=== TRANSCRIPT OF THE SESSION ===\n";
    prompt += transcript;
    prompt += "\n\n=== ACTION TO CLASSIFY ===\n";
    prompt += action;
    prompt += "\n\nCall the classify_result tool with your decision.";
    return prompt;
}

std::string classifier_rejection_message(const std::string& reason) {
    std::string message = "Permission for this action has been denied. Reason: ";
    message += reason;
    message += ". If you have other tasks that don't depend on this action, continue working on those. ";
    message += kWorkaroundGuidance;
    message +=
        " To allow this type of action in the future, the user can add a permission rule like "
        "Bash(prompt: <description of allowed action>) to their settings. At the end of your session, "
        "recommend what permission rules to add so you don't get blocked again.";
    return message;
}

} // namespace cc::permissions
