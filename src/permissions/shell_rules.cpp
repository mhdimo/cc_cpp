#include "permissions/shell_rules.hpp"

#include "utils/string.hpp"

#include <cctype>
#include <regex>

namespace cc::permissions {
namespace {

/// The variables that may be stripped from the front of a command.
///
/// These variables cannot execute code nor load a library, so a command that
/// carries one of them is the command it names.
///
/// The variables that name a library (`PATH`, `LD_PRELOAD`, `PYTHONPATH`,
/// `NODE_OPTIONS` and the like) are never in this list.
const std::vector<std::string>& safe_env_vars() {
    static const std::vector<std::string> vars = {
        // Go — the settings of the build and of the runtime.
        "GOEXPERIMENT",
        "GOOS",
        "GOARCH",
        "CGO_ENABLED",
        "GO111MODULE",
        // Rust — the logging and the backtraces.
        "RUST_BACKTRACE",
        "RUST_LOG",
        // Node — the name of the environment.
        "NODE_ENV",
        // Python — the flags of the behaviour.
        "PYTHONUNBUFFERED",
        "PYTHONDONTWRITEBYTECODE",
        // Pytest — the settings of the tests.
        "PYTEST_DISABLE_PLUGIN_AUTOLOAD",
        "PYTEST_DEBUG",
        // The keys of the API.
        "ANTHROPIC_API_KEY",
        // The locale and the encoding.
        "LANG",
        "LANGUAGE",
        "LC_ALL",
        "LC_CTYPE",
        "LC_TIME",
        "CHARSET",
        // The terminal and the display.
        "TERM",
        "COLORTERM",
        "NO_COLOR",
        "FORCE_COLOR",
        "TZ",
        // The colours of the tools.
        "LS_COLORS",
        "LSCOLORS",
        "GREP_COLOR",
        "GREP_COLORS",
        "GCC_COLORS",
        // The shape of the output.
        "TIME_STYLE",
        "BLOCK_SIZE",
        "BLOCKSIZE",
    };
    return vars;
}

/// The pattern of an environment variable in front of a command.
///
/// The value of the variable may be quoted, the way the reference reads it,
/// so that `FOO=bar bazel …` matches the pattern `bazel:*`.
const std::regex& env_var_pattern() {
    static const std::regex pattern(
        R"RX(^([A-Za-z_][A-Za-z0-9_]*(?:\[[^\]]*\])?)\+?=)RX"
        R"RX((?:'[^'\n\r]*'|)RX"
        R"RX("(?:\\.|[^"$`\\\n\r])*"|)RX"
        R"RX(\\.|[^ \t\n\r$`;|&()<>\\'"])*[ \t]+)RX");
    return pattern;
}

/// The narrower pattern of an environment variable that may be stripped.
///
/// Only the values that hold nothing but safe characters are read, so that
/// the command that follows is the one that is run.
const std::regex& safe_env_var_pattern() {
    static const std::regex pattern(R"RX(^([A-Za-z_][A-Za-z0-9_]*)=([A-Za-z0-9_./:-]+)[ \t]+)RX");
    return pattern;
}

/// The wrappers that run another command and are safe to strip.
///
/// The patterns are the ones of the reference.
const std::vector<std::regex>& safe_wrapper_patterns() {
    static const std::vector<std::regex> patterns = {
        std::regex(
            R"RX(^timeout[ \t]+(?:(?:--(?:foreground|preserve-status|verbose)|)RX"
            R"RX(--(?:kill-after|signal)=[A-Za-z0-9_.+-]+|)RX"
            R"RX(--(?:kill-after|signal)[ \t]+[A-Za-z0-9_.+-]+|)RX"
            R"RX(-v|-[ks][ \t]+[A-Za-z0-9_.+-]+|)RX"
            R"RX(-[ks][A-Za-z0-9_.+-]+)[ \t]+)*)RX"
            R"RX((?:--[ \t]+)?\d+(?:\.\d+)?[smhd]?[ \t]+)RX"),
        std::regex(R"RX(^time[ \t]+(?:--[ \t]+)?)RX"),
        std::regex(R"RX(^nice(?:[ \t]+-n[ \t]+-?\d+|[ \t]+-\d+)?[ \t]+(?:--[ \t]+)?)RX"),
        std::regex(R"RX(^stdbuf(?:[ \t]+-[ioe][LN0-9]+)+[ \t]+(?:--[ \t]+)?)RX"),
        std::regex(R"RX(^nohup[ \t]+(?:--[ \t]+)?)RX"),
    };
    return patterns;
}

/// The lines of a command that hold something.
///
/// The comment lines and the empty ones are left out, the way the reference
/// leaves them out before it strips anything.
std::string strip_comment_lines(std::string_view command) {
    std::vector<std::string> kept;
    for (const std::string& line : cc::split_lines(command)) {
        const std::string trimmed = cc::trim(line);
        if (trimmed.empty()) continue;
        if (cc::starts_with(trimmed, "#")) continue;
        kept.push_back(line);
    }
    if (kept.empty()) return std::string(command);
    return cc::join(kept, "\n");
}

/// Removes the whitespace at the end of the text.
void trim_end_in_place(std::string& text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
}

/// True when the character ends a word of a command.
bool ends_word(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '>' || c == '<' || c == '|' || c == '&' ||
           c == ';';
}

} // namespace

