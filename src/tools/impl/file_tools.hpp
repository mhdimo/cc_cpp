#pragma once

#include "tools/impl/common.hpp"
#include "tools/tool.hpp"

#include <memory>

namespace cc::tools {

/// Reads a file from disk and returns it with `cat -n` line numbers.
class ReadTool : public Tool {
public:
    explicit ReadTool(std::shared_ptr<FileStateTracker> tracker);

    std::string_view name() const override { return "Read"; }
    std::string description() const override { return "Read a file from the local filesystem."; }
    std::string prompt() const override;
    boost::json::object input_schema() const override;
    ToolResult call(const boost::json::object& input, ToolUseContext& context) override;

    std::optional<std::string> search_hint() const override {
        return "read files, images, PDFs, notebooks";
    }
    bool is_concurrency_safe(const boost::json::object& input) const override { return true; }
    bool is_read_only(const boost::json::object& input) const override { return true; }
    std::optional<std::string> get_path(const boost::json::object& input) const override;

    /// The rules of the paths decide the call, the way the reference lets
    /// the file decide it.
    PermissionResult check_permissions(const boost::json::object& input,
                                       ToolUseContext& context) const override;

    /// True when the model of the session reads the PDF files.
    ///
    /// The reference leaves the line about the PDF files out of the prompt
    /// of the models that do not.
    void set_pdf_supported(bool supported) { pdf_supported_ = supported; }

private:
    std::shared_ptr<FileStateTracker> tracker_;

    /// True when the model of the session reads the PDF files.
    bool pdf_supported_ = true;
};

/// Writes a file, creating it when it does not exist.
class WriteTool : public Tool {
public:
    explicit WriteTool(std::shared_ptr<FileStateTracker> tracker);

    std::string_view name() const override { return "Write"; }
    std::string description() const override { return "Write a file to the local filesystem."; }
    std::string prompt() const override;
    boost::json::object input_schema() const override;
    ToolResult call(const boost::json::object& input, ToolUseContext& context) override;

    bool is_concurrency_safe(const boost::json::object& input) const override { return false; }
    std::optional<std::string> get_path(const boost::json::object& input) const override;

    /// The rules of the paths decide the call, the way the reference lets
    /// the file decide it.
    PermissionResult check_permissions(const boost::json::object& input,
                                       ToolUseContext& context) const override;

private:
    std::shared_ptr<FileStateTracker> tracker_;
};

/// Performs an exact string replacement in a file.
class EditTool : public Tool {
public:
    explicit EditTool(std::shared_ptr<FileStateTracker> tracker);

    std::string_view name() const override { return "Edit"; }
    std::string description() const override { return "A tool for editing files"; }
    std::string prompt() const override;
    boost::json::object input_schema() const override;
    ToolResult call(const boost::json::object& input, ToolUseContext& context) override;

    bool is_concurrency_safe(const boost::json::object& input) const override { return false; }
    std::optional<std::string> get_path(const boost::json::object& input) const override;

    /// The rules of the paths decide the call, the way the reference lets
    /// the file decide it.
    PermissionResult check_permissions(const boost::json::object& input,
                                       ToolUseContext& context) const override;

private:
    std::shared_ptr<FileStateTracker> tracker_;
};

} // namespace cc::tools
