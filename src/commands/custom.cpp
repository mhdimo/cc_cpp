#include "commands/custom.hpp"

#include "config/managed_settings.hpp"
#include "utils/fs.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>

namespace cc::commands {
namespace {

/// The parts of the frontmatter of a command file the port reads.
struct Frontmatter {
    std::string name;
    std::string description;
    std::string argument_hint;
    std::vector<std::string> argument_names;
    /// Whether the user may call the command by typing its name; empty when
    /// the file does not say.
    std::optional<bool> user_invocable;
    std::string content;
};

/// The names of the arguments a command takes.
///
/// The `arguments` frontmatter holds a list of the names or a string that
/// holds them all, the way the reference reads them. The names that are
/// only digits are left out: they would collide with the `$0` and `$1`
/// placeholders of the body.
std::vector<std::string> parse_argument_names(const std::string& value) {
    std::vector<std::string> names;

    std::string text = cc::trim(value);
    if (text.size() >= 2 && text.front() == '[' && text.back() == ']') {
        // A list that is written on one line: `[name, name]`.
        for (const std::string& item : cc::split(text.substr(1, text.size() - 2), ',')) {
            names.push_back(cc::trim(item));
        }
    } else {
        // A string that holds the names, separated by spaces.
        for (const std::string& item : cc::split_any(text, " \t")) {
            names.push_back(cc::trim(item));
        }
    }

    std::vector<std::string> out;
    for (const std::string& name : names) {
        if (name.empty()) continue;

        bool number = true;
        for (const char c : name) {
            if (!std::isdigit(static_cast<unsigned char>(c))) number = false;
        }
        if (number) continue;

        out.push_back(name);
    }
    return out;
}

/// Splits the frontmatter of a markdown file from its body.
///
/// The frontmatter sits between two `---` lines at the top of the file and
/// holds the keys the reference reads: `name`, `description`,
/// `argument-hint`, `arguments` and `user-invocable`.
Frontmatter parse_frontmatter(const std::string& text) {
    Frontmatter out;

    const auto lines = cc::split_lines(text);
    if (lines.empty() || cc::trim(lines.front()) != "---") {
        out.content = text;
        return out;
    }

    size_t i = 1;
    for (; i < lines.size(); ++i) {
        if (cc::trim(lines[i]) == "---") break;

        const std::string line = lines[i];
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        const std::string key = cc::trim(line.substr(0, colon));
        std::string value = cc::trim(line.substr(colon + 1));

        // A value that is quoted loses its quotes.
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                                  (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }

        if (key == "name") {
            out.name = value;
        } else if (key == "description") {
            out.description = value;
        } else if (key == "argument-hint") {
            out.argument_hint = value;
        } else if (key == "user-invocable") {
            // The reference reads the value as a boolean: only `true` lets
            // the user call the command by typing its name.
            out.user_invocable =
                value == "true" || value == "True" || value == "TRUE";
        } else if (key == "arguments") {
            if (value.empty()) {
                // The list of the names may start on one of the next lines.
                while (i + 1 < lines.size()) {
                    const std::string next = cc::trim(lines[i + 1]);
                    if (!cc::starts_with(next, "- ")) break;
                    ++i;
                    out.argument_names.push_back(cc::trim(next.substr(2)));
                }
            } else {
                out.argument_names = parse_argument_names(value);
            }
        }
    }

    // The body is everything after the frontmatter.
    std::string body;
    for (size_t k = i + 1; k < lines.size(); ++k) {
        if (!body.empty()) body += "\n";
        body += lines[k];
    }
    out.content = body;
    return out;
}

/// Takes the heading off a line, the way the reference does.
///
/// The reference only strips a heading that carries a space and some text
/// after the `#`s: `#Foo` keeps its `#`, and a line of `#`s alone keeps
/// them all.
std::string strip_heading(const std::string& text) {
    size_t hashes = 0;
    while (hashes < text.size() && text[hashes] == '#') ++hashes;
    if (hashes == 0) return text;

    // The `#`s have to be followed by a space and by some text.
    size_t start = hashes;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
    if (start == hashes || start == text.size()) return text;

    return text.substr(start);
}

/// The description of a file that does not name one.
///
/// The reference takes the first line of the file, without the `#` of a
/// heading, and falls back to the label of the kind of the file — `Custom
/// command` for a command, `Skill` for a skill.
std::string description_from_content(const std::string& content, const std::string& fallback) {
    for (const auto& line : cc::split_lines(content)) {
        const std::string trimmed = cc::trim(line);
        if (trimmed.empty()) continue;

        const std::string text = strip_heading(trimmed);
        if (text.size() > 100) return text.substr(0, 97) + "...";
        return text;
    }
    return fallback;
}

/// The name a command file gets.
///
/// The name of the file without its extension, with the folders it is in
/// joined by colons: `git/commit.md` becomes `git:commit`.
std::string command_name(const std::string& path, const std::string& base) {
    std::string relative = cc::relative(path, base);
    if (!relative.empty() && relative.front() == '.') relative.erase(0, 1);
    while (!relative.empty() && relative.front() == '/') relative.erase(0, 1);

    if (cc::ends_with(relative, ".md")) relative.erase(relative.size() - 3);

    std::string name;
    for (const auto& part : cc::split(relative, '/')) {
        if (part.empty()) continue;
        if (!name.empty()) name += ":";
        name += part;
    }
    return name;
}

/// Reads the commands of one folder.
///
/// The commands of the folder come from the source of the settings the
/// folder belongs to, the way the reference names them.
std::vector<CustomCommand> load_directory(const std::string& directory, const std::string& source) {
    std::vector<CustomCommand> out;
    if (directory.empty()) return out;

    for (const auto& path : cc::walk_files(directory)) {
        if (!cc::ends_with(path, ".md")) continue;

        const auto text = cc::read_file(path);
        if (!text) continue;

        CustomCommand command;
        command.name = command_name(path, directory);
        if (command.name.empty()) continue;

        const Frontmatter frontmatter = parse_frontmatter(*text);
        command.description = frontmatter.description;
        if (command.description.empty()) {
            command.description = description_from_content(frontmatter.content, "Custom command");
        }
        command.argument_hint = frontmatter.argument_hint;
        command.argument_names = frontmatter.argument_names;
        command.content = frontmatter.content;
        command.path = path;
        command.source = source;
        out.push_back(std::move(command));
    }
    return out;
}

/// Reads the skills of one folder.
///
/// The reference reads the skills of the user, of the project and of the
/// policy of the machine: every skill is a folder of its own, and the skill
/// itself is the `SKILL.md` of the folder. A skill whose frontmatter says
/// `user-invocable: false` is kept out of the list — the user may not call
/// it by typing its name.
std::vector<CustomCommand> load_skills(const std::string& directory, const std::string& source) {
    std::vector<CustomCommand> out;
    if (directory.empty()) return out;

    for (const std::string& entry : cc::list_dir(directory)) {
        const std::string skill_directory = cc::path_join(directory, entry);
        if (!cc::is_directory(skill_directory)) continue;

        const std::string file = cc::path_join(skill_directory, "SKILL.md");
        const auto text = cc::read_file(file);
        if (!text) continue;

        const Frontmatter frontmatter = parse_frontmatter(*text);
        if (frontmatter.user_invocable && !*frontmatter.user_invocable) continue;

        CustomCommand command;
        // The name of the skill is the name of its folder, unless the file
        // names another one.
        command.name = frontmatter.name.empty() ? entry : frontmatter.name;
        if (command.name.empty()) continue;

        command.description = frontmatter.description;
        if (command.description.empty()) {
            command.description = description_from_content(frontmatter.content, "Skill");
        }
        command.argument_hint = frontmatter.argument_hint;
        command.argument_names = frontmatter.argument_names;

        // The model is told where the skill lives, so that it can use the
        // files of the folder; the skill may point at the folder itself.
        command.content = "Base directory for this skill: " + skill_directory + "\n\n" +
                          frontmatter.content;
        command.content =
            cc::replace_all(command.content, "${CLAUDE_SKILL_DIR}", skill_directory);

        command.path = file;
        command.source = source;
        out.push_back(std::move(command));
    }
    return out;
}

/// Splits the arguments the user typed into the arguments themselves.
///
/// The arguments are split at the spaces, but a quoted stretch stays whole.
std::vector<std::string> parse_arguments(std::string_view args) {
    std::vector<std::string> out;
    std::string current;
    bool quoted = false;
    char quote = '\0';

    for (size_t i = 0; i < args.size(); ++i) {
        const char c = args[i];
        if (quoted) {
            if (c == quote) {
                quoted = false;
            } else {
                current.push_back(c);
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quoted = true;
            quote = c;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

/// Replaces `$ARGUMENTS[0]` with the argument at that place.
std::string replace_indexed(std::string text, const std::vector<std::string>& args) {
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        const size_t marker = text.find("$ARGUMENTS[", i);
        if (marker == std::string::npos) {
            out += text.substr(i);
            break;
        }
        out += text.substr(i, marker - i);

        const size_t close = text.find(']', marker);
        if (close == std::string::npos) {
            out += text.substr(marker);
            break;
        }

        const std::string digits = text.substr(marker + 11, close - marker - 11);
        bool number = !digits.empty();
        for (const char c : digits) {
            if (!std::isdigit(static_cast<unsigned char>(c))) number = false;
        }

        if (!number) {
            out += text.substr(marker, close - marker + 1);
            i = close + 1;
            continue;
        }

        const size_t index = static_cast<size_t>(std::stoul(digits));
        if (index < args.size()) out += args[index];
        i = close + 1;
    }
    return out;
}

/// Replaces `$0`, `$1` and the like with the argument at that place.
std::string replace_shorthand(std::string text, const std::vector<std::string>& args) {
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] != '$') {
            out.push_back(text[i]);
            ++i;
            continue;
        }

        size_t j = i + 1;
        while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) ++j;

        // A digit that is part of a word is not an argument.
        if (j == i + 1 || (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) ||
                                                text[j] == '_'))) {
            out.push_back(text[i]);
            ++i;
            continue;
        }

