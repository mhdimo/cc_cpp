#include "query/engine.hpp"

#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/version.hpp"
#include "permissions/permissions.hpp"
#include "query/model.hpp"
#include "utils/json_util.hpp"
#include "utils/messages.hpp"
#include "utils/string.hpp"

#include <ai/model/call_options.hpp>
#include <ai/prompt/content_part.hpp>
#include <ai/prompt/message.hpp>
#include <ai/tool/tool_choice.hpp>
#include <ai/util/cancellation.hpp>

#include <algorithm>

namespace cc::query {
namespace {

/// The tool input the model sent, accumulated while it streamed in.
struct PendingToolCall {
    std::string id;
    std::string name;
    std::string json;
};

/// Builds the API tool description for a Claude Code tool.
ai::FunctionTool to_function_tool(const Tool& tool) {
    ai::FunctionTool result;
    result.name = std::string(tool.name());
    result.description = tool.prompt();
    result.input_schema = ai::schema::JsonSchema(tool.input_schema());
    result.strict = false;
    return result;
}

/// Converts a content block into the API's file part, when it is one.
std::optional<ai::FilePart> to_file_part(const ContentBlock& block) {
    const auto* image = as_image(block);
    if (!image) return std::nullopt;

    ai::FilePart part;
    ai::DataFileData data;
    // The base64 payload is stored without a data: prefix.
    static const std::string kBase64Prefix = "base64,";
    std::string payload = image->data;
    if (payload.rfind(kBase64Prefix, 0) == 0) {
        payload = payload.substr(kBase64Prefix.size());
    }
    data.data.assign(payload.begin(), payload.end());
    part.data = std::move(data);
    part.media_type = image->media_type;
    return part;
}

/// The error text Claude Code sends back when a tool throws.
std::string tool_error_content(const std::string& tool_name, const std::string& message) {
    return "<tool_use_error>Error calling tool (" + tool_name + "): " + message +
           "</tool_use_error>";
}

/// The error text Claude Code sends back for an unknown tool.
std::string unknown_tool_content(const std::string& tool_name) {
    return "<tool_use_error>Error: No such tool available: " + tool_name + "</tool_use_error>";
}

/// The name the reference gives a JSON value's type in its validation errors.
std::string json_type_name(const boost::json::value& value) {
    if (value.is_null()) return "null";
    if (value.is_bool()) return "boolean";
    if (value.is_int64() || value.is_uint64() || value.is_double()) return "number";
    if (value.is_string()) return "string";
    if (value.is_array()) return "array";
    return "object";
}

/// The error text Claude Code sends back when the model's tool input does
/// not match the tool's schema.
///
/// The shape follows the reference: the name of the tool, the issues the
/// validation found and one line per issue.
std::string input_validation_error(const std::string& tool_name,
                                   const boost::json::object& schema,
                                   const boost::json::object& input) {
    std::vector<std::string> issues;

    const boost::json::object* properties = nullptr;
    if (schema.contains("properties") && schema.at("properties").is_object()) {
        properties = &schema.at("properties").as_object();
    }

    // The required parameters the input does not have.
    if (schema.contains("required") && schema.at("required").is_array()) {
        for (const auto& required : schema.at("required").as_array()) {
            if (!required.is_string()) continue;
            const std::string key = required.as_string().c_str();
            if (input.find(key) == input.end()) {
                issues.push_back("The required parameter `" + key + "` is missing");
            }
        }
    }

    // A schema that is open takes the fields it does not know, the way the
    // reference's schemas that are built with `z.object` do.
    bool closed = true;
    if (const auto additional = schema.find("additionalProperties");
        additional != schema.end() && additional->value().is_bool()) {
        closed = !additional->value().as_bool();
    }

    if (properties && closed) {
        // The parameters the tool does not know.
        for (const auto& [key, value] : input) {
            if (properties->find(key) == properties->end()) {
                issues.push_back("An unexpected parameter `" + std::string(key) +
                                 "` was provided");
            }
        }

        // The parameters whose type does not match.
        for (const auto& [key, value] : input) {
            const auto property = properties->find(key);
            if (property == properties->end()) continue;
            if (!property->value().is_object()) continue;
            const auto& property_schema = property->value().as_object();
            const auto type = property_schema.find("type");
            if (type == property_schema.end() || !type->value().is_string()) continue;
            std::string expected = type->value().as_string().c_str();
            if (expected == "integer") expected = "number";
            const std::string received = json_type_name(value);
            if (expected != received) {
                issues.push_back("The parameter `" + std::string(key) +
                                 "` type is expected as `" + expected + "` but provided as `" +
                                 received + "`");
            }
        }
    }

    std::string detail;
    if (issues.empty()) {
        // The reference falls back to the raw validation message here.
        detail = tool_name + " failed due to the following issue:\nThe tool input is not valid JSON";
    } else {
        detail = tool_name + " failed due to the following " +
                 (issues.size() > 1 ? "issues" : "issue") + ":\n" + join(issues, "\n");
    }
    return "<tool_use_error>InputValidationError: " + detail + "</tool_use_error>";
}

/// The error text Claude Code sends back when the model's tool input is not
/// an object at all.
std::string invalid_input_error(const std::string& received_type) {
    return "<tool_use_error>InputValidationError: Invalid input: expected object, received " +
           received_type + "</tool_use_error>";
}

/// The error the model sees when its tool input could not be read.
///
/// A tool that does not exist is left to the tool pipeline, which reports
/// it the way the reference does.
std::string tool_input_error(const Tools& tools, const std::string& tool_name,
                              const boost::json::object& input) {
    const Tool* tool = find_tool_by_name(tools, tool_name);
    if (!tool) return {};
    return input_validation_error(tool_name, tool->input_schema(), input);
}

} // namespace

Engine::Engine(Options options) : options_(std::move(options)) {
    context_.options.cwd = options_.cwd;
    context_.options.tools = options_.tools;
    context_.options.session_id = options_.session_id;
    context_.permission_context = options_.permission_context;
    context_.on_progress = options_.hooks.on_progress;
    context_.on_file_edit = options_.on_file_edit;
    // The tools that need the user ask through this hook.
    context_.on_user_interaction = options_.hooks.on_user_interaction;
    // The goal the model proposes is approved by the user through this hook.
    context_.on_goal_proposal = options_.hooks.on_goal_proposal;
    context_.original_cwd = options_.cwd;

    // A token the caller gave us cancels the engine too, so that either side
    // can interrupt the turn.
    if (options_.cancel) {
        options_.cancel->on_cancel([this] { cancel(); });
    }
}

void Engine::cancel() {
    std::lock_guard lock(cancel_mutex_);
    // The provider's request is aborted through the token it was given.
    cancel_source_.cancel();
    // The tools of the running turn see the abort as well.
    if (context_.abort_signal) context_.abort_signal->abort();
}

ai::CancellationToken Engine::request_token() {
    std::lock_guard lock(cancel_mutex_);
    return cancel_source_.token();
}

ai::Prompt Engine::build_prompt() const {
    ai::Prompt prompt;

    // The prompt is built again for every request, so that a change of the
    // permission mode is picked up.
    if (options_.system_prompt_provider) {
        const std::string prompt_text = options_.system_prompt_provider();
        if (!prompt_text.empty()) prompt.push_back(ai::SystemMessage{.content = prompt_text});
    } else if (!options_.system_prompt.empty()) {
        prompt.push_back(ai::SystemMessage{.content = options_.system_prompt});
    }

    for (size_t index = 0; index < messages_.size(); ++index) {
        const Message& message = messages_[index];
        // The reference puts a cache breakpoint on the last message, so that
        // the prompt of the turn that follows is read from the cache.
        const bool is_last = index + 1 == messages_.size();

        // A message that carries tool results is sent as a tool message; the
        // Anthropic API puts tool results in a user turn.
        const bool has_tool_result = message.has_tool_result();

        if (has_tool_result) {
            ai::ToolContent content;
            for (const auto& block : message.content) {
                const auto* result = as_tool_result(block);
                if (!result) continue;
                ai::ToolResultPart part;
                part.tool_call_id = result->tool_use_id;
                part.tool_name = std::string();
                if (result->is_error) {
                    part.output = ai::ErrorTextOutput{.value = result->content};
                } else {
                    part.output = ai::TextOutput{.value = result->content};
                }
                content.push_back(std::move(part));
            }
            prompt.push_back(ai::ToolMessage{.content = std::move(content)});
            continue;
        }

        if (message.role == Role::User) {
            ai::UserContent content;
            // The reference puts a cache breakpoint on the last message, so
            // that the prompt of the turn that follows is read from the
            // cache instead of being sent again.
            size_t last_text = content.size();
            for (const auto& block : message.content) {
                if (const auto* text = as_text(block)) {
                    content.push_back(ai::TextPart{.text = text->text});
                    last_text = content.size() - 1;
                } else if (auto file = to_file_part(block)) {
                    content.push_back(std::move(*file));
                }
            }
            // The breakpoint is only sent to Anthropic's own models: a
            // gateway that speaks to another model may refuse a block it
            // does not know.
            if (is_last && last_text < content.size() &&
                query::is_anthropic_model(options_.model_name)) {
                boost::json::object anthropic;
                anthropic["cache_control"] = boost::json::value(true);
                std::get<ai::TextPart>(content[last_text]).provider_options["anthropic"] =
                    boost::json::value(anthropic);
            }
            prompt.push_back(ai::UserMessage{.content = std::move(content)});
            continue;
        }

        ai::AssistantContent content;
        size_t last_text = content.size();
        for (const auto& block : message.content) {
            if (const auto* text = as_text(block)) {
                content.push_back(ai::TextPart{.text = text->text});
                last_text = content.size() - 1;
            } else if (const auto* thinking = as_thinking(block)) {
                ai::ReasoningPart part;
                part.text = thinking->thinking;
                if (!thinking->signature.empty()) part.signature = thinking->signature;
                content.push_back(std::move(part));
            } else if (const auto* tool_use = as_tool_use(block)) {
                content.push_back(ai::ToolCallPart{
                    .tool_call_id = tool_use->id,
                    .tool_name = tool_use->name,
                    .input = tool_use->input,
                });
            } else if (auto file = to_file_part(block)) {
                content.push_back(std::move(*file));
            }
        }
        if (is_last && last_text < content.size() &&
            query::is_anthropic_model(options_.model_name)) {
            boost::json::object anthropic;
            anthropic["cache_control"] = boost::json::value(true);
            std::get<ai::TextPart>(content[last_text]).provider_options["anthropic"] =
                boost::json::value(anthropic);
        }
        prompt.push_back(ai::AssistantMessage{.content = std::move(content)});
    }

    return prompt;
}

ToolResult Engine::run_tool(const ToolCallInfo& call, bool& ran) {
    ran = false;

    Tool* tool = find_tool_by_name(options_.tools, call.name);
    if (!tool) {
        ToolResult result;
        result.content = unknown_tool_content(call.name);
        result.is_error = true;
        result.tool_use_result = boost::json::value("Error: No such tool available: " + call.name);
        return result;
    }

    // The permission pipeline decides whether the call may proceed.
    PermissionResult decision =
        permissions::has_permissions_to_use_tool(*tool, call.input, context_);

    if (decision.is_ask() && options_.hooks.on_permission_request) {
        decision = options_.hooks.on_permission_request(*tool, call.input, decision);
    }

    if (decision.is_deny()) {
        ToolResult result;
        result.content = decision.message.empty()
                             ? messages::auto_reject_message(call.name)
                             : decision.message;
        result.is_error = true;
        result.tool_use_result = boost::json::value(result.content);
        return result;
    }

    // A hook may veto the call before it runs.
    if (options_.hooks.on_before_tool) {
        options_.hooks.on_before_tool(*tool, call.input,
                                      [this](const std::string& output,
                                            const std::string& reason) {
                                          hook_block_ = output;
                                          hook_reason_ = reason;
                                      });
        if (!hook_block_.empty()) {
            ToolResult result;
            result.content = hook_block_;
            result.is_error = true;
            result.tool_use_result = boost::json::value(hook_reason_);
            hook_block_.clear();
            hook_reason_.clear();
            return result;
        }
    }

    ran = true;
    try {
        // The permission may hand back the input the tool should run with;
        // the reference replaces the tool's input with it.
        const boost::json::object& tool_input =
            decision.is_allow() && decision.updated_input.has_value() ? *decision.updated_input
                                                                      : call.input;

        ToolResult result = tool->call(tool_input, context_);
        // The instructions the user gave while they approved the call
        // travel with the result, the way the reference hands them to the
        // model.
        if (!decision.accept_feedback.empty()) {
            if (!result.content.empty()) result.content += "\n\n";
            result.content += decision.accept_feedback;
        }
        // A tool that attaches a payload of its own keeps it; the plain
        // content is only the fallback, the way the reference keeps the
        // tool's own result.
        if (result.tool_use_result.is_null()) {
            result.tool_use_result = boost::json::value(result.content);
        }
        return result;
    } catch (const ToolError& error) {
        ToolResult result;
        result.content = tool_error_content(call.name, error.what());
        result.is_error = true;
        result.tool_use_result = boost::json::value(error.what());
        return result;
    } catch (const std::exception& error) {
        ToolResult result;
        result.content = tool_error_content(call.name, error.what());
        result.is_error = true;
        result.tool_use_result = boost::json::value(error.what());
        return result;
    }
}

ai::Task<void> Engine::run_turn(std::string prompt, bool record_prompt) {
    // A fresh cancellation source and abort signal for the turn, so that an
    // interrupt from an earlier turn does not carry over.
    {
        std::lock_guard lock(cancel_mutex_);
        cancel_source_ = ai::CancellationSource{};
    }
    context_.abort_signal = std::make_shared<AbortSignal>();

    // Record the user's message. A turn that follows a command of the shell
    // records none: the command and its output are already there.
    if (record_prompt) {
        Message user_message;
        user_message.role = Role::User;
        user_message.uuid = uuid_v4();
        // The message belongs to the session it is written in, so that the
        // transcript of a session holds the id of that session.
        user_message.session_id = options_.session_id;
        user_message.cwd = options_.cwd;
        user_message.version = version();
        user_message.model = options_.model_name;
        user_message.content.push_back(TextBlock{prompt, false});
        messages_.push_back(user_message);
        if (options_.hooks.on_message) options_.hooks.on_message(messages_.back());
        if (options_.hooks.on_user_message) options_.hooks.on_user_message(user_message.uuid);
    }

    // The tools are asked for again for every turn: a server of an MCP
    // server may have connected while the session was waiting.
    if (options_.tools_provider) {
        options_.tools = options_.tools_provider();
        context_.options.tools = options_.tools;
    }

    for (int turn = 0; turn < options_.max_turns; ++turn) {
        ai::CallOptions call_options;
        call_options.prompt = build_prompt();

        // The number of tokens the model is asked for is the one of the
        // model itself, unless the session asks for another number.
        const query::OutputTokens tokens = query::output_tokens_for(options_.model_name);
        call_options.max_output_tokens =
            options_.max_output_tokens > 0
                ? std::min(options_.max_output_tokens, tokens.upper_limit)
                : tokens.default_tokens;

        // The thinking is asked for when the model is one of Anthropic's: a
        // gateway that speaks to another model does not know the parameter.
        if (query::is_anthropic_model(options_.model_name)) {
            boost::json::object thinking;
            if (query::supports_adaptive_thinking(options_.model_name)) {
                thinking["type"] = "adaptive";
            } else {
                thinking["type"] = "enabled";
                // The budget has to be smaller than the number of the output
                // tokens, or the API refuses the request.
                thinking["budget_tokens"] =
                    std::min(call_options.max_output_tokens.value_or(32000) - 1, 31999);
            }
            boost::json::object anthropic;
            anthropic["thinking"] = boost::json::value(thinking);
            call_options.provider_options["anthropic"] = boost::json::value(anthropic);
        }

        // The token of the engine's own source, so that `cancel()` aborts
        // the request in flight.
        call_options.cancel = request_token();
        for (const auto& tool : options_.tools) {
            if (tool) call_options.tools.push_back(to_function_tool(*tool));
        }
        // The reference leaves the choice of the tool to the model, and does
        // not send the field at all.

        std::string text;
        std::string thinking;
        std::vector<PendingToolCall> pending;
        Usage step_usage;
        bool interrupted = false;

        try {
            ai::StreamResult stream_result = co_await options_.model->do_stream(call_options);

            while (auto part = co_await stream_result.stream.next()) {
                if (const auto* delta = std::get_if<ai::TextDelta>(&*part)) {
                    text += delta->delta;
                    if (options_.hooks.on_text) options_.hooks.on_text(delta->delta);
                } else if (const auto* delta = std::get_if<ai::ReasoningDelta>(&*part)) {
                    thinking += delta->delta;
                    if (options_.hooks.on_thinking) options_.hooks.on_thinking(delta->delta);
                } else if (const auto* start = std::get_if<ai::ToolInputStart>(&*part)) {
                    pending.push_back(PendingToolCall{start->id, start->tool_name, {}});
                } else if (const auto* delta = std::get_if<ai::ToolInputDelta>(&*part)) {
                    for (auto& call : pending) {
                        if (call.id == delta->id) {
                            call.json += delta->delta;
                            break;
                        }
                    }
                } else if (const auto* finish = std::get_if<ai::FinishPart>(&*part)) {
                    if (finish->usage.input_tokens.total) {
                        step_usage.input_tokens = *finish->usage.input_tokens.total;
                    }
                    if (finish->usage.output_tokens.total) {
                        step_usage.output_tokens = *finish->usage.output_tokens.total;
                    }
                    if (finish->usage.input_tokens.cache_read) {
                        step_usage.cache_read_input_tokens = *finish->usage.input_tokens.cache_read;
                    }
                    if (finish->usage.input_tokens.cache_write) {
                        step_usage.cache_creation_input_tokens =
                            *finish->usage.input_tokens.cache_write;
                    }
                } else if (const auto* error = std::get_if<ai::ErrorPart>(&*part)) {
                    if (options_.hooks.on_error) options_.hooks.on_error(error->message);
                }
            }
        } catch (const ai::OperationCancelled&) {
            // The turn was interrupted. Whatever the model managed to say is
            // kept, the way the reference keeps a partial answer.
            interrupted = true;
        }

        usage_ += step_usage;
        // The context of the session is the size of the last call.
        last_usage_ = step_usage;

        // Record the assistant's turn.
        Message assistant_message;
        assistant_message.role = Role::Assistant;
        assistant_message.uuid = uuid_v4();
        assistant_message.session_id = options_.session_id;
        assistant_message.cwd = options_.cwd;
        assistant_message.model = options_.model_name;
        assistant_message.usage = step_usage;
        if (!thinking.empty()) {
            assistant_message.content.push_back(ThinkingBlock{thinking, {}});
        }
        if (!text.empty()) {
            assistant_message.content.push_back(TextBlock{text, false});
        }

        // The tool calls the model asked for, with the error the engine
        // produced for any input it could not read.
        std::vector<ToolCallInfo> calls;
        std::vector<std::string> input_errors;

        for (const auto& pending_call : pending) {
            ToolCallInfo info;
            info.id = pending_call.id;
            info.name = pending_call.name;

            // The reference reads a tool input that does not parse as an
            // empty object, which then fails the tool's own validation.
            boost::system::error_code error;
            const boost::json::value parsed = boost::json::parse(pending_call.json, error);
            std::string input_error;
            if (error) {
                info.input = boost::json::object{};
                input_error = tool_input_error(options_.tools, info.name, info.input);
            } else if (parsed.is_object()) {
                info.input = parsed.as_object();
            } else {
                info.input = boost::json::object{};
                input_error = invalid_input_error(json_type_name(parsed));
            }

            assistant_message.content.push_back(ToolUseBlock{info.id, info.name, info.input});
            calls.push_back(std::move(info));
            input_errors.push_back(std::move(input_error));
        }

        if (interrupted) {
            // The partial answer is kept, the way the reference keeps it. A
            // message without any content would be an empty turn, which the
            // API does not accept.
            if (!text.empty() || !thinking.empty()) {
                assistant_message.stop_reason = "interrupted";
                messages_.push_back(std::move(assistant_message));
                if (options_.hooks.on_message) options_.hooks.on_message(messages_.back());
            }
            co_return;
        }

        if (calls.empty()) {
            assistant_message.stop_reason = "end_turn";
            messages_.push_back(std::move(assistant_message));
            if (options_.hooks.on_message) options_.hooks.on_message(messages_.back());
            co_return;
        }

        assistant_message.stop_reason = "tool_use";
        messages_.push_back(std::move(assistant_message));
        if (options_.hooks.on_message) options_.hooks.on_message(messages_.back());

        // Run the tools the model asked for and feed the results back.
        Message results_message;
        results_message.role = Role::User;
        results_message.uuid = uuid_v4();
        results_message.session_id = options_.session_id;
        results_message.cwd = options_.cwd;

        for (size_t i = 0; i < calls.size(); ++i) {
            const ToolCallInfo& call = calls[i];

            if (options_.hooks.on_tool_start) options_.hooks.on_tool_start(call);

            bool ran = false;
            ToolResult result;
            if (input_errors[i].empty()) {
                result = run_tool(call, ran);
            } else {
                // The model's input was not usable, so the tool never runs.
                result.content = input_errors[i];
                result.is_error = true;
                result.tool_use_result = boost::json::value(result.content);
            }

            results_message.content.push_back(
                ToolResultBlock{call.id, result.content, result.is_error, result.structured_content});

            // The tool's own output is kept with the message, so that the
            // transcript can show the summary of the result later on. The
            // reference keeps it in the same place.
            if (calls.size() == 1 && !result.tool_use_result.is_null()) {
                results_message.tool_use_result = result.tool_use_result;
            }

            // The PostToolUse hook runs once the result is part of the
            // conversation, the way the reference orders it.
            if (ran && options_.hooks.on_after_tool) {
                if (const Tool* tool = find_tool_by_name(options_.tools, call.name)) {
                    options_.hooks.on_after_tool(*tool, call.input, result);
                }
            }

            if (options_.hooks.on_tool_end) options_.hooks.on_tool_end(call, result);
        }

        messages_.push_back(std::move(results_message));
        if (options_.hooks.on_message) options_.hooks.on_message(messages_.back());

        // A message the user sent while the turn was running joins the
        // conversation here, so the model sees it alongside the results of
        // the tools that just ran.
        if (options_.hooks.on_pending_messages) {
            for (auto& message : options_.hooks.on_pending_messages()) {
                messages_.push_back(std::move(message));
                if (options_.hooks.on_message) options_.hooks.on_message(messages_.back());
            }
        }
    }
}

} // namespace cc::query
