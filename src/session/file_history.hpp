#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cc::session {

/// The backup of one file.
struct Backup {
    /// The name of the backup file; empty when the file did not exist.
    std::string backup_file_name;

    /// The version of the backup.
    int version = 1;
};

/// The state of the files at one point of the conversation.
struct Snapshot {
    /// The id of the message the snapshot belongs to.
    std::string message_id;

    /// The backup of every tracked file, by path.
    std::vector<std::pair<std::string, Backup>> backups;
};

/// The code that changed between two states of the files.
///
/// The reference counts the files that changed and the lines that were
/// added and removed, the way a diff does.
struct DiffStats {
    /// The files that changed.
    std::vector<std::string> files_changed;

    /// The lines that were added.
    int insertions = 0;

    /// The lines that were removed.
    int deletions = 0;
};

/// The history of the files the session has changed.
///
/// The reference keeps a copy of every file it edits, so that a rewind can
/// put the files back the way they were. The copies live in
/// `~/.claude/file-history/<session id>/`, named after the hash of the path
/// of the file and the version of the backup.
class FileHistory {
public:
    FileHistory() = default;

    /// The directory the copies are kept in; the one of the reference when
    /// it is not named.
    FileHistory(std::string session_id, std::string cwd, std::string directory = {});

    /// Records that a file is about to be changed.
    ///
    /// The contents of the file are copied aside, so that the change can
    /// be undone. The call has to come before the file is changed.
    void track_edit(const std::string& path);

    /// Takes a snapshot of the files that changed.
    ///
    /// The call has to come after the turn that changed them.
    void make_snapshot(const std::string& message_id);

    /// True when the files can be put back the way they were at a message.
    bool can_restore(const std::string& message_id) const;

    /// Puts the files back the way they were at a message.
    ///
    /// Returns the paths of the files that changed.
    std::vector<std::string> rewind(const std::string& message_id);

    /// The difference between the files as they are now and the way they
    /// were at a message.
    ///
    /// This is the code that a rewind to that message would put back. It is
    /// empty when there is no snapshot of the message.
    std::optional<DiffStats> diff_to_now(const std::string& message_id) const;

    /// The difference between the files as they were at a message and the
    /// way they were at the message that came after it.
    ///
    /// This is the code that the turn of the message changed. It is empty
    /// when there is no snapshot of the message.
    std::optional<DiffStats> diff_to_next(const std::string& message_id) const;

    /// The files the session has changed.
    const std::vector<std::string>& tracked() const { return tracked_; }

private:
    /// The name of the backup of a file.
    static std::string backup_name(const std::string& path, int version);

    /// The path of a backup file.
    std::string backup_path(const std::string& name) const;

    /// Copies a file aside.
    ///
    /// The backup of a file that does not exist is empty.
    Backup create_backup(const std::string& path, int version) const;

    /// The backup of a file in the latest snapshot, if there is one.
    const Backup* latest_backup(const std::string& path) const;

    /// The backup of a file in a snapshot, if there is one.
    static const Backup* backup_in(const Snapshot& snapshot, const std::string& path);

    /// The contents of a backup; empty when the backup holds nothing.
    std::string backup_contents(const Backup& backup) const;

    std::string session_id_;
    std::string cwd_;

    /// The directory the copies are kept in.
    std::string directory_;

    /// The files the session has changed.
    std::vector<std::string> tracked_;

    /// The snapshots of the conversation.
    std::vector<Snapshot> snapshots_;
};

} // namespace cc::session
