#include "ui/diff_view.hpp"

#include "ui/figures.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cc::ui {
namespace {

/// The number of files the list shows at once.
constexpr size_t kMaxVisibleFiles = 5;

/// The number of diff lines that are shown for one file.
constexpr size_t kMaxDiffLines = 400;

/// The padding of the pane.
///
/// The reference's Pane puts two columns of padding on the left of its
/// content and a blank line above the rule that tops it.
constexpr size_t kPanePadding = 2;

/// The padding of the pane of the modal slot.
///
/// The pane of the modal slot has one column of padding of its own, so
/// that its contents sit three columns in.
constexpr size_t kModalPanePadding = 3;

/// The number of rows of the conversation that stay above the modal slot.
///
/// The reference keeps two rows of the conversation visible above the rule
/// of the modal slot, so that the pane floats over it.
constexpr int kModalPeek = 2;

/// A string of `count` copies of `text`.
std::string repeat(std::string_view text, size_t count) {
    std::string out;
    out.reserve(text.size() * count);
    for (size_t i = 0; i < count; ++i) out += text;
    return out;
}

/// Truncates `text` to `max_width` columns, keeping the end of it.
///
/// The ellipsis goes in front, so that the name of the file stays visible.
std::string truncate_start(std::string_view text, size_t max_width) {
    if (cc::display_width(text) <= max_width) return std::string(text);
    if (max_width <= 1) return "…";

    const auto clusters = cc::graphemes(text);
    size_t width = 0;
    size_t index = clusters.size();
    while (index > 0) {
        const size_t cluster = cc::display_width(clusters[index - 1]);
        if (width + cluster > max_width - 1) break;
        width += cluster;
        --index;
    }

    std::string out = "…";
    for (size_t i = index; i < clusters.size(); ++i) out += clusters[i];
    return out;
}

/// The line numbers a hunk starts at.
struct HunkHeader {
    int old_start = 0;
    int new_start = 0;
};

/// Reads the number that starts at `index`.
int read_number(const std::string& text, size_t& index) {
    int value = 0;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
        value = value * 10 + (text[index] - '0');
        ++index;
    }
    return value;
}

/// Parses the `@@ -old,count +new,count @@` header of a hunk.
HunkHeader parse_hunk_header(const std::string& line) {
    HunkHeader header;

    size_t index = 2;  // Past the "@@".
    while (index < line.size() && line[index] == ' ') ++index;
    if (index >= line.size() || line[index] != '-') return header;
    ++index;
    header.old_start = read_number(line, index);

    while (index < line.size() && line[index] != '+') ++index;
    if (index >= line.size()) return header;
    ++index;
    header.new_start = read_number(line, index);
    return header;
}

/// Parses the unified diff git produced into the lines the port renders.
///
/// The line numbers come from the hunk headers, so they point at the lines
/// of the file rather than at the lines of the hunk.
std::vector<DiffLine> parse_diff(const std::string& diff) {
    std::vector<DiffLine> lines;

    int old_line = 0;
    int new_line = 0;
    bool in_hunk = false;

    for (const auto& raw : cc::split_lines(diff)) {
        if (!in_hunk) {
            if (!cc::starts_with(raw, "@@ ")) continue;
            in_hunk = true;
            const HunkHeader header = parse_hunk_header(raw);
            old_line = header.old_start;
            new_line = header.new_start;
            continue;
        }

        if (raw.empty()) {
            lines.push_back(DiffLine{DiffLine::Kind::Context, "", old_line, new_line});
            ++old_line;
            ++new_line;
            continue;
        }

        switch (raw[0]) {
        case ' ':
            lines.push_back(DiffLine{DiffLine::Kind::Context, raw.substr(1), old_line, new_line});
            ++old_line;
            ++new_line;
            break;
        case '-':
            lines.push_back(DiffLine{DiffLine::Kind::Removed, raw.substr(1), old_line, 0});
            ++old_line;
            break;
        case '+':
            lines.push_back(DiffLine{DiffLine::Kind::Added, raw.substr(1), 0, new_line});
            ++new_line;
            break;
        default:
            // The "\ No newline at end of file" marker, and anything else
            // git writes between the hunks.
            break;
        }
    }

    return lines;
}

} // namespace

DiffView::DiffView(Theme theme, std::vector<git::ChangedFile> files)
    : theme_(std::move(theme)) {
    DiffSource current;
    current.name = "Current";
    current.title = "Uncommitted changes";
    current.subtitle = " (git diff HEAD)";
    current.files = std::move(files);
    sources_.push_back(std::move(current));
}

