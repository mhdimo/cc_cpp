#pragma once

#include <exception>
#include <string>

namespace cc::query {

/// The message of a call to the API that failed.
///
/// The reference shows the answer of the API next to the status of the
/// call — `API Error: 400 <what the API said>` — so the port reads the
/// message out of the body the API sent, the way the reference's SDK does.
std::string api_error_message(const std::exception& error);

} // namespace cc::query
