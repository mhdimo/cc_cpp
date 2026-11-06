#pragma once

#include <optional>
#include <string>
#include <vector>

namespace cc::git {

/// One file that changed in the working tree.
struct ChangedFile {
    /// The path, relative to the repository root.
    std::string path;
    /// True when the file is not in the repository yet.
    bool untracked = false;
    /// The number of lines that were added and removed.
    int added = 0;
    int removed = 0;
    /// The diff of the file, as git produced it.
    std::string diff;
    /// True when the file is binary and cannot be shown.
    bool binary = false;
    /// True when the diff of the file is bigger than the 1 MB limit.
    bool large = false;
    /// True when the diff of the file was cut short at the line limit.
    bool truncated = false;
};

/// The files that changed in the working tree of `cwd`.
///
/// The paths are relative to the root of the repository, the way `git diff`
/// reports them. Untracked files are included, with the `untracked` flag set
/// and no diff.
///
/// Returns an empty list when `cwd` is not in a repository, or when git is
/// not installed.
std::vector<ChangedFile> changed_files(const std::string& cwd);

/// The branch the changes of the session are compared against.
///
/// The reference asks git for the branch the head of the remote points at
/// and falls back to `main`. The name is empty when `cwd` is not in a
/// repository.
std::string default_branch(const std::string& cwd);

/// The state of the review of a pull request.
enum class PrReviewState {
    Draft,             ///< The pull request is a draft.
    Approved,          ///< The pull request is approved.
    ChangesRequested,  ///< The pull request asks for changes.
    Pending,           ///< Nobody has looked at the pull request yet.
};

/// The pull request of the branch of the working tree.
struct PrStatus {
    /// The number of the pull request.
    int number = 0;

    /// The address of the pull request.
    std::string url;

    /// The state of the review.
    PrReviewState review = PrReviewState::Pending;
};

/// The pull request of the branch of `cwd`.
///
/// The reference asks the command of GitHub, `gh pr view`, and says nothing
/// when the command is not there or when the branch has no pull request.
/// The pull request of the branch of the main is left out as well: the
/// command answers with the pull request that was merged last.
std::optional<PrStatus> pr_status(const std::string& cwd);

/// The branch that is checked out in `cwd`.
///
/// The reference asks git for it with `rev-parse --abbrev-ref HEAD`; the
/// name is empty when `cwd` is not in a repository, or when git is not
/// installed.
std::string current_branch(const std::string& cwd);

/// The number of files that changed in the working tree of `cwd`.
///
/// The number is asked for without the diffs, the way the reference asks
/// git for the totals first: a diff of hundreds of files is not read at
/// all. Returns zero when `cwd` is not in a repository, or when git is
/// not installed.
int changed_file_count(const std::string& cwd);

} // namespace cc::git
