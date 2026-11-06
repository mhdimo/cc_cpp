#include "tools/impl/bash_tool.hpp"

#include "permissions/content_match.hpp"
#include "sandbox/profile.hpp"
#include "tools/tool_use_context.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cc::tools {
namespace {

/// The longest a command may run when the caller does not say otherwise.
constexpr int64_t kDefaultTimeoutMs = 120000;

/// The longest a command may run.
constexpr int64_t kMaxTimeoutMs = 600000;

/// The exit code reported for a command that ran out of time.
constexpr int kTimeoutExitCode = 124;

/// The number of background commands started in this session.
std::atomic<int> g_background_counter{0};

/// Formats a duration the way Claude Code does, e.g. `2m0s`.
std::string format_duration(int64_t ms) {
    if (ms < 60000) {
        if (ms <= 0) return "0s";
        return std::to_string(ms / 1000) + "s";
    }
    const int64_t minutes = ms / 60000;
    const int64_t seconds = (ms % 60000) / 1000;
    return std::to_string(minutes) + "m" + std::to_string(seconds) + "s";
}

/// Removes the leading lines that hold nothing but whitespace.
std::string strip_leading_blank_lines(const std::string& text) {
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t eol = text.find('\n', pos);
        const std::string_view line = eol == std::string::npos
                                          ? std::string_view(text).substr(pos)
                                          : std::string_view(text).substr(pos, eol - pos);
        if (!trim(line).empty()) break;
        if (eol == std::string::npos) return {};
        pos = eol + 1;
    }
    return text.substr(pos);
}

/// The directory background commands write their output to.
std::string background_output_dir() {
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) base = "/tmp";
    return (base / "claude-code-background").string();
}

/// Starts `command` without waiting for it and returns the path of the file
/// its output is written to.
ShellResult start_background_command(const std::string& command, const std::string& working_dir) {
    ShellResult result;

    const std::string directory = background_output_dir();
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);

    const int id = ++g_background_counter;
    const std::string output_path = directory + "/bash_" + std::to_string(id) + ".log";

    const int fd = ::open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        result.output = std::string("Failed to open the output file: ") + std::strerror(errno);
        result.exit_code = 1;
        return result;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fd);
        result.output = std::string("Failed to fork: ") + std::strerror(errno);
        result.exit_code = 1;
        return result;
    }

    if (pid == 0) {
        // Child: leave the session, then run the command with its output
        // going to the file.
        ::setsid();
        ::dup2(fd, STDOUT_FILENO);
        ::dup2(fd, STDERR_FILENO);
        ::close(fd);
        if (!working_dir.empty() && ::chdir(working_dir.c_str()) != 0) ::_exit(127);
        ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }

    ::close(fd);
    result.output = "Command running in background with ID: bash_" + std::to_string(id) +
                    ". Output is being written to: " + output_path;
    result.exit_code = 0;
    return result;
}

} // namespace

namespace {

/// Runs the command, reporting the output as it arrives.
ShellResult run_command(const std::string& command, const std::string& working_dir,
                         int64_t timeout_ms, const std::function<void(const std::string&)>& on_output) {
    ShellResult result;

    int fds[2] = {-1, -1};
    int err_fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        result.output = std::string("Failed to create pipe: ") + std::strerror(errno);
        result.exit_code = 1;
        return result;
    }
    if (::pipe(err_fds) != 0) {
        result.output = std::string("Failed to create pipe: ") + std::strerror(errno);
        result.exit_code = 1;
        ::close(fds[0]);
        ::close(fds[1]);
        return result;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        result.output = std::string("Failed to fork: ") + std::strerror(errno);
        result.exit_code = 1;
        ::close(fds[0]);
        ::close(fds[1]);
        ::close(err_fds[0]);
        ::close(err_fds[1]);
        return result;
    }

    if (pid == 0) {
        // Child: lead a process group of its own so the timeout can take the
        // whole tree down, then run the command. The standard error keeps
        // a pipe of its own, the way Claude Code keeps it apart.
        ::setpgid(0, 0);
        ::close(fds[0]);
        ::close(err_fds[0]);
        if (::dup2(fds[1], STDOUT_FILENO) < 0) ::_exit(127);
        if (::dup2(err_fds[1], STDERR_FILENO) < 0) ::_exit(127);
        ::close(fds[1]);
        ::close(err_fds[1]);
        if (!working_dir.empty() && ::chdir(working_dir.c_str()) != 0) ::_exit(127);
        ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }

    // Parent: the child may already have made itself a group leader, so
    // setting it here too is harmless and closes the race.
    ::setpgid(pid, pid);
    ::close(fds[1]);
    ::close(err_fds[1]);

