#include "utils/git.hpp"

#include "utils/json_util.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace cc::git {
namespace {

/// How long a git command may run before it is killed.
constexpr int kGitTimeoutMs = 5000;

/// The largest diff the port keeps for one file.
constexpr size_t kMaxDiffBytes = 1'000'000;

/// The number of diff lines the port keeps for one file.
constexpr int kMaxLinesPerFile = 400;

/// The result of running a command.
struct CommandResult {
    /// What the command wrote to stdout.
    std::string output;
    /// The command's exit code.
    int exit_code = 0;
    /// True when the command could not be run at all.
    bool failed = false;
};

/// Runs `argv` in `cwd` and returns what it wrote to stdout.
///
/// The command's stderr is dropped, so that git's warnings do not end up in
/// the output. The command is killed when it takes longer than the timeout.
CommandResult run(const std::vector<std::string>& argv, const std::string& cwd) {
    CommandResult result;

    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        result.failed = true;
        result.exit_code = 127;
        return result;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        result.failed = true;
        result.exit_code = 127;
        return result;
    }

    if (pid == 0) {
        // The child leads a process group of its own, so that the timeout can
        // take the whole tree down.
        ::setpgid(0, 0);
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::close(fds[1]);

        const int null_fd = ::open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            ::dup2(null_fd, STDERR_FILENO);
            ::close(null_fd);
        }

        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) ::_exit(127);

        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const auto& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
        args.push_back(nullptr);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }

    ::close(fds[1]);

    const auto start = std::chrono::steady_clock::now();
    bool timed_out = false;
    char buffer[4096];
    for (;;) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        if (elapsed >= kGitTimeoutMs) {
            timed_out = true;
            break;
        }

        struct pollfd pfd {};
        pfd.fd = fds[0];
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, 50);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;

        const ssize_t got = ::read(fds[0], buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) break;
        result.output.append(buffer, static_cast<size_t>(got));
    }

    if (timed_out) {
        ::kill(-pid, SIGKILL);
        ::kill(pid, SIGKILL);
        // Take whatever the command wrote before it was killed.
        for (;;) {
            const ssize_t got = ::read(fds[0], buffer, sizeof(buffer));
            if (got <= 0) break;
            result.output.append(buffer, static_cast<size_t>(got));
        }
    }

    ::close(fds[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    if (timed_out) {
        result.exit_code = 124;
    } else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}

/// Runs `git` with the given arguments in `cwd`.
///
/// The optional locks are left alone, so that looking at the diff does not
/// stop git from taking the index lock in the background.
CommandResult git(const std::string& cwd, std::vector<std::string> args) {
    args.insert(args.begin(), "git");
    args.insert(args.begin() + 1, "--no-optional-locks");
    return run(args, cwd);
}

/// Removes the quotes git puts around paths with unusual characters.
std::string unquote(std::string path) {
    if (path.size() < 2 || path.front() != '"' || path.back() != '"') return path;
    path = path.substr(1, path.size() - 2);

    std::string out;
    out.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] != '\\' || i + 1 >= path.size()) {
            out += path[i];
            continue;
        }
        ++i;
        switch (path[i]) {
        case 'n':
            out += '\n';
            break;
        case 't':
            out += '\t';
            break;
        case '"':
            out += '"';
            break;
        case '\\':
            out += '\\';
            break;
        default:
            out += path[i];
            break;
        }
    }
    return out;
}

/// Removes the `a/` or `b/` prefix git puts in front of the paths of a diff.
std::string strip_prefix(std::string path) {
    if (path.size() > 2 && path[1] == '/' && (path[0] == 'a' || path[0] == 'b')) {
        return path.substr(2);
    }
    return path;
}

/// One entry of `git status --porcelain`.
struct StatusEntry {
    std::string path;
    bool untracked = false;
};

/// Parses the output of `git status --porcelain`.
std::vector<StatusEntry> parse_status(const std::string& output) {
    std::vector<StatusEntry> entries;
    for (const auto& line : cc::split_lines(output)) {
        if (line.size() < 4) continue;

        StatusEntry entry;
        entry.untracked = line[0] == '?' && line[1] == '?';

        std::string path = line.substr(3);
        // A rename is reported as `old -> new`; the new name is the file.
        const auto arrow = path.rfind(" -> ");
        if (arrow != std::string::npos) path = path.substr(arrow + 4);
        entry.path = unquote(path);
        if (entry.path.empty()) continue;

        entries.push_back(std::move(entry));
    }
    return entries;
}

/// The line counts of one file.
struct Numstat {
    int added = 0;
    int removed = 0;
    bool binary = false;
};

