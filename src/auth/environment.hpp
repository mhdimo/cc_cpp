#pragma once

#include <string>
#include <vector>

namespace cc::auth {

/// The address of the API the session talks to.
///
/// The reference reads the address from `ANTHROPIC_BASE_URL`, and falls back
/// to the address of Anthropic when the variable is not set. A gateway that
/// speaks the API of Anthropic is reached the same way.
std::string base_api_url();

/// The headers the session sends in addition to the ones of the API.
///
/// The reference reads them from `ANTHROPIC_CUSTOM_HEADERS`, one header per
/// line, in the shape `Name: Value`.
std::vector<std::pair<std::string, std::string>> custom_headers();

/// The model of the environment, when there is one.
///
/// The reference reads the model from `ANTHROPIC_MODEL` before it reads the
/// settings of the user.
std::string environment_model();

/// The token of the environment, when there is one.
///
/// The reference sends `ANTHROPIC_AUTH_TOKEN` as a bearer token; a gateway
/// that speaks the API of Anthropic is reached that way.
std::string auth_token();

/// The key of the API that is in the environment, cut short.
///
/// The reference keeps the last twenty characters of the key, so that the
/// user can tell which key it is without the whole of it being written
/// down. The string is empty when the environment holds no key.
std::string custom_api_key();

/// True when the session runs in the internal cloud of Anthropic.
///
/// The reference keeps the key of the API out of the questions there: the
/// environment holds the key for the child processes, but the session itself
/// does not use it.
bool running_on_homespace();

} // namespace cc::auth
