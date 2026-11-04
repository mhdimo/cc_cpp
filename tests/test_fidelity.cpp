#include <catch2/catch_test_macros.hpp>

#include "commands/custom.hpp"
#include "commands/registry.hpp"
#include "session/file_history.hpp"
#include "ui/ansi.hpp"
#include "ui/help_overlay.hpp"
#include "ui/keybindings.hpp"
#include "ui/markdown.hpp"
#include "ui/prompt_input.hpp"
#include "ui/select.hpp"
#include "ui/spinner.hpp"
#include "ui/suggestions.hpp"
#include "ui/theme.hpp"
#include "ui/tool_display.hpp"
#include "utils/fs.hpp"
#include "utils/hash.hpp"
#include "utils/json_util.hpp"
#include "utils/slash_command.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace cc;

namespace {

/// The text of the lines, without the colours.
std::string plain(const std::vector<std::string>& lines) {
    std::string joined;
    for (const auto& line : lines) {
        joined += ui::strip(line);
        joined += "\n";
    }
    return joined;
}

/// A configuration of its own, so that the tests do not depend on the
/// commands the user has used.
///
/// The commands that were used recently stand at the top of the list, and
/// the use is kept in the global configuration file of the user.
class EmptyConfig {
public:
    EmptyConfig() {
        const char* previous = std::getenv("CLAUDE_CONFIG_DIR");
        if (previous != nullptr) previous_ = previous;

        directory_ = std::filesystem::temp_directory_path() / "cc_empty_config_test";
        std::filesystem::create_directories(directory_);
        std::ofstream file(directory_ / ".claude.json");
        file << "{}";

        setenv("CLAUDE_CONFIG_DIR", directory_.c_str(), 1);
    }

    ~EmptyConfig() {
        if (previous_.empty()) {
            unsetenv("CLAUDE_CONFIG_DIR");
        } else {
            setenv("CLAUDE_CONFIG_DIR", previous_.c_str(), 1);
        }
    }

private:
    std::string previous_;
    std::filesystem::path directory_;
};

} // namespace

TEST_CASE("The shortcuts menu names the newline instruction") {
    const auto menu = ui::PromptInput::shortcuts_menu(100);
    const std::string text = plain(menu);

    REQUIRE(text.find("backslash (\\) + return") != std::string::npos);
    REQUIRE(text.find("newline") != std::string::npos);

    // The instruction is in the second column, the way the reference puts
    // it there.
    bool in_second_column = false;
    for (const auto& line : menu) {
        const std::string stripped = ui::strip(line);
        if (stripped.find("backslash") == std::string::npos) continue;
        // The second column starts after the first one and the gap.
        in_second_column = stripped.find("ctrl + _") != std::string::npos ||
                           stripped.find("backslash") < 30;
        break;
    }
    REQUIRE(in_second_column);
}

TEST_CASE("The prompt box shows an example command and drops it after the first message") {
    const auto theme = ui::Theme::dark();
    ui::PromptInput input(theme, "/tmp");

    const std::string placeholder = input.placeholder();
    REQUIRE(placeholder.rfind("Try \"", 0) == 0);
    REQUIRE(placeholder.back() == '"');

    // The character under the caret is drawn in reverse video.
    const auto lines = input.render_box(60, false);
    REQUIRE(lines.size() == 3);
    REQUIRE(lines[1].find("\x1b[7m") != std::string::npos);

    // The placeholder is gone once a message has been sent.
    input.note_submit();
    REQUIRE(input.placeholder().empty());
}

TEST_CASE("The prompt box wraps the text to the width of the terminal") {
    const auto theme = ui::Theme::dark();
    ui::PromptInput input(theme, "/tmp");

    input.set_text("one two three four five six seven eight nine ten eleven twelve");

    const auto lines = input.render_box(30, false);

    // The rule, the text and the rule.
    REQUIRE(lines.size() > 3);

    // Every line of the text fits in the box.
    for (size_t i = 1; i + 1 < lines.size(); ++i) {
        REQUIRE(cc::display_width(ui::strip(lines[i])) <= 30);
    }
}

