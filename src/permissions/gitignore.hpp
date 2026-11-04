#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cc::permissions {

/// True when the pattern of a gitignore rule matches the path.
///
/// The rules of a `.gitignore` are read: `*` matches any text but the
/// separator of the folders, `**` matches the separator too, `?` matches one
/// character, and a pattern that begins with `!` takes the file back out of
/// the match.
///
/// A pattern that names a folder matches everything inside it.
bool gitignore_matches(std::string_view pattern, std::string_view path);

/// The pattern of the rule that matches the path.
///
/// The patterns are read in the order they are given and the last one that
/// matches decides, the way the rules of a `.gitignore` do; a pattern that
/// begins with `!` takes the file back out of the match.
std::optional<std::string> gitignore_match(const std::vector<std::string>& patterns,
                                            std::string_view path);

} // namespace cc::permissions