DiffView::DiffView(Theme theme, std::vector<DiffSource> sources)
    : theme_(std::move(theme)), sources_(std::move(sources)) {}

void DiffView::set_state(State state) {
    state_ = state;
}

void DiffView::set_files(std::vector<git::ChangedFile> files) {
    if (sources_.empty()) {
        DiffSource current;
        current.name = "Current";
        current.title = "Uncommitted changes";
        current.subtitle = " (git diff HEAD)";
        sources_.push_back(std::move(current));
    }
    const size_t count = files.size();
    sources_[source_].files = std::move(files);
    if (selected_ >= count) selected_ = 0;
    scroll_ = 0;
}

void DiffView::select_file(size_t index) {
    if (index >= files().size()) return;
    selected_ = index;
    // The scroll of the list is the scroll of the pane, and it is kept when
    // the selection moves; the scroll of the diff belongs to the file.
    if (detail_) scroll_ = 0;
}

void DiffView::select_source(size_t index) {
    if (index >= sources_.size()) return;
    source_ = index;
    selected_ = 0;
    scroll_ = 0;
}

void DiffView::scroll_to(size_t line) {
    scroll_ = line;
}

void DiffView::show_detail(bool show) {
    detail_ = show;
    scroll_ = 0;
}

void DiffView::set_modal(bool modal) {
    modal_ = modal;
}

const DiffSource& DiffView::source() const {
    static const DiffSource kEmpty;
    if (source_ >= sources_.size()) return kEmpty;
    return sources_[source_];
}

const std::vector<git::ChangedFile>& DiffView::files() const {
    return source().files;
}

std::string DiffView::pane_padding() const {
    // The pane of the modal slot has one column of padding of its own, so
    // that its contents sit three columns in.
    return std::string(modal_ ? kModalPanePadding : kPanePadding, ' ');
}

std::string DiffView::title() const {
    Style bold;
    bold.bold = true;
    bold.foreground = theme_.background;

    // The reference resolves the dim colour of the suffix to the inactive
    // grey of the theme, and the suffix keeps the bold of the title.
    Style dim;
    dim.foreground = theme_.inactive;
    dim.bold = true;

    const DiffSource& current = source();
    return styled(current.title, bold) + styled(current.subtitle, dim);
}

std::string DiffView::source_selector() const {
    Style dim;
    dim.foreground = theme_.inactive;

    std::string out;
    if (source_ > 0) out += styled("◀ ", dim);

    for (size_t i = 0; i < sources_.size(); ++i) {
        Style style;
        style.bold = i == source_;
        if (i != source_) style.foreground = theme_.inactive;

        if (i > 0) out += styled(" · ", style);
        out += styled(sources_[i].name, style);
    }

    if (source_ + 1 < sources_.size()) out += styled(" ▶", dim);
    return out;
}

std::string DiffView::stats() const {
    const auto& changed = files();

    int added = 0;
    int removed = 0;
    for (const auto& file : changed) {
        added += file.added;
        removed += file.removed;
    }

    Style dim;
    dim.foreground = theme_.inactive;

    std::string out = styled(std::to_string(changed.size()) +
                                 " " + (changed.size() == 1 ? "file" : "files") + " changed",
                             dim);

    if (added > 0) {
        Style word;
        word.foreground = theme_.diff_added_word;
        out += " " + styled("+" + std::to_string(added), word);
    }
    if (removed > 0) {
        Style word;
        word.foreground = theme_.diff_removed_word;
        out += " " + styled("-" + std::to_string(removed), word);
    }
    return out;
}

std::string DiffView::empty_message() const {
    if (state_ == State::Loading) return "Loading diff…";

    // The changes of a turn are the ones of that turn alone.
    if (source_ > 0) return "No file changes in this turn";

    if (state_ == State::TooMany) return "Too many files to display details";

    return "Working tree is clean";
}

std::string DiffView::file_stats(const git::ChangedFile& file, bool selected) const {
    if (file.untracked || file.binary || file.large) {
        Style note;
        // The reference dims the note of a row only when the row is not the
        // one that is selected.
        note.foreground = selected ? theme_.text : theme_.inactive;
        note.italic = true;

        if (file.untracked) return styled("untracked", note);
        if (file.binary) return styled("Binary file", note);
        return styled("Large file modified", note);
    }

    std::string out;

    if (file.added > 0) {
        Style word;
        word.foreground = theme_.diff_added_word;
        word.bold = selected;
        out += styled("+" + std::to_string(file.added), word);
    }
    if (file.added > 0 && file.removed > 0) out += " ";
    if (file.removed > 0) {
        Style word;
        word.foreground = theme_.diff_removed_word;
        word.bold = selected;
        out += styled("-" + std::to_string(file.removed), word);
    }
    if (file.truncated) {
        Style note;
        // The reference dims the note of a row only when the row is not the
        // one that is selected.
        note.foreground = selected ? theme_.text : theme_.inactive;
        out += styled(" (truncated)", note);
    }
    return out;
}

