#include <catch2/catch_test_macros.hpp>

#include "ide.hpp"
#include "ide_diff.hpp"
#include "ui/clipboard.hpp"
#include "utils/fs.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

using namespace cc;

namespace {

/// A folder that is gone when the test is over.
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const char* base = std::getenv("TMPDIR");
        std::string root = base != nullptr ? base : "/tmp";
        while (!root.empty() && root.back() == '/') root.pop_back();

        path_ = path_join(root, "cc-ide-test");
        remove_dir_all(path_);
        mkdirp(path_);
    }

    ~TemporaryDirectory() { remove_dir_all(path_); }

    /// The folder the test writes in.
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

/// Writes `contents` in `path`, creating the folders it lives in.
void write(const std::string& path, const std::string& contents) {
    std::string error;
    REQUIRE(write_file(path, contents, &error));
}

} // namespace

TEST_CASE("The IDE of the terminal is read out of the environment") {
    // The trace of Cursor is the surest sign of it.
    ::setenv("CURSOR_TRACE_ID", "1", 1);
    REQUIRE(terminal_ide_type() == "cursor");
    ::unsetenv("CURSOR_TRACE_ID");

    // The askpass of the family of VS Code tells which IDE it is.
    ::setenv("VSCODE_GIT_ASKPASS_MAIN", "/Applications/Cursor.app/...", 1);
    REQUIRE(terminal_ide_type() == "cursor");
    ::unsetenv("VSCODE_GIT_ASKPASS_MAIN");

    // The name of the program of the terminal is the last word.
    ::setenv("TERM_PROGRAM", "vscode", 1);
    REQUIRE(terminal_ide_type() == "vscode");
    ::unsetenv("TERM_PROGRAM");

    // A terminal that belongs to no IDE is not one.
    ::setenv("TERM_PROGRAM", "iTerm.app", 1);
    REQUIRE(terminal_ide_type().empty());
    ::unsetenv("TERM_PROGRAM");
}

TEST_CASE("The command of the IDE of the family of VS Code is named") {
    REQUIRE(vscode_command("vscode") == "code");
    REQUIRE(vscode_command("cursor") == "cursor");
    REQUIRE(vscode_command("windsurf") == "windsurf");

    // The IDEs of JetBrains have no command of their own, the way the
    // reference has none.
    REQUIRE(vscode_command("pycharm").empty());

    REQUIRE(is_vscode_ide("vscode"));
    REQUIRE(is_vscode_ide("cursor"));
    REQUIRE_FALSE(is_vscode_ide("pycharm"));
}

TEST_CASE("The versions of the extension of the IDE are compared") {
    REQUIRE(older_version("1.0.0", "1.0.1"));
    REQUIRE(older_version("1.9.9", "1.10.0"));
    REQUIRE(older_version("0.2.3", "1.0.0"));
    REQUIRE_FALSE(older_version("1.0.0", "1.0.0"));
    REQUIRE_FALSE(older_version("1.0.1", "1.0.0"));
}

TEST_CASE("The tab of the IDE is named after the file") {
    const std::string tab = ide_tab_name("/tmp/notes.md");

    REQUIRE(tab.find("notes.md") != std::string::npos);
    REQUIRE(tab.find("Claude Code") != std::string::npos);

    // Every tab has a number of its own.
    REQUIRE(ide_tab_name("/tmp/notes.md") != tab);
}

TEST_CASE("The change of a file is worked out before it is shown") {
    TemporaryDirectory home;

    const std::string path = path_join(home.path(), "notes.md");
    write(path, "The first line.\nThe second line.\n");

    boost::json::object edit;
    edit["file_path"] = boost::json::value(path);
    edit["old_string"] = boost::json::value("first");
    edit["new_string"] = boost::json::value("second");

    const auto contents = contents_after_edit(path, edit);
    REQUIRE(contents.has_value());
    REQUIRE(*contents == "The second line.\nThe second line.\n");

    // A file that is written whole holds what the tool writes.
    boost::json::object write_input;
    write_input["file_path"] = boost::json::value(path);
    write_input["content"] = boost::json::value("All new.\n");

    const auto written = contents_after_edit(path, write_input);
    REQUIRE(written.has_value());
    REQUIRE(*written == "All new.\n");

    // The notebooks are not shown in the IDE.
    boost::json::object notebook = edit;
    notebook["file_path"] = boost::json::value(path_join(home.path(), "book.ipynb"));
    REQUIRE_FALSE(contents_after_edit(path_join(home.path(), "book.ipynb"), notebook).has_value());
}

TEST_CASE("The answer of the IDE is read out of the reply of the extension") {
    REQUIRE(ide_answer("FILE_SAVEDThe new contents") == IdeDiffAnswer::Accepted);
    REQUIRE(ide_answer("TAB_CLOSED") == IdeDiffAnswer::Accepted);
    REQUIRE(ide_answer("DIFF_REJECTED") == IdeDiffAnswer::Refused);
    REQUIRE(ide_answer("") == IdeDiffAnswer::Waiting);
    REQUIRE(ide_answer("something else") == IdeDiffAnswer::Waiting);
}

TEST_CASE("The escape of the clipboard holds the text in base64") {
    // The terminals take the text of the clipboard in the base64 of the
    // standard.
    const std::string sequence = ui::clipboard_sequence("Hi");

    REQUIRE(sequence.find("\x1b]52;c;") == 0);
    REQUIRE(sequence.find("SGk=") != std::string::npos);
    REQUIRE(sequence.back() == '\a');
}
