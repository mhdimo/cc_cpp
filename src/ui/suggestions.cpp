#include "ui/suggestions.hpp"

#include "commands/registry.hpp"
#include "config/global_config.hpp"
#include "utils/fs.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cc::ui {
namespace {

/// The icon the reference draws in front of an entry.
const char* icon_for(Suggestion::Kind kind) {
    switch (kind) {
    case Suggestion::Kind::File:
    case Suggestion::Kind::Directory:
        return "+";
    case Suggestion::Kind::Agent:
        return "*";
    default:
        return "+";
    }
}

/// The files and the folders under `cwd` that start with `prefix`.
///
/// With nothing typed the reference offers the entries of the folder itself,
/// the folders among them with a slash at the end.
std::vector<std::string> matching_files(const std::string& cwd, const std::string& prefix) {
    std::vector<std::string> matches;

    if (prefix.empty()) {
        // The entries of the folder, the folders among them. The reference
        // lists every entry of the folder, the ones whose name begins with a
        // dot included.
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(cwd, ec)) {
            const std::string name = entry.path().filename().string();
            if (name.empty()) continue;
            if (entry.is_directory(ec)) {
                matches.push_back(name + "/");
            } else {
                matches.push_back(name);
            }
        }
        std::sort(matches.begin(), matches.end());
        return matches;
    }

    // A path that ends in a separator names a folder the user is walking
    // into: the entries of that folder are offered, the folders among them
    // with a slash at the end. The path may point anywhere — `~/` is the
    // home of the user, `../` the folder above, `/` the root.
    if (!prefix.empty() && prefix.back() == '/') {
        const std::string folder = cc::resolve(prefix, cwd);
        std::error_code ec;
        if (std::filesystem::is_directory(folder, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
                const std::string name = entry.path().filename().string();
                if (name.empty()) continue;
                if (entry.is_directory(ec)) {
                    matches.push_back(prefix + name + "/");
                } else {
                    matches.push_back(prefix + name);
                }
            }
            std::sort(matches.begin(), matches.end());
            return matches;
        }
    }

    // The index of the reference holds the paths relative to the root and
    // searches the whole of them, so `src/ma` finds `src/main.cpp`.
    const auto files = walk_files(cwd, 0);
    for (const auto& file : files) {
        const std::string relative = cc::relative(file, cwd);
        if (relative.empty()) continue;
        const std::string name = basename(relative);
        if (starts_with(relative, prefix) || starts_with(name, prefix)) {
            matches.push_back(relative);
        }
    }

    std::sort(matches.begin(), matches.end());
    return matches;
}

/// The runs of white space of `text` become one space.
///
/// The reference squeezes the description of an entry before it cuts it.
std::string squeeze_spaces(const std::string& text) {
    std::string out;
    bool space = false;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out.push_back(' ');
        space = false;
        out.push_back(c);
    }
    return out;
}

/// The separators the reference splits the parts of a name at.
constexpr std::string_view kNameSeparators = ":-_";

/// The width of the description of an entry of the list of the `@`.
///
/// The reference cuts the description of an agent at sixty columns.
constexpr size_t kAgentDescriptionWidth = 60;

/// The share of a match that may be wrong and still be a match.
///
/// The reference searches the commands with a fuzzy index whose threshold is
/// 0.3.
constexpr double kFuseThreshold = 0.3;

/// The distance the reference gives the index.
///
/// A match that lies further away from the beginning of the text is worth
/// less.
constexpr double kFuseDistance = 100.0;

/// The weights the reference gives the keys of its index.
constexpr double kNameWeight = 3.0;
constexpr double kPartWeight = 2.0;
constexpr double kAliasWeight = 2.0;
constexpr double kDescriptionWeight = 0.5;

/// A command the search looks at.
///
/// The built-in commands, the commands of the user and the prompts of the
/// MCP servers are searched the same way, so they are put in one shape.
struct Entry {
    /// The name of the command, without the leading slash.
    std::string name;

    /// The aliases of the command.
    std::vector<std::string> aliases;

    /// The description of the command, the one the search looks at.
    std::string description;

    /// The description that is shown after the name.
    std::string shown;

    /// The alias the user typed, the way the reference shows it.
    std::string alias;

    /// The text that replaces the input when the command is chosen.
    std::string insert;

