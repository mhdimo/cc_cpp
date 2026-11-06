#include <catch2/catch_test_macros.hpp>

#include "ui/ansi.hpp"
#include "ui/diff_view.hpp"
#include "ui/theme.hpp"
#include "utils/git.hpp"
#include "utils/string.hpp"

#include <string>
#include <vector>

using namespace cc;

namespace {

/// The lines of the view, without the colours.
std::string plain(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) out += ui::strip(line) + "\n";
    return out;
}

/// The line that holds `text`, without the colours.
std::string row_with(const std::vector<std::string>& lines, const std::string& text) {
    for (const auto& line : lines) {
        if (ui::strip(line).find(text) != std::string::npos) return line;
    }
    return {};
}

/// The row that holds `text`.
size_t row_of(const std::vector<std::string>& lines, const std::string& text) {
    for (size_t i = 0; i < lines.size(); ++i) {
        if (ui::strip(lines[i]).find(text) != std::string::npos) return i;
    }
    return lines.size();
}

/// A file that changed.
git::ChangedFile changed(const std::string& path, int added = 0, int removed = 0) {
    git::ChangedFile file;
    file.path = path;
    file.added = added;
    file.removed = removed;
    return file;
}

} // namespace

TEST_CASE("The diff pane begins with a blank line and indents its rows") {
    const auto theme = ui::Theme::dark();
    ui::DiffView view(theme, std::vector<git::ChangedFile>{});

    const auto lines = view.render(60, 24);

    REQUIRE(lines.size() > 2);
    // The pane keeps a blank line above the rule that tops it.
    REQUIRE(lines[0].empty());
    REQUIRE(ui::strip(lines[1]).find("─") != std::string::npos);
    // The rows of the pane are indented by two columns.
    REQUIRE(ui::strip(lines[2]).rfind("  Uncommitted changes", 0) == 0);
}

TEST_CASE("The title of the diff pane is bold and carries the colour of the background") {
    const auto theme = ui::Theme::dark();
    ui::DiffView view(theme, std::vector<git::ChangedFile>{});

    const auto lines = view.render(60, 24);
    const std::string title = lines[2];

    // The title is bold and cyan, the way the reference draws it.
    REQUIRE(title.find("\x1b[1;38;2;0;204;204mUncommitted changes") != std::string::npos);
    // The suffix keeps the bold of the title and takes the inactive grey
    // the reference resolves its dim colour to.
    REQUIRE(title.find("\x1b[1;38;2;153;153;153m (git diff HEAD)") != std::string::npos);
}

TEST_CASE("The diff pane says why it has nothing to show") {
    const auto theme = ui::Theme::dark();

    SECTION("a clean working tree") {
        ui::DiffView view(theme, std::vector<git::ChangedFile>{});
        REQUIRE(plain(view.render(60, 24)).find("Working tree is clean") != std::string::npos);
    }

    SECTION("a diff that is still being fetched") {
        ui::DiffView view(theme, std::vector<git::ChangedFile>{});
        view.set_state(ui::DiffView::State::Loading);
        REQUIRE(plain(view.render(60, 24)).find("Loading diff…") != std::string::npos);
    }

    SECTION("more files than the list holds") {
        ui::DiffView view(theme, std::vector<git::ChangedFile>{});
        view.set_state(ui::DiffView::State::TooMany);
        REQUIRE(plain(view.render(60, 24)).find("Too many files to display details") !=
                std::string::npos);
    }

    SECTION("a turn that changed nothing") {
        std::vector<ui::DiffSource> sources = {
            ui::DiffSource{"Current", "Uncommitted changes", " (git diff HEAD)", {}},
            ui::DiffSource{"T1", "Turn 1", "", {}},
        };
        ui::DiffView view(theme, std::move(sources));
        view.select_source(1);
        REQUIRE(plain(view.render(60, 24)).find("No file changes in this turn") != std::string::npos);
    }
}

TEST_CASE("The selected file is painted with the colour of the background") {
    const auto theme = ui::Theme::dark();
    std::vector<git::ChangedFile> files = {changed("src/main.cpp", 3, 1)};
    ui::DiffView view(theme, files);

    const auto lines = view.render(60, 24);
    const std::string row = row_with(lines, "src/main.cpp");

    // The pointer and the path share the band of the selected row.
    REQUIRE(row.find("\x1b[1;7;38;2;0;204;204m❯ src/main.cpp") != std::string::npos);
}

