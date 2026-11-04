#pragma once

#include <stdexcept>
#include <string>

namespace cc {

/// Base class for every error the CLI raises deliberately.
class Error : public std::runtime_error {
public:
    explicit Error(const std::string& what) : std::runtime_error(what) {}
};

/// A tool invocation that failed in a way the model should see as a tool
/// result rather than a crash. Mirrors the TypeScript convention where a
/// tool returns `{ isError: true, content: "..." }`.
class ToolError : public Error {
public:
    ToolError(std::string message, std::string tool_name = "")
        : Error(std::move(message)), tool_name_(std::move(tool_name)) {}

    const std::string& tool_name() const noexcept { return tool_name_; }

private:
    std::string tool_name_;
};

/// The user declined a permission request.
class PermissionDenied : public Error {
public:
    explicit PermissionDenied(const std::string& what = "permission denied")
        : Error(what) {}
};

/// Configuration on disk is missing or malformed.
class ConfigError : public Error {
public:
    explicit ConfigError(const std::string& what) : Error(what) {}
};

/// The CLI was invoked with an argument combination that cannot be honoured.
class UsageError : public Error {
public:
    explicit UsageError(const std::string& what) : Error(what) {}
};

/// An upstream (Anthropic API, MCP server, network) call failed.
class ApiError : public Error {
public:
    ApiError(int status, std::string what)
        : Error(std::to_string(status) + ": " + what), status_(status) {}

    int status() const noexcept { return status_; }

private:
    int status_;
};

} // namespace cc
