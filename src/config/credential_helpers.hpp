#pragma once

#include <optional>
#include <string>

namespace cc::config {

/// Runs the script of `apiKeyHelper` and returns the key it wrote.
///
/// The reference runs the command through the shell and takes what it wrote
/// to its standard output; a command that fails, or that writes nothing, is
/// an error. The command is given ten minutes, the way the reference gives
/// it.
///
/// The reason of the failure is written to `error` when there is one.
std::optional<std::string> run_api_key_helper(const std::string& command,
                                              std::string* error = nullptr);

/// Runs the command of `awsAuthRefresh`.
///
/// The reference gives the command three minutes. A command that is not
/// configured is a success, the way the reference treats it.
bool run_aws_auth_refresh(const std::string& command);

/// Runs the script of `awsCredentialExport` and returns what it wrote.
std::optional<std::string> run_aws_credential_export(const std::string& command,
                                                      std::string* error = nullptr);

} // namespace cc::config
