#include "feedback.hpp"

#include "utils/json_util.hpp"
#include "utils/string.hpp"

#include <ai/http/client.hpp>

#include <chrono>
#include <ctime>
#include <exception>
#include <regex>

namespace cc::feedback {
namespace {

/// The pattern of a key of the API.
const std::regex& api_key_pattern() {
    static const std::regex pattern("(sk-ant-[A-Za-z0-9_-]+)");
    return pattern;
}

/// The pattern of a token of an account.
const std::regex& bearer_pattern() {
    static const std::regex pattern("(Bearer\\s+\\S+)");
    return pattern;
}

/// The moment the report is sent, in the format the API expects.
std::string now_iso() {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm parts{};
    gmtime_r(&seconds, &parts);

    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &parts);
    return std::string(stamp) + ".000Z";
}

} // namespace

std::string redact(const std::string& text) {
    std::string out = std::regex_replace(text, api_key_pattern(), "[REDACTED_API_KEY]");
    out = std::regex_replace(out, bearer_pattern(), "Bearer [REDACTED]");
    return out;
}

std::string submit(const std::string& api_key, const std::string& access_token,
                   const Report& report) {
    boost::json::object data;
    data["description"] = boost::json::value(report.description);
    data["message_count"] = boost::json::value(static_cast<int64_t>(report.message_count));
    data["datetime"] = boost::json::value(now_iso());
    data["platform"] = boost::json::value(std::string("darwin"));
    data["version"] = boost::json::value(report.version);
    data["gitRepo"] = boost::json::value(false);

    boost::json::array transcript;
    for (const boost::json::value& message : report.transcript) transcript.push_back(message);
    data["transcript"] = boost::json::value(std::move(transcript));

    boost::json::object body;
    body["content"] = boost::json::value(cc::json_dump(data));

    ai::http::Headers headers;
    headers["Content-Type"] = "application/json";
    headers["User-Agent"] = "Claude-Code-CLI";
    if (!api_key.empty()) {
        headers["x-api-key"] = api_key;
    } else if (!access_token.empty()) {
        headers["Authorization"] = "Bearer " + access_token;
        headers["anthropic-beta"] = "oauth-2025-04-20";
    } else {
        return {};
    }

    boost::asio::io_context context;
    ai::http::HttpClient client(context);

    auto task = client.post_json(std::string(kEndpoint), boost::json::value(body), headers);
    task.start();
    while (!task.done()) {
        context.run_one_for(std::chrono::milliseconds(20));
    }

    try {
        const ai::http::HttpResponse response = task.get();
        if (response.status_code != 200) return {};

        const boost::json::value parsed = cc::json_parse(response.body);
        if (!parsed.is_object()) return {};
        return jget_string(parsed.as_object(), "feedback_id");
    } catch (const std::exception&) {
        return {};
    }
}

std::string github_issue_url(const std::string& feedback_id, const std::string& title,
                             const std::string& description, const std::string& version) {
    std::string body = "**Bug Description**\n" + redact(description) + "\n\n";
    body += "**Environment Info**\n";
    body += "- Version: " + (version.empty() ? std::string("unknown") : version) + "\n";
    body += "- Feedback ID: " + feedback_id + "\n";

    std::string address = std::string(kIssuesUrl) + "/new?title=";
    // The title and the body are put in the address as they are; the address
    // is opened by the browser, which takes care of the rest.
    address += cc::escape_json(title);
    address += "&body=";
    address += cc::escape_json(body);
    return address;
}

} // namespace cc::feedback
