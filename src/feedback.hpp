#pragma once

#include <boost/json.hpp>

#include <string>
#include <vector>

namespace cc::feedback {

/// The report the user sends.
///
/// The report holds what the user wrote and the conversation it was written
/// about, the way the reference sends it.
struct Report {
    /// The description the user wrote.
    std::string description;

    /// The number of the messages of the session.
    size_t message_count = 0;

    /// The conversation, as the API expects it.
    std::vector<boost::json::value> transcript;

    /// The version of the CLI.
    std::string version;
};

/// The address the reports are sent to.
inline constexpr std::string_view kEndpoint =
    "https://api.anthropic.com/api/claude_cli_feedback";

/// The address of the issues of the CLI.
inline constexpr std::string_view kIssuesUrl = "https://github.com/anthropics/claude-code/issues";

/// Sends the report to the API.
///
/// The id of the report is returned; an empty string means the report could
/// not be sent. The key is the one of the environment, or the token of the
/// account of the user.
std::string submit(const std::string& api_key, const std::string& access_token,
                   const Report& report);

/// The address of the issue on GitHub, with the report in it.
///
/// The address is the one the reference builds, so that the user may open
/// the issue in the browser.
std::string github_issue_url(const std::string& feedback_id, const std::string& title,
                             const std::string& description, const std::string& version);

/// Hides the secrets of a text.
///
/// The keys of the API and the tokens are taken out, the way the reference
/// takes them out of a report.
std::string redact(const std::string& text);

} // namespace cc::feedback
