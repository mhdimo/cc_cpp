#pragma once

#include "sandbox/manager.hpp"
#include "tools/impl/common.hpp"
#include "tools/tool.hpp"

#include <functional>

namespace cc::tools {

/// The result of running a shell command.
struct ShellResult {
    /// The command's standard output.
    std::string output;
    /// The command's standard error.
    std::string error_output;
    /// The command's exit code.
    int exit_code = 0;
    /// True when the command was stopped before it finished.
    bool interrupted = false;
    /// True when the command exceeded its timeout.
    bool timed_out = false;
};

/// Runs a shell command in a working directory.
///
/// The default implementation shells out to `/bin/sh -c` with stderr merged
/// into stdout, which is what Claude Code does.
using ShellRunner = std::function<ShellResult(const std::string& command,
                                              const std::string& working_dir, int64_t timeout_ms)>;

/// A shell that stays alive for the whole session.
///
/// The reference keeps one shell for the session, so that a `cd` of a
/// command moves the folder the session is in, the way the shell of a
/// person does. When the folder leaves the folder of the project, it is
/// put back and the model is told about it, the way the reference tells it.
class ShellSession {
public:
    /// Starts a shell in the folder of the session.
    explicit ShellSession(std::string working_dir);

    ~ShellSession();

    ShellSession(const ShellSession&) = delete;
    ShellSession& operator=(const ShellSession&) = delete;

    /// Runs one command in the shell and returns its result.
    ///
    /// The output is given to the callback as it arrives, the way the
    /// reference streams the output of a running command into the
    /// interface.
    ShellResult run(const std::string& command, int64_t timeout_ms,
                    const std::function<void(const std::string&)>& on_output = {});

    /// The folder the shell is in.
    const std::string& cwd() const { return cwd_; }

    /// The folder the session began in.
    const std::string& working_dir() const { return working_dir_; }

    /// The folders the session may work in, besides the folder of the
    /// session.
    ///
    /// A command may leave the folder of the session for one of them, the
    /// way the reference lets it.
    void set_allowed_directories(std::vector<std::string> directories) {
        allowed_directories_ = std::move(directories);
    }

    /// True when the folder of the session is kept whatever the commands
    /// do.
    ///
    /// The reference reads it from `CLAUDE_BASH_MAINTAIN_PROJECT_WORKING_DIR`.
    void set_maintain_project_working_dir(bool maintain) { maintain_project_working_dir_ = maintain; }

private:
    /// Starts the shell, if it is not running.
    void start();

    /// Stops the shell.
    void stop();

    /// Puts the shell back into the folder of the session.
    void reset_cwd();

    /// True when the folder is inside the folder of the session or inside
    /// one of the folders that were added to it.
    bool in_allowed_directory(const std::string& path) const;

    /// The folder the session began in.
    std::string working_dir_;

    /// The folder the shell is in.
    std::string cwd_;

    /// The folders the session may work in.
    std::vector<std::string> allowed_directories_;

    /// True when the folder of the session is kept whatever the commands do.
    bool maintain_project_working_dir_ = false;

    /// The process of the shell.
    int pid_ = -1;

    /// The ends of the pipes of the shell.
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    int stderr_fd_ = -1;

    /// The number of the commands that have been run, which makes the mark
    /// of each of them its own.
    int counter_ = 0;
};

/// A shell runner that reports the output as it arrives.
///
/// The callback is given each chunk of output, the way the reference streams
/// the output of a running command into the UI.
using StreamingShellRunner =
    std::function<ShellResult(const std::string& command, const std::string& working_dir,
                               int64_t timeout_ms, const std::function<void(const std::string&)>&)>;

/// The default shell runner.
ShellResult run_shell_command(const std::string& command, const std::string& working_dir,
                              int64_t timeout_ms);

/// The default shell runner, reporting the output as it arrives.
ShellResult run_shell_command_streaming(const std::string& command, const std::string& working_dir,
                                        int64_t timeout_ms,
                                        const std::function<void(const std::string&)>& on_output);

/// Executes a shell command and returns its output.
class BashTool : public Tool {
public:
    explicit BashTool(ShellRunner runner = {});

    /// Builds a tool whose runner reports the output as it arrives.
    static BashTool streaming(StreamingShellRunner runner);

    std::string_view name() const override { return "Bash"; }
    std::string description() const override { return "Run shell command"; }
    std::string prompt() const override;
    boost::json::object input_schema() const override;
    ToolResult call(const boost::json::object& input, ToolUseContext& context) override;

    bool is_concurrency_safe(const boost::json::object& input) const override { return true; }

    /// The command the model wants to run; the rules of the shell are
    /// matched against it.
    std::optional<std::string> permission_content(const boost::json::object& input) const override;

    /// The rules of the shell decide the call, the way the reference lets
    /// the command decide it.
    PermissionResult check_permissions(const boost::json::object& input,
                                       ToolUseContext& context) const override;

    /// Gives the tool the sandbox of the session.
    ///
    /// The commands run inside it, the way the reference holds them, and the
    /// prompt of the tool tells the model what the sandbox does.
    void set_sandbox(sandbox::Manager* manager) { sandbox_ = manager; }

    /// Gives the tool the shell of the session.
    ///
    /// The shell stays alive for the whole session, so that a `cd` of a
    /// command moves the folder the session is in, the way the reference
    /// does it.
    void set_shell_session(ShellSession* session) { session_ = session; }

    /// True when the instructions about git belong in the prompt.
    void set_include_git_instructions(bool included) { include_git_instructions_ = included; }

private:
    ShellRunner runner_;
    StreamingShellRunner streaming_runner_;

    /// The sandbox of the session, when the tool knows it.
    sandbox::Manager* sandbox_ = nullptr;

    /// The shell of the session, when the tool knows it.
    ShellSession* session_ = nullptr;

    /// True when the instructions about git belong in the prompt.
    bool include_git_instructions_ = true;
};

} // namespace cc::tools
