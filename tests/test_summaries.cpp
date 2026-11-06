#include <catch2/catch_test_macros.hpp>

#include "tools/impl/search_tools.hpp"
#include "tools/tool_names.hpp"
#include "ui/ansi.hpp"
#include "ui/theme.hpp"
#include "ui/tool_display.hpp"
#include "utils/json_util.hpp"

using namespace cc;

namespace {

/// Builds the structured payload a tool attaches to its result.
boost::json::value payload(boost::json::object object) {
    return boost::json::value(std::move(object));
}

} // namespace

TEST_CASE("A read shows how many lines it read") {
    const auto theme = ui::Theme::dark();

    boost::json::object result;
    result["type"] = "text";
    result["filePath"] = "/tmp/a.txt";
    result["numLines"] = 42;

    const auto lines = ui::render_tool_result_lines(tools::READ, "the whole file", payload(result),
                                                    "/tmp", theme, 60, false);

    REQUIRE(lines.size() == 1);
    const std::string text = ui::strip(lines[0]);
    REQUIRE(text.find("Read 42 lines") != std::string::npos);
    // The raw content is not shown.
    REQUIRE(text.find("the whole file") == std::string::npos);
}

TEST_CASE("A read of a single line is counted in the singular") {
    const auto theme = ui::Theme::dark();

    boost::json::object result;
    result["type"] = "text";
    result["numLines"] = 1;

    const auto lines = ui::render_tool_result_lines(tools::READ, "", payload(result), "/tmp", theme,
                                                    60, false);
    REQUIRE(ui::strip(lines[0]).find("Read 1 line") != std::string::npos);
}

TEST_CASE("A read of an unchanged file says so") {
    const auto theme = ui::Theme::dark();

    boost::json::object result;
    result["type"] = "file_unchanged";
    result["filePath"] = "/tmp/a.txt";

    const auto lines = ui::render_tool_result_lines(tools::READ, "", payload(result), "/tmp", theme,
                                                    60, false);
    REQUIRE(ui::strip(lines[0]).find("Unchanged since last read") != std::string::npos);
}

TEST_CASE("A write says how many lines it wrote and previews the file") {
    const auto theme = ui::Theme::dark();

    boost::json::object result;
    result["type"] = "create";
    result["filePath"] = "/tmp/main.cpp";
    result["content"] = "int main() {}\nint other() {}\n";

    const auto lines = ui::render_tool_result_lines(tools::WRITE, "", payload(result), "/tmp", theme,
                                                    60, false);

    REQUIRE(lines.size() >= 2);
    const std::string first = ui::strip(lines[0]);
    REQUIRE(first.find("Wrote 2 lines to") != std::string::npos);
    REQUIRE(first.find("main.cpp") != std::string::npos);
    REQUIRE(ui::strip(lines[1]).find("int main()") != std::string::npos);
}

TEST_CASE("A write that replaced a file shows the diff") {
    const auto theme = ui::Theme::dark();

    boost::json::object result;
    result["type"] = "update";
    result["filePath"] = "/tmp/main.cpp";
    result["originalFile"] = "one\ntwo\n";
    result["content"] = "one\nTWO\nthree\n";

    const auto lines = ui::render_tool_result_lines(tools::WRITE, "", payload(result), "/tmp", theme,
                                                    60, false);

    REQUIRE(lines.size() >= 2);
    const std::string first = ui::strip(lines[0]);
    REQUIRE(first.find("Added 2 lines") != std::string::npos);
    REQUIRE(first.find("removed 1 line") != std::string::npos);
}

TEST_CASE("A search says how many files it found") {
    const auto theme = ui::Theme::dark();

    boost::json::object result;
    result["mode"] = "files_with_matches";
    result["numFiles"] = 3;
    result["filenames"] = boost::json::array();

    const auto lines = ui::render_tool_result_lines(tools::GLOB, "a\nb\nc", payload(result), "/tmp",
                                                    theme, 60, false);
    REQUIRE(ui::strip(lines[0]).find("Found 3 files") != std::string::npos);
}

