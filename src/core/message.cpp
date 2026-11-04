#include "core/message.hpp"

#include "utils/json_util.hpp"

#include <algorithm>

namespace cc {
namespace {

/// True when a value carries nothing: null, an empty object, an empty array
/// or an empty string.
bool is_empty_value(const boost::json::value& value) {
    if (value.is_null()) return true;
    if (value.is_object()) return value.as_object().empty();
    if (value.is_array()) return value.as_array().empty();
    if (value.is_string()) return value.as_string().empty();
    return false;
}

} // namespace

std::string_view to_string(Role role) {
    return role == Role::User ? "user" : "assistant";
}

bool is_text(const ContentBlock& b) { return std::holds_alternative<TextBlock>(b); }
bool is_thinking(const ContentBlock& b) { return std::holds_alternative<ThinkingBlock>(b); }
bool is_tool_use(const ContentBlock& b) { return std::holds_alternative<ToolUseBlock>(b); }
bool is_tool_result(const ContentBlock& b) { return std::holds_alternative<ToolResultBlock>(b); }
bool is_image(const ContentBlock& b) { return std::holds_alternative<ImageBlock>(b); }

const TextBlock* as_text(const ContentBlock& b) { return std::get_if<TextBlock>(&b); }
const ThinkingBlock* as_thinking(const ContentBlock& b) { return std::get_if<ThinkingBlock>(&b); }
const ToolUseBlock* as_tool_use(const ContentBlock& b) { return std::get_if<ToolUseBlock>(&b); }
const ToolResultBlock* as_tool_result(const ContentBlock& b) { return std::get_if<ToolResultBlock>(&b); }
const ImageBlock* as_image(const ContentBlock& b) { return std::get_if<ImageBlock>(&b); }

Usage& Usage::operator+=(const Usage& other) {
    input_tokens += other.input_tokens;
    output_tokens += other.output_tokens;
    cache_creation_input_tokens += other.cache_creation_input_tokens;
    cache_read_input_tokens += other.cache_read_input_tokens;
    return *this;
}

boost::json::value Usage::to_json() const {
    boost::json::object o;
    o["input_tokens"] = input_tokens;
    o["output_tokens"] = output_tokens;
    o["cache_creation_input_tokens"] = cache_creation_input_tokens;
    o["cache_read_input_tokens"] = cache_read_input_tokens;
    return std::move(o);
}

Usage Usage::from_json(const boost::json::value& v) {
    Usage u;
    if (!v.is_object()) return u;
    const auto& o = v.as_object();
    u.input_tokens = jget_int(o, "input_tokens", 0);
    u.output_tokens = jget_int(o, "output_tokens", 0);
    u.cache_creation_input_tokens = jget_int(o, "cache_creation_input_tokens", 0);
    u.cache_read_input_tokens = jget_int(o, "cache_read_input_tokens", 0);
    return u;
}

std::string Message::text() const {
    std::string out;
    for (const auto& block : content) {
        if (const auto* t = as_text(block)) {
            if (!out.empty()) out.push_back('\n');
            out += t->text;
        }
    }
    return out;
}

const ToolUseBlock* Message::first_tool_use() const {
    for (const auto& block : content) {
        if (const auto* tu = as_tool_use(block)) return tu;
    }
    return nullptr;
}

const ToolResultBlock* Message::first_tool_result() const {
    for (const auto& block : content) {
        if (const auto* tr = as_tool_result(block)) return tr;
    }
    return nullptr;
}

bool Message::has_tool_use() const { return first_tool_use() != nullptr; }
bool Message::has_tool_result() const { return first_tool_result() != nullptr; }

boost::json::array Message::to_api_content() const {
    boost::json::array arr;
    for (const auto& block : content) {
        if (const auto* t = as_text(block)) {
            boost::json::object o;
            o["type"] = "text";
            o["text"] = t->text;
            if (t->cache_control) {
                boost::json::object cc_obj;
                cc_obj["type"] = "ephemeral";
                o["cache_control"] = std::move(cc_obj);
            }
            arr.push_back(std::move(o));
        } else if (const auto* th = as_thinking(block)) {
            boost::json::object o;
            o["type"] = "thinking";
            o["thinking"] = th->thinking;
            if (!th->signature.empty()) o["signature"] = th->signature;
            arr.push_back(std::move(o));
        } else if (const auto* tu = as_tool_use(block)) {
            boost::json::object o;
            o["type"] = "tool_use";
            o["id"] = tu->id;
            o["name"] = tu->name;
            o["input"] = tu->input;
            arr.push_back(std::move(o));
        } else if (const auto* tr = as_tool_result(block)) {
            boost::json::object o;
            o["type"] = "tool_result";
            o["tool_use_id"] = tr->tool_use_id;
            if (tr->is_error) o["is_error"] = true;
            o["content"] = tr->content;
            // The structured content of an MCP tool is kept, so that it
            // survives a session that is resumed.
            if (!is_empty_value(tr->structured_content)) {
                o["structuredContent"] = tr->structured_content;
            }
            arr.push_back(std::move(o));
        } else if (const auto* img = as_image(block)) {
            boost::json::object source;
            boost::json::object data;
            data["data"] = img->data;
            data["media_type"] = img->media_type;
            source["type"] = "base64";
            source["media_type"] = img->media_type;
            source["data"] = img->data;
            boost::json::object o;
            o["type"] = "image";
            o["source"] = std::move(source);
            arr.push_back(std::move(o));
        }
    }
    return arr;
}

boost::json::array to_api_content(const std::vector<Message>& messages) {
    boost::json::array out;
    for (const auto& m : messages) {
        boost::json::object o;
        o["role"] = std::string(to_string(m.role));
        o["content"] = m.to_api_content();
        out.push_back(std::move(o));
    }
    return out;
}

boost::json::value Message::to_json() const {
    boost::json::object o;
    o["type"] = std::string(to_string(role));
    o["uuid"] = uuid;
    o["sessionId"] = session_id;
    o["timestamp"] = timestamp;
    o["cwd"] = cwd;
    o["version"] = version;
    if (!parent_tool_use_id.empty()) o["parentToolUseId"] = parent_tool_use_id;
    if (!git_branch.empty()) o["gitBranch"] = git_branch;
    if (!model.empty()) o["model"] = model;
    o["isMeta"] = is_meta;
    o["isSidechain"] = is_sidechain;
    if (is_compact_summary) o["isCompactSummary"] = true;
    if (is_api_error_message) o["isApiErrorMessage"] = true;
    if (!stop_reason.empty()) o["stopReason"] = stop_reason;
    o["usage"] = usage.to_json();

    // The API payload lives under "message" so transcripts stay compatible with
    // the on-disk format Claude Code writes.
    boost::json::object api;
    api["role"] = std::string(to_string(role));
    api["model"] = model;
    api["content"] = to_api_content();
    if (!stop_reason.empty()) api["stop_reason"] = stop_reason;
    api["stop_sequence"] = nullptr;
    api["usage"] = usage.to_json();
    o["message"] = std::move(api);

    if (!tool_use_result.is_null()) o["toolUseResult"] = tool_use_result;
    return std::move(o);
}

Message Message::from_json(const boost::json::value& v) {
    Message m;
    if (!v.is_object()) return m;
    const auto& o = v.as_object();

    const std::string type = jget_string(o, "type", "user");
    m.role = (type == "assistant") ? Role::Assistant : Role::User;

    m.uuid = jget_string(o, "uuid", "");
    m.session_id = jget_string(o, "sessionId", "");
    m.timestamp = jget_string(o, "timestamp", "");
    m.cwd = jget_string(o, "cwd", "");
    m.version = jget_string(o, "version", "");
    m.parent_tool_use_id = jget_string(o, "parentToolUseId", "");
    m.git_branch = jget_string(o, "gitBranch", "");
    m.is_meta = jget_bool(o, "isMeta", false);
    m.is_sidechain = jget_bool(o, "isSidechain", false);
    m.is_compact_summary = jget_bool(o, "isCompactSummary", false);
    m.is_api_error_message = jget_bool(o, "isApiErrorMessage", false);
    m.stop_reason = jget_string(o, "stopReason", "");

    if (o.contains("toolUseResult")) m.tool_use_result = o.at("toolUseResult");

    const boost::json::value* payload = nullptr;
    if (o.contains("message") && o.at("message").is_object()) {
        payload = &o.at("message");
    } else {
        payload = &v;
    }
    const auto& p = payload->as_object();

    m.model = jget_string(p, "model", "");
    if (m.stop_reason.empty()) m.stop_reason = jget_string(p, "stop_reason", "");
    if (p.contains("usage")) m.usage = Usage::from_json(p.at("usage"));

    if (p.contains("content")) {
        const auto& c = p.at("content");
        if (c.is_string()) {
            m.content.emplace_back(TextBlock{c.as_string().c_str(), false});
        } else if (c.is_array()) {
            for (const auto& raw : c.as_array()) {
                if (!raw.is_object()) continue;
                const auto& cb = raw.as_object();
                const std::string btype = jget_string(cb, "type", "");
                if (btype == "text") {
                    m.content.emplace_back(
                        TextBlock{jget_string(cb, "text", ""), cb.contains("cache_control")});
                } else if (btype == "thinking" || btype == "redacted_thinking") {
                    m.content.emplace_back(ThinkingBlock{
                        jget_string(cb, "thinking", ""), jget_string(cb, "signature", "")});
                } else if (btype == "tool_use") {
                    ToolUseBlock tu;
                    tu.id = jget_string(cb, "id", "");
                    tu.name = jget_string(cb, "name", "");
                    tu.input = cb.contains("input") ? cb.at("input") : boost::json::value{boost::json::object{}};
                    m.content.emplace_back(std::move(tu));
                } else if (btype == "tool_result") {
                    ToolResultBlock tr;
                    tr.tool_use_id = jget_string(cb, "tool_use_id", "");
                    tr.is_error = jget_bool(cb, "is_error", false);
                    if (cb.contains("content")) {
                        const auto& tc = cb.at("content");
                        if (tc.is_string()) {
                            tr.content = tc.as_string().c_str();
                        } else if (tc.is_array()) {
                            // Flatten any nested text blocks into the string form.
                            for (const auto& inner : tc.as_array()) {
                                if (inner.is_object() &&
                                    jget_string(inner.as_object(), "type", "") == "text") {
                                    tr.content += jget_string(inner.as_object(), "text", "");
                                }
                            }
                        }
                    }
                    if (cb.contains("structuredContent")) {
                        tr.structured_content = cb.at("structuredContent");
                    }
                    m.content.emplace_back(std::move(tr));
                } else if (btype == "image") {
                    ImageBlock img;
                    if (cb.contains("source") && cb.at("source").is_object()) {
                        const auto& src = cb.at("source").as_object();
                        img.media_type = jget_string(src, "media_type", "");
                        img.data = jget_string(src, "data", "");
                    }
                    m.content.emplace_back(std::move(img));
                }
            }
        }
    }
    return m;
}

} // namespace cc
