#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "tools/impl/common.hpp"
#include "tools/impl/file_tools.hpp"
#include "tools/impl/search_tools.hpp"
#include "tools/impl/bash_tool.hpp"
#include "tools/registry.hpp"
#include "tools/tool_use_context.hpp"

#include "utils/fs.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace cc;

namespace {

/// A temporary directory that cleans itself up.
class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() / ("cc_test_" + std::to_string(::getpid()) + "_" +
                                              std::to_string(counter_++));
        path_string_ = path_.string();
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const std::string& path() const { return path_string_; }

    std::string write(const std::string& name, const std::string& contents) const {
        const fs::path file = path_ / name;
        fs::create_directories(file.parent_path());
        std::ofstream out(file);
        out << contents;
        out.close();
        return file.string();
    }

private:
    fs::path path_;
    std::string path_string_;
    static inline int counter_ = 0;
};

/// A context that runs tools in `cwd`.
ToolUseContext make_context(const std::string& cwd) {
    ToolUseContext context;
    context.options.cwd = cwd;
    context.original_cwd = cwd;
    return context;
}

} // namespace

TEST_CASE("Read numbers the lines it returns") {
    TempDir dir;
    const std::string file = dir.write("a.txt", "one\ntwo\nthree\n");

    std::shared_ptr<tools::FileStateTracker> tracker =
        std::make_shared<tools::FileStateTracker>();
    tools::ReadTool tool(tracker);
    auto context = make_context(dir.path());

    boost::json::object input;
    input["file_path"] = file;

    const ToolResult result = tool.call(input, context);

    REQUIRE(result.content.starts_with("1\tone\n2\ttwo\n3\tthree\n"));
    REQUIRE_FALSE(result.is_error);
}

TEST_CASE("Read honours the offset and limit") {
    TempDir dir;
    const std::string file = dir.write("a.txt", "one\ntwo\nthree\nfour\n");

    std::shared_ptr<tools::FileStateTracker> tracker =
        std::make_shared<tools::FileStateTracker>();
    tools::ReadTool tool(tracker);
    auto context = make_context(dir.path());

    boost::json::object input;
    input["file_path"] = file;
    input["offset"] = 2;
    input["limit"] = 2;

    const ToolResult result = tool.call(input, context);
    REQUIRE(result.content.starts_with("2\ttwo\n3\tthree\n"));
}

TEST_CASE("Read reports a missing file the way Claude Code does") {
    TempDir dir;
    std::shared_ptr<tools::FileStateTracker> tracker =
        std::make_shared<tools::FileStateTracker>();
    tools::ReadTool tool(tracker);
    auto context = make_context(dir.path());

    boost::json::object input;
    input["file_path"] = dir.path() + "/nope.txt";

    REQUIRE_THROWS_WITH(tool.call(input, context),
                        Catch::Matchers::ContainsSubstring("File does not exist."));
    REQUIRE_THROWS_WITH(tool.call(input, context),
                        Catch::Matchers::ContainsSubstring(
                            "Note: your current working directory is"));
}

TEST_CASE("Write refuses a file that was not read") {
    TempDir dir;
    const std::string file = dir.write("a.txt", "old\n");

    std::shared_ptr<tools::FileStateTracker> tracker =
        std::make_shared<tools::FileStateTracker>();
    tools::WriteTool tool(tracker);
    auto context = make_context(dir.path());

    boost::json::object input;
    input["file_path"] = file;
    input["content"] = "new\n";

    REQUIRE_THROWS_WITH(tool.call(input, context),
                        Catch::Matchers::ContainsSubstring("File has not been read yet."));
}

TEST_CASE("Write creates a file that does not exist") {
    TempDir dir;
    const std::string file = dir.path() + "/new.txt";

    std::shared_ptr<tools::FileStateTracker> tracker =
        std::make_shared<tools::FileStateTracker>();
    tools::WriteTool tool(tracker);
    auto context = make_context(dir.path());

    boost::json::object input;
    input["file_path"] = file;
    input["content"] = "hello\n";

    const ToolResult result = tool.call(input, context);
    REQUIRE(result.content == "File created successfully at: " + file);
    REQUIRE(read_file(file).value() == "hello\n");
}

TEST_CASE("Edit replaces a unique string") {
    TempDir dir;
    const std::string file = dir.write("a.txt", "alpha\nbeta\n");

    std::shared_ptr<tools::FileStateTracker> tracker =
        std::make_shared<tools::FileStateTracker>();
    tools::ReadTool reader(tracker);
    tools::EditTool editor(tracker);
    auto context = make_context(dir.path());

    boost::json::object read_input;
    read_input["file_path"] = file;
    reader.call(read_input, context);

    boost::json::object input;
    input["file_path"] = file;
    input["old_string"] = "beta";
    input["new_string"] = "gamma";

    const ToolResult result = editor.call(input, context);
    REQUIRE(result.content == "The file " + file + " has been updated successfully.");
    REQUIRE(read_file(file).value() == "alpha\ngamma\n");
}