    const auto start = std::chrono::steady_clock::now();
    std::string output;
    std::string error_output;
    char buffer[4096];
    bool timed_out = false;

    // The two pipes are read together, so that neither of them holds the
    // command up.
    struct pollfd pfds[2] {};
    pfds[0].fd = fds[0];
    pfds[0].events = POLLIN;
    pfds[1].fd = err_fds[0];
    pfds[1].events = POLLIN;
    bool out_open = true;
    bool err_open = true;

    while (out_open || err_open) {
        int wait_ms = -1;
        if (timeout_ms > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
            const int64_t remaining = timeout_ms - elapsed;
            if (remaining <= 0) {
                timed_out = true;
                break;
            }
            wait_ms = static_cast<int>(std::min<int64_t>(remaining, 200));
        }

        const int ready = ::poll(pfds, 2, wait_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;

        for (int i = 0; i < 2; ++i) {
            if ((pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;

            const ssize_t got = ::read(pfds[i].fd, buffer, sizeof(buffer));
            if (got < 0) {
                if (errno == EINTR) continue;
                if (i == 0) {
                    out_open = false;
                } else {
                    err_open = false;
                }
                continue;
            }
            if (got == 0) {
                if (i == 0) {
                    out_open = false;
                } else {
                    err_open = false;
                }
                continue;
            }

            const std::string chunk(buffer, static_cast<size_t>(got));
            if (i == 0) {
                output.append(chunk);
            } else {
                error_output.append(chunk);
            }
            if (on_output) on_output(chunk);
        }
    }

    if (timed_out) {
        ::kill(-pid, SIGKILL);
        ::kill(pid, SIGKILL);
        // Take whatever the command wrote before it was killed.
        for (int i = 0; i < 2; ++i) {
            for (;;) {
                const ssize_t got = ::read(pfds[i].fd, buffer, sizeof(buffer));
                if (got <= 0) break;
                const std::string chunk(buffer, static_cast<size_t>(got));
                if (i == 0) {
                    output.append(chunk);
                } else {
                    error_output.append(chunk);
                }
                if (on_output) on_output(chunk);
            }
        }
    }

    ::close(fds[0]);
    ::close(err_fds[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    result.output = std::move(output);
    result.error_output = std::move(error_output);
    result.timed_out = timed_out;
    if (timed_out) {
        result.exit_code = kTimeoutExitCode;
    } else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
        result.interrupted = false;
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
        result.interrupted = true;
    }
    return result;
}

} // namespace

ShellResult run_shell_command(const std::string& command, const std::string& working_dir,
                              int64_t timeout_ms) {
    return run_command(command, working_dir, timeout_ms, {});
}

ShellResult run_shell_command_streaming(const std::string& command, const std::string& working_dir,
                                        int64_t timeout_ms,
                                        const std::function<void(const std::string&)>& on_output) {
    return run_command(command, working_dir, timeout_ms, on_output);
}

ShellSession::ShellSession(std::string working_dir) : working_dir_(std::move(working_dir)) {
    cwd_ = working_dir_;
    start();
}

ShellSession::~ShellSession() { stop(); }

void ShellSession::start() {
    if (pid_ > 0) return;

    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (::pipe(in_pipe) != 0) return;
    if (::pipe(out_pipe) != 0) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        return;
    }
    if (::pipe(err_pipe) != 0) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        return;
    }

    if (pid == 0) {
        // The shell is its own group, so that a command that is stopped
        // takes nothing else with it.
        ::setsid();

        ::dup2(in_pipe[0], STDIN_FILENO);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);

        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);

        if (!working_dir_.empty() && ::chdir(working_dir_.c_str()) != 0) {
            ::_exit(127);
        }

        // A plain shell: no profile, no history, nothing of its own. The
        // commands of the session are the only thing it reads.
        ::execl("/bin/bash", "bash", "--norc", "--noprofile", static_cast<char*>(nullptr));
        ::_exit(127);
    }

    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);

    stdin_fd_ = in_pipe[1];
    stdout_fd_ = out_pipe[0];
    stderr_fd_ = err_pipe[0];
    pid_ = pid;
}

void ShellSession::stop() {
    if (stdin_fd_ >= 0) {
        ::close(stdin_fd_);
        stdin_fd_ = -1;
    }
    if (stdout_fd_ >= 0) {
        ::close(stdout_fd_);
        stdout_fd_ = -1;
    }
    if (stderr_fd_ >= 0) {
        ::close(stderr_fd_);
        stderr_fd_ = -1;
    }
    if (pid_ > 0) {
        ::kill(-pid_, SIGKILL);
        ::kill(pid_, SIGKILL);
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
    }
}

