#pragma once

#include <optional>
#include <string>
#include <vector>

namespace cc::config {

/// One output style, as it is configured.
struct OutputStyle {
    /// The name of the style.
    std::string name;
    /// What the style is for.
    std::string description;
    /// The prompt of the style.
    std::string prompt;
    /// The scope the style comes from: `userSettings`, `projectSettings` or
    /// `policySettings`.
    std::string source;
    /// True when the style keeps the instructions of the coding.
    ///
    /// The reference leaves it out when the style does not say.
    std::optional<bool> keep_coding_instructions;
};

/// The output styles of the user and of the project.
///
/// The reference reads the markdown files of `~/.claude/output-styles` and of
/// the `.claude/output-styles` folder of every folder of the project. The
/// name of a style is the name of its file, and the frontmatter of the file
/// carries the name, the description and whether the instructions of the
/// coding are kept.
///
/// A style of the project wins over a style of the user that has the same
/// name, the way the reference lets it win.
std::vector<OutputStyle> load_output_styles(const std::string& cwd);

} // namespace cc::config