    /// True when the command is a prompt, the way the reference knows the
    /// commands that are remembered when they are used.
    bool prompt = false;

    /// The score of the use of the command, the way the reference computes
    /// it: the commands that are not prompts are never remembered.
    double usage = 0.0;

    /// True when the command is one of the ones that were used recently, and
    /// that stand at the top of the list.
    bool recent = false;

    /// Where the command comes from, for the order of the list.
    enum class Origin {
        Builtin, ///< A command that is built into the CLI.
        User,    ///< A command of the user.
        Project, ///< A command of the project.
        Managed, ///< A command of the policy of the machine.
        Other,   ///< A command of a plugin or of an MCP server.
    };

    Origin origin = Origin::Other;
};

/// The words of a description, cleaned the way the reference cleans them.
///
/// The reference keeps the letters and the digits of a word, drops the rest
/// and puts the word in lower case.
std::vector<std::string> description_words(const std::string& description) {
    std::vector<std::string> words;
    for (const std::string& word : cc::split(description, ' ')) {
        std::string clean;
        for (const char c : word) {
            if (std::isalnum(static_cast<unsigned char>(c))) {
                clean.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }
        if (!clean.empty()) words.push_back(std::move(clean));
    }
    return words;
}

/// The parts of a name, split at the separators the reference uses.
std::vector<std::string> name_parts(const std::string& name) {
    return cc::split_any(name, kNameSeparators);
}

/// The score of the best match of `query` in `text`.
///
/// The reference searches the commands with a fuzzy index: the score of a
/// match is the share of the query that is wrong, plus the distance of the
/// match from the beginning of the text divided by a hundred. A text that
/// holds no match of the query has the score -1.
double fuzzy_score(std::string_view query, std::string_view text) {
    if (query.empty()) return 0.0;
    if (text.empty()) return -1.0;

    const size_t query_length = query.size();
    const size_t text_length = text.size();

    // The fewest errors that match the first `i` letters of the query with
    // a piece of the text that ends at `j`, and the place that piece begins
    // at. The empty query matches an empty piece of the text at any place.
    std::vector<size_t> errors(text_length + 1, 0);
    std::vector<size_t> starts(text_length + 1, 0);
    std::vector<size_t> row_errors(text_length + 1, 0);
    std::vector<size_t> row_starts(text_length + 1, 0);
    for (size_t j = 0; j <= text_length; ++j) starts[j] = j;

    for (size_t i = 1; i <= query_length; ++i) {
        row_errors[0] = i;
        row_starts[0] = 0;
        for (size_t j = 1; j <= text_length; ++j) {
            // The letter of the query is not in the text.
            size_t best = errors[j] + 1;
            size_t start = starts[j];

            // The letter of the text is not in the query.
            if (row_errors[j - 1] + 1 < best ||
                (row_errors[j - 1] + 1 == best && row_starts[j - 1] < start)) {
                best = row_errors[j - 1] + 1;
                start = row_starts[j - 1];
            }

            // The letters are the same, or one of them is wrong.
            const size_t cost = query[i - 1] == text[j - 1] ? 0 : 1;
            const size_t candidate = errors[j - 1] + cost;
            if (candidate < best || (candidate == best && starts[j - 1] < start)) {
                best = candidate;
                start = starts[j - 1];
            }

            row_errors[j] = best;
            row_starts[j] = start;
        }
        errors.swap(row_errors);
        starts.swap(row_starts);
    }

    // The best match over all the places the text ends at.
    double best = -1.0;
    for (size_t j = 0; j <= text_length; ++j) {
        const double accuracy = static_cast<double>(errors[j]) / static_cast<double>(query_length);
        const double proximity = static_cast<double>(starts[j]) / kFuseDistance;
        const double score = accuracy + proximity;
        if (best < 0.0 || score < best) best = score;
    }
    return best;
}

/// The best score of the query in a list of texts.
double best_score(const std::string& query, const std::vector<std::string>& texts) {
    double best = -1.0;
    for (const std::string& text : texts) {
        const double score = fuzzy_score(query, text);
        if (score < 0.0) continue;
        if (best < 0.0 || score < best) best = score;
    }
    return best;
}

/// What the search found for a command.
struct Found {
    /// The score of the command; -1 when the query matched nothing.
    double score = -1.0;

    /// The score of the use of the command, the one the reference breaks the
    /// ties of the search with.
    double usage = 0.0;

    /// True when the name of the command is the query itself.
    bool exact_name = false;

    /// True when an alias of the command is the query itself.
    bool exact_alias = false;

    /// True when the name of the command starts with the query.
    bool prefix_name = false;

    /// True when an alias of the command starts with the query.
    bool prefix_alias = false;

    /// The length of the name of the command.
    size_t name_length = 0;

    /// The length of the alias that matched.
    size_t alias_length = 0;
};

/// Searches a command, the way the index of the reference searches it.
///
/// The reference looks at the name of the command, at the parts of the name,
/// at the aliases and at the words of the description, and weighs the hits:
/// the name weighs three, the parts and the aliases two and the words of the
/// description half. The score of the command is the product of the scores
/// of the keys that matched, each raised to its weight.
Found search_command(Entry& entry, const std::string& query) {
    Found found;
    found.usage = entry.usage;

    const std::string name = cc::to_lower(entry.name);
    found.name_length = name.size();
    found.exact_name = name == query;
    found.prefix_name = cc::starts_with(name, query);

    std::vector<std::string> aliases;
    for (const std::string& alias : entry.aliases) {
        const std::string lower = cc::to_lower(alias);
        if (lower == query) found.exact_alias = true;
        // The reference shows the first alias that the query is the
        // beginning of.
        if (entry.alias.empty() && cc::starts_with(lower, query)) entry.alias = lower;
        aliases.push_back(std::move(lower));
    }
    found.prefix_alias = !entry.alias.empty();
    found.alias_length = entry.alias.size();

    double total = 1.0;
    bool matched = false;
    const auto weigh = [&](double score, double weight) {
        if (score < 0.0 || score > kFuseThreshold) return;
        total *= std::pow(score == 0.0 ? std::numeric_limits<double>::epsilon() : score, weight);
        matched = true;
    };

    weigh(fuzzy_score(query, name), kNameWeight);

    const std::vector<std::string> parts = name_parts(entry.name);
    if (parts.size() > 1) {
        std::vector<std::string> lowered;
        lowered.reserve(parts.size());
        for (const std::string& part : parts) lowered.push_back(cc::to_lower(part));
        weigh(best_score(query, lowered), kPartWeight);
    }

    weigh(best_score(query, aliases), kAliasWeight);
    weigh(best_score(query, description_words(entry.description)), kDescriptionWeight);

    if (!matched) return found;
    found.score = total;
    return found;
}

/// The order the reference puts the commands in.
///
/// The exact name comes first, then the exact alias, then the names that
/// start with what was typed — the shorter ones before the longer ones —
/// then the aliases that start with it, and last the commands that only
/// matched the description. Two commands whose scores lie within a tenth of
/// each other are ordered by the use they had.
bool better(const Found& a, const Found& b) {
    if (a.exact_name != b.exact_name) return a.exact_name;
    if (a.exact_alias != b.exact_alias) return a.exact_alias;
    if (a.prefix_name != b.prefix_name) return a.prefix_name;
    if (a.prefix_name && a.name_length != b.name_length) return a.name_length < b.name_length;
    if (a.prefix_alias != b.prefix_alias) return a.prefix_alias;
    if (a.prefix_alias && a.alias_length != b.alias_length) return a.alias_length < b.alias_length;

    const double difference = a.score - b.score;
    if (std::abs(difference) > 0.1) return difference < 0.0;
    return a.usage > b.usage;
}

/// Where a command of the user comes from.
///
/// The reference groups the commands of the user and of the copy of the
/// project that the repository ignores together, and puts the commands of a
/// plugin and of an MCP server last.
Entry::Origin origin_of(const commands::CustomCommand& command) {
    if (!command.plugin_id.empty()) return Entry::Origin::Other;
    if (command.source == "projectSettings") return Entry::Origin::Project;
    if (command.source == "policySettings") return Entry::Origin::Managed;
    return Entry::Origin::User;
}

/// True when the session knows a command with that name.
///
/// The reference looks the name up in the list of the commands, and the
/// aliases are not part of it.
bool known_command(const std::string& name, const std::vector<commands::CustomCommand>& custom) {
    for (const auto& command : commands::all()) {
        if (command.name == name) return true;
    }
    for (const auto& command : custom) {
        if (command.name == name) return true;
    }
    return false;
}

} // namespace