std::optional<std::string> extract_prefix(std::string_view rule) {
    if (rule.size() < 3) return std::nullopt;
    if (rule.compare(rule.size() - 2, 2, ":*") != 0) return std::nullopt;
    return std::string(rule.substr(0, rule.size() - 2));
}

bool has_wildcards(std::string_view pattern) {
    // The `:*` at the end is the old shape of a prefix rule, not a wildcard.
    if (cc::ends_with(pattern, ":*")) return false;

    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '*') continue;

        // The asterisk is escaped when an odd number of backslashes comes
        // before it.
        size_t backslashes = 0;
        size_t j = i;
        while (j > 0 && pattern[j - 1] == '\\') {
            ++backslashes;
            --j;
        }
        if (backslashes % 2 == 0) return true;
    }
    return false;
}

bool matches_wildcard(std::string_view pattern, std::string_view command) {
    const std::string trimmed = cc::trim(pattern);

    // The escaped asterisks and backslashes are kept aside while the
    // pattern is turned into a regular expression.
    constexpr std::string_view kStar = "\x01";
    constexpr std::string_view kBackslash = "\x02";

    std::string processed;
    for (size_t i = 0; i < trimmed.size(); ++i) {
        if (trimmed[i] == '\\' && i + 1 < trimmed.size()) {
            if (trimmed[i + 1] == '*') {
                processed += kStar;
                ++i;
                continue;
            }
            if (trimmed[i + 1] == '\\') {
                processed += kBackslash;
                ++i;
                continue;
            }
        }
        processed += trimmed[i];
    }

    // The asterisks that are not escaped are the wildcards. They match the
    // newlines of the command too, the way the reference matches them with
    // the flag `s`.
    std::string expression;
    size_t stars = 0;
    for (const char c : processed) {
        if (c == '*') {
            expression += "[\\s\\S]*";
            ++stars;
            continue;
        }
        // The asterisks and the backslashes that were escaped become the
        // characters they stand for.
        if (c == kStar[0]) {
            expression += "\\*";
            continue;
        }
        if (c == kBackslash[0]) {
            expression += "\\\\";
            continue;
        }
        if (std::string_view{".+?^${}()|[]\\\"'"} .find(c) != std::string_view::npos) {
            expression += '\\';
        }
        expression += c;
    }

    // A pattern that ends with a space and a single wildcard matches the
    // command without the arguments too.
    constexpr std::string_view kTrailingWildcard = " [\\s\\S]*";
    if (stars == 1 && cc::ends_with(expression, kTrailingWildcard)) {
        expression = expression.substr(0, expression.size() - kTrailingWildcard.size()) +
                     "( [\\s\\S]*)?";
    }

    try {
        const std::regex matcher("^" + expression + "$", std::regex::ECMAScript);
        return std::regex_match(std::string(command), matcher);
    } catch (const std::regex_error&) {
        return false;
    }
}

