#include "agents/agent.hpp"

#include "utils/fs.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace cc::agents {
namespace {

/// The name of the agent used when the model does not name one.
constexpr std::string_view kGeneralPurposeName = "general-purpose";

/// The description the reference gives the general-purpose agent.
constexpr std::string_view kGeneralPurposeDescription =
    "General-purpose agent for researching complex questions, searching for code, and executing "
    "multi-step tasks. When you are searching for a keyword or file and are not confident that you "
    "will find the right match in the first few times use this agent to perform the search.";

/// The system prompt the reference gives the general-purpose agent.
constexpr std::string_view kGeneralPurposePrompt =
    "You are an agent for Claude Code, Anthropic's official CLI for Claude. Given the user's message, "
    "you should use the tools available to complete the task. Complete the task fully—don't gold-plate, "
    "but don't leave it half-done. When you complete the task, respond with a concise report covering "
    "what was done and any key findings — the caller will relay this to the user, so it only needs the "
    "essentials.\n"
    "\n"
    "Your strengths:\n"
    "- Searching for code, configurations, and patterns across large codebases\n"
    "- Analyzing multiple files to understand system architecture\n"
    "- Investigating complex questions that require exploring many files\n"
    "- Performing multi-step research tasks\n"
    "\n"
    "Guidelines:\n"
    "- For file searches: search broadly when you don't know where something lives. Use Read when you "
    "know the specific file path.\n"
    "- For analysis: Start broad and narrow down. Use multiple search strategies if the first doesn't "
    "yield results.\n"
    "- Be thorough: Check multiple locations, consider different naming conventions, look for related "
    "files.\n"
    "- NEVER create files unless they're absolutely necessary for achieving your goal. ALWAYS prefer "
    "editing an existing file to creating a new one.\n"
    "- NEVER proactively create documentation files (*.md) or README files. Only create documentation "
    "files if explicitly requested.";

/// A markdown document split into its frontmatter and its body.
struct Document {
    /// The YAML source between the `---` delimiters.
    std::string frontmatter;
    /// Everything after the closing delimiter.
    std::string body;
    /// True when the document opened with a frontmatter block.
    bool has_frontmatter = false;
};

/// Splits a markdown document into its frontmatter and its body.
///
/// A document that does not open with a `---` line has no frontmatter; its
/// whole contents are the body.
Document split_document(const std::string& contents) {
    Document document;

    size_t start = 0;
    // Skip a byte-order mark, which some editors put in front of the file.
    if (contents.size() >= 3 && static_cast<unsigned char>(contents[0]) == 0xEF &&
        static_cast<unsigned char>(contents[1]) == 0xBB &&
        static_cast<unsigned char>(contents[2]) == 0xBF) {
        start = 3;
    }

    if (!cc::starts_with(contents.substr(start), "---")) return document;

    const size_t opening_end = contents.find('\n', start);
    if (opening_end == std::string::npos) return document;
    if (!cc::trim(contents.substr(start + 3, opening_end - start - 3)).empty()) return document;

    const size_t body_start = opening_end + 1;

    size_t cursor = body_start;
    while (cursor <= contents.size()) {
        const size_t line_end = contents.find('\n', cursor);
        const size_t line_stop = line_end == std::string::npos ? contents.size() : line_end;
        const std::string line = contents.substr(cursor, line_stop - cursor);
        const std::string trimmed = cc::trim(line);

        if (cc::starts_with(trimmed, "---")) {
            document.has_frontmatter = true;
            document.frontmatter = contents.substr(body_start, cursor - body_start);
            document.body = contents.substr(line_end == std::string::npos ? contents.size() : line_end + 1);
            return document;
        }

        if (line_end == std::string::npos) break;
        cursor = line_end + 1;
    }

    return document;
}

/// Removes a trailing YAML comment from a value.
///
/// A `#` only starts a comment at the beginning of the value or after
/// whitespace, and never inside quotes.
std::string strip_comment(std::string_view value) {
    bool in_single = false;
    bool in_double = false;
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '\'' && !in_double) {
            in_single = !in_single;
        } else if (c == '"' && !in_single) {
            in_double = !in_double;
        } else if (c == '#' && !in_single && !in_double) {
            if (i == 0 || std::isspace(static_cast<unsigned char>(value[i - 1]))) {
                return std::string(value.substr(0, i));
            }
        }
    }
    return std::string(value);
}

