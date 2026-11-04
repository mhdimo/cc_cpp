#include <catch2/catch_test_macros.hpp>

#include "ui/ansi.hpp"
#include "ui/message_selector.hpp"
#include "ui/status_line.hpp"
#include "ui/theme.hpp"
#include "utils/json_util.hpp"
#include "utils/time.hpp"

#include <string>
#include <vector>

using namespace cc;

TEST_CASE("The message selector lists the prompts of the session") {
    const auto theme = ui::Theme::dark();

    std::vector<ui::RewindOption> options = {
        {.text = "the first prompt", .ago = "2 minutes ago"},
        {.text = "the second prompt", .ago = "just now"},
        {.text = "", .ago = "", .current = true},
    };
    const ui::MessageSelector selector(theme, std::move(options));

    const auto lines = selector.render(60);
    const std::string screen = ui::strip([&] {
        std::string joined;
        for (const auto& line : lines) joined += line + "\n";
        return joined;
    }());

    REQUIRE(screen.find("Rewind") != std::string::npos);
    REQUIRE(screen.find("the first prompt") != std::string::npos);
    REQUIRE(screen.find("the second prompt") != std::string::npos);
    REQUIRE(screen.find("(current)") != std::string::npos);
    REQUIRE(screen.find("Enter to continue") != std::string::npos);
}

TEST_CASE("The message selector starts on the newest prompt") {
    const auto theme = ui::Theme::dark();

    std::vector<ui::RewindOption> options = {
        {.text = "the first prompt"},
        {.text = "the second prompt"},
    };
    const ui::MessageSelector selector(theme, std::move(options));

    // The newest prompt is the one that is highlighted to begin with.
    REQUIRE(selector.selected() == 1);
}

TEST_CASE("The message selector names the code that can be restored") {
    const auto theme = ui::Theme::dark();

    std::vector<ui::RewindOption> options = {
        {.text = "the first prompt", .ago = "2 minutes ago"},
        {.text = "the second prompt", .ago = "just now", .diff = ui::RewindDiff{{"notes.txt"}, 12, 3}},
        {.text = "the third prompt", .ago = "just now", .diff = ui::RewindDiff{}},
        {.text = "the fourth prompt", .ago = "just now"},
        {.text = "", .ago = "", .current = true},
    };
    const ui::MessageSelector selector(theme, std::move(options));

    const auto lines = selector.render(60);
    const std::string screen = ui::strip([&] {
        std::string joined;
        for (const auto& line : lines) joined += line + "\n";
        return joined;
    }());

    // The files of the session are checkpointed, so the code can be put
    // back as well.
    REQUIRE(screen.find("Restore the code and/or conversation to the point before") != std::string::npos);

    // The row of every prompt tells what the code of the prompt changed.
    REQUIRE(screen.find("notes.txt +12 -3") != std::string::npos);
    REQUIRE(screen.find("No code changes") != std::string::npos);
    REQUIRE(screen.find("No code restore") != std::string::npos);
}

TEST_CASE("The options of the rewind are numbered") {
    const auto theme = ui::Theme::dark();

    std::vector<ui::RewindOption> options = {
        {.text = "the first prompt", .ago = "2 minutes ago"},
        {.text = "", .ago = "", .current = true},
    };
    ui::MessageSelector selector(theme, std::move(options));
    selector.set_selected(0);

    const auto lines = selector.render_confirm(60);
    const std::string screen = ui::strip([&] {
        std::string joined;
        for (const auto& line : lines) joined += line + "\n";
        return joined;
    }());

    REQUIRE(screen.find("1. Restore conversation") != std::string::npos);
    REQUIRE(screen.find("2. Summarize from here") != std::string::npos);
    REQUIRE(screen.find("3. Never mind") != std::string::npos);

    // The code cannot be restored, so the two code actions are not offered.
    REQUIRE(screen.find("Restore code") == std::string::npos);
}

