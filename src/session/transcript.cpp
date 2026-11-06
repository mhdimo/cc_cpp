#include "session/transcript.hpp"

#include "core/ids.hpp"
#include "core/version.hpp"
#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <vector>

namespace cc::session {
namespace {

namespace fs = std::filesystem;

/// The number of bytes read from the head and the tail of a transcript when
/// its metadata is extracted. Claude Code reads the same amount.
constexpr size_t kMetadataWindow = 64 * 1024;

/// The number of characters of the first prompt that make up the title of a
/// session that has no title of its own.
constexpr size_t kTitleLength = 200;

/// The head and the tail of a file, each at most `limit` bytes.
struct FileWindow {
    std::string head;
    std::string tail;
};

/// The metadata a transcript holds about its session.
struct SessionMetadata {
    std::string custom_title;
    std::string ai_title;
    std::string first_prompt;
};

/// Formats a time as an ISO-8601 timestamp in UTC, the form Claude Code
/// writes into transcripts.
std::string iso_timestamp(int64_t milliseconds) {
    const auto seconds = static_cast<std::time_t>(milliseconds / 1000);
    auto fraction = static_cast<int>(milliseconds % 1000);
    if (fraction < 0) fraction += 1000;

    std::tm parts{};
    gmtime_r(&seconds, &parts);

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour,
                  parts.tm_min, parts.tm_sec, fraction);
    return buffer;
}

/// The current time, as an ISO-8601 timestamp in UTC.
std::string now_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return iso_timestamp(milliseconds);
}

/// Reads `count` bytes of a file, starting at `offset`.
std::string read_at(std::ifstream& file, std::streamoff offset, std::streamoff count) {
    if (count <= 0) return {};

    std::vector<char> buffer(static_cast<size_t>(count));
    file.seekg(offset, std::ios::beg);
    file.read(buffer.data(), static_cast<std::streamsize>(count));
    return std::string(buffer.data(), static_cast<size_t>(file.gcount()));
}

/// Reads the head and the tail of a file, each at most `limit` bytes.
FileWindow read_window(const std::string& path, size_t limit) {
    FileWindow window;

    std::ifstream file(path, std::ios::binary);
    if (!file) return window;

    file.seekg(0, std::ios::end);
    const auto size = static_cast<std::streamoff>(file.tellg());
    if (size <= 0) return window;

    const auto window_size = static_cast<std::streamoff>(limit);
    window.head = read_at(file, 0, std::min(size, window_size));
    window.tail = read_at(file, std::max<std::streamoff>(0, size - window_size),
                           std::min(size, window_size));
    return window;
}

/// Reads the entries of a chunk of a transcript.
///
/// The chunk holds whole lines and, at its edges, parts of lines. A line that
/// does not parse is skipped.
void scan_entries(const std::string& chunk, SessionMetadata& metadata, bool want_prompt) {
    for (const auto& line : split_lines(chunk)) {
        if (trim(line).empty()) continue;
        const boost::json::value value = json_parse(line);
        if (!value.is_object()) continue;
        const auto& entry = value.as_object();
        const std::string type = jget_string(entry, "type", "");

        if (type == "custom-title") {
            metadata.custom_title = jget_string(entry, "customTitle", "");
            continue;
        }
        if (type == "ai-title") {
            metadata.ai_title = jget_string(entry, "aiTitle", "");
            continue;
        }
        if (!want_prompt || type != "user" || !metadata.first_prompt.empty()) continue;

        const Message message = Message::from_json(value);
        if (message.is_meta || message.is_compact_summary) continue;
        const std::string text = message.text();
        if (text.empty()) continue;
        metadata.first_prompt = text;
    }
}

/// Flattens the first prompt of a session into a one-line title.
std::string flatten_prompt(const std::string& prompt) {
    if (prompt.empty()) return {};
    const std::string flat = trim(replace_all(prompt, "\n", " "));
    return truncate(flat, kTitleLength, "…");
}

/// The title of a session, as the transcript records it.
///
/// The title the user gave the session wins over the title the model
/// generated, which in turn wins over the first thing the user asked.
std::string session_title(const std::string& path) {
    const FileWindow window = read_window(path, kMetadataWindow);

    SessionMetadata metadata;
    scan_entries(window.head, metadata, true);
    scan_entries(window.tail, metadata, false);

    if (!metadata.custom_title.empty()) return metadata.custom_title;
    if (!metadata.ai_title.empty()) return metadata.ai_title;
    return flatten_prompt(metadata.first_prompt);
}

/// The number of the messages of a session, and the branch it ran on.
///
/// The messages are counted by the entries of the transcript, the way the
/// reference counts them.
void session_details(const std::string& path, size_t& message_count, std::string& branch,
                     std::string& cwd) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.find("\"type\":\"user\"") == std::string::npos &&
            line.find("\"type\":\"assistant\"") == std::string::npos) {
            continue;
        }
        ++message_count;

        if (!branch.empty() && !cwd.empty()) continue;
        const boost::json::value value = json_parse(line);
        if (!value.is_object()) continue;
        if (branch.empty()) branch = jget_string(value.as_object(), "gitBranch", "");
        if (cwd.empty()) cwd = jget_string(value.as_object(), "cwd", "");
    }
}

/// Appends a line to the transcript file.
void append_line(const std::string& path, const boost::json::object& entry) {    std::string error;
    if (!mkdirp(dirname(path), &error)) return;
    if (!append_file(path, json_dump(entry) + "\n", &error)) {
        // A transcript that cannot be written must not stop the session.
    }
}

} // namespace

std::string Transcript::project_dir(const std::string& cwd) {
    return path_join(claude_config_dir(), "projects") + "/" + encode_project_dir(cwd);
}