        const size_t index = static_cast<size_t>(std::stoul(text.substr(i + 1, j - i - 1)));
        if (index < args.size()) out += args[index];
        i = j;
    }
    return out;
}

/// The name of the plugin a command comes from.
///
/// The commands of a plugin are named `name@marketplace`; the name of the
/// plugin is the part in front of the `@`.
std::string plugin_name(const CustomCommand& command) {
    const size_t at = command.plugin_id.rfind('@');
    if (at == std::string::npos) return command.plugin_id;
    return command.plugin_id.substr(0, at);
}

} // namespace

std::vector<CustomCommand> load_custom_commands(const std::string& cwd) {
    // The user's own commands come first, the commands of the project
    // after them; a command of the project wins over one of the same name.
    // The commands of the policy of the machine come last and win over
    // both, the way the settings of the machine win over the others.
    //
    // The skills live in the folders of their own, next to the folders of
    // the commands, and they are commands as well.
    const std::string user_directory = cc::path_join(cc::claude_config_dir(), "commands");
    const std::string user_skills = cc::path_join(cc::claude_config_dir(), "skills");
    const std::string project_directory =
        cc::path_join(cc::path_join(cwd, ".claude"), "commands");
    const std::string project_skills = cc::path_join(cc::path_join(cwd, ".claude"), "skills");
    const std::string managed_directory =
        cc::path_join(cc::path_join(cc::config::managed_settings_dir(), ".claude"), "commands");
    const std::string managed_skills =
        cc::path_join(cc::path_join(cc::config::managed_settings_dir(), ".claude"), "skills");

    std::vector<CustomCommand> commands = load_directory(user_directory, "userSettings");

    // A command of a folder that comes later replaces the one of the same
    // name that is already there.
    const auto add = [&commands](std::vector<CustomCommand> loaded) {
        for (auto& command : loaded) {
            const auto existing = std::find_if(commands.begin(), commands.end(),
                                               [&](const CustomCommand& other) {
                                                   return other.name == command.name;
                                               });
            if (existing != commands.end()) {
                *existing = std::move(command);
                continue;
            }
            commands.push_back(std::move(command));
        }
    };

    add(load_skills(user_skills, "userSettings"));
    add(load_directory(project_directory, "projectSettings"));
    add(load_skills(project_skills, "projectSettings"));
    add(load_directory(managed_directory, "policySettings"));
    add(load_skills(managed_skills, "policySettings"));

    std::sort(commands.begin(), commands.end(),
              [](const CustomCommand& a, const CustomCommand& b) { return a.name < b.name; });
    return commands;
}

