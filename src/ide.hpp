#pragma once

#include <optional>
#include <string>
#include <vector>

namespace cc {

/// One IDE that is running and that talks to Claude Code.
struct DetectedIde {
    /// The name of the IDE, e.g. `VS Code`.
    std::string name;

    /// The port the IDE listens on.
    int port = 0;

    /// The address the IDE serves its MCP server on.
    ///
    /// The address is empty when the IDE does not name one.
    std::string url;

    /// The token the IDE wants the session to answer with.
    std::string auth_token;

    /// The folders the IDE has open.
    std::vector<std::string> workspace_folders;

    /// True when the folder of the session is one of them.
    bool matches_cwd = false;

    /// True when the IDE answers on its port.
    bool is_running = false;
};

/// The IDEs that are running.
///
/// The IDEs leave a lock file in `~/.claude/ide`; the name of the file is
/// the port the IDE listens on. The IDEs whose folders do not hold the
/// folder of the session are reported as well, the way the reference
/// reports them.
std::vector<DetectedIde> detect_ides(const std::string& cwd);

/// The kind of the IDE the terminal of the session belongs to.
///
/// The reference reads it from what the IDE leaves in the environment: the
/// trace of Cursor, the askpass of the family of VS Code, the bundle of the
/// application on macOS, or the name of the program of the terminal. The
/// name is empty when the terminal belongs to no IDE that the reference
/// knows.
std::string terminal_ide_type();

/// True when the IDE is one of the family of VS Code.
///
/// The extension of Claude Code is installed in those, the way the
/// reference installs it.
bool is_vscode_ide(const std::string& ide_type);

/// The command that runs the command line of the IDE.
///
/// The name is empty when the IDE is not one of the family of VS Code.
std::string vscode_command(const std::string& ide_type);

/// The name the IDE carries the extension of Claude Code under.
constexpr std::string_view kExtensionId = "anthropic.claude-code";

/// The version of the extension of Claude Code that is installed.
///
/// The command of the IDE is asked about it, the way the reference asks. The
/// version is empty when the extension is not there.
std::optional<std::string> installed_extension_version(const std::string& command);

/// Installs the extension of Claude Code in the IDE.
///
/// The command of the IDE is asked to do it, the way the reference asks.
/// Returns false when the IDE refused, or when it is not there.
bool install_extension(const std::string& command);

/// True when `older` is an older version than `newer`.
///
/// The versions are the ones of the extension of the IDE: three numbers,
/// and whatever follows them.
bool older_version(const std::string& older, const std::string& newer);

} // namespace cc
