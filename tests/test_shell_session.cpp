#include "tools/impl/bash_tool.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include <unistd.h>

namespace cc::tools {
namespace {

/// A folder that is gone when the test is over.
class TempFolder {
public:
    TempFolder() {
        path_ = std::filesystem::temp_directory_path() / ("cc-shell-" + std::to_string(::getpid()));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_ / "project");
        std::filesystem::create_directories(path_ / "other");
    }

    ~TempFolder() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    /// The folder of the session.
    std::string project() const { return (path_ / "project").string(); }

    /// A folder beside the folder of the session.
    std::string other() const { return (path_ / "other").string(); }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("the shell of the session stays alive", "[bash][shell]") {
    TempFolder folder;

    ShellSession session(folder.project());

    SECTION("a command runs in the folder of the session") {
        const ShellResult result = session.run("pwd", 10000);
        REQUIRE(result.exit_code == 0);
        REQUIRE(result.output.find("project") != std::string::npos);
    }

    SECTION("the shell of the session is one shell, not one per command") {
        // A variable that is set by a command is still there for the next
        // one: the shell is the same.
        const ShellResult first = session.run("export CC_MARKER=kept", 10000);
        REQUIRE(first.exit_code == 0);

        const ShellResult second = session.run("echo $CC_MARKER", 10000);
        REQUIRE(second.output.find("kept") != std::string::npos);
    }

    SECTION("a cd of a command moves the folder the session is in") {
        session.set_allowed_directories({folder.other()});

        const ShellResult first = session.run("cd " + folder.other(), 10000);
        REQUIRE(first.exit_code == 0);
        REQUIRE(session.cwd() == folder.other());

        // The folder the shell is in is the one the command left it in.
        const ShellResult second = session.run("pwd", 10000);
        REQUIRE(second.output.find(folder.other()) != std::string::npos);
    }

    SECTION("the folder of the session is put back and the model is told") {
        const ShellResult result = session.run("cd ..", 10000);

        // The shell is back in the folder of the session.
        REQUIRE(session.cwd() == folder.project());
        REQUIRE(result.error_output.find("Shell cwd was reset to ") != std::string::npos);
        REQUIRE(result.error_output.find(folder.project()) != std::string::npos);
    }

    SECTION("a folder that was added to the session may be reached") {
        session.set_allowed_directories({folder.other()});

        const ShellResult result = session.run("cd " + folder.other(), 10000);

        // The shell stays where the command put it: the folder was added
        // to the session.
        REQUIRE(session.cwd() == folder.other());
        REQUIRE(result.error_output.find("Shell cwd was reset") == std::string::npos);
    }

    SECTION("the folder of the session is kept whatever the command does") {
        session.set_maintain_project_working_dir(true);

        const ShellResult result = session.run("cd ..", 10000);

        REQUIRE(session.cwd() == folder.project());
        // The reference says nothing when the folder was to be kept.
        REQUIRE(result.error_output.find("Shell cwd was reset") == std::string::npos);
    }

    SECTION("the two streams are kept apart") {
        const ShellResult result = session.run("echo out; echo err 1>&2", 10000);
        REQUIRE(result.output.find("out") != std::string::npos);
        REQUIRE(result.error_output.find("err") != std::string::npos);
        REQUIRE(result.output.find("err") == std::string::npos);
    }

    SECTION("the exit code of the command is the exit code of the shell") {
        const ShellResult result = session.run("exit 3", 10000);
        REQUIRE(result.exit_code == 3);
    }
}

} // namespace cc::tools