TEST_CASE("Edit refuses an ambiguous match") {
    TempDir dir;
    const std::string file = dir.write("a.txt", "x\nx\n");

    std::shared_ptr<tools::FileStateTracker> tracker =
        std::make_shared<tools::FileStateTracker>();
    tools::ReadTool reader(tracker);
    tools::EditTool editor(tracker);
    auto context = make_context(dir.path());

    boost::json::object read_input;
    read_input["file_path"] = file;
    reader.call(read_input, context);

    boost::json::object input;
    input["file_path"] = file;
    input["old_string"] = "x";
    input["new_string"] = "y";

    REQUIRE_THROWS_WITH(
        editor.call(input, context),
        Catch::Matchers::ContainsSubstring("Found 2 matches of the string to replace"));
}

TEST_CASE("Edit replaces every occurrence when asked to") {
    TempDir dir;
    const std::string file = dir.write("a.txt", "x\nx\n");

    std::shared_ptr<tools::FileStateTracker> tracker =
        std::make_shared<tools::FileStateTracker>();
    tools::ReadTool reader(tracker);
    tools::EditTool editor(tracker);
    auto context = make_context(dir.path());

    boost::json::object read_input;
    read_input["file_path"] = file;
    reader.call(read_input, context);

    boost::json::object input;
    input["file_path"] = file;
    input["old_string"] = "x";
    input["new_string"] = "y";
    input["replace_all"] = true;

    const ToolResult result = editor.call(input, context);
    REQUIRE(result.content ==
            "The file " + file + " has been updated. All occurrences were successfully replaced.");
    REQUIRE(read_file(file).value() == "y\ny\n");
}

TEST_CASE("Glob lists the matching files") {
    TempDir dir;
    dir.write("one.txt", "1");
    dir.write("two.md", "2");

    tools::GlobTool tool;
    auto context = make_context(dir.path());

    boost::json::object input;
    input["pattern"] = "*.txt";

    const ToolResult result = tool.call(input, context);
    REQUIRE(result.content.find("one.txt") != std::string::npos);
    REQUIRE(result.content.find("two.md") == std::string::npos);
}

TEST_CASE("Grep finds the matching lines") {
    TempDir dir;
    dir.write("a.txt", "alpha\nbeta\ngamma\n");

    tools::GrepTool tool;
    auto context = make_context(dir.path());

    boost::json::object input;
    input["pattern"] = "beta";
    input["output_mode"] = "content";

    const ToolResult result = tool.call(input, context);
    REQUIRE(result.content.find("beta") != std::string::npos);
}

TEST_CASE("Bash runs a command and returns its output") {
    tools::BashTool tool;
    auto context = make_context(fs::current_path().string());

    boost::json::object input;
    input["command"] = "echo hello";

    const ToolResult result = tool.call(input, context);
    REQUIRE(result.content == "hello");
    REQUIRE_FALSE(result.is_error);
}

TEST_CASE("Bash reports a failing command") {
    tools::BashTool tool;
    auto context = make_context(fs::current_path().string());

    boost::json::object input;
    input["command"] = "exit 3";

    const ToolResult result = tool.call(input, context);
    REQUIRE(result.is_error);
    REQUIRE(result.content.find("Exit code 3") != std::string::npos);
}

TEST_CASE("The registry offers the tools Claude Code offers") {
    tools::Registry registry;
    const auto names = registry.names();

    for (const auto* expected : {"Read", "Write", "Edit", "Glob", "Grep", "Bash"}) {
        REQUIRE(std::find(names.begin(), names.end(), expected) != names.end());
    }
}

TEST_CASE("The plan mode is entered when the model asks for it") {
    tools::Registry registry;
    const auto names = registry.names();

    REQUIRE(std::find(names.begin(), names.end(), "EnterPlanMode") != names.end());

    Tool* tool = find_tool_by_name(registry.all(), "EnterPlanMode");
    REQUIRE(tool != nullptr);

    // The tool puts the session into the plan mode.
    auto context = make_context(fs::temp_directory_path().string());
    REQUIRE(context.permission_context.mode != PermissionMode::Plan);

    const ToolResult result = tool->call(boost::json::object{}, context);
    REQUIRE(context.permission_context.mode == PermissionMode::Plan);
    REQUIRE(result.content.find("Entered plan mode") != std::string::npos);
}