TEST_CASE("The help overlay cuts the description of a command to one line") {
    const auto theme = ui::Theme::dark();

    const std::vector<ui::HelpCommand> commands = {
        {"/clear",
         "Clear conversation history and free up context. This is a very long description that "
         "would not fit on a single line of the pane."},
    };
    ui::HelpOverlay overlay(theme, commands, {});

    overlay.handle_key(ui::Key{ui::Key::Type::Right, {}});

    const auto lines = overlay.render(60, 40);
    const std::string text = plain(lines);

    REQUIRE(text.find("/clear") != std::string::npos);
    REQUIRE(text.find("would not fit") == std::string::npos);
    REQUIRE(text.find("…") != std::string::npos);
}

TEST_CASE("The help overlay sizes the command list to the terminal") {
    const auto theme = ui::Theme::dark();

    std::vector<ui::HelpCommand> commands;
    for (int i = 0; i < 30; ++i) {
        commands.push_back({"/command" + std::to_string(i), "The description"});
    }
    ui::HelpOverlay overlay(theme, commands, {});

    overlay.handle_key(ui::Key{ui::Key::Type::Right, {}});

    // The pane of the help is at most half the screen, and every command
    // takes two lines of it, so a terminal of thirty rows holds two of them.
    const auto lines = overlay.render(60, 30);
    size_t named = 0;
    for (const auto& line : lines) {
        if (ui::strip(line).find("/command") != std::string::npos) ++named;
    }
    REQUIRE(named == 2);

    // A taller terminal holds more of them.
    const auto tall = overlay.render(60, 60);
    size_t named_tall = 0;
    for (const auto& line : tall) {
        if (ui::strip(line).find("/command") != std::string::npos) ++named_tall;
    }
    REQUIRE(named_tall == 10);
}

TEST_CASE("The picker shows an arrow when there is more to see") {
    const auto theme = ui::Theme::dark();

    std::vector<ui::SelectOption> options;
    for (int i = 0; i < 20; ++i) {
        options.push_back({"Option " + std::to_string(i), ""});
    }
    ui::Select select(theme, options, {}, /*visible_count=*/5);

    const auto lines = select.render(60);
    REQUIRE(lines.size() == 5);

    // The first entry is marked with the pointer, the last with the arrow
    // that says that there is more below.
    REQUIRE(ui::strip(lines.front()).find("❯") != std::string::npos);
    REQUIRE(ui::strip(lines.back()).find("↓") != std::string::npos);

    // Moving down past the last visible entry scrolls the list.
    select.set_selected(5);
    const auto moved = select.render(60);
    REQUIRE(ui::strip(moved.front()).find("↑") != std::string::npos);
}

TEST_CASE("The output of a command is formatted when it is JSON") {
    const auto theme = ui::Theme::dark();

    boost::json::object payload;
    payload["stdout"] = "{\"a\":1}";
    payload["stderr"] = "";

    const auto lines = ui::render_tool_result_lines("Bash", "", boost::json::value(payload), "/tmp",
                                                    theme, 60, false);

    const std::string text = plain(lines);
    REQUIRE(text.find("\"a\": 1") != std::string::npos);
}

TEST_CASE("The standard error of a command is shown in its own colour") {
    const auto theme = ui::Theme::dark();

    boost::json::object payload;
    payload["stdout"] = "out";
    payload["stderr"] = "bad";

    const auto lines = ui::render_tool_result_lines("Bash", "", boost::json::value(payload), "/tmp",
                                                    theme, 60, false);

    const std::string text = plain(lines);
    REQUIRE(text.find("out") != std::string::npos);
    REQUIRE(text.find("bad") != std::string::npos);

    // The error is coloured, the output is not.
    bool error_is_coloured = false;
    for (const auto& line : lines) {
        if (ui::strip(line).find("bad") == std::string::npos) continue;
        error_is_coloured = line.find("\x1b[38;2;") != std::string::npos;
    }
    REQUIRE(error_is_coloured);
}