std::vector<std::string> split_command(std::string_view command) {
    std::vector<std::string> parts;

    std::string current;
    char quote = '\0';
    for (size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];

        if (quote != '\0') {
            current += c;
            if (c == quote && (i == 0 || command[i - 1] != '\\')) quote = '\0';
            continue;
        }

        if (c == '\'' || c == '"') {
            quote = c;
            current += c;
            continue;
        }

        if (c == '&' && i + 1 < command.size() && command[i + 1] == '&') {
            parts.push_back(current);
            current.clear();
            ++i;
            continue;
        }
        if (c == '|' && i + 1 < command.size() && command[i + 1] == '|') {
            parts.push_back(current);
            current.clear();
            ++i;
            continue;
        }
        if (c == ';') {
            parts.push_back(current);
            current.clear();
            continue;
        }

        current += c;
    }
    parts.push_back(current);

    return parts;
}

std::string strip_output_redirections(std::string_view command) {
    std::string out;
    char quote = '\0';

    for (size_t i = 0; i < command.size();) {
        const char c = command[i];

        if (quote != '\0') {
            out.push_back(c);
            if (c == quote && (i == 0 || command[i - 1] != '\\')) quote = '\0';
            ++i;
            continue;
        }

        if (c == '\'' || c == '"') {
            quote = c;
            out.push_back(c);
            ++i;
            continue;
        }

        if (c != '>') {
            out.push_back(c);
            ++i;
            continue;
        }

        // The operator of the redirection: `>`, `>>` or `>&`.
        size_t op_end = i + 1;
        if (op_end < command.size() && command[op_end] == '>') {
            ++op_end;
        } else if (op_end < command.size() && command[op_end] == '&') {
            ++op_end;
        }

        // The target of the redirection: the word that follows, or the
        // number of a file descriptor (`2>&1`).
        size_t target = op_end;
        while (target < command.size() && (command[target] == ' ' || command[target] == '\t')) {
            ++target;
        }
        size_t target_end = target;
        if (target_end < command.size() && command[target_end] == '&') {
            // The number of a file descriptor belongs to the operator.
            size_t digits = target_end + 1;
            while (digits < command.size() && command[digits] >= '0' && command[digits] <= '9') {
                ++digits;
            }
            target_end = digits > target_end + 1 ? digits : target_end;
        } else {
            // A word, with the quotes it may hold.
            char target_quote = '\0';
            while (target_end < command.size()) {
                const char t = command[target_end];
                if (target_quote != '\0') {
                    if (t == target_quote) target_quote = '\0';
                    ++target_end;
                    continue;
                }
                if (t == '\'' || t == '"') {
                    target_quote = t;
                    ++target_end;
                    continue;
                }
                if (ends_word(t)) break;
                ++target_end;
            }
        }

        // The number of the file descriptor is written before the operator
        // and belongs to it (`ls 2>file`).
        if (!out.empty() && out.back() >= '0' && out.back() <= '9') {
            size_t digits = out.size();
            while (digits > 0 && out[digits - 1] >= '0' && out[digits - 1] <= '9') --digits;
            out.erase(digits);
        }
        // The space that stood before the operator goes with it.
        trim_end_in_place(out);
        // What follows the redirection keeps a space of its own, so that the
        // words of the command do not run into one another.
        if (!out.empty() && target_end < command.size() && command[target_end] != ' ' &&
            command[target_end] != '\t') {
            out.push_back(' ');
        }

        i = target_end;
    }

    return cc::trim(out);
}

std::string strip_all_leading_env_vars(std::string_view command,
                                       const std::function<bool(std::string_view)>& blocklist) {
    std::string stripped(command);
    std::string previous;

    while (stripped != previous) {
        previous = stripped;
        stripped = strip_comment_lines(stripped);

        std::smatch match;
        if (!std::regex_search(stripped, match, env_var_pattern())) continue;
        if (blocklist && blocklist(match[1].str())) break;
        stripped = stripped.substr(match[0].length());
    }

    return cc::trim(stripped);
}

