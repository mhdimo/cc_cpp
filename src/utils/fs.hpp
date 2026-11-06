#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cc {

/// A snapshot of a file's identity, used to detect changes between reads.
struct FileState {
    bool exists = false;
    int64_t size = 0;
    int64_t mtime_ms = 0;

    bool operator==(const FileState& other) const {
        return exists == other.exists && size == other.size && mtime_ms == other.mtime_ms;
    }
    bool operator!=(const FileState& other) const { return !(*this == other); }
};

/// Reads a file's current state (size and modification time).
FileState file_state(const std::string& path);

/// True when the path exists (following symlinks).
bool file_exists(const std::string& path);

/// True when the path is a regular file.
bool is_file(const std::string& path);

/// True when the path is a directory.
bool is_directory(const std::string& path);

/// Reads a whole file. Returns `std::nullopt` when it cannot be read.
std::optional<std::string> read_file(const std::string& path);

/// Reads a whole file as bytes.
std::optional<std::vector<uint8_t>> read_file_bytes(const std::string& path);

/// Writes a file, creating parent directories as needed. Returns false on
/// failure and fills `error` when provided.
bool write_file(const std::string& path, std::string_view contents,
                std::string* error = nullptr);

/// Appends to a file, creating it (and its parents) when missing.
bool append_file(const std::string& path, std::string_view contents,
                 std::string* error = nullptr);

/// Creates a directory and all missing parents.
bool mkdirp(const std::string& path, std::string* error = nullptr);

/// Lists the entries of a directory, excluding `.` and `..`. The result is
/// sorted by name.
std::vector<std::string> list_dir(const std::string& path);

/// Removes a file. Returns true when the file is gone afterwards.
bool remove_file(const std::string& path);

/// Removes a directory and its contents.
bool remove_dir_all(const std::string& path);

/// Recursively copies a file or directory tree.
bool copy_recursive(const std::string& from, const std::string& to,
                    std::string* error = nullptr);

/// Moves a file or directory.
bool move_path(const std::string& from, const std::string& to,
               std::string* error = nullptr);

/// The number of lines in a file, or 0 when it cannot be read.
size_t count_file_lines(const std::string& path);

/// Matches a path against a glob pattern.
///
/// Supports `*` (any run of characters except `/`), `?` (one character
/// except `/`), `**` (any run of characters including `/`), `[abc]` and
/// `[a-z]` character classes, and `{a,b}` alternatives.
bool glob_match(std::string_view path, std::string_view pattern);

/// Expands a glob pattern into the matching paths under `root`.
///
/// `pattern` may be absolute or relative to `root`. Results are sorted and
/// deduplicated. `limit` caps the number of results; 0 means unlimited.
std::vector<std::string> glob(const std::string& pattern, const std::string& root = "",
                              size_t limit = 0);

/// One line of a grep match.
struct GrepMatch {
    std::string file;
    int line = 0;             ///< 1-based line number
    std::string text;         ///< the full line, without the terminator
    std::string match;        ///< the matched substring
    int column = 0;           ///< 0-based column of the match
};

/// Searches files for a regular expression.
///
/// `pattern` is an ECMAScript regular expression. `glob_filter` restricts the
/// search to files matching a glob (for example `*.ts`). `limit` caps the
/// number of matches; 0 means unlimited.
std::vector<GrepMatch> grep(const std::string& pattern, const std::string& root,
                             std::string_view glob_filter = "", size_t limit = 0,
                             bool case_insensitive = false);

/// Searches files for a literal string, which is faster and needs no
/// escaping.
std::vector<GrepMatch> grep_literal(const std::string& needle, const std::string& root,
                                     std::string_view glob_filter = "", size_t limit = 0,
                                     bool case_insensitive = false);

/// Recursively collects the files under `root`, skipping directories that
/// are conventionally ignored (`.git`, `node_modules`, and the like).
std::vector<std::string> walk_files(const std::string& root, size_t limit = 0);

/// Names that are skipped when walking a directory tree.
bool is_ignored_dir_name(std::string_view name);

} // namespace cc