std::string DiffView::render_file(const git::ChangedFile& file, bool selected, int width) const {
    const size_t w = width > 1 ? static_cast<size_t>(width) : 1;

    const std::string pointer = selected ? std::string(figures::kPointer) + " " : "  ";
    const std::string stats = file_stats(file, selected);

    // The path is cut to a fixed width, the way the reference cuts it, so
    // that the counts always fit at the end of the line.
    const size_t path_width = w > 23 ? w - 23 : 20;

    // The row that is selected is painted with the colour of the theme's
    // background and turned upside down, the way the reference paints it.
    Style path_style;
    path_style.foreground = selected ? theme_.background : theme_.text;
    path_style.bold = selected;
    path_style.inverse = selected;

    const std::string path = truncate_start(file.path, path_width);

    // The pointer and the path share the band of the selected row.
    std::string line = pane_padding() + styled(pointer + path, path_style);

    // The counts sit at the right end of the line, so the path gets what is
    // left over.
    const size_t indent = pane_padding().size();
    const size_t room = w > 2 * indent ? w - 2 * indent : 1;
    const size_t used = cc::display_width(pointer) + cc::display_width(path);
    const size_t stats_width = cc::display_width(stats);
    const size_t padding = room > used + stats_width ? room - used - stats_width : 0;
    line += std::string(padding, ' ') + stats;
    return line;
}

std::vector<std::string> DiffView::render_list(int width) const {
    const auto& changed = files();

    Style dim;
    dim.foreground = theme_.inactive;

    if (changed.empty()) {
        return {pane_padding() + styled(empty_message(), dim)};
    }

    // The list is paginated when there are more files than it shows. The two
    // rows that say so are always there then, so that the list keeps its
    // height and the files do not jump under the cursor.
    const bool needs_pagination = changed.size() > kMaxVisibleFiles;

    // The reference's list always holds five files: it knows nothing of the
    // height of the terminal, and the pane around it scrolls.
    const size_t visible = kMaxVisibleFiles;

    // The list follows the selection.
    size_t start = selected_ > visible / 2 ? selected_ - visible / 2 : 0;
    size_t end = start + visible;
    if (end > changed.size()) {
        end = changed.size();
        start = end > visible ? end - visible : 0;
    }

    std::vector<std::string> lines;

    if (needs_pagination) {
        const std::string above = start > 0 ? " ↑ " + std::to_string(start) + " more " +
                                                   (start == 1 ? "file" : "files")
                                             : " ";
        lines.push_back(pane_padding() + styled(above, dim));
    }

    for (size_t i = start; i < end; ++i) {
        lines.push_back(render_file(changed[i], i == selected_, width));
    }

    if (needs_pagination) {
        const size_t below = changed.size() - end;
        const std::string more = below > 0 ? " ↓ " + std::to_string(below) + " more " +
                                                      (below == 1 ? "file" : "files")
                                            : " ";
        lines.push_back(pane_padding() + styled(more, dim));
    }

    return lines;
}

int DiffView::chrome() const {
    // The blank line above the rule, the rule, the title, the blank line
    // below it, the stats, the two blank lines below them, the blank line
    // above the byline and the byline itself. The modal slot has no blank
    // line above its rule.
    return (modal_ ? 8 : 9) + (sources_.size() > 1 ? 2 : 0);
}

int DiffView::diff_room(int content_height) const {
    const auto& changed = files();
    if (changed.empty() || selected_ >= changed.size()) return 1;

    const git::ChangedFile& file = changed[selected_];

    // The path and the rule are always there, and the note that says the
    // diff was cut short sits under the diff.
    const int fixed = 2 + (file.truncated ? 1 : 0);
    return std::max(1, content_height - fixed);
}

size_t DiffView::max_scroll(int content_height) const {
    const auto& changed = files();
    if (changed.empty() || selected_ >= changed.size()) return 0;

    const git::ChangedFile& file = changed[selected_];
    if (file.untracked || file.binary || file.large) return 0;

    const size_t lines = std::min(kMaxDiffLines, parse_diff(file.diff).size());
    const size_t room = static_cast<size_t>(diff_room(content_height));
    return lines > room ? lines - room : 0;
}