std::string setting_source_name(const std::string& source) {
    if (source == "userSettings") return "user";
    if (source == "projectSettings") return "project";
    if (source == "localSettings") return "project, gitignored";
    if (source == "flagSettings") return "cli flag";
    if (source == "policySettings") return "managed";
    return {};
}

std::string format_description_with_source(const CustomCommand& command) {
    // The commands of a plugin carry the name of the plugin in front of
    // their description, the way the reference shows them.
    if (!command.plugin_id.empty()) {
        const std::string plugin = plugin_name(command);
        if (!plugin.empty()) return "(" + plugin + ") " + command.description;
        return command.description + " (plugin)";
    }

    // The commands of the user carry the place they come from at the end of
    // their description.
    const std::string source = setting_source_name(command.source);
    if (source.empty()) return command.description;
    return command.description + " (" + source + ")";
}

std::vector<std::string> argument_names_of(const CustomCommand& command) {
    if (!command.argument_names.empty()) return command.argument_names;

    // The commands of the plugins are read by the loader of the plugins,
    // which cannot carry the names of the arguments over; their file is
    // read for them, the way the reference reads the frontmatter of every
    // command when it loads it.
    if (command.plugin_id.empty() || command.path.empty()) return {};

    const auto contents = cc::read_file(command.path);
    if (!contents) return {};
    return parse_frontmatter(*contents).argument_names;
}

