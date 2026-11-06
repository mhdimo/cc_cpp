#include <catch2/catch_test_macros.hpp>

#include "tools/impl/agent_tool.hpp"
#include "tools/impl/file_tools.hpp"
#include "tools/impl/common.hpp"
#include "tools/tool_names.hpp"
#include "ui/ansi.hpp"
#include "ui/spinner.hpp"
#include "ui/theme.hpp"
#include "ui/tool_display.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace cc;

namespace {

/// Builds the structured payload a tool attaches to its result.
boost::json::value payload(boost::json::object object) {
    return boost::json::value(std::move(object));
}

/// The escape sequence that draws the text in the colour of the shimmer.
std::string shimmer_sequence(const ui::Theme& theme) {
    return "\x1b[38;2;" + std::to_string(theme.claude_shimmer.r) + ";" +
           std::to_string(theme.claude_shimmer.g) + ";" + std::to_string(theme.claude_shimmer.b) + "m";
}

/// The text that is drawn in the colour of the shimmer.
std::string shimmer_text(const std::string& line, const ui::Theme& theme) {
    const std::string sequence = shimmer_sequence(theme);
    const size_t at = line.find(sequence);
    if (at == std::string::npos) return {};
    const size_t start = at + sequence.size();
    const size_t end = line.find("\x1b[0m", start);
    if (end == std::string::npos) return {};
    return line.substr(start, end - start);
}

} // namespace

TEST_CASE("The result of an edit shows the hunks of the change") {
    const auto theme = ui::Theme::dark();

    std::string before;
    for (int i = 1; i <= 11; ++i) before += "line " + std::to_string(i) + "\n";

    std::string after = before;
    const size_t second = after.find("line 2\n");
    after.replace(second, 6, "line X");
    const size_t tenth = after.find("line 10\n");
    after.replace(tenth, 7, "line Y");

    const auto lines = ui::render_diff_result(before, after, theme, 80);

    std::string text;
    for (const auto& line : lines) text += ui::strip(line) + "\n";

    // The two changes are shown with three lines of context around them.
    REQUIRE(text.find("line 1") != std::string::npos);
    REQUIRE(text.find("line 5") != std::string::npos);
    REQUIRE(text.find("line 7") != std::string::npos);
    REQUIRE(text.find("line 11") != std::string::npos);

    // The lines between the two changes are left out.
    REQUIRE(text.find("line 6") == std::string::npos);

    // The hunks are separated by three dots.
    REQUIRE(text.find("...") != std::string::npos);
}

TEST_CASE("The answers of the question tool carry the gutter in front of the first one only") {
    const auto theme = ui::Theme::dark();

    boost::json::object answers;
    answers["First?"] = "Yes";
    answers["Second?"] = "No";

    boost::json::object result;
    result["answers"] = boost::json::value(answers);

    const auto lines = ui::render_tool_result_lines(tools::ASK_USER_QUESTION, "",
                                                    payload(result), "/tmp", theme, 60, false);

    REQUIRE(lines.size() == 3);

    // The line that names the tool has no gutter of its own.
    REQUIRE(ui::strip(lines[0]).find("User answered Claude's questions") != std::string::npos);
    REQUIRE(ui::strip(lines[0]).find("⎿") == std::string::npos);

    // The first answer carries the gutter, the second one is indented under
    // it.
    REQUIRE(ui::strip(lines[1]).rfind("  ⎿  ", 0) == 0);
    REQUIRE(ui::strip(lines[2]).rfind("     ", 0) == 0);
    REQUIRE(ui::strip(lines[2]).find("⎿") == std::string::npos);
}

TEST_CASE("The plan that was approved carries the gutter in front of its first line only") {
    const auto theme = ui::Theme::dark();

    boost::json::object result;
    result["plan"] = "The plan of the session.";
    result["filePath"] = "/tmp/plan.md";

    const auto lines = ui::render_tool_result_lines(tools::EXIT_PLAN_MODE, "", payload(result), "/tmp",
                                                    theme, 60, false);

    REQUIRE(lines.size() > 2);

    // The line that names the tool has no gutter of its own.
    REQUIRE(ui::strip(lines[0]).find("User approved Claude's plan") != std::string::npos);
    REQUIRE(ui::strip(lines[0]).find("⎿") == std::string::npos);

    // The block of the plan carries the gutter in front of its first line
    // only.
    REQUIRE(ui::strip(lines[1]).rfind("  ⎿  ", 0) == 0);
    REQUIRE(ui::strip(lines[2]).rfind("     ", 0) == 0);
    REQUIRE(ui::strip(lines[2]).find("⎿") == std::string::npos);
}

