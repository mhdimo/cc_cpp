#include "config/project_key.hpp"

#include "utils/fs.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <filesystem>

namespace fs = std::filesystem;

namespace cc::config {
namespace {

/// The path of a file, with the symbolic links resolved.
///
/// The reference resolves the links before it compares the paths of a
/// worktree with the ones of its repository; a path that cannot be
/// resolved is returned as it is.
std::string canonical_path(const std::string& path) {
    std::error_code ec;
    const fs::path resolved = fs::canonical(fs::path(path), ec);
    if (ec) return path;
    return resolved.string();
}

/// The root of the repository a worktree belongs to.
///
/// The `.git` file of a worktree names the directory of the repository;
/// the `commondir` file of that directory names the directory the
/// repository shares with its worktrees. The chain is only followed when
/// it looks like the one `git worktree add` writes — the directory of the
/// worktree lies in the `worktrees` of the common directory, and the
/// common directory points back at the `.git` of the worktree. A
/// repository that was downloaded cannot borrow the trust of another one
/// that way.
std::string resolve_canonical_root(const std::string& git_root) {
    // In a worktree the `.git` is a file that names the directory of the
    // repository; in a plain repository it is a directory, and reading it
    // fails.
    const auto git_file = cc::read_file(cc::path_join(git_root, ".git"));
    if (!git_file) return git_root;

    const std::string content = cc::trim(*git_file);
    const std::string kGitDirPrefix = "gitdir:";
    if (!cc::starts_with(content, kGitDirPrefix)) return git_root;

    const std::string worktree_git_dir = cc::resolve(
        cc::trim(content.substr(kGitDirPrefix.size())), git_root);

    // A submodule has no `commondir`; it is a repository of its own.
    const auto common = cc::read_file(cc::path_join(worktree_git_dir, "commondir"));
    if (!common) return git_root;

    const std::string common_dir =
        cc::resolve(cc::trim(*common), worktree_git_dir);

    if (cc::dirname(worktree_git_dir) != cc::path_join(common_dir, "worktrees")) {
        return git_root;
    }

    const auto backlink = cc::read_file(cc::path_join(worktree_git_dir, "gitdir"));
    if (!backlink) return git_root;
    // The directory is resolved, and not the `.git` file itself: git writes
    // the link with the links resolved, and a `.git` that is a symbolic
    // link must not be followed.
    if (canonical_path(cc::trim(*backlink)) != cc::path_join(canonical_path(git_root), ".git")) {
        return git_root;
    }

    // The common directory of a bare repository does not lie in a working
    // directory; it is then the identity of the repository itself.
    if (cc::basename(common_dir) != ".git") return common_dir;
    return cc::dirname(common_dir);
}

} // namespace

std::optional<std::string> find_git_root(const std::string& cwd) {
    std::string current = cc::resolve(cwd);
    while (!current.empty()) {
        if (cc::file_exists(cc::path_join(current, ".git"))) return current;
        const std::string parent = cc::dirname(current);
        if (parent.empty() || parent == current) break;
        current = parent;
    }
    return std::nullopt;
}

std::vector<std::string> project_directories(const std::string& cwd) {
    std::vector<std::string> ancestors;
    std::string current = cc::resolve(cwd);
    while (!current.empty() && current != "/") {
        ancestors.push_back(current);
        const std::string parent = cc::dirname(current);
        if (parent == current) break;
        current = parent;
    }
    if (ancestors.empty()) return {};

    // The root of the repository is the topmost folder with a `.git`
    // directory, so the search starts at the outermost folder and walks
    // inwards.
    size_t root = ancestors.size() - 1;
    for (size_t i = ancestors.size(); i-- > 0;) {
        if (cc::is_directory(cc::path_join(ancestors[i], ".git"))) {
            root = i;
            break;
        }
    }

    // Keep the folders from `cwd` up to the root of the repository, then
    // reverse them so the root comes first.
    ancestors.resize(root + 1);
    std::reverse(ancestors.begin(), ancestors.end());
    return ancestors;
}

std::optional<std::string> find_canonical_git_root(const std::string& cwd) {
    const auto root = find_git_root(cwd);
    if (!root) return std::nullopt;
    return resolve_canonical_root(*root);
}

std::string project_config_key(const std::string& cwd) {
    if (const auto root = find_canonical_git_root(cwd)) {
        return cc::normalize_path_for_config_key(*root);
    }
    return cc::normalize_path_for_config_key(cc::resolve(cwd));
}

} // namespace cc::config
