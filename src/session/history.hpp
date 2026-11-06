#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cc::session {

/// One entry of the history of the prompts.
///
/// The reference keeps the history of the prompts in `history.jsonl`, in the
/// folder of the configuration of the user, so that the history is shared by
/// every session and by every project.
struct HistoryEntry {
    /// The text the user wrote.
    std::string display;

    /// The time the prompt was written, in milliseconds since the epoch.
    int64_t timestamp = 0;

    /// The folder the session ran in.
    std::string project;

    /// The id of the session the prompt was written in.
    std::string session_id;
};

/// The path of the file the history is kept in.
///
/// It is `history.jsonl` in the folder of the configuration of the user.
std::string history_path();

/// Reads the history, newest first.
///
/// The entries of the sessions of other projects are read as well: the
/// reference shares the history between the projects.
std::vector<HistoryEntry> read_history();

/// Appends one entry to the history.
///
/// The file is opened in append mode and the line is written in one go, so
/// that the reference and the port can write to the same file without
/// interleaving.
void append_history(const HistoryEntry& entry);

} // namespace cc::session