size_t DiffView::max_list_scroll(int width, int content_height) const {
    if (detail_) return 0;
    const size_t lines = render_list(width).size();
    const size_t room = static_cast<size_t>(std::max(1, content_height));
    return lines > room ? lines - room : 0;
}

std::vector<std::string> DiffView::render_detail(int width, int height) const {
    const auto& changed = files();

    Style dim;
    dim.foreground = theme_.inactive;

    std::vector<std::string> lines;

    if (changed.empty() || selected_ >= changed.size()) {
        lines.push_back(pane_padding() + styled(empty_message(), dim));
        return lines;
    }

    const git::ChangedFile& file = changed[selected_];
    const size_t w = width > 1 ? static_cast<size_t>(width) : 1;

    // The path of the file.
    Style bold;
    bold.bold = true;
    std::string header = styled(file.path, bold);
    if (file.untracked) {
        header += styled(" (untracked)", dim);
    } else if (file.truncated) {
        header += styled(" (truncated)", dim);
    }
    lines.push_back(pane_padding() + header);

    // The rule that separates the path from the diff.
    Style rule;
    rule.foreground = theme_.inactive;
    lines.push_back(pane_padding() + styled(repeat(figures::kHorizontalRule, w > 4 ? w - 4 : 1), rule));

    Style note;
    note.foreground = theme_.inactive;
    note.italic = true;

    if (file.untracked) {
        lines.push_back(pane_padding() + styled("New file not yet staged.", note));
        lines.push_back(pane_padding() + styled("Run `git add " + file.path + "` to see line counts.", note));
        return lines;
    }
    if (file.binary) {
        lines.push_back(pane_padding() + styled("Binary file - cannot display diff", note));
        return lines;
    }
    if (file.large) {
        lines.push_back(pane_padding() + styled("Large file - diff exceeds 1 MB limit", note));
        return lines;
    }

    const auto parsed = parse_diff(file.diff);
    if (parsed.empty()) {
        Style plain;
        plain.foreground = theme_.inactive;
        lines.push_back(pane_padding() + styled("No diff content", plain));
        return lines;
    }

    // The diff itself. The reference shows every line of it and lets the
    // pane scroll, so the whole diff is drawn here and the window that is on
    // screen is moved with the scroll keys. The reference draws no summary
    // of the change in the pane; the summary belongs to the result of the
    // tool, where the reference draws it too.
    const auto rendered =
        render_diff(parsed, theme_, static_cast<int>(w) - static_cast<int>(2 * pane_padding().size()));
    const size_t shown = std::min(kMaxDiffLines, rendered.size());

    const size_t room = static_cast<size_t>(diff_room(height));
    const size_t offset = std::min(scroll_, shown > room ? shown - room : 0);
    for (size_t i = offset; i < shown && i < offset + room; ++i) {
        lines.push_back(pane_padding() + rendered[i]);
    }

    if (file.truncated) {
        lines.push_back(pane_padding() + styled("… diff truncated (exceeded 400 line limit)", note));
    }

    return lines;
}

std::string DiffView::byline() const {
    if (detail_) {
        // The reference scrolls the diff in the pane around the dialog, so
        // the byline names no scroll key.
        return "← back · esc close";
    }

    std::string out;
    if (sources_.size() > 1) out = "←/→ source · ";
    out += "↑/↓ select · Enter view · esc close";
    return out;
}

