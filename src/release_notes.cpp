#include "release_notes.hpp"

#include "utils/fs.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <ai/http/client.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace cc {
namespace {

/// Where the changelog of the reference is kept.
constexpr std::string_view kChangelogUrl =
    "https://raw.githubusercontent.com/anthropics/claude-code/refs/heads/main/CHANGELOG.md";

/// The numbers of a version.
std::vector<int64_t> version_numbers(const std::string& version) {
    std::vector<int64_t> numbers;
    for (const auto& part : split(version, '.')) {
        // The version may carry a suffix, as in `1.2.3-beta`.
        size_t end = 0;
        while (end < part.size() && part[end] >= '0' && part[end] <= '9') ++end;
        if (end == 0) break;
        numbers.push_back(std::stoll(part.substr(0, end)));
    }
    return numbers;
}

} // namespace

std::vector<std::pair<std::string, std::vector<std::string>>> parse_changelog(
    std::string_view content) {
    // The changelog is a list of sections that begin with `## `; the notes
    // of a section are the lines that begin with a dash.
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    std::string version;
    std::vector<std::string> notes;

    const auto flush = [&out, &version, &notes]() {
        if (version.empty()) return;
        if (!notes.empty()) out.emplace_back(version, notes);
        version.clear();
        notes.clear();
    };

    for (const auto& line : split(content, '\n')) {
        if (starts_with(line, "## ")) {
            flush();
            // The version may be followed by the day of the release.
            version = trim(split_once(trim(line.substr(3)), '-').first);
            continue;
        }
        if (version.empty()) continue;

        const std::string trimmed = trim(line);
        if (!starts_with(trimmed, "- ")) continue;
        const std::string note = trim(trimmed.substr(2));
        if (!note.empty()) notes.push_back(note);
    }
    flush();

    return out;
}

bool version_is_newer(const std::string& version, const std::string& other) {
    const std::vector<int64_t> a = version_numbers(version);
    const std::vector<int64_t> b = version_numbers(other);
    if (a.empty() || b.empty()) return false;

    const size_t count = std::max(a.size(), b.size());
    for (size_t i = 0; i < count; ++i) {
        const int64_t left = i < a.size() ? a[i] : 0;
        const int64_t right = i < b.size() ? b[i] : 0;
        if (left != right) return left > right;
    }
    return false;
}

std::vector<std::string> recent_release_notes(const std::string& previous_version,
                                              std::string_view content) {
    if (content.empty()) return {};

    std::vector<std::string> notes;
    for (const auto& [version, version_notes] : parse_changelog(content)) {
        // The notes of the versions that came after the one the user has
        // seen.
        if (!previous_version.empty() && !version_is_newer(version, previous_version)) continue;
        for (const auto& note : version_notes) {
            notes.push_back(note);
            if (notes.size() >= kMaxReleaseNotes) return notes;
        }
    }
    return notes;
}

std::string changelog_cache_path() {
    return cc::path_join(cc::path_join(cc::claude_config_dir(), "cache"), "changelog.md");
}

std::string bundled_changelog() {
    // The file is looked for in the places it may be: next to the program,
    // in the folder of the data of the program — the layout of the install
    // of the reference — in the folder of the data beside a program that
    // lives in the bin of a prefix, and in the root of the repository,
    // which is where it is when the program is run from the build folder.
    const std::string directory = cc::program_directory();
    if (directory.empty()) return {};

    const std::string parent = cc::dirname(directory);
    const std::string candidates[] = {
        cc::path_join(directory, "CHANGELOG.md"),
        cc::path_join(parent, "CHANGELOG.md"),
        cc::path_join(cc::path_join(parent, "share"), "claude/CHANGELOG.md"),
        cc::path_join(cc::dirname(parent), "CHANGELOG.md"),
    };

    for (const std::string& candidate : candidates) {
        const auto text = cc::read_file(candidate);
        if (text.has_value() && !text->empty()) return *text;
    }
    return {};
}

std::string stored_changelog() {
    const auto text = cc::read_file(changelog_cache_path());
    return text.value_or(std::string());
}

bool store_changelog(std::string_view content) {
    const std::string path = changelog_cache_path();
    std::string error;
    if (!cc::write_file(path, content, &error)) return false;

    // The file is a cache, so the user may write it.
    std::error_code code;
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, code);
    return true;
}

bool fetch_changelog(boost::asio::io_context& io_context) {
    ai::http::HttpClient client(io_context);

    ai::http::Headers headers;
    headers["Accept"] = "text/plain";

    auto task = client.get(std::string(kChangelogUrl), headers);
    task.start();
    while (!task.done()) {
        io_context.run_one_for(std::chrono::milliseconds(20));
    }

    try {
        const ai::http::HttpResponse response = task.get();
        if (response.status_code != 200) return false;
        return store_changelog(response.body);
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace cc
