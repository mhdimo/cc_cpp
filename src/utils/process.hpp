#pragma once

#include <string>

namespace cc {

/// What a child process produced.
struct ProcessOutcome {
    /// Everything the process wrote to its standard output.
    std::string out;
    /// Everything the process wrote to its standard error.
    std::string err;
    /// The process's exit code, or 128 plus the signal that killed it.
    int exit_code = 0;
    /// True when the process was killed because it ran for too long.
    bool timed_out = false;
    /// True when the process could not be started at all.
    bool failed = false;
};

/// Runs `command` through `/bin/sh -c`.
///
/// The command runs in `cwd`, with `stdin_text` on its standard input, and
/// with `CLAUDE_PROJECT_DIR` set to `cwd` in its environment. Its standard
/// output and standard error are collected, and it is killed when it runs
/// for longer than `timeout_seconds`.
///
/// A `timeout_seconds` of zero or less means the process may run for the
/// default of sixty seconds.
ProcessOutcome run_process(const std::string& command, const std::string& cwd,
                           const std::string& stdin_text, int timeout_seconds);

} // namespace cc
