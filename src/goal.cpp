#include "goal.hpp"

#include "core/error.hpp"
#include "tools/impl/common.hpp"
#include "tools/tool_use_context.hpp"
#include "utils/json_util.hpp"
#include "utils/string.hpp"

namespace cc::goal {
namespace {

/// The rules the evaluator follows.
///
/// The prompt of the reference is not in the tree that was read, so the port
/// writes one of its own with the same purpose: to tell whether the goal of
/// the session has been met.
constexpr std::string_view kSystemPrompt =
    "You are the evaluator of the goals of Claude Code, an assistant that works on a project for "
    "the user.\n"
    "\n"
    "You are given the condition of the goal of the session and the conversation so far. Decide "
    "whether the condition has been met.\n"
    "\n"
    "The condition is met when the conversation shows that it holds: the work was done, the tests "
    "pass, the file has the contents it was asked for. Read the results of the tools of the "
    "assistant as well as its own words: a change that was asked for is not a change that was "
    "made.\n"
    "\n"
    "The condition is not met when the work is unfinished, when a step failed, or when the "
    "conversation does not say enough to tell. When in doubt, say that the condition is not met "
    "and say what is missing.";

/// True when the character is one of the ones that are not shown.
///
/// The reference takes them away before it looks at the condition of a goal:
/// the space, the tab, the line ends and the characters that take no room
/// on the screen.
bool is_invisible(uint32_t codepoint) {
    if (codepoint == ' ' || codepoint == '\t' || codepoint == '\n' || codepoint == '\r' ||
        codepoint == '\v' || codepoint == '\f') {
        return true;
    }
    // The space that takes no room, and the marks that are not shown.
    if (codepoint == 0x00A0 || codepoint == 0x200B || codepoint == 0x200C || codepoint == 0x200D ||
        codepoint == 0x200E || codepoint == 0x200F || codepoint == 0x2060 || codepoint == 0xFEFF) {
        return true;
    }
    return false;
}

} // namespace

bool check_condition(const std::string& condition, std::string* error) {
    if (error) error->clear();

    // The condition is looked at without the characters that are not shown:
    // a condition that holds nothing visible is no condition at all.
    std::string visible;
    for (const uint32_t codepoint : cc::utf8_codepoints(condition)) {
        if (is_invisible(codepoint)) continue;
        visible += cc::utf8_encode(codepoint);
    }

    if (visible.empty()) {
        if (error) {
            *error =
                "The goal condition is empty once whitespace and invisible characters are removed. "
                "Provide a visible condition.";
        }
        return false;
    }

    if (visible.size() > kMaxLength) {
        if (error) {
            *error = "Goal condition is limited to " + std::to_string(kMaxLength) +
                     " characters (got " + std::to_string(visible.size()) + ")";
        }
        return false;
    }

    return true;
}

boost::json::object EvaluatorTool::input_schema() const {
    return tools::schema_object(
        {
            {"thinking", tools::schema_string("Brief step-by-step reasoning.")},
            {"met", tools::schema_boolean("Whether the goal has been met (true) or not (false)")},
            {"reason", tools::schema_string("Brief explanation of the answer")},
        },
        {"thinking", "met", "reason"});
}

ToolResult EvaluatorTool::call(const boost::json::object& input, ToolUseContext& /*context*/) {
    answered_ = true;
    met_ = jget_bool(input, "met", false);
    reason_ = jget_string(input, "reason");

    ToolResult result;
    result.content = "The goal was checked: " + std::string(met_ ? "met" : "not met");
    return result;
}

boost::json::object ProposeGoalTool::input_schema() const {
    return tools::schema_object(
        {
            {"condition",
             tools::schema_string(
                 "The completion condition to propose, written so a separate evaluator can verify it "
                 "from the conversation (e.g. \"all tests in test/auth pass (bun test exits "
                 "0)\"). At most " +
                 std::to_string(kMaxLength) + " characters.")},
            {"ask_user",
             tools::schema_boolean(
                 "Whether the user was asked for approval (true) or the goal was set directly "
                 "(false)")},
        },
        {"condition", "ask_user"});
}

ToolResult ProposeGoalTool::call(const boost::json::object& input, ToolUseContext& context) {
    condition_ = jget_string(input, "condition");
    const bool ask_user = jget_bool(input, "ask_user", true);

    // The condition is looked at before it is proposed.
    std::string error;
    if (!check_condition(condition_, &error)) {
        throw ToolError(error, "ProposeGoal");
    }

    // A proposal that nobody can approve is refused.
    if (ask_user && !context.on_goal_proposal) {
        throw ToolError(
            "Goal proposals need an interactive session to render the approval prompt; none is "
            "available here.",
            "ProposeGoal");
    }

    // The proposal is shown to the user, who approves it with one keypress.
    approved_ = ask_user ? (context.on_goal_proposal && context.on_goal_proposal(condition_))
                         : true;

    ToolResult result;
    if (approved_) {
        result.content = "The goal was set: " + condition_;
    } else {
        result.content = "The goal was not approved; the session keeps working on what it was "
                         "asked to do.";
    }
    return result;
}

std::string evaluator_prompt(const std::string& condition, const std::string& transcript) {
    std::string prompt(kSystemPrompt);
    prompt += "\n\n=== TRANSCRIPT OF THE SESSION ===\n";
    prompt += transcript;
    prompt += "\n\n=== GOAL OF THE SESSION ===\n";
    prompt += condition;
    prompt += "\n\nCall the goal_result tool with your answer.";
    return prompt;
}

std::string goal_prompt(const std::string& condition) {
    // The wording is the one of the reference: the goal is a directive for
    // the model, and the model is asked to work towards it at once.
    return "A session-scoped Stop hook is now active with condition: \"" + condition +
           "\". Briefly acknowledge the goal, then immediately start (or continue) working toward "
           "it — treat the condition itself as your directive and do not pause to ask the user what "
           "to do. The hook will block stopping until the condition holds. It auto-clears once the "
           "condition is met — do not tell the user to run `/goal clear` after success; that's only "
           "for clearing a goal early.";
}

} // namespace cc::goal
