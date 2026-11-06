#include "config/output_styles.hpp"

#include "config/project_key.hpp"
#include "utils/fs.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>

namespace cc::config {
namespace {

/// The frontmatter of an output style.
struct Frontmatter {
    /// The name of the style.
    std::optional<std::string> name;
    /// The description of the style.
    std::optional<std::string> description;
    /// Whether the style keeps the instructions of the coding.
    std::optional<bool> keep_coding_instructions;
    /// The body of the file.
    std::string content;
};

/// Splits the frontmatter of a markdown file from its body.
///
/// The frontmatter sits between two `---` lines at the top of the file. A
/// value that is quoted loses its quotes.
Frontmatter parse_frontmatter(const std::string& text) {
    Frontmatter out;
    out.content = text;

    const auto lines = cc::split_lines(text);
    if (lines.empty() || cc::trim(lines.front()) != "---") return out;

    size_t i = 1;
    for (; i < lines.size(); ++i) {
        if (cc::trim(lines[i]) == "---") break;

        const std::string line = lines[i];
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        const std::string key = cc::trim(line.substr(0, colon));
        std::string value = cc::trim(line.substr(colon + 1));

        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                                  (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }

        if (key == "name") {
            out.name = value;
        } else if (key == "description") {
            out.description = value;
        } else if (key == "keep-coding-instructions") {
            // The reference reads the flag as a boolean and as the string
            // that spells it.
            if (value == "true") {
                out.keep_coding_instructions = true;
            } else if (value == "false") {
                out.keep_coding_instructions = false;
            }
        }
    }

    std::string body;
    for (size_t k = i + 1; k < lines.size(); ++k) {
        if (!body.empty()) body += "\n";
        body += lines[k];
    }
    out.content = body;
    return out;
}

/// The description of a style that does not name one.
///
/// The reference takes the first line of the file, without the `#` of a
/// heading.
std::string description_from_content(const std::string& content,
                                     const std::string& style_name) {
    for (const auto& line : cc::split_lines(content)) {
        const std::string trimmed = cc::trim(line);
        if (trimmed.empty()) continue;

        std::string text = trimmed;
        if (!text.empty() && text.front() == '#') {
            size_t k = 0;
            while (k < text.size() && text[k] == '#') ++k;
            while (k < text.size() && text[k] == ' ') ++k;
            text = text.substr(k);
        }

        if (text.size() > 100) text = text.substr(0, 97) + "...";
        return text;
    }
    return "Custom " + style_name + " output style";
}

/// The styles of the directory `directory`.
std::vector<OutputStyle> styles_of_directory(const std::string& directory,
                                             const std::string& source) {
    std::vector<OutputStyle> out;

    for (const auto& name : cc::list_dir(directory)) {
        if (!cc::ends_with(name, ".md")) continue;

        const std::string path = cc::path_join(directory, name);
        if (!cc::is_file(path)) continue;

        const auto contents = cc::read_file(path);
        if (!contents) continue;

        const Frontmatter frontmatter = parse_frontmatter(*contents);

        // The name of the file is the name of the style, and the frontmatter
        // may name another one.
        const std::string style_name = cc::stem(name);
        OutputStyle style;
        style.name = frontmatter.name.value_or(style_name);
        style.description = frontmatter.description.value_or(
            description_from_content(frontmatter.content, style_name));
        style.prompt = cc::trim(frontmatter.content);
        style.source = source;
        style.keep_coding_instructions = frontmatter.keep_coding_instructions;
        out.push_back(std::move(style));
    }

    return out;
}

} // namespace

std::vector<OutputStyle> load_output_styles(const std::string& cwd) {
    std::vector<OutputStyle> out;

    // The styles of the user come first; a style of the project with the
    // same name wins over them.
    for (auto& style : styles_of_directory(cc::path_join(cc::claude_config_dir(), "output-styles"),
                                           "userSettings")) {
        out.push_back(std::move(style));
    }

    for (const auto& directory : project_directories(cwd)) {
        for (auto& style :
             styles_of_directory(cc::path_join(cc::path_join(directory, ".claude"), "output-styles"),
                                 "projectSettings")) {
            const auto existing = std::find_if(out.begin(), out.end(), [&](const OutputStyle& other) {
                return other.name == style.name;
            });
            if (existing != out.end()) out.erase(existing);
            out.push_back(std::move(style));
        }
    }

    return out;
}

} // namespace cc::config
