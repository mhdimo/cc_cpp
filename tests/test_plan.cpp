#include <catch2/catch_test_macros.hpp>

#include "agents/agent.hpp"
#include "session/plan.hpp"
#include "utils/fs.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"
#include "utils/words.hpp"

#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace cc;

namespace {

/// A folder that is removed when the test is over.
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const char* base = std::getenv("TMPDIR");
        std::string root = base != nullptr ? base : "/tmp";
        while (!root.empty() && root.back() == '/') root.pop_back();

        path_ = path_join(path_join(root, "cc-plan-test-" + std::to_string(::getpid())), "home");
        remove_dir_all(path_);
        mkdirp(path_);

        const char* previous = std::getenv("CLAUDE_CONFIG_DIR");
        had_previous_ = previous != nullptr;
        if (previous != nullptr) previous_ = previous;
        ::setenv("CLAUDE_CONFIG_DIR", path_.c_str(), 1);
    }

    ~TemporaryDirectory() {
        if (had_previous_) {
            ::setenv("CLAUDE_CONFIG_DIR", previous_.c_str(), 1);
        } else {
            ::unsetenv("CLAUDE_CONFIG_DIR");
        }
        remove_dir_all(path_);
    }

    std::string path() const { return path_; }

private:
    std::string path_;
    std::string previous_{};
    bool had_previous_ = false;
};

} // namespace

TEST_CASE("A slug is three words joined by dashes") {
    const std::string slug = generate_word_slug();
    const std::vector<std::string> parts = split(slug, '-');
    REQUIRE(parts.size() == 3);
    for (const std::string& part : parts) REQUIRE_FALSE(part.empty());

    const std::string shorter = generate_short_word_slug();
    REQUIRE(split(shorter, '-').size() == 2);
}

TEST_CASE("The plan of a session is written and read") {
    TemporaryDirectory home;
    const std::string session = "11111111-2222-3333-4444-555555555555";

    REQUIRE_FALSE(session::read_plan(session).has_value());

    std::string error;
    REQUIRE(session::write_plan(session, "The plan of the session.\n", &error));
    REQUIRE(error.empty());

    const auto plan = session::read_plan(session);
    REQUIRE(plan.has_value());
    REQUIRE(*plan == "The plan of the session.\n");

    // The plan lives in the folder of the plans, under a name of three
    // words.
    REQUIRE(session::plan_path(session).find(session::plans_directory()) == 0);
    REQUIRE(ends_with(session::plan_path(session), ".md"));
}

TEST_CASE("The slug of a session is kept") {
    TemporaryDirectory home;
    const std::string session = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";

    const std::string first = session::plan_slug(session);
    const std::string second = session::plan_slug(session);
    REQUIRE(first == second);

    // A session of its own gets a slug of its own.
    REQUIRE_FALSE(session::plan_slug("another-session") == first);
}

TEST_CASE("The agents that ship with the CLI are there") {
    const auto builtins = agents::builtin_definitions();
    REQUIRE(builtins.size() == 3);

    REQUIRE(builtins[0].name == "Explore");
    REQUIRE_FALSE(builtins[0].description.empty());
    REQUIRE_FALSE(builtins[0].prompt.empty());
    // The agent that explores may not change anything.
    for (const std::string& tool : builtins[0].tools) {
        REQUIRE(tool != "Write");
        REQUIRE(tool != "Edit");
    }

    REQUIRE(builtins[1].name == "Plan");
    REQUIRE_FALSE(builtins[1].prompt.empty());

    REQUIRE(builtins[2].name == "statusline-setup");
    REQUIRE(builtins[2].tools.size() == 2);
}
