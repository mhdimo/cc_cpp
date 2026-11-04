#include "config/credential_helpers.hpp"

#include "utils/path.hpp"
#include "utils/process.hpp"
#include "utils/string.hpp"

namespace cc::config {
namespace {

/// The number of seconds the command of `apiKeyHelper` is given.
constexpr int kApiKeyHelperTimeoutSeconds = 10 * 60;

/// The number of seconds the command of `awsAuthRefresh` is given.
constexpr int kAwsAuthRefreshTimeoutSeconds = 3 * 60;

/// The number of seconds the script of `awsCredentialExport` is given.
constexpr int kAwsCredentialExportTimeoutSeconds = 60;

/// The reason a command failed.
std::string failure(const cc::ProcessOutcome& outcome) {
    std::string why;
    if (outcome.timed_out) {
        why = "timed out";
    } else if (outcome.failed) {
        why = "could not be started";
    } else {
        why = "exited " + std::to_string(outcome.exit_code);
    }

    const std::string err = cc::trim(outcome.err);
    if (!err.empty()) why += ": " + err;
    return why;
}

} // namespace

std::optional<std::string> run_api_key_helper(const std::string& command, std::string* error) {
    if (command.empty()) return std::nullopt;

    const cc::ProcessOutcome outcome =
        cc::run_process(command, cc::cwd(), "", kApiKeyHelperTimeoutSeconds);
    if (outcome.failed || outcome.timed_out || outcome.exit_code != 0) {
        if (error) *error = failure(outcome);
        return std::nullopt;
    }

    const std::string value = cc::trim(outcome.out);
    if (value.empty()) {
        if (error) *error = "did not return a value";
        return std::nullopt;
    }
    return value;
}

bool run_aws_auth_refresh(const std::string& command) {
    // A command that is not configured is a success, the way the reference
    // treats it.
    if (command.empty()) return true;

    const cc::ProcessOutcome outcome =
        cc::run_process(command, cc::cwd(), "", kAwsAuthRefreshTimeoutSeconds);
    return !outcome.failed && !outcome.timed_out && outcome.exit_code == 0;
}

std::optional<std::string> run_aws_credential_export(const std::string& command,
                                                      std::string* error) {
    if (command.empty()) return std::nullopt;

    const cc::ProcessOutcome outcome =
        cc::run_process(command, cc::cwd(), "", kAwsCredentialExportTimeoutSeconds);
    if (outcome.failed || outcome.timed_out || outcome.exit_code != 0) {
        if (error) *error = failure(outcome);
        return std::nullopt;
    }

    const std::string value = cc::trim(outcome.out);
    if (value.empty()) {
        if (error) *error = "did not return a value";
        return std::nullopt;
    }
    return value;
}

} // namespace cc::config
