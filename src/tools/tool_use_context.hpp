#pragma once

#include "permissions/decision.hpp"
#include "tools/tool.hpp"
#include "types/permissions.hpp"

#include <boost/json.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cc {

/// A cancellation flag shared by the query engine and the tools it runs.
class AbortSignal {
public:
    void abort() { flag_->store(true, std::memory_order_release); }
    bool aborted() const { return flag_->load(std::memory_order_acquire) != 0; }
    /// Invokes `cb` when the signal is aborted (immediately if it already is).
    void on_abort(std::function<void()> cb);

private:
    std::shared_ptr<std::atomic<bool>> flag_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::vector<std::function<void()>>> listeners_ =
        std::make_shared<std::vector<std::function<void()>>>();
    std::shared_ptr<std::mutex> mutex_ = std::make_shared<std::mutex>();
};

/// One of the choices a question offers.
struct UserQuestionOption {
    /// The text the user picks.
    std::string label;
    /// The explanation shown underneath the label.
    std::string description;
    /// The content the user compares the options by, when the model wrote one.
    std::string preview;
};

/// A question a tool wants to ask the user.
struct UserQuestion {
    /// The question, as the model wrote it.
    std::string text;
    /// A short label for the question, shown as a chip.
    std::string header;
    /// The choices the user can pick from.
    std::vector<UserQuestionOption> options;
    /// True when the user may pick more than one choice.
    bool multi_select = false;
};

/// The answers the user gave, keyed by the text of the question.
///
/// An empty map means the user declined to answer.
using UserAnswers = std::map<std::string, std::string>;

/// Everything a tool needs while it runs.
struct ToolUseContext {
    /// Session-wide options.
    struct Options {
        std::string cwd;
        Tools tools;
        bool debug = false;
        bool verbose = false;
        bool is_non_interactive_session = false;
        std::string main_loop_model;
        std::vector<boost::json::value> commands;
        std::optional<std::string> custom_system_prompt;
        std::optional<std::string> append_system_prompt;
        /// The id of the session; the tools that keep something of their own
        /// name it after the session, the way the plan file does.
        std::string session_id;
    };

    Options options;

    /// The permission configuration in force for this call.
    ToolPermissionContext permission_context;

    /// The cancellation flag for the current turn.
    std::shared_ptr<AbortSignal> abort_signal = std::make_shared<AbortSignal>();

    /// The conversation so far, as JSON message objects.
    std::vector<boost::json::value> messages;

    /// Reports progress for long-running tools.
    std::function<void(const std::string& text)> on_progress;

    /// Called when a tool wants to ask the user something.
    ///
    /// The callback shows the questions and returns the user's answers. An
    /// empty map means the user declined to answer.
    std::function<UserAnswers(const std::vector<UserQuestion>&)> on_user_interaction;

    /// The hook runner, invoked around tool execution.
    std::function<void(const std::string& event, const boost::json::object& payload)> on_hook_event;

    /// Called when the model proposes a goal for the session.
    ///
    /// The callback shows the proposal to the user and sets the goal when
    /// the user approves it. The tool only reports what happened.
    std::function<bool(const std::string& condition)> on_goal_proposal;

    /// Called before a file is changed, so that the change can be undone.
    ///
    /// The reference keeps a copy of every file it edits; the callback is
    /// how the port is told to do the same.
    std::function<void(const std::string& path)> on_file_edit;

    /// The directory the session started in, used to resolve relative rules.
    std::string original_cwd;
};

} // namespace cc