std::vector<Suggestion> suggestions_for(const std::string& input, const std::string& cwd,
                                        const std::vector<commands::CustomCommand>& custom_commands,
                                        const std::vector<mcp::McpCommand>& mcp_commands,
                                        const std::vector<AgentEntry>& agents) {
    std::vector<Suggestion> items;

    if (input.empty()) return items;

    // The list of the files is opened by an `@` that stands on its own, or
    // that follows a space — not only by one at the very start of the
    // input, the way the reference opens it.
    long at_token = -1;
    for (size_t i = input.size(); i-- > 0;) {
        if (input[i] != '@') continue;
        if (i == 0 || std::isspace(static_cast<unsigned char>(input[i - 1]))) {
            at_token = static_cast<long>(i);
        }
        break;
    }

    if (input[0] == '/' && at_token < 0) {
        // The reference shows no suggestions when the command has arguments,
        // and none either when the name of a known command is followed by a
        // space: the hint of the arguments is drawn alone.
        const size_t space = input.find(' ');
        if (space != std::string::npos) {
            // The arguments the user is writing.
            if (!cc::trim(input.substr(space + 1)).empty()) return items;
            // The name of a command and a space.
            if (known_command(input.substr(1, space - 1), custom_commands)) return items;
        }

        // Every command the session knows, in one shape.
        std::vector<Entry> entries;

        // The commands the reference keeps out of the listing. They are
        // found when their name is typed exactly.
        std::vector<Entry> hidden;

        for (const auto& command : commands::all()) {
            // The commands that may not be used right now are not offered at
            // all, the way the reference keeps them out of the list.
            if (command.enabled && !command.enabled()) continue;

            Entry entry;
            entry.name = command.name;
            entry.aliases = command.aliases;
            entry.description = command.description;
            entry.shown = command.description;
            entry.insert = "/" + command.name + " ";
            entry.prompt = command.type == "prompt";
            // The reference puts the commands that are prompts at the end of
            // the list, after the commands that run inside the CLI.
            entry.origin = entry.prompt ? Entry::Origin::Other : Entry::Origin::Builtin;
            if (entry.prompt) entry.usage = config::skill_usage_score(entry.name);

            if (command.hidden) {
                hidden.push_back(std::move(entry));
                continue;
            }

            entries.push_back(std::move(entry));
        }

        // The commands the user wrote themselves are offered as well, and so
        // are the commands of the plugins. The reference gives the commands
        // of the user no tag of their own: the place they come from is
        // written at the end of their description, and the name of the
        // plugin in front of it.
        for (const auto& command : custom_commands) {
            Entry entry;
            entry.name = command.name;
            entry.description = command.description;
            entry.shown = commands::format_description_with_source(command);

            // The arguments the command takes are named at the end of the
            // description.
            const std::vector<std::string> names = commands::argument_names_of(command);
            if (!names.empty()) {
                entry.shown += " (arguments: " + cc::join(names, ", ") + ")";
            }

            entry.insert = "/" + command.name + " ";
            entry.origin = origin_of(command);
            entry.prompt = true;
            entry.usage = config::skill_usage_score(entry.name);
            entries.push_back(std::move(entry));
        }

        // The prompts of the MCP servers are commands as well; the user
        // sees them as `<server>:<prompt> (MCP)`.
        for (const auto& command : mcp_commands) {
            Entry entry;
            entry.name = command.user_facing_name;
            entry.description = command.description;
            entry.shown = command.description;
            if (!command.argument_names.empty()) {
                entry.shown += " (arguments: " + cc::join(command.argument_names, ", ") + ")";
            }
            entry.insert = "/" + command.user_facing_name + " ";
            entry.origin = Entry::Origin::Other;
            entries.push_back(std::move(entry));
        }

        const std::string query = cc::to_lower(cc::trim(input.substr(1)));

        if (query.empty()) {
            // The reference puts the five commands that were used most
            // recently at the top of the list, before the commands that run
            // inside the CLI.
            std::vector<size_t> used;
            for (size_t i = 0; i < entries.size(); ++i) {
                const Entry& entry = entries[i];
                if (!entry.prompt || entry.usage <= 0.0) continue;
                used.push_back(i);
            }
            // The commands with the highest score come first; the ones with
            // the same score keep the order they have.
            std::stable_sort(used.begin(), used.end(), [&](size_t a, size_t b) {
                return entries[a].usage > entries[b].usage;
            });
            if (used.size() > 5) used.resize(5);

            std::vector<Entry> recent;
            for (const size_t i : used) {
                recent.push_back(entries[i]);
                entries[i].recent = true;
            }

            // The commands of that group are taken out of their own group.
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                                         [](const Entry& entry) { return entry.recent; }),
                          entries.end());

            // With nothing typed the reference shows the built-in commands
            // first, then the commands of the user, then the ones of the
            // project, then the ones of the policy of the machine and the
            // others last; every group is sorted by name.
            std::stable_sort(entries.begin(), entries.end(),
                             [](const Entry& a, const Entry& b) {
                                 if (a.origin != b.origin) return a.origin < b.origin;
                                 return a.name < b.name;
                             });

            entries.insert(entries.begin(), recent.begin(), recent.end());
        } else {
            // The reference searches the commands with a fuzzy index and
            // ranks the hits: the exact name first, then the exact alias,
            // then the names that start with what was typed, the shorter
            // ones before the longer ones, then the aliases that start with
            // it, and last the commands that only matched the description.
            std::vector<std::pair<Found, Entry>> hits;
            for (Entry& entry : entries) {
                const Found found = search_command(entry, query);
                if (found.score < 0.0) continue;
                hits.emplace_back(found, std::move(entry));
            }

            std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
                return better(a.first, b.first);
            });

            entries.clear();
            entries.reserve(hits.size());
            for (auto& hit : hits) entries.push_back(std::move(hit.second));

            // The reference puts the command that is kept out of the listing
            // at the top of the results when its name is what was typed —
            // unless a command that is shown answers to the same name.
            for (const Entry& entry : hidden) {
                if (cc::to_lower(entry.name) != query) continue;
                const bool shown = std::any_of(entries.begin(), entries.end(),
                                               [&](const Entry& other) {
                                                   return cc::to_lower(other.name) == query;
                                               });
                if (shown) break;
                entries.insert(entries.begin(), entry);
                break;
            }
        }

        for (const Entry& entry : entries) {
            Suggestion item;
            item.kind = Suggestion::Kind::Command;
            item.display = "/" + entry.name;
            // The alias the user typed is shown in brackets, the way the
            // reference shows it.
            if (!entry.alias.empty()) item.display += " (" + entry.alias + ")";
            item.description = entry.shown;
            item.insert = entry.insert;
            // A command replaces the whole of the input.
            item.insert_at = 0;
            item.insert_length = input.size();
            items.push_back(std::move(item));
        }
    } else if (at_token >= 0) {
        const std::string prefix = input.substr(static_cast<size_t>(at_token) + 1);
        // The reference offers fifteen files at most; the commands are not
        // limited that way, so that the overlay can be scrolled through.
        size_t files = 0;
        for (const auto& file : matching_files(cwd, prefix)) {
            if (files++ >= kMaxFileSuggestions) break;
            const bool directory = !file.empty() && file.back() == '/';
            Suggestion item;
            item.kind = directory ? Suggestion::Kind::Directory : Suggestion::Kind::File;
            item.display = file;
            item.insert = "@" + file + " ";
            // Only the `@` and the text that follows it are replaced, the
            // way the reference replaces the token it completes.
            item.insert_at = static_cast<size_t>(at_token);
            item.insert_length = prefix.size() + 1;
            items.push_back(std::move(item));
        }

        // The sub-agents come after the files, the way the reference puts
        // them. Their name is the type of the agent and their description
        // says when the agent is to be used.
        const std::string query = cc::to_lower(prefix);
        for (const AgentEntry& agent : agents) {
            if (!query.empty() && cc::to_lower(agent.name).find(query) == std::string::npos) {
                continue;
            }
            Suggestion item;
            item.kind = Suggestion::Kind::Agent;
            item.display = agent.name + " (agent)";
            item.description = cc::truncate(agent.description, kAgentDescriptionWidth);
            // The name of the agent holds a space, so the reference writes it
            // in quotes.
            item.insert = "@\"" + item.display + " ";
            item.insert_at = static_cast<size_t>(at_token);
            item.insert_length = prefix.size() + 1;
            items.push_back(std::move(item));
        }

        // The reference cuts the whole of the list at fifteen entries.
        if (items.size() > kMaxFileSuggestions) items.resize(kMaxFileSuggestions);
    }

    return items;
}