std::string strip_safe_wrappers(std::string_view command) {
    std::string stripped(command);
    std::string previous;

    // The variables that are safe to strip go first: in the shell, a
    // variable in front of a command is a variable of the environment.
    while (stripped != previous) {
        previous = stripped;
        stripped = strip_comment_lines(stripped);

        std::smatch match;
        if (!std::regex_search(stripped, match, safe_env_var_pattern())) continue;
        if (!is_safe_env_var(match[1].str())) continue;
        stripped = stripped.substr(match[0].length());
    }

    // The wrappers come next. A variable after a wrapper is the command
    // that is run, not a variable of the environment, so none is stripped
    // here.
    previous.clear();
    while (stripped != previous) {
        previous = stripped;
        stripped = strip_comment_lines(stripped);

        for (const std::regex& pattern : safe_wrapper_patterns()) {
            stripped = std::regex_replace(stripped, pattern, "");
        }
    }

    return cc::trim(stripped);
}

std::string strip_wrappers_only(std::string_view command) {
    std::string stripped(command);
    std::string previous;

    while (stripped != previous) {
        previous = stripped;
        stripped = strip_comment_lines(stripped);

        for (const std::regex& pattern : safe_wrapper_patterns()) {
            stripped = std::regex_replace(stripped, pattern, "");
        }
    }

    return cc::trim(stripped);
}

bool is_safe_env_var(std::string_view name) {
    for (const std::string& var : safe_env_vars()) {
        if (var == name) return true;
    }
    return false;
}

std::string command_prefix_rule(std::string_view command) {
    // The command is split into its words.
    std::vector<std::string> tokens;
    {
        std::string current;
        for (const char c : command) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!current.empty()) tokens.push_back(current);
                current.clear();
                continue;
            }
            current.push_back(c);
        }
        if (!current.empty()) tokens.push_back(current);
    }
    if (tokens.empty()) return {};

    // The variables at the start are skipped, but only the safe ones: a
    // rule that begins with an unsafe variable could never match, because
    // the matcher does not strip it.
    size_t index = 0;
    while (index < tokens.size()) {
        const std::string& token = tokens[index];
        const size_t equals = token.find('=');
        if (equals == std::string::npos || equals == 0) break;
        if (!is_safe_env_var(token.substr(0, equals))) return {};
        ++index;
    }

    const std::vector<std::string> rest(tokens.begin() + static_cast<long>(index), tokens.end());
    if (rest.empty()) return {};

    // The second word has to look like a subcommand — lower case, with
    // digits and dashes — and not a flag, a file name or a number.
    const auto is_subcommand = [](const std::string& word) {
        if (word.empty()) return false;
        if (word[0] < 'a' || word[0] > 'z') return false;
        for (size_t i = 1; i < word.size(); ++i) {
            const char c = word[i];
            const bool letter = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            const bool dash = c == '-' && i + 1 < word.size() && word[i + 1] >= 'a' &&
                             word[i + 1] <= 'z';
            if (!letter && !dash) return false;
            if (dash) ++i;
        }
        return true;
    };

    if (rest.size() >= 2 && is_subcommand(rest[1])) {
        return rest[0] + " " + rest[1];
    }

    // There is no subcommand: the first word alone is the prefix, unless
    // it is a shell of its own, which would allow anything.
    if (!is_subcommand(rest[0])) return {};
    static const char* kBareShells[] = {"sh", "bash", "zsh", "fish", "pkexec"};
    for (const char* shell : kBareShells) {
        if (rest[0] == shell) return {};
    }
    return rest[0];
}

ShellRule parse_shell_rule(std::string_view content) {
    if (auto prefix = extract_prefix(content)) {
        return ShellRule{ShellRuleType::Prefix, *prefix};
    }
    if (has_wildcards(content)) {
        return ShellRule{ShellRuleType::Wildcard, std::string(content)};
    }
    return ShellRule{ShellRuleType::Exact, std::string(content)};
}

} // namespace cc::permissions
