#include "session/plan.hpp"

#include "utils/fs.hpp"
#include "utils/path.hpp"
#include "utils/words.hpp"

#include <map>
#include <mutex>

namespace cc::session {
namespace {

/// The name of the folder the plans are kept in.
constexpr std::string_view kPlansDirectoryName = "plans";

/// How many times a slug is tried when its file is already there.
constexpr int kMaxSlugRetries = 10;

/// The slugs of the sessions, kept for the session.
std::map<std::string, std::string>& slug_cache() {
    static std::map<std::string, std::string> cache;
    return cache;
}

/// Guards the cache of the slugs.
std::mutex& slug_mutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace

std::string plans_directory() {
    return cc::path_join(cc::claude_config_dir(), kPlansDirectoryName);
}

std::string plan_slug(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(slug_mutex());

    auto& cache = slug_cache();
    const auto known = cache.find(session_id);
    if (known != cache.end()) return known->second;

    // A slug whose file is already there is tried again, so that two
    // sessions do not share a plan.
    std::string slug = generate_word_slug();
    for (int i = 0; i < kMaxSlugRetries; ++i) {
        if (!cc::file_exists(cc::path_join(plans_directory(), slug + ".md"))) break;
        slug = generate_word_slug();
    }

    cache[session_id] = slug;
    return slug;
}

std::string plan_path(const std::string& session_id) {
    return cc::path_join(plans_directory(), plan_slug(session_id) + ".md");
}

std::optional<std::string> read_plan(const std::string& session_id) {
    return cc::read_file(plan_path(session_id));
}

bool write_plan(const std::string& session_id, std::string_view content, std::string* error) {
    if (!cc::mkdirp(plans_directory(), error)) return false;
    return cc::write_file(plan_path(session_id), content, error);
}

void forget_plan_slug(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(slug_mutex());
    slug_cache().erase(session_id);
}

} // namespace cc::session
