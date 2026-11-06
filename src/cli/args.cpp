#include "cli/args.hpp"

#include "core/error.hpp"
#include "core/version.hpp"

#include "utils/string.hpp"

namespace cc::cli {
namespace {

/// Splits a comma-separated list, dropping empty entries.
std::vector<std::string> split_list(const std::string& value) {
    std::vector<std::string> result;
    for (const auto& part : split(value, ',')) {
        const std::string trimmed = trim(part);
        if (!trimmed.empty()) result.push_back(trimmed);
    }
    return result;
}

/// Reads the value that follows a flag.
const std::string& take_value(const std::vector<std::string>& argv, size_t& index,
                              const std::string& flag) {
    if (index + 1 >= argv.size()) {
        throw UsageError("Missing value for " + flag);
    }
    ++index;
    return argv[index];
}

} // namespace

Args parse_args(const std::vector<std::string>& argv) {
    Args args;
    std::vector<std::string> positional;

    for (size_t i = 0; i < argv.size(); ++i) {
        const std::string& arg = argv[i];

        if (arg == "--") {
            for (size_t j = i + 1; j < argv.size(); ++j) {
                positional.push_back(argv[j]);
            }
            break;
        }

        if (!starts_with(arg, "-") || arg == "-") {
            positional.push_back(arg);
            continue;
        }

        // Support `--flag=value`.
        std::string flag = arg;
        std::optional<std::string> inline_value;
        if (const size_t equals = arg.find('='); equals != std::string::npos) {
            flag = arg.substr(0, equals);
            inline_value = arg.substr(equals + 1);
        }

        auto value = [&](const std::string& name) -> std::string {
            if (inline_value) return *inline_value;
            return take_value(argv, i, name);
        };

        if (flag == "-p" || flag == "--print") {
            args.print = true;
        } else if (flag == "--output-format") {
            args.output_format = value(flag);
            if (args.output_format != "text" && args.output_format != "json") {
                throw UsageError("Invalid output format: " + args.output_format);
            }
        } else if (flag == "--json") {
            args.print = true;
            args.output_format = "json";
        } else if (flag == "--text") {
            args.print = true;
            args.output_format = "text";
        } else if (flag == "--model") {
            args.model = value(flag);
        } else if (flag == "--fallback-model") {
            args.fallback_model = value(flag);
        } else if (flag == "--permission-mode") {
            const std::string mode = value(flag);
            if (!is_internal_permission_mode(permission_mode_from_string(mode))) {
                throw UsageError("Invalid permission mode: " + mode);
            }
            args.permission_mode = permission_mode_from_string(mode);
        } else if (flag == "--dangerously-skip-permissions") {
            args.dangerously_skip_permissions = true;
        } else if (flag == "--allowed-tools") {
            args.allowed_tools = split_list(value(flag));
        } else if (flag == "--disallowed-tools") {
            args.disallowed_tools = split_list(value(flag));
        } else if (flag == "--add-dir") {
            args.additional_directories.push_back(value(flag));
        } else if (flag == "-c" || flag == "--continue") {
            args.continue_session = true;
        } else if (flag == "--resume") {
            if (inline_value) {
                args.resume_session = *inline_value;
            } else if (i + 1 < argv.size() && !starts_with(argv[i + 1], "-")) {
                args.resume_session = argv[++i];
            } else {
                // The reference opens a picker of the conversations of the
                // folder when `--resume` is given without the id of one.
                args.resume_picker = true;
            }
        } else if (flag == "--session-id") {
            args.session_id = value(flag);
        } else if (flag == "--system-prompt") {
            args.system_prompt = value(flag);
        } else if (flag == "--system-prompt-file") {
            args.system_prompt_file = value(flag);
        } else if (flag == "--append-system-prompt") {
            args.append_system_prompt = value(flag);
        } else if (flag == "--append-system-prompt-file") {
            args.append_system_prompt_file = value(flag);
        } else if (flag == "--settings") {
            args.settings = value(flag);
        } else if (flag == "--setting-sources") {
            args.setting_sources = split_list(value(flag));
        } else if (flag == "--debug") {
            args.debug = true;
        } else if (flag == "--debug-file") {
            args.debug_file = value(flag);
        } else if (flag == "--verbose") {
            args.verbose = true;
        } else if (flag == "-v" || flag == "--version") {
            args.show_version = true;
        } else if (flag == "-h" || flag == "--help") {
            args.show_help = true;
        } else if (flag == "--include-partial-messages") {
            args.include_partial_messages = true;
        } else if (flag == "--replay-user-messages") {
            // Accepted for compatibility; the transcript already replays them.
        } else if (flag == "--input-format") {
            args.input_format = value(flag);
        } else if (flag == "--max-turns") {
            args.max_turns = std::atoi(value(flag).c_str());
            if (args.max_turns <= 0) throw UsageError("--max-turns must be positive");
        } else if (flag == "--max-output-tokens") {
            args.max_output_tokens = std::atoi(value(flag).c_str());
            if (args.max_output_tokens <= 0) {
                throw UsageError("--max-output-tokens must be positive");
            }
        } else if (flag == "--thinking") {
            args.thinking = true;
        } else if (flag == "--effort") {
            args.effort = value(flag);
        } else if (flag == "--worktree" || flag == "--fork-session" || flag == "--init-only" ||
                   flag == "--bare" || flag == "--brief" || flag == "--strict-mcp-config" ||
                   flag == "--include-hook-events") {
            // Accepted for compatibility; these change how the session runs
            // but not the shape of the command line.
        } else if (flag == "--agents" || flag == "--mcp-config" || flag == "--plugin-dir" ||
                   flag == "--ide" || flag == "--task-budget" || flag == "--parent-session-id") {
            value(flag);
        } else {
            throw UsageError("unknown option: " + arg);
        }
    }

    if (!positional.empty()) {
        args.prompt = positional.front();
    }

    return args;
}

std::string help_text() {
    return "Usage: cc++ [options] [prompt]\n"
           "\n"
           "  -p, --print                 Print the response and exit\n"
           "      --output-format <fmt>  The format --print writes in: text or json\n"
           "      --model <model>         The model to use\n"
           "      --fallback-model <m>    The model to use when the primary one is unavailable\n"
           "      --permission-mode <m>   default, acceptEdits, plan, bypassPermissions, dontAsk\n"
           "      --dangerously-skip-permissions\n"
           "                              Skip all permission prompts\n"
           "      --allowed-tools <list>  Comma-separated tools that need no approval\n"
           "      --disallowed-tools <l>  Comma-separated tools that are never allowed\n"
           "      --add-dir <dir>         Add a working directory for this session\n"
           "  -c, --continue              Resume the most recent session\n"
           "      --resume [id]           Resume a previous session\n"
           "      --session-id <id>       Use a specific session id\n"
           "      --system-prompt <text>  Replace the system prompt\n"
           "      --system-prompt-file <p>\n"
           "                              Replace the system prompt with a file's contents\n"
           "      --append-system-prompt <t>\n"
           "                              Append to the system prompt\n"
           "      --append-system-prompt-file <p>\n"
           "                              Append a file's contents to the system prompt\n"
           "      --settings <json>       Settings overrides, as a JSON object\n"
           "      --setting-sources <l>   The setting sources to load\n"
           "      --debug                 Print debug information\n"
           "      --debug-file <path>     Write debug information to a file\n"
           "      --verbose               Print verbose output\n"
           "  -v, --version               Print the version and exit\n"
           "  -h, --help                  Print this help and exit";
}

std::string version_text() {
    return std::string(version()) + " (Claude Code C++ port)";
}

} // namespace cc::cli
