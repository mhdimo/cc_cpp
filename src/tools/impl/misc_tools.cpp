#include "tools/impl/misc_tools.hpp"

#include "session/plan.hpp"
#include "tools/tool_use_context.hpp"
#include "utils/json_util.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace cc::tools {
namespace {

/// The message Claude Code shows when a todo list was updated.
constexpr std::string_view kTodoListUpdated =
    "Todos have been modified successfully. Ensure that you continue to use the todo list to "
    "track your progress. Please proceed with the current tasks if applicable";

/// The message Claude Code shows when a plan was approved.
constexpr std::string_view kPlanApproved =
    "User has approved your plan. You can now start coding. Start with updating your todo list if "
    "applicable";

/// The answers the user gave, keyed by the text of the question.
///
/// The reference collects the answers through the permission layer and
/// passes them back in the tool's input, so an input that already carries
/// them is used as it is.
UserAnswers read_answers(const boost::json::object& input) {
    UserAnswers answers;
    const boost::json::object raw = jget_object(input, "answers");
    for (const auto& [question_text, answer] : raw) {
        if (answer.is_string()) {
            answers.emplace(question_text, std::string(answer.as_string()));
        } else {
            answers.emplace(question_text, json_dump(answer));
        }
    }
    return answers;
}

} // namespace

// ---------------------------------------------------------------------------
// Todo
// ---------------------------------------------------------------------------

boost::json::value Todo::to_json() const {
    boost::json::object item;
    item["content"] = content;
    item["status"] = status;
    item["activeForm"] = active_form;
    return boost::json::value(std::move(item));
}

boost::json::object TodoWriteTool::input_schema() const {
    const boost::json::object item = schema_object(
        {
            {"content", schema_string("The imperative form of the task, e.g. \"Run the tests\"")},
            {"status",
             schema_enum("The state of the task: \"pending\", \"in_progress\" or \"completed\".",
                         {"pending", "in_progress", "completed"})},
            {"activeForm",
             schema_string("The present continuous form of the task, e.g. \"Running the tests\"")},
        },
        {"content", "status", "activeForm"});

    return schema_object({{"todos", schema_array("The updated todo list", item)}}, {"todos"});
}

std::string TodoWriteTool::prompt() const { return todo_write_tool_prompt(); }

ToolResult TodoWriteTool::call(const boost::json::object& input, ToolUseContext& context) {
    std::vector<Todo> todos;
    for (const auto& entry : jget_array(input, "todos")) {
        if (!entry.is_object()) continue;
        const boost::json::object& item = entry.as_object();

        Todo todo;
        todo.content = jget_string(item, "content");
        todo.status = jget_string(item, "status", "pending");
        todo.active_form = jget_string(item, "activeForm");
        todos.push_back(std::move(todo));
    }

    todos_ = std::move(todos);

    ToolResult result;
    result.content = std::string(kTodoListUpdated);
    return result;
}

// ---------------------------------------------------------------------------
// ExitPlanMode
// ---------------------------------------------------------------------------

boost::json::object ExitPlanModeTool::input_schema() const {
    const boost::json::object allowed_prompt = schema_object(
        {
            {"tool", schema_enum("The tool this prompt applies to.", {"Bash"})},
            {"prompt", schema_string("Semantic description of the action, e.g. \"run tests\", "
                                     "\"install dependencies\"")},
        },
        {"tool", "prompt"});

    // The reference takes the fields that the SDK adds to the tool on the
    // way, so the object of the tool is open.
    return schema_object(
        {
            {"allowedPrompts",
             schema_array("Prompt-based permissions needed to implement the plan. These describe "
                          "categories of actions rather than specific commands.",
                          allowed_prompt)},
        },
        {},
        /*closed=*/false);
}

std::string ExitPlanModeTool::prompt() const { return exit_plan_mode_tool_prompt(); }

