#pragma once

#include <string>

namespace cc::ui {

/// The escape that puts `text` in the clipboard of the terminals.
///
/// The text is written in the base64 of the standard, the way the terminals
/// take it.
std::string clipboard_sequence(const std::string& text);

/// Puts `text` in the clipboard of the machine.
///
/// The reference writes the escape of the clipboard of the terminals, and it
/// copies the text with the tool of the system as well; the tool of the
/// system is not used over a connection of a shell, the way the reference
/// does not use it there.
void copy_to_clipboard(const std::string& text);

} // namespace cc::ui
