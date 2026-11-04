#pragma once

#include <optional>
#include <string>
#include <vector>

namespace cc::config {

/// The root of the repository `cwd` belongs to.
///
/// The walk starts at `cwd` and goes up until a directory holds a `.git`
/// entry — a directory for a plain repository, a file for a worktree or a
/// submodule. Nothing is returned when `cwd` is not in a repository.
std::optional<std::string> find_git_root(const std::string& cwd);

/// The folders that belong to the project of `cwd`.
///
/// The walk starts at `cwd` and stops at the topmost folder that holds a
/// `.git` directory; when no folder on the way holds one, the walk stops at
/// the filesystem root.
///
/// The result is ordered from the root of the repository downwards to
/// `cwd`, which is the order the reference loads the files of a folder in:
/// the file nearest the working directory is read last, so it has the most
/// weight.
std::vector<std::string> project_directories(const std::string& cwd);

/// The root of the repository, with the worktrees resolved.
///
/// A worktree holds a `.git` file that names the directory of the
/// repository it belongs to; the root of the main repository is returned
/// then, so that every worktree of a repository shares one project.
std::optional<std::string> find_canonical_git_root(const std::string& cwd);

/// The key a project is stored under in the global configuration file.
///
/// The reference keys the projects on the root of the repository, so that a
/// folder inside a repository and the repository itself share one bucket.
std::string project_config_key(const std::string& cwd);

} // namespace cc::config