bool ShellSession::in_allowed_directory(const std::string& path) const {
    std::error_code error;
    const std::filesystem::path target = std::filesystem::weakly_canonical(path, error);
    if (error) return false;

    const std::vector<std::string> roots = [&] {
        std::vector<std::string> out;
        out.push_back(working_dir_);
        for (const auto& directory : allowed_directories_) out.push_back(directory);
        return out;
    }();

    for (const std::string& root : roots) {
        if (root.empty()) continue;
        std::error_code root_error;
        const std::filesystem::path base = std::filesystem::weakly_canonical(root, root_error);
        if (root_error) continue;

        // The folder itself and everything under it.
        if (target == base) return true;
        const std::string base_text = base.string();
        const std::string target_text = target.string();
        if (target_text.size() > base_text.size() && target_text.compare(0, base_text.size(), base_text) == 0 &&
            target_text[base_text.size()] == '/') {
            return true;
        }
    }
    return false;
}

void ShellSession::reset_cwd() {
    if (cwd_ == working_dir_) return;

    // The shell is put back into the folder of the session.
    if (pid_ > 0 && stdin_fd_ >= 0) {
        const std::string command = "cd " + working_dir_ + "\n";
        const ssize_t written = ::write(stdin_fd_, command.data(), command.size());
        (void)written;
    }
    cwd_ = working_dir_;
}

