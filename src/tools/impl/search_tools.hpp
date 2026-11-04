#pragma once

#include "tools/impl/common.hpp"
#include "tools/tool.hpp"

namespace cc::tools {

/// Finds files matching a glob pattern.
class GlobTool : public Tool {
public:
    std::string_view name() const override { return "Glob"; }
    std::string description() const override { return glob_tool_prompt(); }
    std::string prompt() const override;
    boost::json::object input_schema() const override;
    ToolResult call(const boost::json::object& input, ToolUseContext& context) override;

    bool is_concurrency_safe(const boost::json::object& input) const override { return true; }
    bool is_read_only(const boost::json::object& input) const override { return true; }

};

/// Searches file contents with a regular expression.
class GrepTool : public Tool {
public:
    std::string_view name() const override { return "Grep"; }
    std::string description() const override { return grep_tool_prompt(); }
    std::string prompt() const override;
    boost::json::object input_schema() const override;
    ToolResult call(const boost::json::object& input, ToolUseContext& context) override;

    bool is_concurrency_safe(const boost::json::object& input) const override { return true; }
    bool is_read_only(const boost::json::object& input) const override { return true; }

};

} // namespace cc::tools
