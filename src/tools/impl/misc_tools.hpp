#pragma once

#include "tools/impl/common.hpp"
#include "tools/tool.hpp"
#include "tools/tool_use_context.hpp"

#include <string>
#include <vector>

namespace cc::tools {

/// One entry of the todo list.
struct Todo {
    std::string content;
    std::string status = "pending"; ///< "pending", "in_progress" or "completed".
    std::string active_form;

    boost::json::value to_json() const;
};

/// Tracks the session's todo list and renders it for the model.
class TodoWriteTool : public Tool {
public:
    std::string_view name() const override { return "TodoWrite"; }
    std::string description() const override { return todo_write_tool_description(); }
    std::string prompt() const override;
    boost::json::object input_schema() const override;
    ToolResult call(const boost::json::object& input, ToolUseContext& context) override;

    /// The current todo list.
    const std::vector<Todo>& todos() const { return todos_; }

private:
    std::vector<Todo> todos_;
};

/// Signals that the model has finished planning and wants the user's
/// approval.
class ExitPlanModeTool : public Tool {
public:
    std::string_view name() const override { return "ExitPlanMode"; }
    std::string description() const override {
        return "Prompts the user to exit plan mode and start coding";
    }
    std::string prompt() const override;
    boost::json::object input_schema() const override;
    ToolResult call(const boost::json::object& input, ToolUseContext& context) override;

    bool requires_user_interaction() const override { return true; }
};

/// Asks the user one or more multiple-choice questions.
class AskUserQuestionTool : public Tool {
public:
    std::string_view name() const override { return "AskUserQuestion"; }
    std::string description() const override {
        return "Asks the user multiple choice questions to gather information, clarify ambiguity, "
               "understand preferences, make decisions or offer them choices.";
    }
    std::string prompt() const override;
    boost::json::object input_schema() const override;
    ToolResult call(const boost::json::object& input, ToolUseContext& context) override;

    bool requires_user_interaction() const override { return true; }
};

/// The questions of the `AskUserQuestion` tool, read out of its input.
///
/// The questions are read out of the input of the tool, so that the dialog
/// that asks them and the tool itself read the same questions.
std::vector<UserQuestion> user_questions(const boost::json::object& input);

} // namespace cc::tools
