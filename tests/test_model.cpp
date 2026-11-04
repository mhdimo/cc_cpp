#include <catch2/catch_test_macros.hpp>

#include "config/environment.hpp"
#include "config/settings.hpp"
#include "query/errors.hpp"
#include "query/model.hpp"
#include "utils/fs.hpp"
#include "utils/path.hpp"
#include "utils/time.hpp"

#include <ai/error/api_call_error.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
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

        path_ = path_join(path_join(root, "cc-env-test-" + std::to_string(::getpid())), "home");
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

    const std::string& path() const { return path_; }

private:
    std::string path_;
    std::string previous_;
    bool had_previous_ = false;
};

} // namespace

TEST_CASE("the environment of the user is kept apart from the one of the folder", "[environment]") {
    TemporaryDirectory home;

    // The settings of the user.
    {
        std::ofstream out(home.path() + "/settings.json");
        out << R"({"env": {"ANTHROPIC_BASE_URL": "https://example.test", "ANTHROPIC_MODEL": "test-model"}})";
    }

    // The settings of the folder.
    const auto folder = std::filesystem::temp_directory_path() / "cc_env_project_test";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder / ".claude");
    {
        std::ofstream out(folder / ".claude" / "settings.json");
        out << R"({"env": {"ANTHROPIC_MODEL": "folder-model"}})";
    }

    const auto settings = config::load_settings(folder.string(), false);

    // The variables of the user are the ones of the user settings file.
    REQUIRE(settings.user_env.at("ANTHROPIC_BASE_URL") == "https://example.test");
    REQUIRE(settings.user_env.at("ANTHROPIC_MODEL") == "test-model");

    // The variables of the folder are kept apart; the more specific file
    // wins in the merged map.
    REQUIRE(settings.project_env.at("ANTHROPIC_MODEL") == "folder-model");
    REQUIRE(settings.env.at("ANTHROPIC_MODEL") == "folder-model");
    REQUIRE(settings.env.at("ANTHROPIC_BASE_URL") == "https://example.test");

    std::filesystem::remove_all(folder);
}

TEST_CASE("the environment of the session is applied to the process", "[environment]") {
    ::unsetenv("CC_TEST_VARIABLE");

    const std::map<std::string, std::string> variables = {{"CC_TEST_VARIABLE", "the value"}};
    config::apply_env(variables);

    const char* value = std::getenv("CC_TEST_VARIABLE");
    REQUIRE(value != nullptr);
    REQUIRE(std::string(value) == "the value");

    ::unsetenv("CC_TEST_VARIABLE");
}

TEST_CASE("the tag of the context window is taken off the name of the model", "[model]") {
    SECTION("the tag of a million tokens") {
        REQUIRE(query::model_for_api("MiniMax-M3.1-Flash[1m]") == "MiniMax-M3.1-Flash");
    }

    SECTION("the tag of two million tokens") {
        REQUIRE(query::model_for_api("claude-opus-4-6[2m]") == "claude-opus-4-6");
    }

    SECTION("a name without a tag is left alone") {
        REQUIRE(query::model_for_api("claude-sonnet-4-6") == "claude-sonnet-4-6");
    }

    SECTION("the tag is taken off wherever it sits") {
        REQUIRE(query::model_for_api("[1m]claude-sonnet-4-6") == "claude-sonnet-4-6");
    }

    SECTION("the case of the tag does not matter") {
        REQUIRE(query::model_for_api("claude-sonnet-4-6[1M]") == "claude-sonnet-4-6");
    }
}

TEST_CASE("the tag of a million tokens is recognised", "[model]") {
    REQUIRE(query::has_1m_context("MiniMax-M3.1-Flash[1m]"));
    REQUIRE(query::has_1m_context("claude-sonnet-4-6[1M]"));
    REQUIRE_FALSE(query::has_1m_context("claude-sonnet-4-6"));
    REQUIRE_FALSE(query::has_1m_context("claude-opus-4-6[2m]"));
}

TEST_CASE("only the safe variables may be set before the folder is trusted", "[environment]") {
    // The variables of the behaviour of Claude Code itself are safe.
    REQUIRE(config::is_safe_env_var("ANTHROPIC_MODEL"));
    REQUIRE(config::is_safe_env_var("ANTHROPIC_CUSTOM_HEADERS"));
    REQUIRE(config::is_safe_env_var("DISABLE_TELEMETRY"));

    // The variables that could send the session somewhere else are not.
    REQUIRE_FALSE(config::is_safe_env_var("ANTHROPIC_BASE_URL"));
    REQUIRE_FALSE(config::is_safe_env_var("ANTHROPIC_API_KEY"));
    REQUIRE_FALSE(config::is_safe_env_var("ANTHROPIC_AUTH_TOKEN"));
    REQUIRE_FALSE(config::is_safe_env_var("HTTPS_PROXY"));
    REQUIRE_FALSE(config::is_safe_env_var("NODE_EXTRA_CA_CERTS"));

    // The name of a variable is not case sensitive.
    REQUIRE(config::is_safe_env_var("anthropic_model"));
}

TEST_CASE("the days of the calendar follow one another", "[time]") {
    REQUIRE(cc::next_day("2026-11-04") == "2026-11-05");
    REQUIRE(cc::next_day("2026-11-30") == "2026-12-01");
    REQUIRE(cc::next_day("2026-12-31") == "2027-01-01");
    REQUIRE(cc::next_day("2028-02-28") == "2028-02-29");

    REQUIRE(cc::previous_day("2026-11-05") == "2026-11-04");
    REQUIRE(cc::previous_day("2026-03-01") == "2026-02-28");
    REQUIRE(cc::previous_day("2027-01-01") == "2026-12-31");
    REQUIRE(cc::previous_day("2028-03-01") == "2028-02-29");

    // The days of a day are counted from the epoch.
    REQUIRE(cc::parse_day("1970-01-01") == 0);
    REQUIRE(cc::parse_day("1970-01-02") == 24 * 60 * 60 * 1000);
    REQUIRE(cc::parse_day("not a day") == 0);
}

TEST_CASE("the message of the API is shown next to the status of the call", "[errors]") {
    SECTION("the body of the answer holds the message") {
        const ai::error::ApiCallError error(
            "API call failed with status 400", "", 400,
            R"({"type":"error","error":{"type":"invalid_request_error","message":"unknown model"}})",
            {}, false);
        REQUIRE(query::api_error_message(error) == "API Error: 400 unknown model");
    }

    SECTION("the message is one level deeper in the body") {
        const ai::error::ApiCallError error(
            "API call failed with status 400", "", 400,
            R"({"error":{"error":{"message":"bad request"}}})", {}, false);
        REQUIRE(query::api_error_message(error) == "API Error: 400 bad request");
    }

    SECTION("a body without a message falls back to the error itself") {
        const ai::error::ApiCallError error(
            "API call failed with status 500", "", 500, "", {}, false);
        REQUIRE(query::api_error_message(error) == error.what());
    }

    SECTION("an error that is not from the API is left alone") {
        const std::runtime_error error("something else");
        REQUIRE(query::api_error_message(error) == "something else");
    }
}
