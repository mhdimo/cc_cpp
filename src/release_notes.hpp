#pragma once

#include <boost/asio.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cc {

/// The notes of the changelog, per version.
///
/// The versions are in the order of the changelog, which is the newest
/// first.
std::vector<std::pair<std::string, std::vector<std::string>>> parse_changelog(std::string_view content);

/// The notes of the versions that are newer than `previous_version`.
///
/// At most five notes are returned, the newest ones first. When the user
/// has seen no version, the notes of the newest ones are shown.
///
/// The version of the port is not part of the comparison: the changelog
/// holds the versions of the reference, which the port does not number the
/// same way.
std::vector<std::string> recent_release_notes(const std::string& previous_version,
                                              std::string_view content);

/// True when `version` is newer than `other`.
///
/// The versions are the ones of the changelog: three numbers separated by
/// dots. A version that cannot be read is not newer than any other.
bool version_is_newer(const std::string& version, const std::string& other);

/// The path of the file the changelog is kept in.
std::string changelog_cache_path();

/// The changelog that was kept, when there is one.
std::string stored_changelog();

/// The changelog that came with the program, when there is one.
///
/// The port brings a changelog of its own, so that the notes the startup
/// screen shows are the notes of the port and not the ones of the
/// reference. The file is looked for next to the program, in the folder of
/// the data of the program, and in the root of the repository — the last
/// one is where it is when the program is run from the build folder.
std::string bundled_changelog();

/// Keeps the changelog, so that the next session can show it.
bool store_changelog(std::string_view content);

/// Fetches the changelog and keeps it.
///
/// The fetch is done over the network; a failure is not an error, the notes
/// are simply not there.
bool fetch_changelog(boost::asio::io_context& io_context);

/// The number of notes the startup screen shows.
inline constexpr size_t kMaxReleaseNotes = 5;

} // namespace cc
