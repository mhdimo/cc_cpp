#pragma once

#include "tools/tool.hpp"

#include <string>
#include <string_view>

namespace cc::goal {

/// The largest condition a goal may have.
///
/// The reference puts a limit on the condition of a goal, so that it may be
/// read at a glance.
constexpr size_t kMaxLength = 500;

/// Checks the condition of a goal.
///
/// The reason the condition is no good is given back in `error`; the error
/// is empty when the condition is good.
bool check_condition(const std::string& condition, std::string* error);

/// The tool the evaluator is asked to call.
///
/// The reference gives the model a tool of its own when it checks the goal:
/// the answer of the tool carries the thinking of the model, the answer
/// itself and the reason of it.
class EvaluatorTool : public Tool {
public:
    std::string_view name() const override { return "goal_result"; }

    std::string description() const override {
        return "Report whether the goal of the session has been met";
    }

    boost::json::object input_schema() const override;

    ToolResult call(const boost::json::object& input, ToolUseContext& context) override;

    /// True when the model called the tool.
    bool answered() const { return answered_; }

    /// True when the goal has been met.
    ///
    /// The answer is false when the model did not answer at all.
    bool met() const { return met_; }

    /// The reason the model gave for its answer.
    const std::string& reason() const { return reason_; }

private:
    /// True when the model called the tool.
    bool answered_ = false;

    /// True when the goal has been met.
    bool met_ = false;

    /// The reason the model gave for its answer.
    std::string reason_;
};

/// The tool the model uses to propose a goal for the session.
///
/// The reference gives the model a tool of its own: the proposal is shown to
/// the user, who approves it with one keypress, and the session goes on
/// working until the goal is met.
class ProposeGoalTool : public Tool {
public:
    std::string_view name() const override { return "ProposeGoal"; }

    std::string description() const override {
        return "Propose a session goal condition, with one-keypress user approval; once set, Claude "
               "keeps working until a separate evaluator confirms it is met";
    }

    boost::json::object input_schema() const override;

    ToolResult call(const boost::json::object& input, ToolUseContext& context) override;

    /// The condition the model proposed.
    const std::string& condition() const { return condition_; }

    /// True when the user approved the proposal.
    bool approved() const { return approved_; }

private:
    /// The condition the model proposed.
    std::string condition_;

    /// True when the user approved the proposal.
    bool approved_ = false;
};

/// The prompt the evaluator sends to the model.
///
/// The reference sends the transcript of the session and the condition of the
/// goal; the port sends the same. The wording of the prompt of the reference
/// is not in the tree that was read, so the port writes one of its own with
/// the same purpose.
std::string evaluator_prompt(const std::string& condition, const std::string& transcript);

/// The prompt the session sends to the model when a goal is set.
///
/// The reference makes the goal a directive for the model: the model is
/// asked to work towards it at once, without asking the user what to do.
std::string goal_prompt(const std::string& condition);

} // namespace cc::goal
