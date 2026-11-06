#include "ui/message_selector.hpp"

#include "ui/figures.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace cc::ui {
namespace {

/// The number of prompts that are shown at once.
///
/// The reference shows seven of them, with the selected one in the middle.
constexpr size_t kMaxVisibleMessages = 7;

/// The time the second press of the Ctrl-C has to come in.
constexpr auto kExitTimeout = std::chrono::milliseconds(800);

/// The sign the reference puts in front of the warning.
constexpr std::string_view kWarning = "⚠";

/// The actions the user can pick in the confirmation.
struct Action {
    /// The label of the action.
    const char* label;
    /// What the action does to the conversation.
    RewindAction action;
    /// The explanation shown under the label.
    const char* description;
    /// True when the action takes the note the user writes.
    bool takes_note = false;
};

/// The actions of the confirmation, in the order the reference lists them.
///
/// The code can only be restored when there are files to put back, so the
/// two actions that restore it are only offered then.
std::vector<Action> actions_for(bool can_restore_code) {
    std::vector<Action> actions;
    if (can_restore_code) {
        actions.push_back({"Restore code and conversation", RewindAction::RestoreBoth,
                           "The conversation will be forked."});
    }
    actions.push_back({"Restore conversation", RewindAction::Restore,
                       "The conversation will be forked."});
    if (can_restore_code) {
        actions.push_back({"Restore code", RewindAction::RestoreCode,
                           "The conversation will be unchanged."});
    }
    actions.push_back({"Summarize from here", RewindAction::Summarize,
                       "Messages after this point will be summarized.", true});
    actions.push_back({"Never mind", RewindAction::Cancel, "The conversation will be unchanged."});
    return actions;
}

/// A string of `count` copies of `character`.
std::string repeat(std::string_view character, size_t count) {
    std::string out;
    out.reserve(character.size() * count);
    for (size_t i = 0; i < count; ++i) out += character;
    return out;
}

/// The style of the title.
Style title_style(const Theme& theme) {
    Style style;
    style.bold = true;
    style.foreground = theme.suggestion;
    return style;
}

/// The style of the text of a prompt that is not highlighted.
Style text_style(const Theme& theme) {
    Style style;
    style.foreground = theme.text;
    return style;
}

/// The style of the text of the prompt that is highlighted.
Style selected_style(const Theme& theme) {
    Style style;
    style.foreground = theme.suggestion;
    return style;
}

/// The style of the dimmed text.
Style dim_style(const Theme& theme) {
    Style style;
    style.foreground = theme.inactive;
    return style;
}

/// The style of the pointer in front of the chosen entry.
Style pointer_style(const Theme& theme) {
    Style style;
    style.bold = true;
    style.foreground = theme.permission;
    return style;
}

/// The number of the option, padded to the width of the largest one.
///
/// The reference pads it so that every label starts in the same column.
std::string option_number(size_t index, size_t count) {
    std::string number = std::to_string(index + 1) + ".";
    number.resize(std::to_string(count).size() + 1, ' ');
    return number;
}

/// The row of the list that tells what the code of a prompt changed.
///
/// The reference names the files that changed and the lines that were
/// added and removed, or says that the prompt changed no code at all. A
/// prompt whose files cannot be put back carries a warning instead.
std::string stats_line(const RewindOption& option, const Theme& theme) {
    // The prompt that is being written has no code of its own.
    if (option.current) return {};

    if (!option.diff) {
        Style style;
        style.foreground = theme.warning;
        return styled(std::string(kWarning) + " No code restore", style);
    }

    const RewindDiff& diff = *option.diff;
    Style style;
    style.foreground = theme.inactive;

    if (diff.files_changed.empty()) return styled("No code changes", style);

    std::string text;
    if (diff.files_changed.size() == 1) {
        text = cc::basename(diff.files_changed.front()) + " ";
    } else {
        text = std::to_string(diff.files_changed.size()) + " files changed ";
    }

    Style added;
    added.foreground = theme.diff_added_word;
    Style removed;
    removed.foreground = theme.diff_removed_word;

    return styled(text, style) + styled("+" + std::to_string(diff.insertions) + " ", added) +
           styled("-" + std::to_string(diff.deletions), removed);
}

/// The names of the files that a restore would touch.
///
/// The reference names the first one, and the second one as well when there
/// are only two.
std::string file_label(const std::vector<std::string>& files) {
    if (files.size() == 1) return cc::basename(files.front());
    if (files.size() == 2) {
        return cc::basename(files[0]) + " and " + cc::basename(files[1]);
    }
    return cc::basename(files[0]) + " and " + std::to_string(files.size() - 1) + " other files";
}

/// The option a number key names, or -1 when the key is not a number.
long number_index(const std::string& text) {
    if (text.empty()) return -1;

    long value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return -1;
        value = value * 10 + (c - '0');
        if (value > 100000) return -1;
    }
    return value - 1;
}

} // namespace

