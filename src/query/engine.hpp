#pragma once

#include "core/message.hpp"
#include "permissions/decision.hpp"
#include "tools/tool.hpp"
#include "tools/tool_use_context.hpp"

#include <ai/model/language_model.hpp>
#include <ai/util/cancellation.hpp>

#include <boost/json.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace cc::query {

/// A tool call the model asked for.
struct ToolCallInfo {
    std::string id;
    std::string name;
    boost::json::object input;
};

/// The hooks the UI provides to the engine.
struct EngineHooks {
    /// Called with each chunk of assistant text as it streams in.
    std::function<void(const std::string&)> on_text;

    /// Called with each chunk of extended thinking as it streams in.
    std::function<void(const std::string&)> on_thinking;

    /// Called when the user's message is recorded, with its id.
    ///
    /// The file history takes its snapshot at that point, so that a rewind
    /// can put the files back.
    std::function<void(const std::string& uuid)> on_user_message;

    /// Called when the model asks for a tool.
    std::function<void(const ToolCallInfo&)> on_tool_start;

    /// Called once a tool has run.
    std::function<void(const ToolCallInfo&, const ToolResult&)> on_tool_end;

    /// Called when the model asks the user a question.
    ///
    /// The hook shows the questions and returns the user's answers, keyed by
    /// the text of the question. An empty map means the user declined to
    /// answer.
    std::function<UserAnswers(const std::vector<UserQuestion>&)> on_user_interaction;

    /// Called when a tool call needs the user's approval.
    ///
    /// The hook is given the decision the rules produced and returns the
    /// decision to apply.
    std::function<PermissionResult(const Tool&, const boost::json::object&,
                                   const PermissionResult&)>
        on_permission_request;

    /// Called when the turn fails.
    std::function<void(const std::string& message)> on_error;

    /// Called with the output of a tool that produces output while it runs.
    std::function<void(const std::string&)> on_progress;

    /// Called before a tool runs, so a hook can veto it.
    ///
    /// The callback returns the hook's result; a blocked result stops the
    /// tool from running.
    std::function<void(const Tool&, const boost::json::object&,
                       const std::function<void(const std::string&, const std::string&)>& run_hook)>
        on_before_tool;

    /// Called after a tool has run.
    ///
    /// The hook runs once the result is part of the conversation, the way
    /// the reference orders the PostToolUse hook.
    std::function<void(const Tool&, const boost::json::object&, const ToolResult&)> on_after_tool;

    /// Called whenever a message joins the conversation.
    ///
    /// The app uses this to write the transcript.
    std::function<void(const Message&)> on_message;

    /// Called at the end of a tool round, so that a message the user sent
    /// while the turn was running can join the conversation.
    ///
    /// The messages are added to the conversation as they are, so the model
    /// sees them within the turn that is running rather than in a turn of
    /// its own. The reference drains its queue at the same point.
    std::function<std::vector<Message>()> on_pending_messages;

    /// Called when the model proposes a goal for the session.
    ///
    /// The hook asks the user, and answers whether the goal was approved.
    /// The reference asks the user with one keypress, and the session goes
    /// on working until the goal is met.
    std::function<bool(const std::string& condition)> on_goal_proposal;
};

/// The engine that runs the conversation.
///
/// The engine owns the conversation, asks the model what to do next, runs the
/// tools the model asks for and feeds the results back, until the model
/// answers without asking for a tool.
class Engine {
public:
    /// The options an engine is built with.
    struct Options {
        /// The model to talk to.
        ai::LanguageModelPtr model;

        /// The name of the model, shown in the UI.
        std::string model_name;

        /// The working directory the session runs in.
        std::string cwd;

        /// The id of the session.
        std::string session_id;

        /// The system prompt sent with every request.
        std::string system_prompt;

        /// A function that builds the system prompt.
        ///
        /// The reference builds the prompt again for every request, so that
        /// a change of the permission mode is picked up. When the provider
        /// is set, it is asked for the prompt and `system_prompt` is left
        /// alone.
        std::function<std::string()> system_prompt_provider;

