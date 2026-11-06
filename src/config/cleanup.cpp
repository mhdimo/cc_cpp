#include "config/cleanup.hpp"

#include "utils/fs.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"
#include "utils/time.hpp"

namespace cc::config {
namespace {

/// The number of milliseconds of a day.
constexpr int64_t kDayMs = 24 * 60 * 60 * 1000;

/// True when the file was changed before the cutoff.
bool is_old(const std::string& path, int64_t cutoff) {
    const cc::FileState state = cc::file_state(path);
    if (!state.exists) return false;
    return state.mtime_ms < cutoff;
}

/// Removes the file when it is old, and counts it.
void remove_if_old(const std::string& path, int64_t cutoff, CleanupResult& result) {
    if (!cc::is_file(path)) return;
    if (!is_old(path, cutoff)) return;
    if (cc::remove_file(path)) {
        result.messages++;
    } else {
        result.errors++;
    }
}

} // namespace

CleanupResult prune_old_files(int period_days) {
    CleanupResult result;
    if (period_days < 0) period_days = 0;

    const int64_t cutoff =
        cc::now_ms() - static_cast<int64_t>(period_days) * kDayMs;
    const std::string config_dir = cc::claude_config_dir();

    // The transcripts of the sessions.
    const std::string projects = cc::path_join(config_dir, "projects");
    for (const auto& project : cc::list_dir(projects)) {
        const std::string project_dir = cc::path_join(projects, project);
        if (!cc::is_directory(project_dir)) continue;
        for (const auto& name : cc::list_dir(project_dir)) {
            if (!cc::ends_with(name, ".jsonl") && !cc::ends_with(name, ".cast")) continue;
            remove_if_old(cc::path_join(project_dir, name), cutoff, result);
        }
    }

    // The plans of the sessions.
    const std::string plans = cc::path_join(config_dir, "plans");
    for (const auto& name : cc::list_dir(plans)) {
        if (!cc::ends_with(name, ".md")) continue;
        remove_if_old(cc::path_join(plans, name), cutoff, result);
    }

    // The copies of the files the sessions changed.
    const std::string file_history = cc::path_join(config_dir, "file-history");
    for (const auto& name : cc::list_dir(file_history)) {
        const std::string directory = cc::path_join(file_history, name);
        if (!cc::is_directory(directory)) continue;
        if (!is_old(directory, cutoff)) continue;
        if (cc::remove_dir_all(directory)) {
            result.messages++;
        } else {
            result.errors++;
        }
    }

    // The logs of the debugger; the link to the log of the session is kept.
    const std::string debug = cc::path_join(config_dir, "debug");
    for (const auto& name : cc::list_dir(debug)) {
        if (name == "latest") continue;
        if (!cc::ends_with(name, ".txt")) continue;
        remove_if_old(cc::path_join(debug, name), cutoff, result);
    }

    return result;
}

} // namespace cc::config