std::vector<std::string> DiffView::render(int width, int height) const {
    const size_t w = width > 1 ? static_cast<size_t>(width) : 1;

    // The modal slot leaves a few rows of the conversation above it, so that
    // the pane floats over the conversation.
    const int room = modal_ ? std::max(1, height - kModalPeek) : height;

    std::vector<std::string> lines;

    if (modal_) {
        // The modal slot of the full-screen layout is topped by a rule of its
        // own, in the colour of the permission.
        Style slot;
        slot.foreground = theme_.permission;
        lines.push_back(styled(repeat(figures::kTopRule, w), slot));
    } else {
        // The blank line above the rule that tops the pane.
        lines.push_back("");

        // The rule that tops the pane. The pane of the reference is drawn with
        // the colour of the theme's background, the colour its title has too.
        Style rule;
        rule.foreground = theme_.background;
        lines.push_back(styled(repeat(figures::kHorizontalRule, w), rule));
    }

    // The title.
    lines.push_back(pane_padding() + title());
    lines.push_back("");

    // The selector that switches between the sources.
    if (sources_.size() > 1) {
        lines.push_back(pane_padding() + source_selector());
        lines.push_back("");
    }

    // The number of files and lines that changed.
    lines.push_back(pane_padding() + stats());
    lines.push_back("");

    // The reference's dialog has a gap of its own between the summary and
    // the content, and the box that holds the content has a margin above it:
    // two blank rows fall between them.
    lines.push_back("");

    // The content: the list of the files, or the diff of the selected one.
    const int content_height = std::max(1, room - chrome());
    auto content = detail_ ? render_detail(static_cast<int>(w), content_height)
                            : render_list(static_cast<int>(w));
    if (!detail_ && static_cast<int>(content.size()) > content_height) {
        // The pane is taller than the room it has, so the window of the pane
        // moves with the scroll keys, the way the pane of the reference
        // scrolls.
        const size_t offset = std::min(scroll_, max_list_scroll(static_cast<int>(w), content_height));
        content.erase(content.begin(), content.begin() + static_cast<long>(offset));
        content.resize(static_cast<size_t>(content_height));
    }
    for (const auto& line : content) {
        lines.push_back(line);
    }

    // The keys.
    lines.push_back("");
    Style hint;
    hint.foreground = theme_.inactive;
    hint.italic = true;
    lines.push_back(pane_padding() + styled(byline(), hint));

    return lines;
}

void DiffView::run(Terminal& terminal) {
    // Without a terminal there are no keys to read, and the loop below
    // would spin.
    if (!terminal.interactive()) return;

    // The reference floats the panes of the slash commands in the modal slot
    // of the full-screen layout.
    modal_ = terminal.fullscreen();

    while (true) {
        const int width = terminal.width();
        const int height = terminal.height();
        // The modal slot leaves a few rows of the conversation above it.
        const int room = modal_ ? std::max(1, height - kModalPeek) : height;
        const int content_height = std::max(1, room - chrome());
        terminal.render_dynamic(render(width, height));

        const auto key = terminal.read_key(100);
        if (!key) {
            // The terminal closed: the view ends rather than wait for a
            // key that will never come.
            if (terminal.closed()) {
                terminal.clear_dynamic();
                return;
            }
            continue;
        }

        switch (key->type) {
        case Key::Type::Up:
            // In the diff the arrow keys move the diff, in the list they move
            // the selection.
            if (detail_) {
                if (scroll_ > 0) --scroll_;
            } else if (selected_ > 0) {
                select_file(selected_ - 1);
            }
            break;

        case Key::Type::Down:
            if (detail_) {
                if (scroll_ < max_scroll(content_height)) ++scroll_;
            } else if (selected_ + 1 < files().size()) {
                select_file(selected_ + 1);
            }
            break;

        case Key::Type::PageUp:
            // In the diff the page keys move the diff, in the list they move
            // the pane.
            if (detail_) {
                const size_t page = static_cast<size_t>(diff_room(content_height));
                scroll_ = scroll_ > page ? scroll_ - page : 0;
            } else {
                const size_t page = static_cast<size_t>(content_height);
                scroll_ = scroll_ > page ? scroll_ - page : 0;
            }
            break;

        case Key::Type::PageDown:
            if (detail_) {
                scroll_ += static_cast<size_t>(diff_room(content_height));
            } else {
                scroll_ += static_cast<size_t>(content_height);
            }
            break;

        case Key::Type::Home:
            scroll_ = 0;
            break;

        case Key::Type::End:
            scroll_ = detail_ ? max_scroll(content_height)
                              : max_list_scroll(width, content_height);
            break;

        case Key::Type::Left:
            // In the diff the left arrow goes back to the list, in the list it
            // switches to the previous source.
            if (detail_) {
                show_detail(false);
            } else if (source_ > 0) {
                select_source(source_ - 1);
            }
            break;

        case Key::Type::Right:
            if (!detail_ && source_ + 1 < sources_.size()) {
                select_source(source_ + 1);
            }
            break;

        case Key::Type::Enter:
            // The list and the diff of the selected file take turns.
            show_detail(!detail_);
            break;

        case Key::Type::Escape:
        case Key::Type::CtrlC:
            terminal.clear_dynamic();
            return;

        default:
            break;
        }

        // The diff may have grown or shrunk, so the offset is brought back
        // into range after every key.
        if (detail_) {
            scroll_ = std::min(scroll_, max_scroll(content_height));
        } else {
            scroll_ = std::min(scroll_, max_list_scroll(width, content_height));
        }
    }
}

} // namespace cc::ui
