#include "utils/json_util.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/string.hpp>

namespace cc {
namespace {

/// Writes `value` with two-space indentation.
void pretty_print(const boost::json::value& value, int depth, std::string& out) {
    const std::string indent(static_cast<size_t>(depth) * 2, ' ');
    const std::string child_indent(static_cast<size_t>(depth + 1) * 2, ' ');

    if (value.is_object()) {
        const auto& object = value.as_object();
        if (object.empty()) {
            out += "{}";
            return;
        }
        out += "{\n";
        bool first = true;
        for (const auto& [key, member] : object) {
            if (!first) out += ",\n";
            first = false;
            out += child_indent;
            out += boost::json::serialize(key);
            out += ": ";
            pretty_print(member, depth + 1, out);
        }
        out += "\n";
        out += indent;
        out += "}";
        return;
    }

    if (value.is_array()) {
        const auto& array = value.as_array();
        if (array.empty()) {
            out += "[]";
            return;
        }
        out += "[\n";
        bool first = true;
        for (const auto& element : array) {
            if (!first) out += ",\n";
            first = false;
            out += child_indent;
            pretty_print(element, depth + 1, out);
        }
        out += "\n";
        out += indent;
        out += "]";
        return;
    }

    out += boost::json::serialize(value);
}

} // namespace

std::string jget_string(const boost::json::object& o, std::string_view key,
                        std::string_view fallback) {
    auto it = o.find(std::string(key));
    if (it == o.end()) return std::string(fallback);
    if (it->value().is_string()) return std::string(it->value().as_string().c_str());
    return std::string(fallback);
}

int64_t jget_int(const boost::json::object& o, std::string_view key, int64_t fallback) {
    auto it = o.find(std::string(key));
    if (it == o.end()) return fallback;
    const auto& v = it->value();
    if (v.is_int64()) return v.as_int64();
    if (v.is_uint64()) return static_cast<int64_t>(v.as_uint64());
    if (v.is_double()) return static_cast<int64_t>(v.as_double());
    return fallback;
}

double jget_double(const boost::json::object& o, std::string_view key, double fallback) {
    auto it = o.find(std::string(key));
    if (it == o.end()) return fallback;
    const auto& v = it->value();
    if (v.is_double()) return v.as_double();
    if (v.is_int64()) return static_cast<double>(v.as_int64());
    if (v.is_uint64()) return static_cast<double>(v.as_uint64());
    return fallback;
}

bool jget_bool(const boost::json::object& o, std::string_view key, bool fallback) {
    auto it = o.find(std::string(key));
    if (it == o.end()) return fallback;
    const auto& v = it->value();
    if (v.is_bool()) return v.as_bool();
    return fallback;
}

boost::json::object jget_object(const boost::json::object& o, std::string_view key) {
    auto it = o.find(std::string(key));
    if (it == o.end() || !it->value().is_object()) return {};
    return it->value().as_object();
}

boost::json::array jget_array(const boost::json::object& o, std::string_view key) {
    auto it = o.find(std::string(key));
    if (it == o.end() || !it->value().is_array()) return {};
    return it->value().as_array();
}

std::optional<std::string> jget_optional_string(const boost::json::object& o,
                                                std::string_view key) {
    auto it = o.find(std::string(key));
    if (it == o.end() || !it->value().is_string()) return std::nullopt;
    return std::string(it->value().as_string().c_str());
}

boost::json::value json_parse(std::string_view text, boost::json::value fallback) {
    boost::system::error_code ec;
    auto value = boost::json::parse(text, ec);
    if (ec) return fallback;
    return value;
}

std::string json_dump(const boost::json::value& v) {
    return boost::json::serialize(v);
}

std::string json_dump_pretty(const boost::json::value& v) {
    // Boost.JSON 1.90 dropped the `pretty_indent` serializer option, so the
    // indented form is produced here.
    std::string out;
    pretty_print(v, 0, out);
    return out;
}

std::string json_tool_result_text(const boost::json::value& v) {
    if (v.is_string()) return std::string(v.as_string().c_str());
    if (!v.is_object()) return json_dump(v);
    const auto& o = v.as_object();
    auto it = o.find("content");
    if (it == o.end()) return json_dump(v);
    const auto& c = it->value();
    if (c.is_string()) return std::string(c.as_string().c_str());
    if (c.is_array()) {
        std::string out;
        for (const auto& part : c.as_array()) {
            if (part.is_string()) {
                out += part.as_string().c_str();
            } else if (part.is_object()) {
                const auto& po = part.as_object();
                if (jget_string(po, "type", "") == "text") {
                    out += jget_string(po, "text", "");
                }
            }
        }
        return out;
    }
    return json_dump(v);
}

std::vector<std::pair<std::string, std::string>> json_string_map(
    const boost::json::object& o) {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(o.size());
    for (const auto& [k, v] : o) {
        if (v.is_string()) {
            out.emplace_back(k, std::string(v.as_string().c_str()));
        }
    }
    return out;
}

} // namespace cc