MessageSelector::MessageSelector(Theme theme, std::vector<RewindOption> options)
    : theme_(std::move(theme)), options_(std::move(options)) {
    // The newest prompt is the one that is highlighted to begin with, the
    // way the reference starts.
    if (!options_.empty()) selected_ = options_.size() - 1;
}

void MessageSelector::set_selected(size_t index) {
    if (options_.empty()) return;
    selected_ = std::min(index, options_.size() - 1);
}

std::vector<std::string> MessageSelector::render_list(int width) const {
    Style rule;
    rule.foreground = theme_.suggestion;

    std::vector<std::string> lines;
    lines.push_back(styled(repeat(figures::kHorizontalRule, static_cast<size_t>(std::max(width, 1))),
                            rule));
    lines.push_back(" " + styled("Rewind", title_style(theme_)));
    lines.push_back("");

    if (options_.empty()) {
        lines.push_back(" Nothing to rewind to yet.");
        return lines;
    }

    // The files of the session are checkpointed, so the code can be put
    // back as well; the reference says so.
    lines.push_back(" Restore the code and/or conversation to the point before…");
    lines.push_back("");

    // The selected prompt sits in the middle of the visible ones.
    const size_t first = selected_ >= visible_ / 2
                             ? std::min(selected_ - visible_ / 2,
                                        options_.size() > visible_ ? options_.size() - visible_ : 0)
                             : 0;

    const size_t last = std::min(first + visible_, options_.size());

    for (size_t i = first; i < last; ++i) {
        const bool is_selected = i == selected_;
        const auto& option = options_[i];

        std::string line = is_selected ? styled(figures::kPointer, pointer_style(theme_)) + " " : "  ";

        if (option.current) {
            // The prompt that is being written is not shown; the reference
            // names it instead. It is drawn in the colour of the text when
            // it is not the one that is chosen.
            Style current;
            current.italic = true;
            if (is_selected) current.foreground = theme_.suggestion;
            line += styled("(current)", current);
        } else {
            const size_t available = width > 4 ? static_cast<size_t>(width) - 4 : 1;
            std::string text = option.text;
            if (cc::display_width(text) > available) text = cc::truncate(text, available);
            line += styled(text, is_selected ? selected_style(theme_) : text_style(theme_));
        }
        lines.push_back(line);

        // Every prompt takes a row of its own for the code it changed.
        const std::string stats = stats_line(option, theme_);
        lines.push_back(stats.empty() ? std::string() : "  " + stats);

        // The reference leaves a row under every prompt.
        lines.push_back("");
    }

    lines.push_back("");
    Style hint;
    hint.italic = true;
    hint.foreground = theme_.inactive;

    // The first Ctrl-C only asks for a second one.
    if (exiting_) {
        lines.push_back(" " + styled("Press Ctrl-C again to exit", hint));
    } else {
        lines.push_back(" " + styled("Enter to continue · Esc to exit", hint));
    }

    return lines;
}

