#pragma once

#include "agents/agent.hpp"
#include "tools/tool.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cc::tools {

/// The kind of the agent that is used when the model names no agent.
///
/// The reference calls it `general-purpose`, and the tool shows the name of
/// the agent to the user, so it is named here as well.
inline constexpr std::string_view kGeneralPurposeAgentType = "general-purpose";

/// Launches a sub-agent that works on a task of its own.
class AgentTool : public Tool {
public:
    /// The callback that runs the sub-agent.
    ///
    /// It receives the agent's system prompt, the task, the tools it may use
    /// and the model it should run on, and returns the text it produced.
    using Runner = std::function<std::string(const std::string& system_prompt,
                                              const std::string& task,
                                              const std::vector<std::string>& tools,
                                              const std::string& model)>;

    /// What a sub-agent run produced.
    ///
    /// A runner that reports the usage of the run returns this instead of
    /// the bare text, so that the result trailer can carry the real numbers.
    struct RunResult {
        /// The text the agent produced.
        std::string text;
        /// The number of tokens the run consumed.
        int64_t total_tokens = 0;
        /// The number of tool calls the agent made.
        int64_t tool_uses = 0;
        /// How long the run took, in milliseconds.
        int64_t duration_ms = 0;
    };

    /// The options the model asked for when it spawned the agent.
    struct SpawnOptions {
        /// The id of the spawned agent.
        std::string agent_id;
        /// The short description of the task.
        std::string description;
        /// The name the agent is addressable by, empty when it was not named.
        std::string name;
        /// True when the agent should run in the background.
        bool run_in_background = false;
        /// The isolation mode, empty when the agent runs in place.
        std::string isolation;
    };

    /// A runner that also reports how the run went and sees the spawn
    /// options, so that it can honour the background, name and isolation
    /// parameters the model passed.
    using DetailedRunner = std::function<RunResult(const SpawnOptions& options,
                                                    const std::string& system_prompt,
                                                    const std::string& task,
                                                    const std::vector<std::string>& tools,
                                                    const std::string& model)>;

    AgentTool(std::vector<agents::Definition> definitions, Runner runner);

    AgentTool(std::vector<agents::Definition> definitions, DetailedRunner runner);

    std::string_view name() const override { return "Agent"; }
    std::string description() const override { return "Launch a new agent"; }
    std::string prompt() const override;
    boost::json::object input_schema() const override;
    ToolResult call(const boost::json::object& input, ToolUseContext& context) override;

    std::optional<std::string> search_hint() const override {
        return "delegate a multi-step task to a sub-agent";
    }
    bool is_concurrency_safe(const boost::json::object& input) const override { return true; }

    /// The prompt of the tool, for the agents that are given.
    static std::string prompt_for(const std::vector<agents::Definition>& definitions);

private:
    /// The definitions the model may spawn.
    std::vector<agents::Definition> definitions_;

    /// The runner that runs the sub-agent, when it only reports its text.
    Runner runner_;

    /// The runner that runs the sub-agent, when it reports its usage.
    DetailedRunner detailed_runner_;
};

} // namespace cc::tools
