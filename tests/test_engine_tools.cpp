#include <catch2/catch_test_macros.hpp>

#include "query/engine.hpp"
#include "tools/impl/bash_tool.hpp"
#include "tools/tool_use_context.hpp"

#include <ai/model/call_options.hpp>
#include <ai/model/generate_result.hpp>
#include <ai/model/language_model.hpp>
#include <ai/model/stream_result.hpp>
#include <ai/stream/async_generator.hpp>
#include <ai/stream/stream_part.hpp>

#include <boost/asio.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace cc;

namespace {

/// One scripted answer from the model.
struct Script {
    /// The text the model answers with.
    std::string text;
    /// The tool the model asks for; empty means it only answers.
    std::string tool_name;
    /// The arguments of the tool call.
    std::string tool_input;
};

/// A model that replays a script.
class ScriptedModel : public ai::LanguageModel {
public:
    explicit ScriptedModel(std::vector<Script> script) : script_(std::move(script)) {}

    std::string_view provider() const override { return "scripted"; }
    std::string_view model_id() const override { return "scripted-model"; }

    /// The prompts the model was asked, one per call.
    const std::vector<ai::CallOptions>& requests() const { return requests_; }

    ai::Task<ai::GenerateResult> do_generate(ai::CallOptions options) override {
        requests_.push_back(options);
        co_return ai::GenerateResult{};
    }

    ai::Task<ai::StreamResult> do_stream(ai::CallOptions options) override {
        requests_.push_back(options);
        if (index_ >= script_.size()) {
            throw std::runtime_error("ScriptedModel: the script ran out");
        }
        const Script step = script_[index_++];

        auto generator = [](Script step) -> ai::AsyncGenerator<ai::StreamPart> {
            if (!step.text.empty()) {
                co_yield ai::TextStart{.id = "0"};
                co_yield ai::TextDelta{.id = "0", .delta = step.text};
                co_yield ai::TextEnd{.id = "0"};
            }
            if (!step.tool_name.empty()) {
                co_yield ai::ToolInputStart{.id = "toolu_1", .tool_name = step.tool_name};
                co_yield ai::ToolInputDelta{.id = "toolu_1", .delta = step.tool_input};
                co_yield ai::ToolInputEnd{.id = "toolu_1"};
            }
            co_yield ai::FinishPart{.reason = step.tool_name.empty() ? ai::FinishReason::Stop
                                                                     : ai::FinishReason::ToolCalls};
        }(step);

        co_return ai::StreamResult{.stream = std::move(generator)};
    }

private:
    std::vector<Script> script_;
    size_t index_ = 0;
    std::vector<ai::CallOptions> requests_;
};

/// Runs a task to completion on the io_context.
template <typename T>
void drive(boost::asio::io_context& io_context, ai::Task<T>& task) {
    task.start();
    while (!task.done()) {
        io_context.run_one();
    }
}

} // namespace

TEST_CASE("The model is told what the tool does, not only what it is called") {
    boost::asio::io_context io_context;

    auto model = std::make_shared<ScriptedModel>(std::vector<Script>{{.text = "hello there"}});

    query::Engine::Options options;
    options.model = model;
    options.model_name = "scripted-model";
    options.cwd = "/tmp";
    options.tools.push_back(std::make_shared<tools::BashTool>());

    query::Engine engine(std::move(options));
    auto task = engine.run_turn("hi");
    drive(io_context, task);

    REQUIRE(model->requests().size() == 1);
    const auto& sent = model->requests()[0].tools;
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].name == "Bash");
    REQUIRE(sent[0].description.has_value());

    // The description of the tool is the prompt of the reference, which is
    // what the model is told about the tool.
    const std::string& description = *sent[0].description;
    REQUIRE(description.find("Executes a given bash command and returns its output.") !=
            std::string::npos);
    REQUIRE(description.find("# Committing changes with git") != std::string::npos);
}

TEST_CASE("The engine takes the flag that turns the sandbox off") {
    boost::asio::io_context io_context;

    auto model = std::make_shared<ScriptedModel>(std::vector<Script>{
        {.tool_name = "Bash",
         .tool_input = "{\"command\":\"echo hello\",\"dangerouslyDisableSandbox\":true}"},
        {.text = "done"},
    });

    query::Engine::Options options;
    options.model = model;
    options.model_name = "scripted-model";
    options.cwd = "/tmp";
    options.tools.push_back(std::make_shared<tools::BashTool>());

    std::vector<std::string> tool_results;
    options.hooks.on_tool_end = [&tool_results](const query::ToolCallInfo&,
                                                const ToolResult& result) {
        tool_results.push_back(result.content);
    };

    query::Engine engine(std::move(options));
    auto task = engine.run_turn("run the command");
    drive(io_context, task);

    REQUIRE(tool_results.size() == 1);
    // The parameter is one the reference's schema carries, so the model may
    // send it.
    REQUIRE(tool_results[0].find("unexpected parameter") == std::string::npos);
    REQUIRE(tool_results[0].find("InputValidationError") == std::string::npos);
}
