#pragma once

#include <map>
#include <string>

namespace cc::config {

/// Tells whether a variable of the environment may be set before the folder
/// is trusted.
///
/// The reference keeps a list of the variables that are safe: they name
/// nothing but the behaviour of Claude Code itself, so a folder that is not
/// trusted yet cannot use them to send the session somewhere else. Every
/// other variable — the address of the API, the key, the proxies — waits for
/// the trust.
bool is_safe_env_var(const std::string& key);

/// Sets the variables in the environment of the process.
///
/// The reference does the same when it applies the `env` block of the
/// settings: the session then talks to the address the user wrote, and the
/// tools it runs inherit the variables.
void apply_env(const std::map<std::string, std::string>& variables);

} // namespace cc::config
