#include <catch2/catch_test_macros.hpp>

#include "query/engine.hpp"
#include "tools/impl/file_tools.hpp"
#include "tools/tool_use_context.hpp"

#include <ai/model/call_options.hpp>
#include <ai/model/generate_result.hpp>
#include <ai/model/language_model.hpp>
#include <ai/model/stream_result.hpp>
#include <ai/stream/async_generator.hpp>
#include <ai/stream/stream_part.hpp>

#include "utils/fs.hpp"

#include <boost/asio.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
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

TEST_CASE("The engine answers when the model does not ask for a tool") {
    boost::asio::io_context io_context;

    auto model = std::make_shared<ScriptedModel>(std::vector<Script>{{.text = "hello there"}});

    query::Engine::Options options;
    options.model = model;
    options.model_name = "scripted-model";
    options.cwd = "/tmp";

    std::string streamed;
    options.hooks.on_text = [&streamed](const std::string& delta) { streamed += delta; };

    query::Engine engine(std::move(options));
    auto task = engine.run_turn("hi");
    drive(io_context, task);

    REQUIRE(streamed == "hello there");
    REQUIRE(engine.messages().size() == 2);
    REQUIRE(engine.messages()[0].role == Role::User);
    REQUIRE(engine.messages()[1].role == Role::Assistant);
    REQUIRE(engine.messages()[1].text() == "hello there");
}

TEST_CASE("The engine runs the tool the model asks for and feeds the result back") {
    boost::asio::io_context io_context;

    const fs::path dir = fs::temp_directory_path() / "cc_engine_test";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "a.txt");
        out << "alpha\nbeta\n";
    }

    auto model = std::make_shared<ScriptedModel>(std::vector<Script>{
        {.tool_name = "Read", .tool_input = "{\"file_path\":\"" + (dir / "a.txt").string() + "\"}"},
        {.text = "the file has two lines"},
    });

    auto tracker = std::make_shared<tools::FileStateTracker>();
    tools::ReadTool read_tool(tracker);

    query::Engine::Options options;
    options.model = model;
    options.model_name = "scripted-model";
    options.cwd = dir.string();
    options.tools.push_back(std::make_shared<tools::ReadTool>(tracker));

    std::vector<std::string> tool_results;
    options.hooks.on_tool_end = [&tool_results](const query::ToolCallInfo&,
                                                const ToolResult& result) {
        tool_results.push_back(result.content);
    };

    query::Engine engine(std::move(options));
    auto task = engine.run_turn("read the file");
    drive(io_context, task);

    REQUIRE(tool_results.size() == 1);
    REQUIRE(tool_results[0].find("alpha") != std::string::npos);

    // The model was asked twice: once for the tool, once for the answer.
    REQUIRE(model->requests().size() == 2);

    // The tool result went back to the model.
    const auto& second = model->requests()[1];
    bool found_result = false;
    for (const auto& message : second.prompt) {
        if (const auto* tool_message = std::get_if<ai::ToolMessage>(&message)) {
            for (const auto& part : tool_message->content) {
                if (const auto* output = std::get_if<ai::TextOutput>(&part.output)) {
                    if (output->value.find("alpha") != std::string::npos) found_result = true;
                }
            }
        }
    }
    REQUIRE(found_result);

    // The conversation holds the user's message, the tool call, the result
    // and the answer.
    REQUIRE(engine.messages().size() == 4);
    REQUIRE(engine.messages()[0].role == Role::User);
    REQUIRE(engine.messages()[1].has_tool_use());
    REQUIRE(engine.messages()[2].has_tool_result());
    REQUIRE(engine.messages()[3].role == Role::Assistant);
    REQUIRE(engine.messages()[3].text() == "the file has two lines");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("The engine stops when the model asks for a tool that does not exist") {
    boost::asio::io_context io_context;

    auto model = std::make_shared<ScriptedModel>(std::vector<Script>{
        {.tool_name = "NoSuchTool", .tool_input = "{}"},
        {.text = "sorry"},
    });

    query::Engine::Options options;
    options.model = model;
    options.cwd = "/tmp";

    std::vector<std::string> tool_results;
    options.hooks.on_tool_end = [&tool_results](const query::ToolCallInfo&,
                                                const ToolResult& result) {
        tool_results.push_back(result.content);
    };

    query::Engine engine(std::move(options));
    auto task = engine.run_turn("go");
    drive(io_context, task);

    REQUIRE(tool_results.size() == 1);
    REQUIRE(tool_results[0].find("No such tool available") != std::string::npos);
}

TEST_CASE("A message the user sent while the turn was running joins the turn") {
    boost::asio::io_context io_context;

    const fs::path dir = fs::temp_directory_path() / "cc_engine_pending_test";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "a.txt");
        out << "alpha\n";
    }

    auto model = std::make_shared<ScriptedModel>(std::vector<Script>{
        {.tool_name = "Read", .tool_input = "{\"file_path\":\"" + (dir / "a.txt").string() + "\"}"},
        {.text = "done"},
    });

    auto tracker = std::make_shared<tools::FileStateTracker>();

    query::Engine::Options options;
    options.model = model;
    options.model_name = "scripted-model";
    options.cwd = dir.string();
    options.tools.push_back(std::make_shared<tools::ReadTool>(tracker));

    // The message the user sent while the tool was running.
    int drains = 0;
    options.hooks.on_pending_messages = [&drains]() {
        ++drains;
        if (drains > 1) return std::vector<Message>{};

        Message message;
        message.role = Role::User;
        message.content.push_back(TextBlock{"the user sent a new message", false});
        return std::vector<Message>{message};
    };

    query::Engine engine(std::move(options));
    auto task = engine.run_turn("read the file");
    drive(io_context, task);

    // The queue was drained once, at the end of the tool round.
    REQUIRE(drains == 1);

    // The message joined the conversation between the tool result and the
    // answer, so the model saw it within the turn that was running.
    const auto& messages = engine.messages();
    REQUIRE(messages.size() == 5);
    REQUIRE(messages[0].role == Role::User);
    REQUIRE(messages[2].has_tool_result());
    REQUIRE(messages[3].role == Role::User);
    REQUIRE(messages[3].text() == "the user sent a new message");
    REQUIRE(messages[4].text() == "done");

    // The model was asked twice, and the second request carried the message.
    REQUIRE(model->requests().size() == 2);
    bool found = false;
    for (const auto& message : model->requests()[1].prompt) {
        if (const auto* user_message = std::get_if<ai::UserMessage>(&message)) {
            for (const auto& part : user_message->content) {
                if (const auto* input = std::get_if<ai::TextPart>(&part)) {
                    if (input->text.find("the user sent a new message") != std::string::npos) {
                        found = true;
                    }
                }
            }
        }
    }
    REQUIRE(found);

    std::error_code ec;
    fs::remove_all(dir, ec);
}