TEST_CASE("A command that printed nothing says so") {
    const auto theme = ui::Theme::dark();

    boost::json::object payload;
    payload["stdout"] = "";
    payload["stderr"] = "";

    const auto lines = ui::render_tool_result_lines("Bash", "", boost::json::value(payload), "/tmp",
                                                    theme, 60, false);
    REQUIRE(plain(lines).find("(No output)") != std::string::npos);
}

TEST_CASE("The output of a command is cut short after three lines") {
    const auto theme = ui::Theme::dark();

    std::string stdout;
    for (int i = 1; i <= 10; ++i) stdout += "line " + std::to_string(i) + "\n";

    boost::json::object payload;
    payload["stdout"] = stdout;
    payload["stderr"] = "";

    const auto lines = ui::render_tool_result_lines("Bash", "", boost::json::value(payload), "/tmp",
                                                    theme, 60, false);

    // Only the lines above the fold are shown, and the number of the lines
    // that are left is said.
    const std::string text = plain(lines);
    REQUIRE(text.find("line 3") != std::string::npos);
    REQUIRE(text.find("line 4") == std::string::npos);
    REQUIRE(text.find("… +7 lines (ctrl+o to expand)") != std::string::npos);
}

TEST_CASE("An output of four lines is shown whole") {
    const auto theme = ui::Theme::dark();

    boost::json::object payload;
    payload["stdout"] = "line 1\nline 2\nline 3\nline 4";
    payload["stderr"] = "";

    const auto lines = ui::render_tool_result_lines("Bash", "", boost::json::value(payload), "/tmp",
                                                    theme, 60, false);

    // A single line that would be left is shown rather than counted.
    const std::string text = plain(lines);
    REQUIRE(text.find("line 4") != std::string::npos);
    REQUIRE(text.find("ctrl+o to expand") == std::string::npos);
}

TEST_CASE("The error of a tool names itself and says how much of it is left") {
    const auto theme = ui::Theme::dark();

    std::string message = "the tool failed";
    for (int i = 0; i < 20; ++i) message += "\nmore of it";

    const auto lines = ui::render_tool_error_lines(message, theme, 60);

    const std::string text = plain(lines);
    REQUIRE(text.find("Error: the tool failed") != std::string::npos);
    REQUIRE(text.find("… +11 lines (ctrl+o to see all)") != std::string::npos);
}

TEST_CASE("The spinner shows the time and the tokens the way Claude Code does") {
    const auto theme = ui::Theme::dark();
    ui::Spinner spinner(theme);

    // The verb is one of Claude Code's own.
    REQUIRE(!spinner.verb().empty());

    // The line carries the verb and the ellipsis.
    const std::string line = ui::strip(spinner.line());
    REQUIRE(line.find("…") != std::string::npos);
}

TEST_CASE("A number is written the way Claude Code writes it") {
    // The wording is the compact notation of the reference: a number of a
    // thousand or more keeps its one decimal.
    REQUIRE(cc::format_number(900) == "900");
    REQUIRE(cc::format_number(1321) == "1.3k");
    REQUIRE(cc::format_number(12000) == "12.0k");
    REQUIRE(cc::format_number(1000000) == "1.0m");

    // A value that rounds up to a thousand is carried into the next unit.
    REQUIRE(cc::format_number(999999) == "1.0m");
}

TEST_CASE("The arguments of a command are put into its prompt") {
    // The whole string of the arguments.
    REQUIRE(cc::commands::substitute_arguments("Do $ARGUMENTS now", "the thing") ==
            "Do the thing now");

    // The arguments at a place.
    REQUIRE(cc::commands::substitute_arguments("Do $0 and $1", "one two") == "Do one and two");

    // A prompt without a placeholder gets the arguments appended.
    REQUIRE(cc::commands::substitute_arguments("Do the thing", "the thing") ==
            "Do the thing\n\nARGUMENTS: the thing");

    // A prompt without arguments is left alone.
    REQUIRE(cc::commands::substitute_arguments("Do the thing", "") == "Do the thing");
}