/// Parses the output of `git diff --numstat`.
std::map<std::string, Numstat> parse_numstat(const std::string& output) {
    std::map<std::string, Numstat> stats;
    for (const auto& line : cc::split_lines(output)) {
        const auto parts = cc::split(line, '\t');
        if (parts.size() < 3) continue;

        Numstat entry;
        entry.binary = parts[0] == "-" || parts[1] == "-";
        if (!entry.binary) {
            entry.added = static_cast<int>(cc::parse_int(parts[0]).value_or(0));
            entry.removed = static_cast<int>(cc::parse_int(parts[1]).value_or(0));
        }

        // The path may contain tabs, so everything after the second one
        // belongs to the path.
        std::string path = parts[2];
        for (size_t i = 3; i < parts.size(); ++i) path += "\t" + parts[i];

        // A rename is reported as `old => new`.
        const auto arrow = path.rfind(" => ");
        if (arrow != std::string::npos) path = path.substr(arrow + 4);
        if (path.size() > 2 && path.front() == '{' && path.back() == '}') {
            path = path.substr(1, path.size() - 2);
        }

        stats[unquote(path)] = entry;
    }
    return stats;
}

/// One file's block of a unified diff.
struct DiffBlock {
    std::string path;
    std::string text;
    bool binary = false;
};

/// The path of the file a block of a unified diff belongs to.
std::string block_path(const std::vector<std::string>& lines) {
    std::string added;
    std::string removed;
    for (const auto& line : lines) {
        if (added.empty() && cc::starts_with(line, "+++ ")) {
            added = cc::trim(line.substr(4));
        } else if (removed.empty() && cc::starts_with(line, "--- ")) {
            removed = cc::trim(line.substr(4));
        }
    }

    // A new file has no `---` line and a deleted one no `+++` line.
    if (!added.empty() && added != "/dev/null") return unquote(strip_prefix(added));
    if (!removed.empty() && removed != "/dev/null") return unquote(strip_prefix(removed));
    return {};
}

/// Parses the output of `git diff` into one block per file.
std::vector<DiffBlock> parse_diff(const std::string& output) {
    std::vector<std::vector<std::string>> raw;
    std::vector<std::string> current;
    for (const auto& line : cc::split_lines(output)) {
        if (cc::starts_with(line, "diff --git ")) {
            if (!current.empty()) raw.push_back(std::move(current));
            current.clear();
        }
        current.push_back(line);
    }
    if (!current.empty()) raw.push_back(std::move(current));

    std::vector<DiffBlock> blocks;
    blocks.reserve(raw.size());
    for (const auto& lines : raw) {
        DiffBlock block;
        block.path = block_path(lines);
        for (const auto& line : lines) {
            if (cc::starts_with(line, "Binary files ") || cc::starts_with(line, "GIT binary patch")) {
                block.binary = true;
            }
            block.text += line;
            block.text += '\n';
        }
        blocks.push_back(std::move(block));
    }
    return blocks;
}

} // namespace

std::vector<ChangedFile> changed_files(const std::string& cwd) {
    std::vector<ChangedFile> files;

    // The root of the repository, so that every path is relative to it.
    const CommandResult root = git(cwd, {"rev-parse", "--show-toplevel"});
    if (root.failed || root.exit_code != 0) return files;
    const std::string repo = cc::trim(root.output);
    if (repo.empty()) return files;

    // The files that changed.
    const CommandResult status = git(repo, {"status", "--porcelain"});
    if (status.failed || status.exit_code != 0) return files;

    // The line counts of the tracked files.
    const CommandResult numstat = git(repo, {"diff", "HEAD", "--numstat"});
    const auto counts =
        numstat.exit_code == 0 ? parse_numstat(numstat.output) : std::map<std::string, Numstat>{};

    // The diffs themselves.
    const CommandResult diff = git(repo, {"diff", "HEAD"});
    const auto blocks =
        diff.exit_code == 0 ? parse_diff(diff.output) : std::vector<DiffBlock>{};

    // The files that are not in the repository yet.
    const CommandResult untracked = git(repo, {"ls-files", "--others", "--exclude-standard"});

    // The files are collected in a map, so that they come out sorted by
    // path, the way the reference sorts them.
    std::map<std::string, ChangedFile> by_path;

    for (const auto& entry : parse_status(status.output)) {
        // The untracked files are listed by `git ls-files` below, which
        // names the files themselves rather than the directory they are in.
        if (entry.untracked) continue;
        ChangedFile file;
        file.path = entry.path;
        by_path.emplace(entry.path, std::move(file));
    }

    for (auto& [path, file] : by_path) {
        const auto found = counts.find(path);
        if (found == counts.end()) continue;
        file.added = found->second.added;
        file.removed = found->second.removed;
        file.binary = found->second.binary;
    }

    for (const auto& block : blocks) {
        const auto found = by_path.find(block.path);
        if (found == by_path.end()) continue;
        ChangedFile& file = found->second;
        if (block.binary) file.binary = true;

        if (block.text.size() > kMaxDiffBytes) {
            file.large = true;
            file.diff.clear();
            continue;
        }

        // The diff itself is kept whole, so that the counts of the change
        // stay right; it is cut short when it is shown.
        file.diff = block.text;
        if (file.added + file.removed > kMaxLinesPerFile) file.truncated = true;
    }

    if (untracked.exit_code == 0) {
        for (const auto& path : cc::split_lines(untracked.output)) {
            if (path.empty()) continue;
            ChangedFile file;
            file.path = path;
            file.untracked = true;
            by_path.emplace(path, std::move(file));
        }
    }

    files.reserve(by_path.size());
    for (auto& [path, file] : by_path) files.push_back(std::move(file));
    return files;
}

