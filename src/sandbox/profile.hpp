#pragma once

#include "sandbox/manager.hpp"

#include <string>

namespace cc::sandbox {

/// The profile of the seatbelt that holds a command in the sandbox.
///
/// The reference hands its settings to the runtime of Anthropic, which is a
/// package of its own; the port writes the profile of the seatbelt of macOS
/// itself, which is what the runtime uses on that system.
std::string seatbelt_profile(const Manager& manager, const std::string& working_dir);

/// The command that runs `command` in the sandbox.
///
/// The command is empty when the sandbox cannot hold it on this machine.
std::string sandboxed_command(const Manager& manager, const std::string& command,
                              const std::string& working_dir);

} // namespace cc::sandbox
