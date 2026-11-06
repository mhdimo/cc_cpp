#include <catch2/catch_test_macros.hpp>

#include "feedback.hpp"

#include <string>

using namespace cc;

TEST_CASE("The secrets of a report are taken out") {
    const std::string text = "the key is sk-ant-api03-abcdefghijklmnop and the token is "
                             "Bearer sk-ant-oat01-1234567890";
    const std::string redacted = feedback::redact(text);

    REQUIRE(redacted.find("sk-ant-api03-abcdefghijklmnop") == std::string::npos);
    REQUIRE(redacted.find("[REDACTED_API_KEY]") != std::string::npos);
    REQUIRE(redacted.find("Bearer [REDACTED]") != std::string::npos);
}

TEST_CASE("The address of the issue holds the report") {
    const std::string address =
        feedback::github_issue_url("abc-123", "A title", "Something happened", "0.1.0");

    REQUIRE(address.find("https://github.com/anthropics/claude-code/issues/new") == 0);
    REQUIRE(address.find("A title") != std::string::npos);
    REQUIRE(address.find("Something happened") != std::string::npos);
    REQUIRE(address.find("abc-123") != std::string::npos);
    REQUIRE(address.find("0.1.0") != std::string::npos);
}

TEST_CASE("A report without a key is not sent") {
    feedback::Report report;
    report.description = "Something happened";

    // There is neither a key nor a token, so the report goes nowhere.
    REQUIRE(feedback::submit("", "", report).empty());
}
