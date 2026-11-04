#pragma once

#include "commands/custom.hpp"
#include "mcp/command.hpp"
#include "ui/theme.hpp"

#include <string>
#include <vector>

namespace cc::ui {

/// A sub-agent the list of the `@` offers.
struct AgentEntry {
    /// The type of the agent, the way the reference names it.
    std::string name;
    /// When the agent is to be used.
    std::string description;
};

/// One entry of the suggestions overlay.
struct Suggestion {
    /// What the entry refers to.
    enum class Kind {
        Command,   ///< A slash command.
        File,      ///< A file in the working directory.
        Directory, ///< A directory in the working directory.
        Agent,     ///< A sub-agent.
        Other,     ///< Anything else.
    };

    Kind kind = Kind::Other;
    /// The text shown for the entry.
    std::string display;
    /// The description shown after the entry.
    std::string description;
    /// An optional tag, e.g. `[plugin]`.
    std::string tag;
    /// The text that replaces the input when the entry is chosen.
    std::string insert;
    /// The place in the input where `insert` is written.
    size_t insert_at = 0;
    /// The length of the piece of the input that `insert` replaces.
    ///
    /// A command replaces the whole of the input; a file replaces the `@`
    /// and the text that follows it, the way the reference replaces the
    /// token it completes.
    size_t insert_length = 0;
};

/// The suggestions for the text in the prompt box.
///
/// A leading `/` matches the slash commands, a leading `@` matches the
/// files in the working directory. Anything else has no suggestions.
///
/// The commands the user wrote themselves are offered as well; they are
/// passed in, so that the files are not read again for every keystroke. The
/// prompts of the MCP servers are offered the same way, and so are the
/// sub-agents, which the `@` offers under the files.
std::vector<Suggestion> suggestions_for(const std::string& input, const std::string& cwd,
                                        const std::vector<commands::CustomCommand>& custom_commands = {},
                                        const std::vector<mcp::McpCommand>& mcp_commands = {},
                                        const std::vector<AgentEntry>& agents = {});

/// The number of entries the overlay shows at once.
inline constexpr size_t kMaxOverlayItems = 5;

/// The number of files the suggestions offer at most.
///
/// The reference offers fifteen of them; the commands are not limited that
/// way, so that the overlay can be scrolled through.
inline constexpr size_t kMaxFileSuggestions = 15;

/// Renders the suggestions overlay.
///
/// The overlay is a list of at most five entries; the chosen one is drawn in
/// the theme's `suggestion` colour and the rest are dim. The window follows
/// the chosen entry, so that the list may be longer than the window.
///
/// `max_name_width` is the width of the column of the names of the
/// commands, taken over all the commands and not only over the ones that
/// are shown: the list does not jump while the user types.
std::vector<std::string> render_suggestions(const std::vector<Suggestion>& items, size_t selected,
                                             const Theme& theme, int width,
                                             size_t max_visible = kMaxOverlayItems,
                                             size_t max_name_width = 0);

} // namespace cc::ui