std::string substitute_arguments(std::string_view content, std::string_view args) {
    if (args.empty()) return std::string(content);

    const std::vector<std::string> parsed = parse_arguments(args);
    std::string text = replace_indexed(std::string(content), parsed);
    text = replace_shorthand(text, parsed);

    // The whole string of the arguments replaces `$ARGUMENTS`.
    size_t marker = text.find("$ARGUMENTS");
    while (marker != std::string::npos) {
        text.replace(marker, 10, std::string(args));
        marker = text.find("$ARGUMENTS", marker + std::string(args).size());
    }

    // A prompt without a placeholder gets the arguments appended.
    if (text == content) {
        text += "\n\nARGUMENTS: ";
        text += args;
    }
    return text;
}

namespace {

/// The names of the commands the session knows.
///
/// The list is kept here, next to the commands of the user, because the
/// prompt box needs it and the header of the commands is the one that
/// declares it.
std::vector<std::string>& known_names() {
    static std::vector<std::string> names;
    return names;
}

} // namespace

const std::vector<std::string>& known_command_names() { return known_names(); }

void set_known_command_names(std::vector<std::string> names) {
    known_names() = std::move(names);
}

std::string progressive_argument_hint(const std::vector<std::string>& names,
                                      std::string_view typed) {
    // The arguments that were typed already are the ones that are filled in.
    const std::vector<std::string> parsed = parse_arguments(typed);

    std::string hint;
    for (size_t i = parsed.size(); i < names.size(); ++i) {
        if (!hint.empty()) hint += " ";
        hint += "[" + names[i] + "]";
    }
    return hint;
}

} // namespace cc::commands