TEST_CASE("The list of the files keeps its height when it is paginated") {
    const auto theme = ui::Theme::dark();

    std::vector<git::ChangedFile> files;
    for (int i = 0; i < 8; ++i) files.push_back(changed("file" + std::to_string(i) + ".cpp"));

    ui::DiffView view(theme, files);

    SECTION("the row above the list is empty when there is nothing above it") {
        const auto lines = view.render(60, 24);
        const size_t first = row_of(lines, "file0.cpp");
        REQUIRE(first > 0);
        // The row that says that there are more files above is empty.
        REQUIRE(ui::strip(lines[first - 1]).find("more files") == std::string::npos);
        // The row that says that there are more files below is not.
        REQUIRE(plain(lines).find("↓ 3 more files") != std::string::npos);
    }

    SECTION("the row below the list is empty when there is nothing below it") {
        view.select_file(7);
        const auto lines = view.render(60, 24);
        const size_t first = row_of(lines, "file3.cpp");
        const size_t last = row_of(lines, "file7.cpp");
        REQUIRE(first > 0);
        REQUIRE(last + 1 < lines.size());
        // The row that says that there are more files above is not empty.
        REQUIRE(ui::strip(lines[first - 1]).find("↑ 3 more files") != std::string::npos);
        // The row that says that there are more files below is empty.
        REQUIRE(ui::strip(lines[last + 1]).find("more files") == std::string::npos);
    }
}

TEST_CASE("The path of a file is cut to the width the reference cuts it to") {
    const auto theme = ui::Theme::dark();
    const std::string path = "a/very/long/path/that/does/not/fit/into/the/terminal/file.cpp";
    std::vector<git::ChangedFile> files = {changed(path)};
    ui::DiffView view(theme, files);

    const auto lines = view.render(60, 24);
    const std::string text = ui::strip(row_with(lines, "file.cpp"));

    // The reference cuts the path to max(20, columns - 23) columns.
    const size_t width = 60 - 23;
    const std::string prefix = "  ❯ ";
    REQUIRE(text.rfind(prefix, 0) == 0);
    // The path is what follows the pointer; the padding behind it is dropped.
    std::string shown = text.substr(prefix.size());
    while (!shown.empty() && shown.back() == ' ') shown.pop_back();
    REQUIRE(cc::display_width(shown) == width);
    // The ellipsis goes in front, so that the name of the file stays visible.
    REQUIRE(shown.rfind("…", 0) == 0);
    REQUIRE(shown.find("file.cpp") != std::string::npos);
}

TEST_CASE("The diff of a file is scrolled, not cut short") {
    const auto theme = ui::Theme::dark();

    std::string diff = "@@ -1,40 +1,40 @@\n";
    for (int i = 0; i < 40; ++i) diff += "+line " + std::to_string(i) + "\n";

    std::vector<git::ChangedFile> files = {changed("a.cpp")};
    files[0].diff = diff;

    ui::DiffView view(theme, files);
    view.show_detail(true);

    SECTION("the lines that do not fit are not announced as missing") {
        const std::string screen = plain(view.render(60, 20));
        REQUIRE(screen.find("… more lines not shown") == std::string::npos);
        // The byline of the reference names no scroll key: the diff scrolls
        // in the pane around the dialog.
        REQUIRE(screen.find("↑/↓ scroll") == std::string::npos);
        REQUIRE(screen.find("← back · esc close") != std::string::npos);
    }

    SECTION("the last line of the diff can be reached") {
        view.scroll_to(1000);
        const std::string screen = plain(view.render(60, 20));
        REQUIRE(screen.find("line 39") != std::string::npos);
    }
}

TEST_CASE("The whole diff of a file is shown when it fits on the screen") {
    const auto theme = ui::Theme::dark();

    std::string diff = "@@ -1,10 +1,10 @@\n";
    for (int i = 0; i < 10; ++i) diff += "+line " + std::to_string(i) + "\n";

    std::vector<git::ChangedFile> files = {changed("a.cpp")};
    files[0].diff = diff;

    ui::DiffView view(theme, files);
    view.show_detail(true);
    const std::string screen = plain(view.render(60, 40));

    REQUIRE(screen.find("line 0") != std::string::npos);
    REQUIRE(screen.find("line 9") != std::string::npos);
    REQUIRE(screen.find("… more lines not shown") == std::string::npos);
}

