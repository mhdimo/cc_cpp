#pragma once

#include "core/message.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cc::session {

/// A session recorded for a project.
///
/// Claude Code keeps the title of a session in the transcript itself, in a
/// `custom-title` entry, and falls back to the title the model generated and
/// then to the first thing the user asked.
struct SessionInfo {
    /// The id of the session, the stem of the transcript file name.
    std::string id;
    /// The title of the session.
    std::string title;
    /// The time the session was last modified, as an ISO-8601 timestamp.
    std::string timestamp;
    /// The time the session was last modified, in milliseconds since the
    /// epoch.
    int64_t modified_ms = 0;

    /// The number of the messages of the session.
    size_t message_count = 0;

    /// The size of the transcript of the session, in bytes.
    int64_t size = 0;

    /// The branch the session ran on.
    std::string git_branch;

    /// The folder the session ran in.
    std::string cwd;

    /// The id of the session, so that a `SessionInfo` can be used where a
    /// session id is expected.
    operator std::string() const { return id; }

    /// True when the session is the one with that id.
    friend bool operator==(const SessionInfo& info, const std::string& id) {
        return info.id == id;
    }
};

/// The transcript of a session.
///
/// Claude Code stores a session as a JSON-lines file, one entry per line,
/// under `~/.claude/projects/<encoded-cwd>/<session-id>.jsonl`.
///
/// A line holds either a message - the API payload under a `message` key,
/// with the metadata (uuid, sessionId, timestamp, cwd, version, isMeta,
/// isSidechain, ...) beside it - or one of the entries that describe the
/// session itself, such as `custom-title`.
class Transcript {
public:
    /// Opens the transcript for `session_id` in the project for `cwd`.
    ///
    /// The file is created when it does not exist.
    static Transcript open(const std::string& cwd, const std::string& session_id);

    /// Appends a message to the transcript.
    void append(const Message& message);

    /// Appends the summary of a compacted conversation to the transcript.
    ///
    /// The summary is stored as a user message marked with
    /// `isCompactSummary`, the way Claude Code marks it.
    void append_summary(const Message& message);

    /// Appends a summary entry to the transcript.
    ///
    /// A summary entry is not a message: it carries the summary of a
    /// conversation next to the uuid of the last message of the branch it
    /// belongs to.
    void write_summary_entry(const std::string& leaf_uuid, const std::string& summary);

    /// The messages the transcript holds.
    const std::vector<Message>& messages() const { return messages_; }

    /// The path of the transcript file.
    const std::string& path() const { return path_; }

    /// The id of the session.
    const std::string& session_id() const { return session_id_; }

    /// The working directory the session runs in.
    const std::string& cwd() const { return cwd_; }

    /// The directory the project transcripts are stored in.
    static std::string project_dir(const std::string& cwd);

    /// The sessions recorded for `cwd`, newest first.
    static std::vector<SessionInfo> list_sessions(const std::string& cwd);

    /// The sessions of every project of the machine, newest first.
    ///
    /// The reference shows them when the user asks for all the chats; the
    /// folder every one of them belongs to is named, so that the user
    /// knows where the conversation comes from.
    static std::vector<SessionInfo> list_all_sessions();

    /// The title of a session, or an empty string when it has none.
    static std::string title(const std::string& cwd, const std::string& session_id);

    /// Renames a session.
    ///
    /// The title is kept in the transcript, in a `custom-title` entry, so
    /// that it survives the session being resumed. Returns false when the
    /// title is empty or the transcript cannot be written.
    static bool rename(const std::string& cwd, const std::string& session_id,
                       const std::string& title);

    /// Deletes a session.
    ///
    /// Returns true when the transcript is gone afterwards.
    static bool remove(const std::string& cwd, const std::string& session_id);

    /// Reads the transcript of a session.
    ///
    /// The entries that are not messages, such as the title of the session,
    /// are skipped.
    static std::vector<Message> read(const std::string& path);

private:
    std::string path_;
    std::string session_id_;
    std::string cwd_;
    std::vector<Message> messages_;
};

} // namespace cc::session
