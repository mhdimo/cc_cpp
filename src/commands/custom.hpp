#pragma once

#include <string>
#include <vector>

namespace cc::commands {

/// A command the user wrote in a markdown file.
///
/// Claude Code reads the files in `.claude/commands` and offers each of them
/// as a slash command; the body of the file is the prompt that is sent to the
/// model.
struct CustomCommand {
    /// The name of the command, without the leading slash.
    std::string name;

    /// The one-line description shown in the help and in the suggestions.
    std::string description;

    /// The hint about the arguments the command takes.
    std::string argument_hint;

    /// The names of the arguments the command takes, from the `arguments`
    /// frontmatter of the file.
    std::vector<std::string> argument_names;

    /// The body of the file: the prompt that is sent to the model.
    std::string content;

    /// The file the command was read from.
    std::string path;

    /// The folder of the plugin the command comes from; empty when the user
    /// wrote the command themselves.
    std::string plugin_root;

    /// The name of the plugin the command comes from, as `name@marketplace`.
    std::string plugin_id;

    /// Where the command comes from: the source of the settings it was read
    /// from — `userSettings`, `projectSettings`, `localSettings` or
    /// `policySettings`.
    std::string source;
};

/// The commands the user wrote, in the order they are shown.
///
/// The files of the user's own folder come first, then the files of the
/// project; a command of the project wins over one of the same name in the
/// user's folder. The commands of the policy of the machine come last and
/// win over both, the way the settings of the machine win over the others.
std::vector<CustomCommand> load_custom_commands(const std::string& cwd);

/// The name the reference shows for a source of the settings.
///
/// The sources are the ones of the settings of the CLI: the user's own
/// (`user`), the project's (`project`), the copy of the project that the
/// repository ignores (`project, gitignored`), the flag of the command line
/// (`cli flag`) and the policy of the machine (`managed`).
std::string setting_source_name(const std::string& source);

/// The description of a command, with the place the command comes from.
///
/// The reference writes the source of a command at the end of its
/// description, and the name of the plugin of a command in front of it.
std::string format_description_with_source(const CustomCommand& command);

/// The names of the arguments a command takes.
///
/// The names are the ones the `arguments` frontmatter of the file of the
/// command declares. The commands of the plugins are not read by the loader
/// of the commands, so their file is read for the names.
std::vector<std::string> argument_names_of(const CustomCommand& command);

/// Puts the arguments the user typed into the prompt of a command.
///
/// The placeholders the reference knows are `$ARGUMENTS`, `$ARGUMENTS[0]`,
/// `$0` and `$1`. When the prompt has no placeholder, the arguments are
/// appended to it.
std::string substitute_arguments(std::string_view content, std::string_view args);

} // namespace cc::commands
