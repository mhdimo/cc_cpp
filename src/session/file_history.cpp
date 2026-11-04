#include "session/file_history.hpp"

#include "utils/fs.hpp"
#include "utils/hash.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace cc::session {
namespace {

/// The number of snapshots the history keeps.
constexpr size_t kMaxSnapshots = 100;

/// The largest table the comparison of two texts may use.
///
/// A text that is longer than this is matched by its common beginning and
/// its common end, and the rest counts as changed.
constexpr size_t kMaxDiffCells = 1'000'000;

/// The lines of a text.
std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string line;
    for (const char c : text) {
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
        } else {
            line += c;
        }
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

/// The number of lines that two texts have in common.
///
/// The longest common subsequence is found with the usual table.
size_t common_lines(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    const size_t n = a.size();
    const size_t m = b.size();
    if (n == 0 || m == 0) return 0;

    // The common beginning and the common end are counted apart, so that a
    // text that is too long for the table is still matched where it can be.
    size_t head = 0;
    while (head < n && head < m && a[head] == b[head]) ++head;

    size_t tail = 0;
    while (tail < n - head && tail < m - head && a[n - 1 - tail] == b[m - 1 - tail]) ++tail;

    const size_t rows = n - head - tail;
    const size_t columns = m - head - tail;
    if (rows == 0 || columns == 0) return head + tail;
    if (rows * columns > kMaxDiffCells) return head + tail;

    // The table of the longest common subsequence.
    std::vector<uint32_t> table((rows + 1) * (columns + 1), 0);
    for (size_t i = rows; i-- > 0;) {
        for (size_t j = columns; j-- > 0;) {
            const size_t here = i * (columns + 1) + j;
            if (a[head + i] == b[head + j]) {
                table[here] = table[here + columns + 2] + 1;
            } else {
                table[here] = std::max(table[here + columns + 1], table[here + 1]);
            }
        }
    }
    return head + table[0] + tail;
}

/// The difference between two states of a file.
///
/// The lines that are only in `after` are the insertions, and the lines
/// that are only in `before` are the deletions.
void count_changes(const std::string& before, const std::string& after, int* insertions,
                   int* deletions) {
    const auto a = split_lines(before);
    const auto b = split_lines(after);
    const size_t common = common_lines(a, b);
    *insertions += static_cast<int>(b.size() - common);
    *deletions += static_cast<int>(a.size() - common);
}

} // namespace

FileHistory::FileHistory(std::string session_id, std::string cwd, std::string directory)
    : session_id_(std::move(session_id)), cwd_(std::move(cwd)), directory_(std::move(directory)) {}

std::string FileHistory::backup_name(const std::string& path, int version) {
    return cc::sha256_hex(path).substr(0, 16) + "@v" + std::to_string(version);
}

std::string FileHistory::backup_path(const std::string& name) const {
    if (!directory_.empty()) return cc::path_join(directory_, name);

    return cc::path_join(cc::path_join(cc::path_join(cc::claude_config_dir(), "file-history"),
                                        session_id_),
                         name);
}

Backup FileHistory::create_backup(const std::string& path, int version) const {
    Backup backup;
    backup.version = version;

    const auto contents = cc::read_file(path);
    if (!contents) {
        // The file does not exist; the backup says so by being empty.
        return backup;
    }

    backup.backup_file_name = backup_name(path, version);

    std::string error;
    if (!cc::write_file(backup_path(backup.backup_file_name), *contents, &error)) {
        backup.backup_file_name.clear();
    }
    return backup;
}

const Backup* FileHistory::latest_backup(const std::string& path) const {
    if (snapshots_.empty()) return nullptr;
    const Snapshot& snapshot = snapshots_.back();
    for (const auto& entry : snapshot.backups) {
        if (entry.first == path) return &entry.second;
    }
    return nullptr;
}

const Backup* FileHistory::backup_in(const Snapshot& snapshot, const std::string& path) {
    for (const auto& entry : snapshot.backups) {
        if (entry.first == path) return &entry.second;
    }
    return nullptr;
}

std::string FileHistory::backup_contents(const Backup& backup) const {
    if (backup.backup_file_name.empty()) return {};
    return cc::read_file(backup_path(backup.backup_file_name)).value_or("");
}

void FileHistory::track_edit(const std::string& path) {
    if (path.empty()) return;

    const bool known = std::find(tracked_.begin(), tracked_.end(), path) != tracked_.end();
    if (!known) tracked_.push_back(path);

    // The contents of the file are copied aside once; the snapshots take
    // the later copies.
    if (latest_backup(path) != nullptr) return;

    if (snapshots_.empty()) snapshots_.push_back(Snapshot{});

    const Backup backup = create_backup(path, 1);
    snapshots_.back().backups.emplace_back(path, backup);
}