/// Removes the quotes around a YAML scalar and resolves its escapes.
std::string unquote(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        std::string out;
        for (size_t i = 1; i + 1 < value.size(); ++i) {
            const char c = value[i];
            if (c == '\\' && i + 2 < value.size()) {
                switch (value[i + 1]) {
                case 'n':
                    out += '\n';
                    ++i;
                    continue;
                case 't':
                    out += '\t';
                    ++i;
                    continue;
                case 'r':
                    out += '\r';
                    ++i;
                    continue;
                case '"':
                    out += '"';
                    ++i;
                    continue;
                case '\\':
                    out += '\\';
                    ++i;
                    continue;
                default:
                    break;
                }
            }
            out += c;
        }
        return out;
    }

    if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
        std::string out(value.substr(1, value.size() - 2));
        // Inside single quotes a doubled quote is an escaped quote.
        std::string unescaped;
        for (size_t i = 0; i < out.size(); ++i) {
            if (out[i] == '\'' && i + 1 < out.size() && out[i + 1] == '\'') ++i;
            unescaped += out[i];
        }
        return unescaped;
    }

    return std::string(value);
}

/// True when the scalar was written in quotes, which makes it a string.
bool is_quoted(std::string_view value) {
    return value.size() >= 2 &&
           ((value.front() == '"' && value.back() == '"') ||
            (value.front() == '\'' && value.back() == '\''));
}

/// Converts a YAML scalar into a JSON value.
boost::json::value scalar_value(std::string_view raw) {
    const std::string trimmed = cc::trim(raw);
    const std::string text = unquote(trimmed);

    if (!is_quoted(trimmed)) {
        if (text == "true" || text == "True" || text == "TRUE") return boost::json::value(true);
        if (text == "false" || text == "False" || text == "FALSE") return boost::json::value(false);
        if (text.empty() || text == "null" || text == "Null" || text == "NULL" || text == "~") {
            return boost::json::value{nullptr};
        }
        if (const auto number = cc::parse_int(text)) return boost::json::value(*number);
    }

    return boost::json::value(text);
}

/// Splits the body of an inline YAML list on its top-level commas.
boost::json::array parse_inline_list(std::string_view value) {
    boost::json::array items;

    bool in_single = false;
    bool in_double = false;
    std::string current;
    auto flush = [&]() {
        const std::string item = cc::trim(current);
        if (!item.empty()) items.emplace_back(scalar_value(item));
        current.clear();
    };

    for (const char c : value) {
        if (c == '\'' && !in_double) {
            in_single = !in_single;
        } else if (c == '"' && !in_single) {
            in_double = !in_double;
        } else if (c == ',' && !in_single && !in_double) {
            flush();
            continue;
        }
        current += c;
    }
    flush();

    return items;
}

/// Finds the `:` that separates a frontmatter key from its value.
///
/// The colon must be followed by whitespace or end the line, and must not
/// sit inside quotes.
size_t find_key_separator(std::string_view line) {
    bool in_single = false;
    bool in_double = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\'' && !in_double) {
            in_single = !in_single;
            continue;
        }
        if (c == '"' && !in_single) {
            in_double = !in_double;
            continue;
        }
        if (c == ':' && !in_single && !in_double) {
            if (i + 1 == line.size() || line[i + 1] == ' ' || line[i + 1] == '\t') return i;
        }
    }
    return std::string::npos;
}

