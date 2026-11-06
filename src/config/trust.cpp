#include "config/trust.hpp"

#include "config/global_config.hpp"
#include "config/project_key.hpp"
#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"

#include <boost/json.hpp>

#include <string_view>
#include <utility>
#include <vector>

namespace cc::config {
namespace {

/// The key the acceptance is stored under, per project.
constexpr std::string_view kAcceptedKey = "hasTrustDialogAccepted";

/// The key the per-project settings live under.
constexpr std::string_view kProjectsKey = "projects";

/// True when the project `key` was trusted.
bool bucket_accepted(const boost::json::object& projects, const std::string& key) {
    const auto entry = projects.find(key);
    if (entry == projects.end() || !entry->value().is_object()) return false;
    return jget_bool(entry->value().as_object(), kAcceptedKey, false);
}

/// Writes the flags of the bucket of the project.
///
/// The rest of the global configuration file is left alone.
void remember_flags(const std::string& path,
                    const std::vector<std::pair<std::string_view, bool>>& flags) {
    if (path.empty() || flags.empty()) return;

    const std::string config_path = global_config_path();

    boost::json::value contents(boost::json::object{});
    if (const auto existing = read_file(config_path)) {
        contents = json_parse(*existing, boost::json::value(boost::json::object{}));
    }
    if (!contents.is_object()) contents = boost::json::value(boost::json::object{});

    boost::json::object object = contents.as_object();
    boost::json::object projects = jget_object(object, kProjectsKey);

    // The flag is kept in the bucket of the project, the way the reference
    // keeps it.
    const std::string project = project_config_key(path);

    boost::json::object entry = jget_object(projects, project);
    for (const auto& [key, value] : flags) entry[std::string(key)] = value;
    projects[project] = std::move(entry);
    object[std::string(kProjectsKey)] = std::move(projects);

    // The file has to exist before it is written, so the directory it lives
    // in is created first.
    const std::string directory = cc::dirname(config_path);
    if (!directory.empty()) mkdirp(directory);

    std::string error;
    save_global_config(object, &error);
}

/// Writes a flag in the bucket of the project.
void remember_trust_flag(const std::string& path, std::string_view key, bool value) {
    remember_flags(path, {{key, value}});
}

} // namespace

bool trust_accepted(const std::string& path) {
    if (path.empty()) return false;

    const auto contents = read_file(global_config_path());
    if (!contents) return false;

    const boost::json::value parsed = json_parse(*contents, boost::json::value(boost::json::object{}));
    if (!parsed.is_object()) return false;

    const boost::json::object projects = jget_object(parsed.as_object(), kProjectsKey);
    if (projects.empty()) return false;

    // The bucket of the project comes first: the reference keeps the
    // acceptance of the folder in the bucket of the root of the repository,
    // so that a folder inside a repository and the repository itself share
    // one bucket.
    if (bucket_accepted(projects, project_config_key(path))) return true;

    // Then the folder itself, and then every folder above it: trusting a
    // project trusts the tree it lives in.
    std::string current = cc::normalize_path_for_config_key(path);
    while (!current.empty()) {
        if (bucket_accepted(projects, current)) return true;
        const std::string parent = cc::dirname(current);
        if (parent.empty() || parent == current) break;
        current = parent;
    }

    return false;
}

void remember_trust(const std::string& path) { remember_trust_flag(path, kAcceptedKey, true); }

namespace {

/// The key the answer about the external includes is stored under, per
/// project.
constexpr std::string_view kExternalIncludesKey = "hasClaudeMdExternalIncludesApproved";

/// The key that says that the question about the external includes was
/// asked, per project.
constexpr std::string_view kExternalIncludesWarningKey = "hasClaudeMdExternalIncludesWarningShown";

/// Reads a flag of the bucket of the project.
///
/// The flag is false when the bucket is not there, or when it does not hold
/// the flag.
bool project_flag(const std::string& path, std::string_view key) {
    if (path.empty()) return false;

    const auto contents = read_file(global_config_path());
    if (!contents) return false;

    const boost::json::value parsed = json_parse(*contents, boost::json::value(boost::json::object{}));
    if (!parsed.is_object()) return false;

    const boost::json::object projects = jget_object(parsed.as_object(), kProjectsKey);
    const auto entry = projects.find(project_config_key(path));
    if (entry == projects.end() || !entry->value().is_object()) return false;

    return jget_bool(entry->value().as_object(), key, false);
}

} // namespace

bool external_includes_approved(const std::string& path) {
    return project_flag(path, kExternalIncludesKey);
}

bool external_includes_warning_shown(const std::string& path) {
    return project_flag(path, kExternalIncludesWarningKey);
}

void remember_external_includes(const std::string& path, bool approved) {
    // The answer is remembered twice: the answer itself, and the fact that
    // the question was asked, the way the reference remembers it.
    remember_flags(path, {{kExternalIncludesKey, approved}, {kExternalIncludesWarningKey, true}});
}

} // namespace cc::config