TEST_CASE("The pane of the diff has two blank rows between the summary and the list") {
    const auto theme = ui::Theme::dark();
    std::vector<git::ChangedFile> files = {changed("a.cpp")};
    ui::DiffView view(theme, files);

    const auto lines = view.render(60, 24);
    const size_t stats = row_of(lines, "1 file changed");
    REQUIRE(stats > 0);

    // The dialog of the reference has a gap of its own between the summary
    // and the content, and the box that holds the content has a margin above
    // it: two blank rows fall between them.
    REQUIRE(ui::strip(lines[stats + 1]).empty());
    REQUIRE(ui::strip(lines[stats + 2]).empty());
    REQUIRE(row_of(lines, "a.cpp") == stats + 3);
}

TEST_CASE("The pane of the diff draws no summary of the change") {
    const auto theme = ui::Theme::dark();

    std::string diff = "@@ -1,10 +1,10 @@\n";
    for (int i = 0; i < 10; ++i) diff += "+line " + std::to_string(i) + "\n";

    std::vector<git::ChangedFile> files = {changed("a.cpp")};
    files[0].diff = diff;

    ui::DiffView view(theme, files);
    view.show_detail(true);

    // The reference goes straight from the rule under the path to the diff.
    const std::string screen = plain(view.render(60, 24));
    REQUIRE(screen.find("Added") == std::string::npos);
}

TEST_CASE("The list of the files holds five files on a short terminal") {
    const auto theme = ui::Theme::dark();

    std::vector<git::ChangedFile> files;
    for (int i = 0; i < 8; ++i) files.push_back(changed("file" + std::to_string(i) + ".cpp"));

    ui::DiffView view(theme, files);

    // The reference's list always holds five files; the pane around it
    // scrolls.
    const auto lines = view.render(60, 15);
    REQUIRE(row_of(lines, "file0.cpp") != lines.size());
    REQUIRE(row_of(lines, "file4.cpp") != lines.size());
}

TEST_CASE("The note of the file that is selected is not dimmed") {
    const auto theme = ui::Theme::dark();

    git::ChangedFile untracked;
    untracked.path = "b.cpp";
    untracked.untracked = true;

    std::vector<git::ChangedFile> files = {changed("a.cpp"), untracked};
    ui::DiffView view(theme, files);

    const std::string grey = "\x1b[3;38;2;153;153;153m";

    // The note of a row that is not selected is grey.
    REQUIRE(row_with(view.render(60, 24), "untracked").find(grey) != std::string::npos);

    // The note of the row that is selected is in the ordinary colour.
    view.select_file(1);
    const std::string row = row_with(view.render(60, 24), "untracked");
    REQUIRE(row.find(grey) == std::string::npos);
    REQUIRE(row.find("\x1b[3;38;2;255;255;255m") != std::string::npos);
}

TEST_CASE("The turns of the conversation are named by their number") {
    const auto theme = ui::Theme::dark();

    std::vector<ui::DiffSource> sources = {
        ui::DiffSource{"Current", "Uncommitted changes", " (git diff HEAD)", {}},
        ui::DiffSource{"T3", "Turn 3", " \"fix the thing\"", {changed("a.cpp")}},
    };
    ui::DiffView view(theme, std::move(sources));

    // The selector names the turn by the number of the turn.
    REQUIRE(plain(view.render(60, 24)).find("Current · T3") != std::string::npos);

    // The title of the pane becomes the number of the turn.
    view.select_source(1);
    const std::string screen = plain(view.render(60, 24));
    REQUIRE(screen.find("Turn 3") != std::string::npos);
    REQUIRE(screen.find("\"fix the thing\"") != std::string::npos);
}

TEST_CASE("The pane of the modal slot is topped by a rule of its own") {
    const auto theme = ui::Theme::dark();

    std::string diff = "@@ -1,40 +1,40 @@\n";
    for (int i = 0; i < 40; ++i) diff += "+line " + std::to_string(i) + "\n";

    std::vector<git::ChangedFile> files = {changed("a.cpp")};
    files[0].diff = diff;

    ui::DiffView view(theme, files);
    view.set_modal(true);
    view.show_detail(true);

    const auto lines = view.render(60, 24);

    // The rule of the modal slot is drawn in the colour of the permission.
    REQUIRE(ui::strip(lines[0]).find("▔") != std::string::npos);
    REQUIRE(lines[0].find("\x1b[38;2;177;185;249m") != std::string::npos);

    // The contents of the pane sit three columns in.
    REQUIRE(ui::strip(lines[1]).rfind("   Uncommitted changes", 0) == 0);
    REQUIRE(ui::strip(row_with(lines, "a.cpp")).rfind("   a.cpp", 0) == 0);

    // Two rows of the conversation stay above the pane, so that it floats
    // over the conversation.
    REQUIRE(lines.size() == static_cast<size_t>(24 - 2));
}