ShellResult ShellSession::run(const std::string& command, int64_t timeout_ms,
                              const std::function<void(const std::string&)>& on_output) {
    ShellResult result;

    if (pid_ <= 0) {
        // The shell died with the last command: another one is started for
        // this command.
        start();
    }
    if (pid_ <= 0) {
        // There is no shell: the command runs on its own, the way it ran
        // before the session had one.
        return run_shell_command(command, cwd_, timeout_ms);
    }

    const int counter = ++counter_;
    const std::string mark = "__CC_END__" + std::to_string(counter) + "__";

    // The command is followed by the mark that says it is done, and by the
    // folder the shell is in afterwards.
    std::string payload = command;
    if (!payload.empty() && payload.back() != '\n') payload += '\n';
    payload += "__cc_status=$?\n";
    payload += "printf '" + mark + "%d__%s\\n' \"$__cc_status\" \"$PWD\"\n";

    size_t written = 0;
    while (written < payload.size()) {
        const ssize_t count = ::write(stdin_fd_, payload.data() + written, payload.size() - written);
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        written += static_cast<size_t>(count);
    }

    const auto start = std::chrono::steady_clock::now();
    std::string output;
    std::string error_output;
    bool out_open = true;
    bool err_open = true;
    bool timed_out = false;
    bool finished = false;

    struct pollfd pfds[2] {};
    pfds[0].fd = stdout_fd_;
    pfds[0].events = POLLIN;
    pfds[1].fd = stderr_fd_;
    pfds[1].events = POLLIN;

    char buffer[4096];

    while (!finished && (out_open || err_open)) {
        int wait_ms = 100;
        if (timeout_ms > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
            const int64_t remaining = timeout_ms - elapsed;
            if (remaining <= 0) {
                timed_out = true;
                break;
            }
            wait_ms = static_cast<int>(std::min<int64_t>(remaining, 100));
        }

        const int ready = ::poll(pfds, 2, wait_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;

        for (int i = 0; i < 2; ++i) {
            if ((pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;

            const ssize_t got = ::read(pfds[i].fd, buffer, sizeof(buffer));
            if (got <= 0) {
                if (got < 0 && errno == EINTR) continue;
                if (i == 0) {
                    out_open = false;
                } else {
                    err_open = false;
                }
                continue;
            }

            const std::string chunk(buffer, static_cast<size_t>(got));
            if (i == 0) {
                // The mark is at the end of the answer of the command; it is
                // taken away before the user and the model see the output.
                const size_t at = chunk.find(mark);
                if (at != std::string::npos) {
                    output.append(chunk, 0, at);
                    const std::string tail = chunk.substr(at + mark.size());
                    const size_t end = tail.find('\n');
                    const std::string fields = tail.substr(0, end);
                    const size_t first = fields.find("__");
                    if (first != std::string::npos) {
                        const std::string status_text = fields.substr(0, first);
                        const std::string path = fields.substr(first + 2);
                        result.exit_code = std::atoi(status_text.c_str());
                        if (!path.empty()) cwd_ = path;
                    }
                    finished = true;
                    break;
                }
                output.append(chunk);
            } else {
                error_output.append(chunk);
            }
            if (on_output) on_output(chunk);
        }
    }

    // The standard error that came after the mark is taken too, the way
    // the reference takes what is left of the stream.
    if (finished && err_open) {
        struct pollfd err_poll {};
        err_poll.fd = stderr_fd_;
        err_poll.events = POLLIN;
        const int ready = ::poll(&err_poll, 1, 50);
        if (ready > 0) {
            for (;;) {
                const ssize_t got = ::read(stderr_fd_, buffer, sizeof(buffer));
                if (got <= 0) break;
                const std::string chunk(buffer, static_cast<size_t>(got));
                error_output.append(chunk);
                if (on_output) on_output(chunk);
            }
        }
    }

    if (timed_out) {
        // The command is stopped, and the shell is started again for the
        // next one.
        stop();
        result.timed_out = true;
        result.interrupted = true;
        result.exit_code = kTimeoutExitCode;
        result.output = std::move(output);
        result.error_output = std::move(error_output);
        return result;
    }

    if (!finished) {
        // The shell died without answering — the command was `exit`, or it
        // killed the shell. Its own exit code is the one of the command,
        // and a new shell is started for the next command.
        int status = 0;
        if (pid_ > 0) {
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
        if (WIFEXITED(status)) {
            result.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.exit_code = 128 + WTERMSIG(status);
            result.interrupted = true;
        }
        stop();
        result.output = std::move(output);
        result.error_output = std::move(error_output);
        return result;
    }

    // The command left the folder of the session: the shell is put back,
    // and the model is told about it, the way the reference tells it.
    if (maintain_project_working_dir_) {
        if (cwd_ != working_dir_) reset_cwd();
    } else if (cwd_ != working_dir_ && !in_allowed_directory(cwd_)) {
        reset_cwd();
        if (!error_output.empty()) error_output += '\n';
        error_output += "Shell cwd was reset to " + working_dir_;
    }

    result.output = std::move(output);
    result.error_output = std::move(error_output);
    return result;
}

BashTool::BashTool(ShellRunner runner) : runner_(std::move(runner)) {}

BashTool BashTool::streaming(StreamingShellRunner runner) {
    BashTool tool;
    tool.streaming_runner_ = std::move(runner);
    return tool;
}

boost::json::object BashTool::input_schema() const {
    return schema_object(
        {
            {"command", schema_string("The command to execute")},
            {"timeout", schema_number("Optional timeout in milliseconds (max 600000)")},
            {"description",
             schema_string(
                 "Clear, concise description of what this command does in active voice. Never use "
                 "words like \"complex\" or \"risk\" in the description - just describe what it "
                 "does.\n\n"
                 "For simple commands (git, npm, standard CLI tools), keep it brief (5-10 "
                 "words):\n"
                 "- ls → \"List files in current directory\"\n"
                 "- git status → \"Show working tree status\"\n"
                 "- npm install → \"Install package dependencies\"\n\n"
                 "For commands that are harder to parse at a glance (piped commands, obscure "
                 "flags, etc.), add enough context to clarify what it does:\n"
                 "- find . -name \"*.tmp\" -exec rm {} \\; → \"Find and delete all .tmp files "
                 "recursively\"\n"
                 "- git reset --hard origin/main → \"Discard all local changes and match remote "
                 "main\"\n"
                 "- curl -s url | jq '.data[]' → \"Fetch JSON from URL and extract data array "
                 "elements\"")},
            {"run_in_background",
             schema_boolean("Set to true to run this command in the background. Use Read to read "
                            "the output later.")},
            {"dangerouslyDisableSandbox",
             schema_boolean("Set this to true to dangerously override sandbox mode and run commands "
                            "without sandboxing.")},
        },
        {"command"});
}

std::string BashTool::prompt() const {
    std::vector<std::string> section;
    if (sandbox_ != nullptr) section = sandbox::prompt_section(*sandbox_);
    return bash_tool_prompt(kMaxTimeoutMs, kDefaultTimeoutMs, section, include_git_instructions_);
}

std::optional<std::string> BashTool::permission_content(const boost::json::object& input) const {
    const std::string command = cc::trim(jget_string(input, "command"));
    if (command.empty()) return std::nullopt;
    return command;
}

PermissionResult BashTool::check_permissions(const boost::json::object& input,
                                             ToolUseContext& context) const {
    const std::string command = cc::trim(jget_string(input, "command"));

    const auto match = permissions::match_content_rules(context.permission_context, *this, input,
                                                        permissions::rule_roots(context));

    if (match.deny.has_value()) {
        PermissionResult result;
        result.behavior = PermissionBehaviorKind::Deny;
        result.message = "Permission to use Bash with command " + command + " has been denied.";
        result.decision_reason = permissions::rule_decision_reason(*match.deny);
        return result;
    }

    if (match.ask.has_value()) {
        PermissionResult result;
        result.behavior = PermissionBehaviorKind::Ask;
        result.message = create_permission_request_message("Bash");
        result.decision_reason = permissions::rule_decision_reason(*match.ask);
        return result;
    }

    if (match.allow.has_value()) {
        PermissionResult result;
        result.behavior = PermissionBehaviorKind::Allow;
        result.updated_input = input;
        result.decision_reason = permissions::rule_decision_reason(*match.allow);
        return result;
    }

    PermissionResult result;
    result.behavior = PermissionBehaviorKind::Passthrough;
    result.message = create_permission_request_message("Bash");
    return result;
}

ToolResult BashTool::call(const boost::json::object& input, ToolUseContext& context) {
    std::string command = jget_string(input, "command");

    int64_t timeout = jget_int(input, "timeout", 0);
    if (timeout <= 0) timeout = kDefaultTimeoutMs;
    if (timeout > kMaxTimeoutMs) timeout = kMaxTimeoutMs;

    const std::string working_dir = context.options.cwd.empty() ? cc::cwd() : context.options.cwd;

    ToolResult result;

    // The command runs inside the sandbox, unless the model asks for it to
    // run outside of it and the policy allows it.
    const bool disable_sandbox = jget_bool(input, "dangerouslyDisableSandbox", false);
    bool sandbox_overridden = false;
    if (sandbox_ != nullptr) {
        if (sandbox_->should_use_sandbox(command, disable_sandbox)) {
            const std::string wrapped = sandbox::sandboxed_command(*sandbox_, command, working_dir);
            if (!wrapped.empty()) command = wrapped;
        } else if (disable_sandbox && sandbox_->is_sandboxing_enabled() &&
                   sandbox_->are_unsandboxed_commands_allowed()) {
            sandbox_overridden = true;
        }
    }

    if (jget_bool(input, "run_in_background", false)) {
        const ShellResult shell = start_background_command(command, working_dir);
        result.content = shell.output;
        result.is_error = shell.exit_code != 0;
        return result;
    }

    // The output is reported as it arrives, so the UI can show the command
    // running.
    std::string streamed;
    const auto on_output = [&streamed, &context](const std::string& chunk) {
        streamed += chunk;
        if (context.on_progress) {
            context.on_progress(chunk);
        }
    };

    ShellResult shell;
    if (session_ != nullptr) {
        // The shell of the session stays alive, so that a `cd` of the
        // command moves the folder the session is in, the way the reference
        // does it.
        shell = session_->run(command, timeout, on_output);
    } else if (streaming_runner_) {
        shell = streaming_runner_(command, working_dir, timeout, on_output);
    } else if (runner_) {
        shell = runner_(command, working_dir, timeout);
    } else {
        shell = run_shell_command_streaming(command, working_dir, timeout, on_output);
    }

    std::string output = strip_leading_blank_lines(shell.output);
    output = trim_end(output);
    std::string error_output = trim_end(strip_leading_blank_lines(shell.error_output));

    // The two streams are kept apart, so that the UI can show the standard
    // error in its own colour.
    boost::json::object payload;
    payload["stdout"] = output;
    payload["stderr"] = error_output;
    if (sandbox_overridden) payload["dangerouslyDisableSandbox"] = true;

    if (shell.timed_out) {
        result.is_error = true;
        result.content = output;
        if (!error_output.empty()) {
            if (!result.content.empty()) result.content += "\n";
            result.content += error_output;
        }
        if (!result.content.empty()) result.content += "\n";
        result.content += "Command timed out after " + format_duration(timeout);
        result.tool_use_result = boost::json::value(std::move(payload));
        return result;
    }

    if (shell.exit_code != 0) {
        result.is_error = true;
        result.content = output;
        if (!error_output.empty()) {
            if (!result.content.empty()) result.content += "\n";
            result.content += error_output;
        }
        if (!result.content.empty()) result.content += "\n";
        result.content += "Exit code " + std::to_string(shell.exit_code);
        result.tool_use_result = boost::json::value(std::move(payload));
        return result;
    }

    result.content = output;
    if (!error_output.empty()) {
        if (!result.content.empty()) result.content += "\n";
        result.content += error_output;
    }
    result.tool_use_result = boost::json::value(std::move(payload));
    return result;
}

} // namespace cc::tools
