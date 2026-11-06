#pragma once

#include "ui/diff.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"
#include "utils/git.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace cc::ui {

/// One source of the changes the pane of the diff shows.
///
/// The first source is the working tree; the others are the turns of the
/// conversation that changed files.
struct DiffSource {
    /// The label of the source in the selector, e.g. "Current" or "T3".
    std::string name;
    /// The title of the pane when this source is showing.
    std::string title;
    /// The subtitle of the title, e.g. the preview of the prompt of the turn.
    std::string subtitle;
    /// The files that changed in this source.
    std::vector<git::ChangedFile> files;
};

/// The view Claude Code shows for `/diff`: the files that changed and the
/// diff of the one that is selected.
class DiffView {
public:
    /// The state the diff of the pane is in.
    ///
    /// The reference says "Loading diff…" while the diff is fetched, and
    /// "Too many files to display details" when the statistics report more
    /// files than the list holds.
    enum class State {
        Ready,    ///< The diff is there.
        Loading,  ///< The diff is still being fetched.
        TooMany,  ///< There are more files than the list holds.
    };

    DiffView(Theme theme, std::vector<git::ChangedFile> files);

    /// A view over several sources of changes, e.g. the working tree and the
    /// changes of an earlier turn. The first source is the working tree.
    DiffView(Theme theme, std::vector<DiffSource> sources);

    /// Says in which state the diff is.
    void set_state(State state);

    /// Gives the view the files that changed.
    ///
    /// The files are fetched after the view is opened, so that the pane can
    /// say that it is loading while the fetch is going on.
    void set_files(std::vector<git::ChangedFile> files);

    /// Selects the file at `index` in the list of the source that is showing.
    void select_file(size_t index);

    /// Shows the changes of the source at `index`, e.g. the changes of an
    /// earlier turn.
    void select_source(size_t index);

    /// Scrolls the diff of the selected file to `line`.
    void scroll_to(size_t line);

    /// Shows the diff of the selected file, or the list of the files.
    void show_detail(bool show);

    /// Says whether the view is drawn in the modal slot.
    ///
    /// The reference floats the panes of the slash commands in the modal
    /// slot of the full-screen layout: they have a rule of their own and
    /// their contents sit three columns in.
    void set_modal(bool modal);

    /// Renders the view.
    std::vector<std::string> render(int width, int height) const;

    /// Runs the view until the user dismisses it.
    void run(Terminal& terminal);

private:
    /// The source that is showing.
    const DiffSource& source() const;

    /// The files of the source that is showing.
    const std::vector<git::ChangedFile>& files() const;

    /// The title of the pane.
    std::string title() const;

    /// The selector that switches between the sources.
    std::string source_selector() const;

    /// The line that holds the number of files and lines that changed.
    std::string stats() const;

    /// The message the pane shows when it has nothing to show.
    std::string empty_message() const;

    /// The list of the files that changed.
    std::vector<std::string> render_list(int width) const;

    /// One line of the file list.
    std::string render_file(const git::ChangedFile& file, bool selected, int width) const;

    /// The counts that sit at the end of a line of the file list.
    std::string file_stats(const git::ChangedFile& file, bool selected) const;

    /// The diff of the selected file.
    std::vector<std::string> render_detail(int width, int height) const;

    /// The number of rows the pane takes besides its content.
    int chrome() const;

    /// The padding of the pane, as a string.
    std::string pane_padding() const;

    /// The number of rows the diff of the selected file may take.
    int diff_room(int content_height) const;

    /// The number of lines of the diff that are below the window.
    size_t max_scroll(int content_height) const;

    /// The number of lines of the list that are below the window of the pane.
    size_t max_list_scroll(int width, int content_height) const;

    /// The line at the bottom that lists the keys.
    std::string byline() const;

    Theme theme_;
    /// Every source of the changes.
    std::vector<DiffSource> sources_;
    /// The source that is showing.
    size_t source_ = 0;
    /// The file that is selected.
    size_t selected_ = 0;
    /// True when the diff of the selected file is showing.
    bool detail_ = false;
    /// True when the view is drawn in the modal slot.
    bool modal_ = false;
    /// The state the diff is in.
    State state_ = State::Ready;
    /// The number of lines that are scrolled out of the way: the lines of the
    /// diff of the file, or the lines of the list of the files.
    size_t scroll_ = 0;
};

} // namespace cc::ui