        /// The tools the model may call.
        Tools tools;

        /// A function that gives the tools of the turn.
        ///
        /// The reference computes the tools of a turn when the turn starts,
        /// so that a server of an MCP server which connected in the
        /// meantime is part of it. When the function is set, it is asked for
        /// the tools of every turn.
        std::function<Tools()> tools_provider;

        /// The permission configuration in force.
        ToolPermissionContext permission_context;

        /// How many model round-trips a single turn may take.
        int max_turns = 50;

        /// The maximum number of output tokens per request.
        int max_output_tokens = 8192;

        /// The hooks the UI provides.
        EngineHooks hooks;

        /// Called before a file is changed, so that the change can be
        /// undone.
        std::function<void(const std::string& path)> on_file_edit;

        /// A token that cancels the turn when it is signalled.
        ///
        /// The engine always hands a token of its own to the provider, so
        /// that `Engine::cancel()` aborts the request in flight. A token
        /// given here cancels the engine in turn.
        std::optional<ai::CancellationToken> cancel;
    };

    explicit Engine(Options options);

    /// Runs one turn: sends `prompt` and keeps going until the model answers
    /// without asking for a tool.
    ///
    /// The returned task must be driven by the io_context the model was
    /// created with.
    /// Runs a turn of the conversation.
    ///
    /// The prompt is taken by value: the task is a coroutine that only
    /// starts when it is started, so a reference to a string that is gone
    /// by then would be a reference to nothing.
    /// Runs a turn of the conversation.
    ///
    /// The prompt is recorded as a message of the user, the way the
    /// reference records it. A turn that follows a command of the shell
    /// records no prompt: the command and its output are already in the
    /// conversation.
    ai::Task<void> run_turn(std::string prompt, bool record_prompt = true);

    /// The conversation so far.
    const std::vector<Message>& messages() const { return messages_; }

    /// Replaces the conversation, for resuming a session.
    void set_messages(std::vector<Message> messages) { messages_ = std::move(messages); }

    /// Replaces the model the engine talks to.
    ///
    /// The session uses it after the user has logged in, when the
    /// credentials of the account have changed.
    void set_model(ai::LanguageModelPtr model) { options_.model = std::move(model); }

    /// The tool use context the engine runs tools in.
    ToolUseContext& context() { return context_; }

    /// The options the engine was built with.
    const Options& options() const { return options_; }

    /// Cancels the current request.
    ///
    /// The provider's request is aborted and the tools of the running turn
    /// see the abort.
    void cancel();

    /// The number of tokens the session has used.
    /// The totals of the session.
    const Usage& usage() const { return usage_; }

    /// The usage of the last call of the model.
    ///
    /// The context of the session is the size of that call, not the sum of
    /// every call: the reference reads it the same way, out of the last
    /// message that carries a usage.
    const Usage& last_usage() const { return last_usage_; }

    /// The output of the hook that blocked the last tool call.
    std::string hook_block_;
    /// The reason the hook gave for blocking the last tool call.
    std::string hook_reason_;

private:
    /// Converts the conversation into the prompt the API expects.
    ai::Prompt build_prompt() const;

    /// Runs one tool call and returns the result the model should see.
    ///
    /// `ran` is set to true when the tool itself ran, which is when the
    /// PostToolUse hook fires.
    ToolResult run_tool(const ToolCallInfo& call, bool& ran);

    /// The token the provider is given for the request in flight.
    ai::CancellationToken request_token();

    Options options_;
    ToolUseContext context_;
    std::vector<Message> messages_;
    Usage usage_;

    /// The usage of the last call of the model.
    Usage last_usage_;

    /// The source the token of the request in flight comes from.
    ai::CancellationSource cancel_source_;

    /// Guards `cancel_source_`.
    std::mutex cancel_mutex_;
};

} // namespace cc::query
