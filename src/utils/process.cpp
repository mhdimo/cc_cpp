#include "utils/process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern char** environ;

namespace cc {
namespace {

/// The number of seconds a process may run when it does not say otherwise.
constexpr int kDefaultTimeoutSeconds = 60;

/// Makes `fd` non-blocking, so that reading from it never blocks.
void set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/// Kills the whole group of a process, so that what it started goes with it.
///
/// The group is tried first, and the process itself when the group is gone
/// already — the process may have died before it made the group.
void kill_group(pid_t pid) {
    if (pid <= 0) return;
    if (kill(-pid, SIGKILL) != 0) kill(pid, SIGKILL);
}

/// Reads whatever is available from `fd` into `out`.
///
/// Returns true when the pipe has reached its end and no more data will
/// come.
bool drain_pipe(int fd, std::string& out) {
    char buffer[4096];
    for (;;) {
        const ssize_t count = ::read(fd, buffer, sizeof buffer);
        if (count > 0) {
            out.append(buffer, static_cast<size_t>(count));
            continue;
        }
        if (count == 0) return true;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
        return true;
    }
}

/// The environment a process runs with.
///
/// The current environment, with `overrides` applied on top of it. The
/// pointers handed to `execve` point into `entries`, so the object must not
/// be copied or modified afterwards.
struct ProcessEnvironment {
    /// The environment entries, in `NAME=value` form.
    std::vector<std::string> entries;
    /// The `execve` argument vector, terminated by a null pointer.
    std::vector<char*> pointers;

    /// Builds the environment.
    explicit ProcessEnvironment(const std::vector<std::pair<std::string, std::string>>& overrides) {
        for (char** variable = environ; variable != nullptr && *variable != nullptr; ++variable) {
            const std::string entry = *variable;
            const size_t equals = entry.find('=');
            const std::string name = equals == std::string::npos ? entry : entry.substr(0, equals);
            bool overridden = false;
            for (const auto& [key, value] : overrides) {
                if (key == name) {
                    overridden = true;
                    break;
                }
            }
            if (!overridden) entries.push_back(entry);
        }
        for (const auto& [key, value] : overrides) entries.push_back(key + "=" + value);

        pointers.reserve(entries.size() + 1);
        for (auto& entry : entries) pointers.push_back(const_cast<char*>(entry.c_str()));
        pointers.push_back(nullptr);
    }
};

} // namespace

ProcessOutcome run_process(const std::string& command, const std::string& cwd,
                           const std::string& stdin_text, int timeout_seconds) {
    ProcessOutcome outcome;

    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(in_pipe) != 0) {
        outcome.failed = true;
        outcome.exit_code = 127;
        outcome.err = std::string("process: cannot create pipe: ") + std::strerror(errno);
        return outcome;
    }
    if (pipe(out_pipe) != 0) {
        outcome.failed = true;
        outcome.exit_code = 127;
        outcome.err = std::string("process: cannot create pipe: ") + std::strerror(errno);
        close(in_pipe[0]);
        close(in_pipe[1]);
        return outcome;
    }
    if (pipe(err_pipe) != 0) {
        outcome.failed = true;
        outcome.exit_code = 127;
        outcome.err = std::string("process: cannot create pipe: ") + std::strerror(errno);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return outcome;
    }

    // The process runs with the current environment, plus the project
    // directory.
    std::vector<std::pair<std::string, std::string>> overrides;
    if (!cwd.empty()) overrides.emplace_back("CLAUDE_PROJECT_DIR", cwd);
    ProcessEnvironment environment(overrides);

    std::vector<std::string> arguments = {"sh", "-c", command};
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    // Writing to a process that exits without reading its input would raise
    // SIGPIPE, so it is ignored for the duration of the run.
    struct sigaction ignore_action {};
    ignore_action.sa_handler = SIG_IGN;
    sigemptyset(&ignore_action.sa_mask);
    struct sigaction old_action {};
    sigaction(SIGPIPE, &ignore_action, &old_action);

    const pid_t pid = fork();
    if (pid < 0) {
        sigaction(SIGPIPE, &old_action, nullptr);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        outcome.failed = true;
        outcome.exit_code = 127;
        outcome.err = std::string("process: cannot fork: ") + std::strerror(errno);
        return outcome;
    }