/// Parses the YAML subset that frontmatter uses into a JSON object.
///
/// The supported shapes are `key: value` pairs, inline lists
/// (`key: [a, b]`), block lists (a `key:` line followed by `- item` lines)
/// and block scalars (`key: |` and `key: >`).
boost::json::object parse_yaml(const std::string& text) {
    boost::json::object result;
    const std::vector<std::string> lines = cc::split_lines(text);

    std::string list_key;
    boost::json::array list_items;
    bool collecting = false;

    auto flush_list = [&]() {
        if (!collecting) return;
        result[list_key] = boost::json::value(std::move(list_items));
        list_items = boost::json::array();
        collecting = false;
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& raw = lines[i];
        const std::string trimmed = cc::trim(raw);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        const size_t indent = raw.find_first_not_of(" \t");

        if (trimmed == "-" || cc::starts_with(trimmed, "- ")) {
            if (list_key.empty()) continue;
            collecting = true;
            const std::string item = trimmed == "-" ? "" : cc::trim(trimmed.substr(2));
            list_items.emplace_back(scalar_value(strip_comment(item)));
            continue;
        }

        const size_t separator = find_key_separator(trimmed);
        if (separator == std::string::npos) continue;

        flush_list();

        const std::string key = cc::trim(trimmed.substr(0, separator));
        if (key.empty()) continue;
        const std::string value = cc::trim(trimmed.substr(separator + 1));

        if (value == "|" || value == ">" || value == "|-" || value == ">-") {
            std::vector<std::string> block;
            size_t j = i + 1;
            for (; j < lines.size(); ++j) {
                const std::string& block_raw = lines[j];
                if (cc::trim(block_raw).empty()) {
                    block.emplace_back();
                    continue;
                }
                const size_t block_indent = block_raw.find_first_not_of(" \t");
                if (block_indent <= indent) break;
                block.push_back(block_raw.substr(block_indent));
            }
            i = j - 1;

            std::string folded;
            for (size_t k = 0; k < block.size(); ++k) {
                if (k > 0) folded += (value[0] == '>' && !block[k].empty()) ? " " : "\n";
                folded += block[k];
            }
            result[key] = boost::json::value(cc::trim(folded));
            continue;
        }

        const std::string scalar = cc::trim(strip_comment(value));
        if (scalar.empty()) {
            // The value may be a list that starts on one of the next lines.
            list_key = key;
            collecting = false;
            result[key] = boost::json::value{nullptr};
            continue;
        }

        if (scalar.front() == '[' && scalar.back() == ']') {
            result[key] = boost::json::value(parse_inline_list(scalar.substr(1, scalar.size() - 2)));
            continue;
        }

        result[key] = scalar_value(scalar);
    }

    flush_list();
    return result;
}

/// Splits a tool list on commas and whitespace.
///
/// Commas inside parentheses belong to the tool name, as in `Bash(git:*)`.
std::vector<std::string> split_tool_list(std::string_view value) {
    std::vector<std::string> tools;
    std::string current;
    bool in_parens = false;

    for (const char c : value) {
        if (c == '(') {
            in_parens = true;
            current += c;
            continue;
        }
        if (c == ')') {
            in_parens = false;
            current += c;
            continue;
        }

        if (!in_parens && (c == ',' || std::isspace(static_cast<unsigned char>(c)))) {
            const std::string tool = cc::trim(current);
            if (!tool.empty()) tools.push_back(tool);
            current.clear();
            continue;
        }
        current += c;
    }

    const std::string tool = cc::trim(current);
    if (!tool.empty()) tools.push_back(tool);

    return tools;
}

/// The tool names in a frontmatter `tools` value.
///
/// The value may be a comma-separated string or a YAML list. A list holding
/// `*` means every tool, which the empty list also means.
std::vector<std::string> parse_tools(const boost::json::value& value) {
    std::vector<std::string> tools;

    if (value.is_string()) {
        tools = split_tool_list(std::string_view(value.as_string()));
    } else if (value.is_array()) {
        for (const auto& item : value.as_array()) {
            if (!item.is_string()) continue;
            for (const std::string& tool : split_tool_list(std::string_view(item.as_string()))) {
                tools.push_back(tool);
            }
        }
    }

    if (std::find(tools.begin(), tools.end(), "*") != tools.end()) tools.clear();
    return tools;
}

