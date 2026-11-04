#pragma once

#include "tools/tool.hpp"

#include <string>
#include <string_view>

namespace cc::permissions {

/// The decision the classifier of the auto mode reached.
enum class ClassifierDecision {
    /// The call may run without the user.
    Allow,

    /// The call is refused.
    Deny,
};

/// What the classifier of the auto mode decided.
struct ClassifierOutcome {
    /// The decision itself.
    ClassifierDecision decision = ClassifierDecision::Allow;

    /// The reason the model gave for its decision.
    std::string reason;
};

/// True when the tool is safe enough to run without the classifier.
///
/// The reference keeps a list of the tools that only read and that change
/// nothing: the classifier is not asked about them, so that the session
/// does not pay for a question it does not need.
bool classifier_allowlisted(std::string_view tool_name);

/// The tool the classifier of the auto mode asks the model to call.
///
/// The reference gives the model a tool of its own when it classifies an
/// action: the answer of the tool carries the thinking of the model, the
/// decision itself and the reason of the decision.
class ClassifierTool : public Tool {
public:
    std::string_view name() const override { return "classify_result"; }

    std::string description() const override {
        return "Report the security classification result for the agent action";
    }

    boost::json::object input_schema() const override;

    ToolResult call(const boost::json::object& input, ToolUseContext& context) override;

    /// True when the model called the tool.
    bool answered() const { return answered_; }

    /// True when the action is to be blocked.
    ///
    /// The decision is false when the model did not answer; the caller
    /// falls back to asking the user then.
    bool should_block() const { return should_block_; }

    /// The reason the model gave for its decision.
    const std::string& reason() const { return reason_; }

private:
    /// True when the model called the tool.
    bool answered_ = false;

    /// True when the action is to be blocked.
    bool should_block_ = false;

    /// The reason the model gave for its decision.
    std::string reason_;
};

/// The prompt the classifier sends to the model.
///
/// The reference sends the transcript of the session and the action that is
/// to be classified; the port sends the same. The wording of the prompt of
/// the reference is not in the tree that was read, so the port writes one
/// of its own with the same purpose.
std::string classifier_prompt(const std::string& action, const std::string& transcript);

/// The message the model is given when the classifier blocks an action.
///
/// The wording is the one of the reference: the reason of the decision,
/// the advice to go on with the other work, and the way to allow the
/// action in the future.
std::string classifier_rejection_message(const std::string& reason);

} // namespace cc::permissions