std::string default_branch(const std::string& cwd) {
    // The head of the remote names the branch the repository is compared
    // against.
    const CommandResult head = git(cwd, {"symbolic-ref", "--short", "refs/remotes/origin/HEAD"});
    if (!head.failed && head.exit_code == 0) {
        const std::string name = cc::trim(head.output);
        const size_t slash = name.rfind('/');
        if (slash != std::string::npos && slash + 1 < name.size()) return name.substr(slash + 1);
    }

    // The branch that is checked out is the one of the working tree.
    const CommandResult current = git(cwd, {"rev-parse", "--abbrev-ref", "HEAD"});
    if (!current.failed && current.exit_code == 0) {
        const std::string name = cc::trim(current.output);
        if (!name.empty() && name != "HEAD") return name;
    }

    return {};
}

std::optional<PrStatus> pr_status(const std::string& cwd) {
    // The pull request of the branch of the main is left out: the command
    // of GitHub answers with the pull request that was merged last.
    const std::string branch = current_branch(cwd);
    if (branch.empty()) return std::nullopt;

    const std::string main_branch = default_branch(cwd);
    if (!main_branch.empty() && branch == main_branch) return std::nullopt;

    const CommandResult result = git(cwd, {"pr", "view", "--json",
                                            "number,url,reviewDecision,isDraft,headRefName,state"});
    if (result.failed || result.exit_code != 0) return std::nullopt;
    if (cc::trim(result.output).empty()) return std::nullopt;

    // The answer of the command is read the way the reference reads it.
    const boost::json::value value = cc::json_parse(result.output, boost::json::value(nullptr));
    if (!value.is_object()) return std::nullopt;
    const boost::json::object& object = value.as_object();

    PrStatus status;
    status.number = static_cast<int>(cc::jget_int(object, "number", 0));
    status.url = cc::jget_string(object, "url");
    if (status.number <= 0 || status.url.empty()) return std::nullopt;

    // The pull request of the branch of the main is left out as well.
    const std::string head = cc::jget_string(object, "headRefName");
    if (head == main_branch || head == "main" || head == "master") return std::nullopt;

    // A pull request that was merged or closed is left out: the command
    // answers with the pull request that was merged last.
    const std::string state = cc::jget_string(object, "state");
    if (state == "MERGED" || state == "CLOSED") return std::nullopt;

    if (cc::jget_bool(object, "isDraft", false)) {
        status.review = PrReviewState::Draft;
    } else {
        const std::string decision = cc::jget_string(object, "reviewDecision");
        if (decision == "APPROVED") {
            status.review = PrReviewState::Approved;
        } else if (decision == "CHANGES_REQUESTED") {
            status.review = PrReviewState::ChangesRequested;
        } else {
            status.review = PrReviewState::Pending;
        }
    }

    return status;
}

std::string current_branch(const std::string& cwd) {
    // The branch that is checked out, the way the reference asks for it.
    const CommandResult current = git(cwd, {"rev-parse", "--abbrev-ref", "HEAD"});
    if (current.failed || current.exit_code != 0) return {};

    const std::string name = cc::trim(current.output);
    if (name.empty() || name == "HEAD") return {};
    return name;
}

int changed_file_count(const std::string& cwd) {
    // The root of the repository, so that the totals are of that one.
    const CommandResult root = git(cwd, {"rev-parse", "--show-toplevel"});
    if (root.failed || root.exit_code != 0) return 0;
    const std::string repo = cc::trim(root.output);
    if (repo.empty()) return 0;

    // The totals are asked for without the diffs, the way the reference asks
    // for them: a diff of hundreds of files is not read at all.
    const CommandResult shortstat = git(repo, {"diff", "HEAD", "--shortstat"});
    if (shortstat.failed || shortstat.exit_code != 0) return 0;

    // The line reads " 12 files changed, 34 insertions(+), 5 deletions(-)".
    for (const std::string& line : cc::split_lines(shortstat.output)) {
        const size_t files = line.find(" file");
        if (files == std::string::npos) continue;

        // The number is the word before " file".
        size_t end = files;
        while (end > 0 && std::isdigit(static_cast<unsigned char>(line[end - 1]))) --end;
        if (end == files) continue;

        try {
            return std::stoi(line.substr(end, files - end));
        } catch (const std::exception&) {
            return 0;
        }
    }

    return 0;
}

} // namespace cc::git