TEST_CASE("A block of code is coloured the way cli-highlight colours it") {
    const auto theme = ui::Theme::dark();

    const auto lines = ui::render_markdown("```python\ndef f(x):\n    # a comment\n    return 'text' + str(1)\n```",
                                            theme, 80);

    std::string joined;
    for (const auto& line : lines) joined += line + "\n";

    // The colours of the theme of cli-highlight: a keyword is blue, a
    // string is red, a comment is green and a number is green.
    INFO("rendered: [" << joined << "]");
    REQUIRE(joined.find("\x1b[34mdef") != std::string::npos);
    REQUIRE(joined.find("\x1b[31m'text'") != std::string::npos);
    REQUIRE(joined.find("\x1b[32m# a comment") != std::string::npos);
    REQUIRE(joined.find("\x1b[32m1") != std::string::npos);
}

TEST_CASE("A language the port does not know is left alone") {
    const auto theme = ui::Theme::dark();

    const auto lines = ui::render_markdown("```not-a-language\nif while return\n```", theme, 80);

    std::string joined;
    for (const auto& line : lines) joined += line + "\n";

    // The text is there, and none of it is coloured.
    REQUIRE(joined.find("if while return") != std::string::npos);
    REQUIRE(joined.find("\x1b[34m") == std::string::npos);
}

TEST_CASE("The keys the user has bound are read from their file") {
    const auto directory = std::filesystem::temp_directory_path() / "cc_keybindings_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    const auto path = directory / "keybindings.json";
    {
        std::ofstream file(path);
        file << R"({"bindings": [
            {"context": "Global", "bindings": {"ctrl+g": "app:toggleTranscript"}},
            {"context": "Chat", "bindings": {"ctrl+t": "app:toggleTodos", "ctrl+r": null}}
        ]})";
    }

    ui::keybindings::Bindings bindings;
    bindings.load(path.string());
    REQUIRE_FALSE(bindings.empty());

    // The key is bound in the context it was bound in.
    ui::Key key{ui::Key::Type::CtrlG, {}};
    REQUIRE(bindings.action_for(key, "Global") == "app:toggleTranscript");
    REQUIRE(bindings.action_for(key, "Chat").empty());

    ui::Key todos{ui::Key::Type::CtrlT, {}};
    REQUIRE(bindings.action_for(todos, "Chat") == "app:toggleTodos");

    // A key that is bound to nothing is unbound.
    ui::Key history{ui::Key::Type::CtrlR, {}};
    REQUIRE(bindings.action_for(history, "Chat").empty());

    // A key that is not bound at all.
    ui::Key enter{ui::Key::Type::Enter, {}};
    REQUIRE(bindings.action_for(enter, "Chat").empty());

    std::filesystem::remove_all(directory);
}

TEST_CASE("The commands of the user are read from their own folders") {
    const auto directory = std::filesystem::temp_directory_path() / "cc_custom_commands_test";
    std::filesystem::create_directories(directory / ".claude" / "commands" / "git");
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory / ".claude" / "commands" / "git");

    {
        std::ofstream file(directory / ".claude" / "commands" / "hello.md");
        file << "---\ndescription: Say hello\nargument-hint: <name>\n---\nSay hello to $ARGUMENTS\n";
    }
    {
        std::ofstream file(directory / ".claude" / "commands" / "git" / "commit.md");
        file << "Commit the changes\n";
    }

    const auto commands = cc::commands::load_custom_commands(directory.string());
    REQUIRE(commands.size() == 2);

    const auto hello = std::find_if(commands.begin(), commands.end(),
                                    [](const cc::commands::CustomCommand& command) {
                                        return command.name == "hello";
                                    });
    REQUIRE(hello != commands.end());
    REQUIRE(hello->description == "Say hello");
    REQUIRE(hello->argument_hint == "<name>");
    REQUIRE(hello->content.find("Say hello to $ARGUMENTS") != std::string::npos);

    // The folders the file is in become part of the name.
    const auto commit = std::find_if(commands.begin(), commands.end(),
                                     [](const cc::commands::CustomCommand& command) {
                                         return command.name == "git:commit";
                                     });
    REQUIRE(commit != commands.end());

    // A file that names no description gets the first line of its body.
    REQUIRE(commit->description == "Commit the changes");

    std::filesystem::remove_all(directory);
}