    if (pid == 0) {
        // The child gets the default SIGPIPE handling back.
        struct sigaction default_action {};
        default_action.sa_handler = SIG_DFL;
        sigemptyset(&default_action.sa_mask);
        sigaction(SIGPIPE, &default_action, nullptr);

        // The child leads a group of its own, so that the whole of what it
        // starts is killed with it. The shell of the command forks what it
        // runs, and the child of the shell is the one that holds the pipes
        // open — killing only the child of the fork would leave it behind,
        // an orphan that runs for ever.
        setpgid(0, 0);

        if (dup2(in_pipe[0], STDIN_FILENO) < 0) _exit(127);
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0) _exit(127);
        if (dup2(err_pipe[1], STDERR_FILENO) < 0) _exit(127);

        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);

        if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
            const std::string message =
                "process: cannot enter " + cwd + ": " + std::strerror(errno) + "\n";
            const ssize_t ignored = ::write(STDERR_FILENO, message.data(), message.size());
            static_cast<void>(ignored);
            _exit(127);
        }

        execve("/bin/sh", argv.data(), environment.pointers.data());

        const std::string message =
            "process: cannot run /bin/sh: " + std::string(std::strerror(errno)) + "\n";
        const ssize_t ignored = ::write(STDERR_FILENO, message.data(), message.size());
        static_cast<void>(ignored);
        _exit(127);
    }

    // The parent: the child owns the other end of every pipe.
    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);

    // The group is made here as well, so that the parent and the child
    // agree about it however quickly the child runs. The child may already
    // be gone, which is no harm.
    setpgid(pid, pid);

    set_nonblocking(in_pipe[1]);
    set_nonblocking(out_pipe[0]);
    set_nonblocking(err_pipe[0]);

    const int timeout = timeout_seconds > 0 ? timeout_seconds : kDefaultTimeoutSeconds;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);

    size_t written = 0;
    bool stdin_open = true;
    bool stdout_open = true;
    bool stderr_open = true;

    while (stdin_open || stdout_open || stderr_open) {
        struct pollfd fds[3];
        int stdin_index = -1;
        int stdout_index = -1;
        int stderr_index = -1;
        int count = 0;

        if (stdin_open) {
            fds[count] = {in_pipe[1], POLLOUT, 0};
            stdin_index = count++;
        }
        if (stdout_open) {
            fds[count] = {out_pipe[0], POLLIN, 0};
            stdout_index = count++;
        }
        if (stderr_open) {
            fds[count] = {err_pipe[0], POLLIN, 0};
            stderr_index = count++;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            outcome.timed_out = true;
            break;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const int wait_ms = static_cast<int>(std::min<int64_t>(remaining < 1 ? 1 : remaining, 1000));

        const int ready = poll(fds, static_cast<nfds_t>(count), wait_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            outcome.err = std::string("process: cannot wait for the process: ") + std::strerror(errno);
            break;
        }
        if (ready == 0) continue;

        if (stdin_index >= 0 && fds[stdin_index].revents != 0) {
            const short events = fds[stdin_index].revents;
            if ((events & POLLOUT) != 0 && written < stdin_text.size()) {
                const ssize_t count_written =
                    ::write(in_pipe[1], stdin_text.data() + written, stdin_text.size() - written);
                if (count_written > 0) {
                    written += static_cast<size_t>(count_written);
                } else if (count_written < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                           errno != EINTR) {
                    written = stdin_text.size();
                }
            }
            if (written >= stdin_text.size() || (events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                close(in_pipe[1]);
                stdin_open = false;
            }
        }

        if (stdout_index >= 0 && fds[stdout_index].revents != 0) {
            if (drain_pipe(out_pipe[0], outcome.out)) {
                close(out_pipe[0]);
                stdout_open = false;
            }
        }

        if (stderr_index >= 0 && fds[stderr_index].revents != 0) {
            if (drain_pipe(err_pipe[0], outcome.err)) {
                close(err_pipe[0]);
                stderr_open = false;
            }
        }
    }

    if (outcome.timed_out) kill_group(pid);

    if (stdin_open) close(in_pipe[1]);
    if (stdout_open) {
        drain_pipe(out_pipe[0], outcome.out);
        close(out_pipe[0]);
    }
    if (stderr_open) {
        drain_pipe(err_pipe[0], outcome.err);
        close(err_pipe[0]);
    }

    // Wait for the process to finish, killing it when it overruns.
    int status = 0;
    bool reaped = false;
    for (;;) {
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            reaped = true;
            break;
        }
        if (waited < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            if (!outcome.timed_out) {
                outcome.timed_out = true;
                kill_group(pid);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    sigaction(SIGPIPE, &old_action, nullptr);

    if (reaped) {
        if (WIFEXITED(status)) {
            outcome.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            outcome.exit_code = 128 + WTERMSIG(status);
        }
    }
    return outcome;
}

} // namespace cc
