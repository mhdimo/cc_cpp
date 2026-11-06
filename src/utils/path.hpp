#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cc {

/// The user's home directory (`$HOME`).
std::string home_dir();

/// The current working directory.
std::string cwd();

/// The directory Claude Code keeps its per-user state in (`~/.claude`).
std::string claude_config_dir();

/// Remembers the path the program was started with.
///
/// The path is the one of `argv[0]`, resolved against the working
/// directory when it is not a path of its own.
void set_program_path(const std::string& path);

/// The path of the program, when it is known.
///
/// The port looks for the files that came with it — the changelog, for one
/// — beside itself.
std::string program_path();

/// The directory the program lives in, when it is known.
std::string program_directory();

/// Joins two path components with a single separator.
std::string path_join(std::string_view a, std::string_view b);

/// The directory portion of a path.
std::string dirname(std::string_view path);

/// The final component of a path.
std::string basename(std::string_view path);

/// The final component of a path without its extension.
std::string stem(std::string_view path);

/// The extension of a path, without the dot.
std::string extension(std::string_view path);

/// Collapses `.` and `..` segments and duplicate separators. Does not touch
/// the filesystem, so it works for paths that do not exist.
std::string normalize(std::string_view path);

/// Normalises a path the way the keys of the global configuration are
/// written.
///
/// The reference writes the separators as `/`, so that the same folder has
/// the same key on every platform.
std::string normalize_path_for_config_key(std::string_view path);

/// Makes `path` absolute relative to `base` (the cwd by default) and
/// normalizes it.
std::string resolve(std::string_view path, std::string_view base = "");

/// Expresses `path` relative to `base`, or returns `path` unchanged when it
/// is not inside `base`.
std::string relative(std::string_view path, std::string_view base);

/// Expresses `path` relative to `base`, the way Node writes it.
///
/// A path that lies outside of `base` is written with the `..` of the way
/// back to it.
std::string relative_from(std::string_view base, std::string_view path);

/// True when `path` is inside `base` (or equal to it).
bool is_inside(std::string_view path, std::string_view base);

/// Replaces the extension of `path`.
std::string with_extension(std::string_view path, std::string_view ext);

/// Splits a path into its components.
std::vector<std::string> path_components(std::string_view path);

/// True when the path is absolute.
bool is_absolute(std::string_view path);

/// Shortens a path for display by replacing the home directory with `~`.
std::string tilde_path(std::string_view path);

/// Returns a short, human-friendly label for a path: the basename, plus the
/// parent directory when the basename is not descriptive.
std::string path_label(std::string_view path);

/// True when the path looks like a URL (has a scheme followed by `://`).
bool is_url(std::string_view path);

} // namespace cc