TEST_CASE("The hash of a text is the one the reference uses") {
    REQUIRE(cc::sha256_hex("") ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(cc::sha256_hex("abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(cc::sha256_hex("/tmp/foo.txt").substr(0, 16) == "065de4c848ef4c8a");
}

TEST_CASE("The files the session changes can be put back") {
    const auto directory = std::filesystem::temp_directory_path() / "cc_file_history_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    const auto file = directory / "notes.txt";
    std::ofstream out(file);
    out << "the first version\n";
    out.close();

    cc::session::FileHistory history("session", directory.string(),
                                     (directory / "backups").string());

    // The snapshot of the message is taken before the model changes
    // anything.
    history.make_snapshot("message-1");

    // The model changes the file; the contents are copied aside first.
    history.track_edit(file.string());
    cc::write_file(file.string(), "the second version\n");

    // The next message takes the next snapshot.
    history.make_snapshot("message-2");

    REQUIRE(history.can_restore("message-1"));

    // The rewind puts the file back the way it was.
    const auto changed = history.rewind("message-1");
    REQUIRE(changed.size() == 1);
    REQUIRE(cc::read_file(file.string()).value_or("") == "the first version\n");

    std::filesystem::remove_all(directory);
}

TEST_CASE("The history counts the lines that a turn changed") {
    const auto directory = std::filesystem::temp_directory_path() / "cc_file_history_diff_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    const auto file = directory / "notes.txt";
    std::ofstream out(file);
    out << "the first version\nthe second line\n";
    out.close();

    cc::session::FileHistory history("session", directory.string(),
                                     (directory / "backups").string());

    // The snapshot of the message is taken before the model changes
    // anything.
    history.make_snapshot("message-1");

    // The model changes the file; the contents are copied aside first.
    history.track_edit(file.string());
    cc::write_file(file.string(), "the first version\nthe third line\n");

    // The next message takes the next snapshot.
    history.make_snapshot("message-2");

    // The turn of the first message changed one line of one file.
    const auto changed = history.diff_to_next("message-1");
    REQUIRE(changed.has_value());
    REQUIRE(changed->files_changed.size() == 1);
    REQUIRE(changed->insertions == 1);
    REQUIRE(changed->deletions == 1);

    // The rewind to the first message would put the line back.
    const auto restored = history.diff_to_now("message-1");
    REQUIRE(restored.has_value());
    REQUIRE(restored->files_changed.size() == 1);
    REQUIRE(restored->insertions == 1);
    REQUIRE(restored->deletions == 1);

    // A message that was never snapshotted has no code of its own.
    REQUIRE(!history.diff_to_next("message-3").has_value());

    std::filesystem::remove_all(directory);
}

TEST_CASE("A slash command is parsed the way the reference parses it") {
    const auto plain_command = cc::parse_slash_command("/compact");
    REQUIRE(plain_command.has_value());
    REQUIRE(plain_command->command_name == "compact");
    REQUIRE(plain_command->args.empty());
    REQUIRE(!plain_command->is_mcp);

    const auto with_args = cc::parse_slash_command("/commit fix the parser");
    REQUIRE(with_args.has_value());
    REQUIRE(with_args->command_name == "commit");
    REQUIRE(with_args->args == "fix the parser");

    // The prompts of an MCP server are written `<server>:<prompt> (MCP)`, so
    // the marker is the second word and the arguments start after it.
    const auto mcp = cc::parse_slash_command("/my-server:review (MCP) src/main.cpp");
    REQUIRE(mcp.has_value());
    REQUIRE(mcp->is_mcp);
    REQUIRE(mcp->command_name == "my-server:review (MCP)");
    REQUIRE(mcp->args == "src/main.cpp");

    // A command that is not a slash command is not one.
    REQUIRE(!cc::parse_slash_command("hello").has_value());
    REQUIRE(!cc::parse_slash_command("/").has_value());
}

TEST_CASE("The model is told which command was run") {
    REQUIRE(cc::commands::command_metadata("compact", "") ==
            "<command-message>compact</command-message>\n"
            "<command-name>/compact</command-name>");

    REQUIRE(cc::commands::command_metadata("commit", "the message") ==
            "<command-message>commit</command-message>\n"
            "<command-name>/commit</command-name>\n"
            "<command-args>the message</command-args>");
}

TEST_CASE("The prompts of an MCP server are offered as commands") {
    cc::mcp::McpCommand command;
    command.name = "mcp__my_server__review";
    command.user_facing_name = "my-server:review (MCP)";
    command.description = "Reviews the changes";
    command.argument_names = {"path", "comment"};
    command.server_name = "my-server";
    command.prompt_name = "review";

    const auto suggestions = ui::suggestions_for("/my-server", "/tmp", {}, {command});
    REQUIRE(suggestions.size() == 1);
    REQUIRE(suggestions[0].display == "/my-server:review (MCP)");
    REQUIRE(suggestions[0].insert == "/my-server:review (MCP) ");
    REQUIRE(suggestions[0].description == "Reviews the changes (arguments: path, comment)");
}

TEST_CASE("The commands are offered in the order the reference offers them") {
    // The commands that were used recently stand at the top of the list, so
    // the test asks for a session that has used none of them.
    const EmptyConfig config;

    // With nothing typed the built-in commands are sorted by name.
    const auto all = ui::suggestions_for("/", "/tmp");
    REQUIRE(all.size() > 1);
    REQUIRE(all[0].display == "/add-dir");

    // The exact name comes first, then the names that start with it, the
    // shorter ones before the longer ones. The alias of `/resume` matches
    // as well, but its name does not start with what was typed, so it comes
    // after the names that do. The commands whose description holds the
    // word come last, the way the reference offers them.
    const auto con = ui::suggestions_for("/con", "/tmp");
    REQUIRE(con.size() > 3);
    REQUIRE(con[0].display == "/config");
    REQUIRE(con[1].display == "/context");
    REQUIRE(con[2].display == "/resume (continue)");

    // An alias matches as well, and the alias that was typed is shown in
    // brackets, the way the reference shows it. The commands whose
    // description holds the word are offered as well.
    const auto settings = ui::suggestions_for("/settings", "/tmp");
    REQUIRE(settings.size() > 1);
    REQUIRE(settings[0].display == "/config (settings)");
}

TEST_CASE("The file suggestions walk into the folder the path names") {
    const std::string cwd = "/tmp";

    // A path that ends in a separator names a folder: its entries are
    // offered, the folders among them with a slash at the end.
    const auto parent = ui::suggestions_for("@../", cwd);
    REQUIRE(!parent.empty());
    bool a_folder = false;
    for (const auto& item : parent) {
        REQUIRE(item.display.rfind("../", 0) == 0);
        REQUIRE(item.insert.rfind("@../", 0) == 0);
        if (item.kind == ui::Suggestion::Kind::Directory) a_folder = true;
    }
    REQUIRE(a_folder);

    // The home of the user is a folder as well.
    const auto home = ui::suggestions_for("@~/", cwd);
    REQUIRE(!home.empty());
    for (const auto& item : home) {
        REQUIRE(item.display.rfind("~/", 0) == 0);
    }

    // The entries of the folder itself are offered without a path, and the
    // folders among them carry a slash at the end.
    const auto entries = ui::suggestions_for("@", cwd);
    for (const auto& item : entries) {
        const size_t slash = item.display.find('/');
        if (slash == std::string::npos) continue;
        REQUIRE(slash == item.display.size() - 1);
        REQUIRE(item.kind == ui::Suggestion::Kind::Directory);
    }
}