TEST_CASE("A file of the plan is named by the tool that writes it") {
    const auto tracker = std::make_shared<tools::FileStateTracker>();

    boost::json::object input;
    input["file_path"] = cc::path_join(cc::claude_config_dir(), "plans/plan.md");

    // The tool that writes the plan names the plan, not the file.
    tools::EditTool edit(tracker);
    REQUIRE(ui::user_facing_tool_args(edit, input, "/tmp").empty());

    tools::WriteTool write(tracker);
    REQUIRE(ui::user_facing_tool_args(write, input, "/tmp").empty());

    // The tool that reads it still names the file.
    tools::ReadTool read(tracker);
    REQUIRE(!ui::user_facing_tool_args(read, input, "/tmp").empty());
}

TEST_CASE("The agent is named by the kind of the agent") {
    tools::AgentTool agent({}, [](const std::string&, const std::string&, const std::vector<std::string>&,
                                  const std::string&) { return std::string(); });

    boost::json::object input;
    input["subagent_type"] = "Explore";
    REQUIRE(ui::user_facing_tool_name(agent, input) == "Explore");

    // The general-purpose agent and the worker are called Agent.
    input["subagent_type"] = "general-purpose";
    REQUIRE(ui::user_facing_tool_name(agent, input) == "Agent");

    input["subagent_type"] = "worker";
    REQUIRE(ui::user_facing_tool_name(agent, input) == "Agent");

    input["subagent_type"] = "";
    REQUIRE(ui::user_facing_tool_name(agent, input) == "Agent");
}

TEST_CASE("The line that says how many lines are left stands under the error") {
    const auto theme = ui::Theme::dark();

    std::string message = "the tool failed";
    for (int i = 0; i < 20; ++i) message += "\nmore of it";

    const auto lines = ui::render_tool_error_lines(message, theme, 60);
    REQUIRE(lines.size() == 11);

    // The line that counts the lines that are left is indented under the
    // error, the way the reference indents it.
    REQUIRE(ui::strip(lines.back()).rfind("     ", 0) == 0);
    REQUIRE(ui::strip(lines.back()).find("… +11 lines (ctrl+o to see all)") != std::string::npos);
}

TEST_CASE("The band of the shimmer is three columns wide") {
    const auto theme = ui::Theme::dark();
    ui::Spinner spinner(theme);
    spinner.set_verb(std::string(60, 'a'));

    // The band sweeps across the word. The clock of the spinner is set, so
    // that the band is looked at at a known moment: at the edge of the
    // word it is cut short.
    int64_t now = 0;
    spinner.set_clock([&now] { return now; });

    bool whole = false;
    for (int column = 0; column < 80 && !whole; ++column) {
        now = column * 200;
        const std::string text = shimmer_text(spinner.line(), theme);
        if (!text.empty() && cc::display_width(text) == 3) whole = true;
    }
    REQUIRE(whole);

    // At the edge of the word the band is cut short, the way the reference
    // cuts it short.
    now = 2000;
    const std::string edge = shimmer_text(spinner.line(), theme);
    REQUIRE(!edge.empty());
    REQUIRE(cc::display_width(edge) < 3);
}

TEST_CASE("The arrow of the spinner points up while the request is on its way") {
    const auto theme = ui::Theme::dark();
    ui::Spinner spinner(theme);
    spinner.set_verb("Herding");

    // The arrow points up while the request is on its way to the model, and
    // down the rest of the time.
    spinner.set_mode(ui::SpinnerMode::Requesting);
    REQUIRE(ui::strip(spinner.line(80, 1000, true)).find("↑") != std::string::npos);

    spinner.set_mode(ui::SpinnerMode::Responding);
    REQUIRE(ui::strip(spinner.line(80, 1000, true)).find("↓") != std::string::npos);
}

TEST_CASE("The word of the spinner pulses as a whole while a tool runs") {
    const auto theme = ui::Theme::dark();
    ui::Spinner spinner(theme);
    spinner.set_verb("Herding");

    spinner.set_mode(ui::SpinnerMode::ToolUse);
    const std::string line = spinner.line();

    // The whole word is drawn in one colour, so the band of the shimmer is
    // not there any more.
    size_t colours = 0;
    for (size_t at = line.find("\x1b[38;2;"); at != std::string::npos;
         at = line.find("\x1b[38;2;", at + 1)) {
        ++colours;
    }
    REQUIRE(colours == 2);
}

TEST_CASE("A duration is written the way Claude Code writes it") {
    REQUIRE(ui::format_duration(0) == "0s");
    REQUIRE(ui::format_duration(3000) == "3s");
    REQUIRE(ui::format_duration(60000) == "1m 0s");
    REQUIRE(ui::format_duration(90000) == "1m 30s");
}