ToolResult ExitPlanModeTool::call(const boost::json::object& input, ToolUseContext& context) {
    ToolResult result;

    if (!context.on_user_interaction) {
        result.is_error = true;
        result.content =
            "The plan could not be approved because user prompts are unavailable: there is no way "
            "to ask the user for approval.";
        return result;
    }

    // The plan of the session is shown to the user, the way the reference
    // shows it before it asks for the approval.
    std::string plan;
    if (!context.options.session_id.empty()) {
        if (const auto contents = session::read_plan(context.options.session_id)) {
            plan = *contents;
        }
    }
    if (cc::trim(plan).empty()) {
        plan = "No plan found. Please write your plan to the plan file first.";
    }

    // The user approves the plan, or does not.
    UserQuestion question;
    question.text = "Claude wants to exit plan mode\n\n" + plan;
    question.options.push_back(UserQuestionOption{"Yes", "", {}});
    question.options.push_back(UserQuestionOption{"No", "", {}});

    const UserAnswers answers = context.on_user_interaction({question});
    const auto answer = answers.find(question.text);
    if (answer == answers.end() || answer->second != "Yes") {
        result.is_error = true;
        result.content = "User declined to approve the plan.";
        return result;
    }

    result.content = std::string(kPlanApproved);

    // The plan and the file it is in are reported, so that the user sees
    // what was approved and where it was written.
    boost::json::object payload;
    payload["plan"] = plan;
    if (!context.options.session_id.empty()) {
        payload["filePath"] = session::plan_path(context.options.session_id);
    }
    result.tool_use_result = boost::json::value(std::move(payload));

    return result;
}

// ---------------------------------------------------------------------------
// AskUserQuestion
// ---------------------------------------------------------------------------

boost::json::object AskUserQuestionTool::input_schema() const {
    const boost::json::object option = schema_object(
        {
            {"label", schema_string("The display text for this option that the user will see and "
                                    "select. Should be concise (1-5 words) and clearly describe "
                                    "the choice.")},
            {"description", schema_string("Explanation of what this option means or what will "
                                          "happen if chosen. Useful for providing context about "
                                          "trade-offs or implications.")},
            {"preview", schema_string("Optional preview content rendered when this option is "
                                      "focused. Use for mockups, code snippets, or visual "
                                      "comparisons that help users compare options. See the tool "
                                      "description for the expected content format.")},
        },
        {"label", "description"},
        /*closed=*/false);

    boost::json::object options =
        schema_array("The available choices for this question. Must have 2-4 options. Each option "
                     "should be a distinct, mutually exclusive choice (unless multiSelect is "
                     "enabled). There should be no 'Other' option, that will be provided "
                     "automatically.",
                     option);
    options["minItems"] = 2;
    options["maxItems"] = 4;

    const boost::json::object question = schema_object(
        {
            {"question", schema_string("The complete question to ask the user. Should be clear, "
                                       "specific, and end with a question mark. Example: \"Which "
                                       "library should we use for date formatting?\" If "
                                       "multiSelect is true, phrase it accordingly, e.g. "
                                       "\"Which features do you want to enable?\"")},
            {"header", schema_string("Very short label displayed as a chip/tag (max 12 chars). "
                                     "Examples: \"Auth method\", \"Library\", \"Approach\".")},
            {"options", options},
            {"multiSelect", schema_boolean("Set to true to allow the user to select multiple "
                                           "options instead of just one. Use when choices are not "
                                           "mutually exclusive.")},
        },
        {"question", "header", "options"},
        /*closed=*/false);

    boost::json::object questions =
        schema_array("Questions to ask the user (1-4 questions)", question);
    questions["minItems"] = 1;
    questions["maxItems"] = 4;

    // The answers the user gave, keyed by the text of the question. The
    // reference collects them through the permission layer and passes them
    // back in the tool's input.
    boost::json::object answers;
    answers["type"] = "object";
    answers["description"] = "User answers collected by the permission component";

    // The annotations the user left on the answers of the questions, keyed
    // by the text of the question.
    boost::json::object annotation;
    annotation["type"] = "object";
    annotation["description"] =
        "Optional per-question annotations from the user (e.g., notes on preview selections). "
        "Keyed by question text.";
    annotation["additionalProperties"] = boost::json::object{
        {"type", "object"},
        {"properties",
         boost::json::object{
             {"preview",
              boost::json::object{{"type", "string"},
                                   {"description", "The preview content of the selected option, if "
                                                    "the question used previews."}}},
             {"notes",
              boost::json::object{{"type", "string"},
                                   {"description", "Free-text notes the user added to their "
                                                    "selection."}}},
         }},
    };

    // The metadata the tool carries for the sake of the analytics.
    boost::json::object metadata;
    metadata["type"] = "object";
    metadata["description"] =
        "Optional metadata for tracking and analytics purposes. Not displayed to user.";
    metadata["properties"] = boost::json::object{
        {"source",
         boost::json::object{{"type", "string"},
                              {"description", "Optional identifier for the source of this question "
                                               "(e.g., \"remember\" for /remember command). Used "
                                               "for analytics tracking."}}},
    };

    return schema_object({{"questions", questions},
                           {"answers", answers},
                           {"annotations", annotation},
                           {"metadata", metadata}},
                          {"questions"});
}