std::vector<std::string> MessageSelector::render_confirm(int width) const {
    Style rule;
    rule.foreground = theme_.suggestion;

    std::vector<std::string> lines;
    lines.push_back(styled(repeat(figures::kHorizontalRule, static_cast<size_t>(std::max(width, 1))),
                            rule));
    lines.push_back(" " + styled("Rewind", title_style(theme_)));
    lines.push_back("");

    const auto& option = options_[selected_];

    // The code can only be restored when there are files to put back.
    const bool can_restore_code = option.restore && !option.restore->files_changed.empty();
    const auto actions = actions_for(can_restore_code);
    const size_t action = std::min(action_, actions.size() - 1);

    // The reference names the conversation only when there is no code that
    // could be restored.
    lines.push_back(std::string(" Confirm you want to restore ") +
                    (option.restore ? "" : "the conversation ") +
                    "to the point before you sent this message:");
    lines.push_back("");

    // The prompt is shown in a box that is open on the right, the way the
    // reference draws it.
    Style border;
    border.foreground = theme_.inactive;

    const size_t available = width > 4 ? static_cast<size_t>(width) - 4 : 1;
    std::string text = option.text;
    if (cc::display_width(text) > available) text = cc::truncate(text, available);

    lines.push_back(" " + styled("│ ", border) + styled(text, text_style(theme_)));
    if (!option.ago.empty()) {
        lines.push_back(" " + styled("│ ", border) + styled("(" + option.ago + ")", dim_style(theme_)));
    }
    lines.push_back("");

    // The explanation of the action that is highlighted.
    const Action& chosen = actions[action];
    lines.push_back(" " + styled(chosen.description, dim_style(theme_)));

    // The line that tells what happens to the code is left out when the
    // conversation is summarised, the way the reference leaves it out.
    if (chosen.action != RewindAction::Summarize) {
        const bool restores_code =
            chosen.action == RewindAction::RestoreBoth || chosen.action == RewindAction::RestoreCode;
        std::string line = " ";
        if (restores_code && option.restore && !option.restore->files_changed.empty()) {
            const RewindDiff& diff = *option.restore;
            Style added;
            added.foreground = theme_.diff_added_word;
            Style removed;
            removed.foreground = theme_.diff_removed_word;

            line += styled("The code will be restored ", dim_style(theme_));
            line += styled("+" + std::to_string(diff.insertions) + " ", added);
            line += styled("-" + std::to_string(diff.deletions), removed);
            line += styled(" in " + file_label(diff.files_changed) + ".", dim_style(theme_));
        } else if (restores_code && option.restore) {
            line += styled("The code has not changed (nothing will be restored).", dim_style(theme_));
        } else {
            line += styled("The code will be unchanged.", dim_style(theme_));
        }
        lines.push_back(line);
    }
    lines.push_back("");

    for (size_t i = 0; i < actions.size(); ++i) {
        const bool is_selected = i == action;
        std::string line = is_selected ? styled(figures::kPointer, pointer_style(theme_)) + " " : "  ";
        line += styled(option_number(i, actions.size()), dim_style(theme_));
        line += " ";

        if (actions[i].takes_note) {
            // The note the user writes stands after the label, the way the
            // reference shows it.
            line += styled(actions[i].label, is_selected ? selected_style(theme_) : text_style(theme_));
            if (is_selected) {
                line += styled(": ", selected_style(theme_));
                if (feedback_.empty()) {
                    Style placeholder;
                    placeholder.foreground = theme_.inactive;
                    line += styled("add context (optional)", placeholder);
                } else {
                    line += styled(feedback_, selected_style(theme_));
                }
                Style caret;
                caret.inverse = true;
                line += styled(" ", caret);
            } else if (!feedback_.empty()) {
                line += styled(": " + feedback_, text_style(theme_));
            }
        } else {
            line += styled(actions[i].label, is_selected ? selected_style(theme_) : text_style(theme_));
        }
        lines.push_back(line);
    }

    // The reference warns that the files that were changed by hand are left
    // alone.
    if (can_restore_code) {
        lines.push_back("");
        lines.push_back(" " + styled(std::string(kWarning) +
                                      " Rewinding does not affect files edited manually or via bash.",
                                      dim_style(theme_)));
        lines.push_back("");
    }

    return lines;
}

std::vector<std::string> MessageSelector::render(int width) const {
    if (confirming_) return render_confirm(width);
    return render_list(width);
}