TEST_CASE("A search that matched one file is counted in the singular") {
    const auto theme = ui::Theme::dark();

    boost::json::object result;
    result["mode"] = "files_with_matches";
    result["numFiles"] = 1;

    const auto lines = ui::render_tool_result_lines(tools::GREP, "", payload(result), "/tmp", theme,
                                                    60, false);
    REQUIRE(ui::strip(lines[0]).find("Found 1 file") != std::string::npos);
}

TEST_CASE("A search that counted matches says how many files they were in") {
    const auto theme = ui::Theme::dark();

    boost::json::object result;
    result["mode"] = "count";
    result["numMatches"] = 5;
    result["numFiles"] = 2;

    const auto lines = ui::render_tool_result_lines(tools::GREP, "", payload(result), "/tmp", theme,
                                                    60, false);
    const std::string text = ui::strip(lines[0]);
    REQUIRE(text.find("Found 5 matches") != std::string::npos);
    REQUIRE(text.find("across 2 files") != std::string::npos);
}

TEST_CASE("A search that printed lines says how many") {
    const auto theme = ui::Theme::dark();

    boost::json::object result;
    result["mode"] = "content";
    result["numLines"] = 12;

    const auto lines = ui::render_tool_result_lines(tools::GREP, "", payload(result), "/tmp", theme,
                                                    60, false);
    REQUIRE(ui::strip(lines[0]).find("Found 12 lines") != std::string::npos);
}

TEST_CASE("The questions the user answered are reported back") {
    const auto theme = ui::Theme::dark();

    boost::json::object answers;
    answers["Which library?"] = "date-fns";

    boost::json::object result;
    result["answers"] = std::move(answers);

    const auto lines = ui::render_tool_result_lines(tools::ASK_USER_QUESTION, "", payload(result),
                                                    "/tmp", theme, 60, false);

    REQUIRE(lines.size() == 2);
    REQUIRE(ui::strip(lines[0]).find("User answered Claude's questions:") != std::string::npos);
    REQUIRE(ui::strip(lines[1]).find("Which library? → date-fns") != std::string::npos);
}

TEST_CASE("A tool without a summary shows its raw result") {
    const auto theme = ui::Theme::dark();

    const auto lines = ui::render_tool_result_lines(tools::BASH, "hello", boost::json::value(nullptr),
                                                    "/tmp", theme, 60, false);

    REQUIRE(lines.size() == 1);
    REQUIRE(ui::strip(lines[0]).find("hello") != std::string::npos);
}

TEST_CASE("The todo tool draws nothing at all") {
    const auto theme = ui::Theme::dark();

    const auto lines = ui::render_tool_result_lines(tools::TODO_WRITE, "Todos have been modified",
                                                    boost::json::value(nullptr), "/tmp", theme, 60,
                                                    false);

    REQUIRE(lines.empty());
}

TEST_CASE("The plan tool says the plan was approved") {
    const auto theme = ui::Theme::dark();

    const auto lines = ui::render_tool_result_lines(tools::EXIT_PLAN_MODE, "", boost::json::value(nullptr),
                                                    "/tmp", theme, 60, false);

    REQUIRE(lines.size() == 1);
    REQUIRE(ui::strip(lines[0]).find("Exited plan mode") != std::string::npos);
}

TEST_CASE("The search tools are called Search") {
    boost::json::object input;
    input["pattern"] = "*.cpp";
    input["path"] = "/tmp";

    tools::GlobTool glob;
    const std::string name = ui::user_facing_tool_name(glob, input);
    REQUIRE(name == "Search");

    const std::string args = ui::user_facing_tool_args(glob, input, "/tmp");
    REQUIRE(args.find("pattern: \"*.cpp\"") != std::string::npos);
    // A path that is the working directory itself is shown in full.
    REQUIRE(args.find("path: \"/tmp\"") != std::string::npos);
}