/// The model named by a frontmatter `model` value.
///
/// `inherit` means the agent runs on the parent's model, which the empty
/// string expresses.
std::string parse_model(const boost::json::value& value) {
    if (!value.is_string()) return "";
    const std::string model = cc::trim(std::string_view(value.as_string()));
    if (model.empty()) return "";
    if (cc::to_lower(model) == "inherit") return "";
    return model;
}

/// Reads a definition from a markdown file.
///
/// Returns nothing when the file is not an agent: the reference requires a
/// `name` and a `description` in the frontmatter and silently skips files
/// without them.
std::optional<Definition> parse_definition(const std::string& path, const std::string& contents) {
    const Document document = split_document(contents);
    if (!document.has_frontmatter) return std::nullopt;

    const boost::json::object frontmatter = parse_yaml(document.frontmatter);

    Definition definition;
    definition.path = path;

    const auto name = frontmatter.find("name");
    if (name == frontmatter.end() || !name->value().is_string()) return std::nullopt;
    definition.name = cc::trim(std::string_view(name->value().as_string()));
    if (definition.name.empty()) return std::nullopt;

    const auto description = frontmatter.find("description");
    if (description == frontmatter.end() || !description->value().is_string()) return std::nullopt;
    definition.description = cc::trim(std::string_view(description->value().as_string()));
    if (definition.description.empty()) return std::nullopt;

    const auto tools = frontmatter.find("tools");
    if (tools != frontmatter.end()) definition.tools = parse_tools(tools->value());

    const auto model = frontmatter.find("model");
    if (model != frontmatter.end()) definition.model = parse_model(model->value());

    definition.prompt = cc::trim(document.body);

    return definition;
}

/// The built-in general-purpose agent.
Definition general_purpose_definition() {
    Definition definition;
    definition.name = std::string(kGeneralPurposeName);
    definition.description = std::string(kGeneralPurposeDescription);
    definition.prompt = std::string(kGeneralPurposePrompt);
    definition.path = std::string(kBuiltInPath);
    return definition;
}

/// Reads every agent definition in a directory.
void load_directory(const std::string& directory, std::vector<Definition>& definitions) {
    if (!cc::is_directory(directory)) return;

    for (const std::string& entry : cc::list_dir(directory)) {
        if (!cc::ends_with(entry, ".md")) continue;

        const std::string path = cc::path_join(directory, entry);
        const auto contents = cc::read_file(path);
        if (!contents) continue;

        const auto definition = parse_definition(path, *contents);
        if (!definition) continue;

        // A definition read later replaces one with the same name.
        const auto existing =
            std::find_if(definitions.begin(), definitions.end(), [&](const Definition& candidate) {
                return candidate.name == definition->name;
            });
        if (existing != definitions.end()) {
            *existing = *definition;
        } else {
            definitions.push_back(*definition);
        }
    }
}

} // namespace

std::vector<Definition> load_definitions(const std::string& cwd, const std::string& home) {
    std::vector<Definition> definitions;
    definitions.push_back(general_purpose_definition());

    // The agents that ship with the CLI come next.
    for (Definition& definition : builtin_definitions()) definitions.push_back(std::move(definition));

    // The user's definitions come first so that the project's definitions,
    // which are read afterwards, take precedence over them.
    if (!home.empty()) {
        load_directory(cc::path_join(cc::claude_config_dir(), "agents"), definitions);
    }
    if (!cwd.empty()) {
        load_directory(cc::path_join(cc::path_join(cwd, ".claude"), "agents"), definitions);
    }

    return definitions;
}

std::optional<Definition> find(const std::vector<Definition>& definitions, const std::string& name) {
    for (const Definition& definition : definitions) {
        if (definition.name == name) return definition;
    }
    return std::nullopt;
}

boost::json::object parse_frontmatter(const std::string& contents) {
    const Document document = split_document(contents);
    if (!document.has_frontmatter) return boost::json::object();
    return parse_yaml(document.frontmatter);
}

} // namespace cc::agents