std::string AskUserQuestionTool::prompt() const {
    // The reference appends the guidance of the previews when the format of
    // the previews is known; the port shows them as markdown.
    return ask_user_question_tool_prompt() + ask_user_question_preview_prompt();
}

std::vector<UserQuestion> user_questions(const boost::json::object& input) {
    std::vector<UserQuestion> questions;
    for (const auto& entry : jget_array(input, "questions")) {
        if (!entry.is_object()) continue;
        const boost::json::object& item = entry.as_object();

        UserQuestion question;
        question.text = jget_string(item, "question");
        question.header = jget_string(item, "header");
        question.multi_select = jget_bool(item, "multiSelect", false);
        for (const auto& option_entry : jget_array(item, "options")) {
            if (!option_entry.is_object()) continue;
            const boost::json::object& option_object = option_entry.as_object();

            UserQuestionOption option;
            option.label = jget_string(option_object, "label");
            option.description = jget_string(option_object, "description");
            option.preview = jget_string(option_object, "preview");
            question.options.push_back(std::move(option));
        }
        questions.push_back(std::move(question));
    }
    return questions;
}

ToolResult AskUserQuestionTool::call(const boost::json::object& input, ToolUseContext& context) {
    const std::vector<UserQuestion> questions = user_questions(input);

    UserAnswers answers = read_answers(input);
    if (answers.empty() && context.on_user_interaction) {
        answers = context.on_user_interaction(questions);
    }

    if (answers.empty()) {
        // The user declined to answer.
        ToolResult result;
        result.is_error = true;
        result.content = "User declined to answer questions";
        return result;
    }

    // The answers are listed in the order the questions were asked.
    std::vector<std::pair<std::string, std::string>> ordered;
    for (const auto& question : questions) {
        for (const auto& entry : answers) {
            if (entry.first == question.text) {
                ordered.push_back(entry);
                break;
            }
        }
    }
    for (const auto& entry : answers) {
        const bool known = std::any_of(ordered.begin(), ordered.end(), [&](const auto& item) {
            return item.first == entry.first;
        });
        if (!known) ordered.push_back(entry);
    }

    std::vector<std::string> parts;
    parts.reserve(ordered.size());
    for (const auto& [question_text, answer] : ordered) {
        parts.push_back("\"" + question_text + "\"=\"" + answer + "\"");
    }

    ToolResult result;
    result.content = "User has answered your questions: " + join(parts, ", ") +
                     ". You can now continue with the user's answers in mind.";

    // The answers travel with the result so the UI can show them.
    boost::json::object answers_object;
    for (const auto& [question_text, answer] : answers) {
        answers_object[question_text] = answer;
    }
    boost::json::object payload;
    payload["answers"] = std::move(answers_object);
    result.tool_use_result = boost::json::value(std::move(payload));

    return result;
}

} // namespace cc::tools