TEST_CASE("The rewind offers the code when there are files to put back") {
    const auto theme = ui::Theme::dark();

    std::vector<ui::RewindOption> options = {
        {.text = "the first prompt", .ago = "2 minutes ago",
         .restore = ui::RewindDiff{{"notes.txt"}, 12, 3}},
        {.text = "", .ago = "", .current = true},
    };
    ui::MessageSelector selector(theme, std::move(options));
    selector.set_selected(0);

    const auto lines = selector.render_confirm(60);
    const std::string screen = ui::strip([&] {
        std::string joined;
        for (const auto& line : lines) joined += line + "\n";
        return joined;
    }());

    REQUIRE(screen.find("1. Restore code and conversation") != std::string::npos);
    REQUIRE(screen.find("2. Restore conversation") != std::string::npos);
    REQUIRE(screen.find("3. Restore code") != std::string::npos);
    REQUIRE(screen.find("4. Summarize from here") != std::string::npos);
    REQUIRE(screen.find("5. Never mind") != std::string::npos);

    // The code that would be restored is named, and the user is warned that
    // the files that were changed by hand are left alone.
    REQUIRE(screen.find("The code will be restored +12 -3 in notes.txt.") != std::string::npos);
    REQUIRE(screen.find("Rewinding does not affect files edited manually or via bash.") !=
            std::string::npos);
}

TEST_CASE("The status line payload carries the state of the session") {
    ui::StatusLineState state;
    state.session_id = "abc123";
    state.transcript_path = "/tmp/transcript.jsonl";
    state.cwd = "/work";
    state.project_dir = "/work";
    state.model_id = "claude-sonnet-4-5";
    state.model_display_name = "Sonnet 4.5";
    state.version = "1.2.3";
    state.has_usage = true;
    state.usage_input_tokens = 1000;
    state.usage_output_tokens = 500;
    state.usage_cache_read_input_tokens = 500;
    state.context_window_size = 10000;

    const auto payload = ui::build_status_line_input(state);

    REQUIRE(jget_string(payload, "session_id") == "abc123");
    REQUIRE(jget_string(payload, "transcript_path") == "/tmp/transcript.jsonl");
    REQUIRE(jget_string(payload, "cwd") == "/work");
    REQUIRE(jget_string(payload, "version") == "1.2.3");

    const auto model = jget_object(payload, "model");
    REQUIRE(jget_string(model, "id") == "claude-sonnet-4-5");
    REQUIRE(jget_string(model, "display_name") == "Sonnet 4.5");

    const auto workspace = jget_object(payload, "workspace");
    REQUIRE(jget_string(workspace, "project_dir") == "/work");

    // Fifteen hundred of the ten thousand tokens are in use.
    const auto context = jget_object(payload, "context_window");
    REQUIRE(jget_int(context, "used_percentage") == 15);
    REQUIRE(jget_int(context, "remaining_percentage") == 85);
}

TEST_CASE("The status line command writes what it prints") {
    const auto payload = boost::json::object{};
    const std::string text = ui::run_status_line("echo hello", payload, ".");
    REQUIRE(text == "hello");
}

TEST_CASE("A status line command that fails leaves the line empty") {
    const auto payload = boost::json::object{};
    REQUIRE(ui::run_status_line("exit 3", payload, ".").empty());
}

TEST_CASE("The clipboard sequence carries the text base64-encoded") {
    const std::string sequence = ui::clipboard_sequence("hi");
    REQUIRE(sequence == "\x1b]52;c;aGk=\a");
}

TEST_CASE("A timestamp is read back as the time it names") {
    // 2026-01-02T03:04:05Z
    const int64_t parsed = parse_timestamp("2026-01-02T03:04:05.000Z");
    REQUIRE(parsed > 0);

    // The time that has passed is said in the way Claude Code says it.
    REQUIRE(relative_time_ago(parsed - 5000, parsed) == "just now");
    REQUIRE(relative_time_ago(parsed - 30'000, parsed) == "30 seconds ago");
    REQUIRE(relative_time_ago(parsed - 120'000, parsed) == "2 minutes ago");
    REQUIRE(relative_time_ago(parsed - 7'200'000, parsed) == "2 hours ago");
    REQUIRE(relative_time_ago(parsed - 172'800'000, parsed) == "2 days ago");
}