void FileHistory::make_snapshot(const std::string& message_id) {
    Snapshot snapshot;
    snapshot.message_id = message_id;

    for (const auto& path : tracked_) {
        const Backup* latest = latest_backup(path);
        const int version = latest != nullptr ? latest->version + 1 : 1;

        // The file is copied aside again only when it changed since the
        // last copy.
        const auto contents = cc::read_file(path);
        if (contents) {
            if (latest != nullptr && !latest->backup_file_name.empty()) {
                const auto backup = cc::read_file(backup_path(latest->backup_file_name));
                if (backup && *backup == *contents) {
                    snapshot.backups.emplace_back(path, *latest);
                    continue;
                }
            }
        } else if (latest != nullptr && latest->backup_file_name.empty()) {
            // The file was already gone.
            snapshot.backups.emplace_back(path, *latest);
            continue;
        }

        snapshot.backups.emplace_back(path, create_backup(path, version));
    }

    snapshots_.push_back(std::move(snapshot));
    while (snapshots_.size() > kMaxSnapshots) snapshots_.erase(snapshots_.begin());
}

bool FileHistory::can_restore(const std::string& message_id) const {
    for (const auto& snapshot : snapshots_) {
        if (snapshot.message_id == message_id) return true;
    }
    return false;
}

std::vector<std::string> FileHistory::rewind(const std::string& message_id) {
    const Snapshot* target = nullptr;
    for (const auto& snapshot : snapshots_) {
        if (snapshot.message_id == message_id) target = &snapshot;
    }
    if (target == nullptr) return {};

    std::vector<std::string> changed;
    for (const auto& path : tracked_) {
        const Backup* backup = nullptr;
        for (const auto& entry : target->backups) {
            if (entry.first == path) backup = &entry.second;
        }
        if (backup == nullptr) continue;

        if (backup->backup_file_name.empty()) {
            // The file did not exist then; it is removed now.
            if (cc::file_exists(path)) {
                cc::remove_file(path);
                changed.push_back(path);
            }
            continue;
        }

        const auto contents = cc::read_file(backup_path(backup->backup_file_name));
        if (!contents) continue;

        const auto current = cc::read_file(path);
        if (current && *current == *contents) continue;

        std::string error;
        if (cc::write_file(path, *contents, &error)) changed.push_back(path);
    }

    return changed;
}

std::optional<DiffStats> FileHistory::diff_to_now(const std::string& message_id) const {
    const Snapshot* target = nullptr;
    for (const auto& snapshot : snapshots_) {
        if (snapshot.message_id == message_id) target = &snapshot;
    }
    if (target == nullptr) return std::nullopt;

    DiffStats stats;
    for (const auto& path : tracked_) {
        const Backup* backup = backup_in(*target, path);
        const std::string then = backup != nullptr ? backup_contents(*backup) : std::string();
        const auto contents = cc::read_file(path);
        const std::string now = contents.value_or("");

        // The restore puts the backup back, so the lines of the backup are
        // the ones that come back.
        int insertions = 0;
        int deletions = 0;
        count_changes(now, then, &insertions, &deletions);
        if (insertions == 0 && deletions == 0) continue;

        stats.files_changed.push_back(path);
        stats.insertions += insertions;
        stats.deletions += deletions;
    }
    return stats;
}

std::optional<DiffStats> FileHistory::diff_to_next(const std::string& message_id) const {
    const Snapshot* target = nullptr;
    const Snapshot* next = nullptr;
    for (size_t i = 0; i < snapshots_.size(); ++i) {
        if (snapshots_[i].message_id != message_id) continue;
        target = &snapshots_[i];
        // The snapshot that came after it holds the state of the files at
        // the end of the turn of the message.
        for (size_t j = i + 1; j < snapshots_.size(); ++j) {
            if (snapshots_[j].message_id.empty()) continue;
            next = &snapshots_[j];
            break;
        }
        break;
    }
    if (target == nullptr) return std::nullopt;

    DiffStats stats;
    for (const auto& path : tracked_) {
        const Backup* before = backup_in(*target, path);
        const std::string then = before != nullptr ? backup_contents(*before) : std::string();

        std::string after;
        if (next != nullptr) {
            const Backup* later = backup_in(*next, path);
            after = later != nullptr ? backup_contents(*later) : std::string();
        } else {
            after = cc::read_file(path).value_or("");
        }

        int insertions = 0;
        int deletions = 0;
        count_changes(then, after, &insertions, &deletions);
        if (insertions == 0 && deletions == 0) continue;

        stats.files_changed.push_back(path);
        stats.insertions += insertions;
        stats.deletions += deletions;
    }
    return stats;
}

} // namespace cc::session