RewindAction MessageSelector::run(Terminal& terminal) {
    const int width = terminal.width();

    // Every prompt takes three rows of its own, so only as many of them are
    // shown as fit on the screen: the region of the view may never be
    // taller than the screen.
    constexpr int kFixedRows = 8;
    const int room = std::max(1, (terminal.height() - kFixedRows) / 3);
    visible_ = std::min<size_t>(kMaxVisibleMessages, static_cast<size_t>(room));

    while (true) {
        terminal.render_dynamic(render(width));

        const auto key = terminal.read_key(100);
        if (!key) {
            // The terminal closed: the view ends rather than wait for a
            // key that will never come.
            if (terminal.closed()) {
                terminal.clear_dynamic();
                return RewindAction::None;
            }
            continue;
        }

        if (confirming_) {
            const auto& option = options_[selected_];
            const bool can_restore_code = option.restore && !option.restore->files_changed.empty();
            const auto actions = actions_for(can_restore_code);
            size_t action = std::min(action_, actions.size() - 1);

            switch (key->type) {
            case Key::Type::Up:
                if (action > 0) --action;
                break;
            case Key::Type::Down:
                if (action + 1 < actions.size()) ++action;
                break;
            case Key::Type::Enter: {
                const RewindAction chosen = actions[action].action;
                // "Never mind" goes back to the list of prompts.
                if (chosen == RewindAction::Cancel) {
                    confirming_ = false;
                    action = 0;
                    break;
                }
                terminal.clear_dynamic();
                return chosen;
            }
            case Key::Type::Escape:
            case Key::Type::CtrlC:
                // The confirmation is dismissed; the list of prompts shows
                // again.
                confirming_ = false;
                action = 0;
                break;
            case Key::Type::Backspace:
                // The note of the summary is edited.
                if (actions[action].takes_note && !feedback_.empty()) feedback_.pop_back();
                break;
            case Key::Type::Character: {
                // The note of the summary takes the keys that are typed
                // while it is the chosen action.
                if (actions[action].takes_note) {
                    feedback_ += key->text;
                    break;
                }
                if (key->text == "k") {
                    if (action > 0) --action;
                } else if (key->text == "j") {
                    if (action + 1 < actions.size()) ++action;
                } else {
                    // A number picks the action with that number.
                    const long index = number_index(key->text);
                    if (index < 0 || static_cast<size_t>(index) >= actions.size()) break;
                    const RewindAction chosen = actions[static_cast<size_t>(index)].action;
                    if (chosen == RewindAction::Cancel) {
                        confirming_ = false;
                        action = 0;
                        break;
                    }
                    terminal.clear_dynamic();
                    return chosen;
                }
                break;
            }
            default:
                break;
            }
            action_ = action;
            continue;
        }

        switch (key->type) {
        case Key::Type::Up:
            if (!key->ctrl && !key->alt) {
                if (selected_ > 0) --selected_;
            } else {
                selected_ = 0;
            }
            break;
        case Key::Type::Down:
            if (!key->ctrl && !key->alt) {
                if (selected_ + 1 < options_.size()) ++selected_;
            } else {
                if (!options_.empty()) selected_ = options_.size() - 1;
            }
            break;
        case Key::Type::ShiftUp:
            selected_ = 0;
            break;
        case Key::Type::ShiftDown:
            if (!options_.empty()) selected_ = options_.size() - 1;
            break;
        case Key::Type::Home:
            selected_ = 0;
            break;
        case Key::Type::End:
            if (!options_.empty()) selected_ = options_.size() - 1;
            break;
        case Key::Type::CtrlP:
            if (selected_ > 0) --selected_;
            break;
        case Key::Type::CtrlN:
            if (selected_ + 1 < options_.size()) ++selected_;
            break;
        case Key::Type::Enter:
            if (options_.empty()) {
                terminal.clear_dynamic();
                return RewindAction::None;
            }
            // The prompt that is being written cannot be rewound to.
            if (options_[selected_].current) break;
            confirming_ = true;
            action_ = 0;
            break;
        case Key::Type::Character:
            if (key->text == "k") {
                if (selected_ > 0) --selected_;
            } else if (key->text == "j") {
                if (selected_ + 1 < options_.size()) ++selected_;
            } else if (key->text == "K") {
                selected_ = 0;
            } else if (key->text == "J") {
                if (!options_.empty()) selected_ = options_.size() - 1;
            }
            break;
        case Key::Type::Escape:
            terminal.clear_dynamic();
            return RewindAction::None;
        case Key::Type::CtrlC: {
            // The reference asks for a second press before it leaves.
            const auto now = std::chrono::steady_clock::now();
            if (exiting_ && now - exit_at_ <= kExitTimeout) {
                terminal.clear_dynamic();
                return RewindAction::None;
            }
            exiting_ = true;
            exit_at_ = now;
            break;
        }
        default:
            break;
        }
    }
}

} // namespace cc::ui
