#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cc::permissions {

/// The prefix a shell command is saved under, the way the reference saves
/// it.
///
/// The reference takes the command and the subcommand that follows it —
/// `git commit -m "fix"` becomes `git commit` — and falls back to the first
/// word alone when there is no subcommand. A command that begins with a
/// variable that is not safe has no prefix at all, so that the rule that is
/// written is an exact one.
///
/// The empty string means there is no prefix.
std::string command_prefix_rule(std::string_view command);

/// The prefix of a rule of the old shape, `command:*`.
///
/// The empty optional means the rule is not of that shape.
std::optional<std::string> extract_prefix(std::string_view rule);

/// True when the pattern holds a wildcard that is not escaped.
///
/// The `:*` at the end is the old shape of a prefix rule, not a wildcard.
bool has_wildcards(std::string_view pattern);

/// True when the command matches the pattern of a wildcard rule.
///
/// The pattern is turned into a regular expression, the way the reference
/// turns it: every `*` matches any text, and the pattern has to match the
/// whole command. The pattern is trimmed first and the regular expression
/// matches the newlines too, the way the reference reads it.
bool matches_wildcard(std::string_view pattern, std::string_view command);

/// The parts of a compound command.
///
/// The command is split at the `&&`, `||` and `;`, so that a command that
/// is denied cannot hide behind another one.
std::vector<std::string> split_command(std::string_view command);

/// The command without its output redirections.
///
/// The redirections of the output (`> file`, `>> file` and `2>&1`) are taken
/// off the command, so that a rule of the shape `Bash(python:*)` matches
/// `python script.py > output.txt`.
std::string strip_output_redirections(std::string_view command);

/// Strips every environment variable that is in front of a command.
///
/// The variables are stripped whatever their names are, so that a denied
/// command stays denied behind `FOO=bar denied_command`. The variables whose
/// names the `blocklist` holds are left alone, and the stripping stops there.
std::string strip_all_leading_env_vars(std::string_view command,
                                       const std::function<bool(std::string_view)>& blocklist = {});

/// Strips the wrappers that run another command, and the variables that are
/// safe to strip.
///
/// The wrappers are `timeout`, `time`, `nice`, `stdbuf` and `nohup`; only
/// the variables that cannot execute code are stripped, the way the
/// reference strips them.
std::string strip_safe_wrappers(std::string_view command);

/// Strips the wrappers that run another command, and nothing else.
///
/// The environment variables are left alone: after a wrapper, a variable is
/// the command that is run, not a variable of the environment.
std::string strip_wrappers_only(std::string_view command);

/// True when the variable is one of the ones that may be stripped.
///
/// These variables cannot execute code nor load a library.
bool is_safe_env_var(std::string_view name);

/// The kind of a rule of a shell command.
enum class ShellRuleType {
    /// The command has to be the one of the rule.
    Exact,
    /// The command has to begin with the prefix of the rule.
    Prefix,
    /// The command has to match the pattern of the rule.
    Wildcard,
};

/// A rule of a shell command.
struct ShellRule {
    /// The kind of the rule.
    ShellRuleType type = ShellRuleType::Exact;

    /// The command, the prefix or the pattern of the rule.
    std::string text;
};

/// Reads a rule of a shell command.
///
/// The rule of the old shape, `command:*`, is a prefix; a rule that holds a
/// wildcard is a pattern; anything else is the command itself.
ShellRule parse_shell_rule(std::string_view content);

/// The way a command is matched against a rule.
enum class ShellMatchMode {
    /// The command has to be the one of the rule.
    Exact,
    /// The command may begin with the prefix of the rule.
    Prefix,
};

} // namespace cc::permissions
