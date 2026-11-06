#include "utils/process.hpp"

#include "utils/string.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace cc {
namespace {

/// The name of a file that does not exist yet.
std::string marker_path() {
    return (std::filesystem::temp_directory_path() /
            ("cc-process-marker-" + std::to_string(::getpid()) + ".txt"))
        .string();
}

TEST_CASE("The output of a process is collected", "[process]") {
    const ProcessOutcome outcome = run_process("echo hello; echo world", {}, "", 10);

    CHECK_FALSE(outcome.failed);
    CHECK_FALSE(outcome.timed_out);
    CHECK(outcome.out.find("hello") != std::string::npos);
    CHECK(outcome.out.find("world") != std::string::npos);
    CHECK(outcome.exit_code == 0);
}

TEST_CASE("The exit code of a process is kept", "[process]") {
    const ProcessOutcome outcome = run_process("exit 7", {}, "", 10);

    CHECK(outcome.exit_code == 7);
}

TEST_CASE("What a process writes on its error is collected", "[process]") {
    const ProcessOutcome outcome = run_process("echo oops >&2", {}, "", 10);

    CHECK(outcome.err.find("oops") != std::string::npos);
}

TEST_CASE("The text of the standard input reaches the process", "[process]") {
    const ProcessOutcome outcome = run_process("cat", {}, "the text", 10);

    CHECK(outcome.out == "the text");
}

TEST_CASE("A process that runs for too long is killed", "[process]") {
    const ProcessOutcome outcome = run_process("sleep 30", {}, "", 1);

    CHECK(outcome.timed_out);
}

TEST_CASE("What a process starts goes with it", "[process]") {
    // The shell of the command starts a process of its own, and that one is
    // the one that holds the pipes open. Killing only the shell would leave
    // the process behind as an orphan that runs for ever, so the whole group
    // has to be killed with the shell.
    const std::string marker = marker_path();
    std::filesystem::remove(marker);

    const std::string command = "(sleep 3; echo alive > " + marker + ") & wait";
    const ProcessOutcome outcome = run_process(command, {}, "", 1);

    CHECK(outcome.timed_out);

    // The process that was started is given the time to write its marker if
    // it were left alone. It is not.
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    CHECK_FALSE(std::filesystem::exists(marker));
}

TEST_CASE("The group of a process is its own", "[process]") {
    // The process runs in a group of its own, so that what it starts can be
    // reached without touching the group of the port itself. The group of
    // the shell is therefore the pid of the shell.
    const ProcessOutcome outcome = run_process("echo $$; ps -o pgid= -p $$", {}, "", 10);

    CHECK_FALSE(outcome.failed);

    const std::vector<std::string> lines = split_lines(outcome.out);
    REQUIRE(lines.size() >= 2);
    CHECK(cc::trim(lines[0]) == cc::trim(lines[1]));
}

} // namespace
} // namespace cc
