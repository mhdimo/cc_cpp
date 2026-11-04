#pragma once

#include <optional>
#include <string>
#include <vector>

namespace cc::config {

/// A CLAUDE.md file that contributes to the system prompt.
struct MemoryFile {
    /// The path the file was read from.
    std::string path;
    /// The file's contents.
    std::string contents;
    /// The directory the file applies to.
    std::string directory;
    /// True when the file is the user's global memory file.
    bool is_global = false;

    /// The file that pulled this one in, when it was pulled in.
    ///
    /// The file that names it with an `@path` import is the parent; the
    /// string is empty for the files that are read on their own.
    std::string parent;
};

/// The memory files Claude Code loads for a session.
///
/// These are, in the order they are added to the system prompt:
///
///  1. The user memory file (`~/.claude/CLAUDE.md`) and the rules of the
///     user (`~/.claude/rules/*.md`).
///  2. The project files, from the working directory up to the repository
///     root: `CLAUDE.md`, `.claude/CLAUDE.md` and the rules of the folder
///     (`.claude/rules/*.md`) in each.
///  3. The files referenced by `@path` imports in those files.
///
/// The files that a project pulls in from outside its folder are only read
/// when `include_external` is true: the reference asks the user about them
/// and reads them only when the user says yes. The memory of the user is
/// always read whole, the way the reference reads it.
std::vector<MemoryFile> load_memory_files(const std::string& cwd, bool include_external = true);

/// The memory files that the project pulls in from outside its folder.
///
/// The reference leaves the files of the user alone: only the files that a
/// project pulls in with an `@path` and that lie outside the folder of the
/// session are external. The paths of the files are returned.
std::vector<std::string> external_includes(const std::string& cwd);

/// The contents of the user memory file, if it exists.
///
/// The file lives in the configuration directory of Claude Code
/// (`~/.claude/CLAUDE.md`).
std::optional<std::string> user_memory();

} // namespace cc::config
