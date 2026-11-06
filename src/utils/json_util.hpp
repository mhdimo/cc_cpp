#pragma once

#include <boost/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cc {

/// Reads a string member, returning `fallback` when absent or of another type.
std::string jget_string(const boost::json::object& o, std::string_view key,
                        std::string_view fallback = "");

/// Reads an integer member, returning `fallback` when absent or unparsable.
int64_t jget_int(const boost::json::object& o, std::string_view key, int64_t fallback = 0);

/// Reads a double member, returning `fallback` when absent or unparsable.
double jget_double(const boost::json::object& o, std::string_view key, double fallback = 0.0);

/// Reads a boolean member, returning `fallback` when absent.
bool jget_bool(const boost::json::object& o, std::string_view key, bool fallback = false);

/// Reads a nested object member, or an empty object when absent.
boost::json::object jget_object(const boost::json::object& o, std::string_view key);

/// Reads an array member, or an empty array when absent.
boost::json::array jget_array(const boost::json::object& o, std::string_view key);

/// Reads an optional string member.
std::optional<std::string> jget_optional_string(const boost::json::object& o,
                                                std::string_view key);

/// Parses JSON, returning `fallback` when the text is not valid JSON.
boost::json::value json_parse(std::string_view text,
                              boost::json::value fallback = boost::json::value{nullptr});

/// Serializes a JSON value to a compact string.
std::string json_dump(const boost::json::value& v);

/// Serializes a JSON value with two-space indentation.
std::string json_dump_pretty(const boost::json::value& v);

/// Extracts the "text" of a tool result payload, accepting either a plain
/// string or an array of content blocks.
std::string json_tool_result_text(const boost::json::value& v);

/// Converts a boost::json value into a plain `std::map`-ish structure of
/// strings, for simple key/value configuration objects.
std::vector<std::pair<std::string, std::string>> json_string_map(
    const boost::json::object& o);

} // namespace cc