Transcript Transcript::open(const std::string& cwd, const std::string& session_id) {
    Transcript transcript;
    transcript.session_id_ = session_id;
    transcript.cwd_ = cwd;
    transcript.path_ = path_join(project_dir(cwd), session_id + ".jsonl");

    if (file_exists(transcript.path_)) {
        transcript.messages_ = read(transcript.path_);
    }
    return transcript;
}

void Transcript::append(const Message& message) {
    // The transcript stamps every message with the session it belongs to,
    // so that a resumed conversation knows where it came from.
    Message stamped = message;
    if (stamped.uuid.empty()) stamped.uuid = uuid_v4();
    if (stamped.timestamp.empty()) stamped.timestamp = now_timestamp();
    if (stamped.session_id.empty()) stamped.session_id = session_id_;
    if (stamped.cwd.empty()) stamped.cwd = cwd_;
    if (stamped.version.empty()) stamped.version = version();

    messages_.push_back(stamped);

    std::string error;
    if (!mkdirp(dirname(path_), &error)) return;

    const std::string line = json_dump(stamped.to_json()) + "\n";
    if (!append_file(path_, line, &error)) {
        // A transcript that cannot be written must not stop the session.
    }
}

void Transcript::append_summary(const Message& message) {
    Message summary = message;
    summary.is_meta = true;
    summary.is_compact_summary = true;

    append(summary);
}

void Transcript::write_summary_entry(const std::string& leaf_uuid, const std::string& summary) {
    boost::json::object entry;
    entry["type"] = "summary";
    entry["leafUuid"] = leaf_uuid;
    entry["summary"] = summary;

    append_line(path_, entry);
}

std::vector<Message> Transcript::read(const std::string& path) {
    std::vector<Message> messages;
    const auto contents = read_file(path);
    if (!contents) return messages;

    for (const auto& line : split_lines(*contents)) {
        if (trim(line).empty()) continue;
        const boost::json::value value = json_parse(line);
        if (!value.is_object()) continue;

        // A transcript holds more than messages: the title of the session,
        // the summary of a fork and the like sit beside them.
        const std::string type = jget_string(value.as_object(), "type", "");
        if (type != "user" && type != "assistant") continue;

        messages.push_back(Message::from_json(value));
    }
    return messages;
}

std::vector<SessionInfo> Transcript::list_sessions(const std::string& cwd) {
    std::vector<SessionInfo> sessions;
    const std::string directory = project_dir(cwd);

    std::error_code ec;
    if (!fs::is_directory(fs::path(directory), ec)) return sessions;

    for (const auto& entry : fs::directory_iterator(fs::path(directory), ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != ".jsonl") continue;

        SessionInfo info;
        info.id = entry.path().stem().string();
        const FileState state = file_state(entry.path().string());
        info.modified_ms = state.mtime_ms;
        info.timestamp = iso_timestamp(state.mtime_ms);
        info.title = session_title(entry.path().string());
        info.size = state.size;
        session_details(entry.path().string(), info.message_count, info.git_branch, info.cwd);
        sessions.push_back(std::move(info));
    }

    // Newest first, by modification time.
    std::sort(sessions.begin(), sessions.end(), [](const SessionInfo& a, const SessionInfo& b) {
        if (a.modified_ms != b.modified_ms) return a.modified_ms > b.modified_ms;
        return a.id > b.id;
    });

    return sessions;
}

std::vector<SessionInfo> Transcript::list_all_sessions() {
    std::vector<SessionInfo> sessions;

    // The transcripts of every project live in their own folder, named
    // after the folder they belong to.
    const std::string root = path_join(claude_config_dir(), "projects");

    std::error_code ec;
    if (!fs::is_directory(fs::path(root), ec)) return sessions;

    for (const auto& project : fs::directory_iterator(fs::path(root), ec)) {
        if (!project.is_directory(ec)) continue;

        for (const auto& entry : fs::directory_iterator(project.path(), ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() != ".jsonl") continue;

            SessionInfo info;
            info.id = entry.path().stem().string();
            const FileState state = file_state(entry.path().string());
            info.modified_ms = state.mtime_ms;
            info.timestamp = iso_timestamp(state.mtime_ms);
            info.title = session_title(entry.path().string());
            info.size = state.size;
            session_details(entry.path().string(), info.message_count, info.git_branch, info.cwd);
            sessions.push_back(std::move(info));
        }
    }

    // Newest first, by modification time.
    std::sort(sessions.begin(), sessions.end(), [](const SessionInfo& a, const SessionInfo& b) {
        if (a.modified_ms != b.modified_ms) return a.modified_ms > b.modified_ms;
        return a.id > b.id;
    });

    return sessions;
}

std::string Transcript::title(const std::string& cwd, const std::string& session_id) {
    const std::string path = path_join(project_dir(cwd), session_id + ".jsonl");
    return session_title(path);
}

bool Transcript::rename(const std::string& cwd, const std::string& session_id,
                        const std::string& title) {
    if (trim(title).empty()) return false;

    const std::string path = path_join(project_dir(cwd), session_id + ".jsonl");

    boost::json::object entry;
    entry["type"] = "custom-title";
    entry["customTitle"] = title;
    entry["sessionId"] = session_id;

    std::string error;
    if (!mkdirp(dirname(path), &error)) return false;
    return append_file(path, json_dump(entry) + "\n", &error);
}

bool Transcript::remove(const std::string& cwd, const std::string& session_id) {
    const std::string path = path_join(project_dir(cwd), session_id + ".jsonl");
    remove_file(path);
    return !file_exists(path);
}

} // namespace cc::session