std::vector<std::string> render_suggestions(const std::vector<Suggestion>& items, size_t selected,
                                             const Theme& theme, int width, size_t max_visible,
                                             size_t max_name_width) {
    std::vector<std::string> lines;
    if (items.empty()) return lines;

    const int w = width > 0 ? width : 1;

    // The window follows the chosen entry, the way the reference shows it.
    // The start is clamped to the rows that are left, and to zero when the
    // whole of the list fits in the window — a list that is no longer than
    // the window keeps every row on the screen.
    if (max_visible == 0) max_visible = 1;
    size_t start = 0;
    if (selected > max_visible / 2) start = selected - max_visible / 2;
    start = std::min(start, items.size() > max_visible ? items.size() - max_visible : 0);
    const size_t end = std::min(start + max_visible, items.size());

    Style chosen;
    chosen.foreground = theme.suggestion;

    // The entries that are not chosen are dimmed, the way the reference dims
    // them: the attribute of the terminal, not a colour of its own.
    Style dim;
    dim.dim = true;

    // The names of the commands line up in a column. The width of the
    // column is the width of the longest of all the commands, and not only
    // of the ones that are shown, so that the list does not jump while the
    // user types.
    size_t name_column = 0;
    bool has_commands = false;
    for (const auto& item : items) {
        if (item.kind != Suggestion::Kind::Command) continue;
        has_commands = true;
        name_column = std::max(name_column, cc::display_width(item.display) + 5);
    }
    if (has_commands) {
        if (max_name_width > 0) name_column = max_name_width;
        // The column never takes more than two fifths of the width.
        name_column = std::min(name_column, static_cast<size_t>(w) * 2 / 5);
    }

    for (size_t i = start; i < end; ++i) {
        const auto& item = items[i];
        const Style& style = i == selected ? chosen : dim;

        std::string text;
        if (item.kind == Suggestion::Kind::Command) {
            // The name is cut to the width of the column, and the column is
            // padded so that the descriptions line up.
            const size_t room = name_column > 2 ? name_column - 2 : 1;
            text = cc::truncate(item.display, room);
            const size_t used = cc::display_width(text);
            if (used < name_column) text += std::string(name_column - used, ' ');
            if (!item.tag.empty()) {
                text += styled("[" + item.tag + "] ", dim);
            }
            // The description is cut to the room that is left.
            const size_t tag_width = item.tag.empty() ? 0 : cc::display_width(item.tag) + 3;
            const size_t description_room =
                static_cast<size_t>(w) > name_column + tag_width + 4
                    ? static_cast<size_t>(w) - name_column - tag_width - 4
                    : 0;
            text += cc::truncate(squeeze_spaces(item.description), description_room);
        } else {
            // The files: the icon, the path and the description.
            const size_t separator = item.description.empty() ? 0 : 3;
            const size_t reserve =
                std::min<size_t>(20, cc::display_width(item.description));
            const size_t path_room = static_cast<size_t>(w) > 10
                                         ? static_cast<size_t>(w) - 2 - 4 - separator - reserve
                                         : 4;
            // The name of an agent is not a path, so it is not cut in the
            // middle, the way the reference writes it.
            const bool agent = item.kind == Suggestion::Kind::Agent;
            const std::string shown =
                agent ? item.display : cc::truncate_path_middle(item.display, path_room);
            text = std::string(icon_for(item.kind)) + " " + shown;
            if (!item.description.empty()) {
                const size_t used = agent ? cc::display_width(shown) : cc::display_width(item.display);
                const size_t room = static_cast<size_t>(w) > 6
                                        ? static_cast<size_t>(w) - 2 - used - separator - 4
                                        : 0;
                text += " – " + cc::truncate(squeeze_spaces(item.description), room);
            }
        }

        lines.push_back(styled(text, style));
    }

    return lines;
}

} // namespace cc::ui
