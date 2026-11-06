#include "session/history.hpp"

#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <boost/json.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <system_error>

namespace cc::session {
namespace {

/// The mode of the file of the history.
///
/// The reference writes the file with the mode 0600, so that the prompts of
/// the user are not read by the other users of the machine.
constexpr int kHistoryFileMode = 0600;

} // namespace

std::string history_path() {
    return cc::path_join(cc::claude_config_dir(), "history.jsonl");
}

std::vector<HistoryEntry> read_history() {
    std::vector<HistoryEntry> entries;

    const auto text = cc::read_file(history_path());
    if (!text) return entries;

    // The file is read backwards, so that the newest entry comes first.
    const std::vector<std::string> lines = cc::split_lines(*text);
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        const std::string& line = cc::trim(*it);
        if (line.empty()) continue;

        const boost::json::value value = cc::json_parse(line);
        if (!value.is_object()) continue;
        const boost::json::object& object = value.as_object();

        HistoryEntry entry;
        entry.display = cc::jget_string(object, "display");
        entry.timestamp = cc::jget_int(object, "timestamp");
        entry.project = cc::jget_string(object, "project");
        entry.session_id = cc::jget_string(object, "sessionId");
        if (entry.display.empty()) continue;

        entries.push_back(std::move(entry));
    }

    return entries;
}

void append_history(const HistoryEntry& entry) {
    boost::json::object object;
    object["display"] = entry.display;
    object["pastedContents"] = boost::json::object();
    object["timestamp"] = entry.timestamp;
    object["project"] = entry.project;
    if (!entry.session_id.empty()) object["sessionId"] = entry.session_id;

    const std::string line = cc::json_dump(boost::json::value(object)) + "\n";

    // The file is opened in append mode and the line is written in one go:
    // the reference writes its own lines the same way, and a write that is
    // not split cannot interleave with it.
    const int fd = ::open(history_path().c_str(), O_WRONLY | O_CREAT | O_APPEND,
                          kHistoryFileMode);
    if (fd < 0) return;

    const char* data = line.data();
    size_t left = line.size();
    while (left > 0) {
        const ssize_t written = ::write(fd, data, left);
        if (written <= 0) {
            if (written < 0 && errno == EINTR) continue;
            break;
        }
        data += written;
        left -= static_cast<size_t>(written);
    }
    ::close(fd);
}

} // namespace cc::session
