#include "config/global_config.hpp"

#include "config/environment.hpp"
#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"
#include "utils/time.hpp"

#include <cmath>
#include <map>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace cc::config {
namespace {

/// True when the corrupted file has already been reported.
///
/// The configuration is read many times during a session; the message of
/// the reference is written once.
bool g_reported_corruption = false;

/// The name of the global configuration file.
///
/// The reference names the file after the OAuth it talks to, so that a
/// session that talks to another server does not share the configuration of
/// the first one.
std::string config_file_name() {
    if (const char* custom = std::getenv("CLAUDE_CODE_CUSTOM_OAUTH_URL"); custom && *custom) {
        return ".claude-custom-oauth.json";
    }
    return ".claude.json";
}

/// The backups of the file `path`, with the time in their names.
std::vector<std::string> timestamped_backups(const std::string& directory,
                                              const std::string& base) {
    std::vector<std::string> out;
    for (const auto& name : cc::list_dir(directory)) {
        if (cc::starts_with(name, base + ".backup.")) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// The most recent backup of the file `path`.
///
/// The reference looks in the backup directory first, then in the place the
/// file lies in, and then at the plain backup of the file.
std::optional<std::string> find_most_recent_backup(const std::string& path) {
    const std::string base = cc::basename(path);

    const std::string backup_dir = config_backup_dir();
    const std::vector<std::string> in_dir = timestamped_backups(backup_dir, base);
    if (!in_dir.empty()) return cc::path_join(backup_dir, in_dir.back());

    const std::string directory = cc::dirname(path);
    const std::vector<std::string> beside = timestamped_backups(directory, base);
    if (!beside.empty()) return cc::path_join(directory, beside.back());

    const std::string legacy = path + ".backup";
    if (cc::file_exists(legacy)) return legacy;

    return std::nullopt;
}

/// Keeps a copy of the file `path` before it is replaced.
///
/// The reference writes the copy in the backup directory, with the time in
/// its name, and keeps the five most recent ones. It does not write a
/// backup for every save — the saves of the start of a session follow each
/// other closely — so the copy is only made when the last one is older
/// than a minute.
void backup_global_config(const std::string& path) {
    if (!cc::file_exists(path)) return;

    const std::string base = cc::basename(path);
    const std::string backup_dir = config_backup_dir();
    cc::mkdirp(backup_dir);

    constexpr int64_t kMinBackupIntervalMs = 60'000;
    constexpr size_t kMaxBackups = 5;

    std::vector<std::string> backups = timestamped_backups(backup_dir, base);

    bool create = true;
    if (!backups.empty()) {
        const std::string& most_recent = backups.back();
        const size_t pos = most_recent.rfind(".backup.");
        int64_t timestamp = 0;
        if (pos != std::string::npos) {
            timestamp = std::strtoll(most_recent.c_str() + pos + 8, nullptr, 10);
        }
        create = timestamp == 0 || cc::now_ms() - timestamp >= kMinBackupIntervalMs;
    }

    if (create) {
        const std::string name = base + ".backup." + std::to_string(cc::now_ms());
        std::string error;
        cc::copy_recursive(path, cc::path_join(backup_dir, name), &error);
        backups.push_back(name);
        std::sort(backups.begin(), backups.end());
    }

    // Only the five most recent backups are kept.
    while (backups.size() > kMaxBackups) {
        cc::remove_file(cc::path_join(backup_dir, backups.front()));
        backups.erase(backups.begin());
    }

    // The plain backup lies next to the file; the reference reads it there,
    // and the two can restore each other because of it.
    std::string error;
    cc::copy_recursive(path, path + ".backup", &error);
}

/// Reads the configuration out of a file that cannot be parsed.
///
/// The corrupted file is kept, so that the user can look at it, and the
/// most recent backup is read in its place, so that a corrupted file does
/// not lose the settings of the user.
boost::json::object recover_corrupted_config(const std::string& path,
                                              const std::string& contents) {
    const std::string base = cc::basename(path);
    const std::string backup_dir = config_backup_dir();
    cc::mkdirp(backup_dir);

    // The file is only copied when it is not already there.
    bool already_backed_up = false;
    for (const auto& name : cc::list_dir(backup_dir)) {
        if (!cc::starts_with(name, base + ".corrupted.")) continue;
        const auto other = cc::read_file(cc::path_join(backup_dir, name));
        if (other && *other == contents) {
            already_backed_up = true;
            break;
        }
    }

    std::string corrupted_path;
    if (!already_backed_up) {
        corrupted_path =
            cc::path_join(backup_dir, base + ".corrupted." + std::to_string(cc::now_ms()));
        std::string error;
        cc::write_file(corrupted_path, contents, &error);
    }

    const std::optional<std::string> backup = find_most_recent_backup(path);

    if (!g_reported_corruption) {
        g_reported_corruption = true;
        std::cerr << "\nClaude configuration file at " << path << " is corrupted.\n";
        if (!corrupted_path.empty()) {
            std::cerr << "The corrupted file has been backed up to: " << corrupted_path << "\n";
        } else if (already_backed_up) {
            std::cerr << "The corrupted file has already been backed up.\n";
        }
        if (backup) {
            std::cerr << "A backup file exists at: " << *backup << "\n"
                      << "You can manually restore it by running: cp \"" << *backup << "\" \"" << path
                      << "\"\n\n";
        } else {
            std::cerr << "\n";
        }
    }

    if (backup) {
        if (const auto restored = cc::read_file(*backup)) {
            const boost::json::value parsed =
                cc::json_parse(*restored, boost::json::value(nullptr));
            if (parsed.is_object()) return parsed.as_object();
        }
    }

    return {};
}

/// Writes the file `path` so that only its owner may read it.
///
/// The reference writes the global configuration with the mode `0600`; it
/// holds the token of the session.
bool write_private(const std::string& path, const std::string& contents, std::string* error) {
    if (!cc::write_file(path, contents, error)) return false;

    std::error_code ec;
    fs::permissions(fs::path(path), fs::perms::owner_read | fs::perms::owner_write, ec);
    return true;
}

} // namespace

std::string global_config_dir() {
    if (const char* dir = std::getenv("CLAUDE_CONFIG_DIR"); dir && *dir) return dir;
    return cc::home_dir();
}

std::string legacy_global_config_path() {
    return cc::path_join(cc::claude_config_dir(), ".config.json");
}

std::string global_config_path() {
    // The legacy file of the reference is preferred when it is there.
    const std::string legacy = legacy_global_config_path();
    if (cc::file_exists(legacy)) return legacy;
    return cc::path_join(global_config_dir(), config_file_name());
}

std::string config_backup_dir() {
    return cc::path_join(cc::claude_config_dir(), "backups");
}

boost::json::object load_global_config() {
    const std::string path = global_config_path();
    const auto contents = cc::read_file(path);
    if (!contents) {
        // The reference tells the user where a backup is when the file is
        // missing.
        if (!g_reported_corruption) {
            if (const auto backup = find_most_recent_backup(path)) {
                g_reported_corruption = true;
                std::cerr << "\nClaude configuration file not found at: " << path << "\n"
                          << "A backup file exists at: " << *backup << "\n"
                          << "You can manually restore it by running: cp \"" << *backup << "\" \""
                          << path << "\"\n\n";
            }
        }
        return {};
    }

    const boost::json::value parsed = cc::json_parse(*contents, boost::json::value(nullptr));
    if (!parsed.is_object()) return recover_corrupted_config(path, *contents);
    return parsed.as_object();
}

std::string global_account_billing_type() {
    // The file is read on every keystroke — the list of the commands is
    // built again and again — so the answer is kept until the file is
    // written.
    static std::string billing;
    static bool known = false;
    if (!known) {
        billing = jget_string(jget_object(load_global_config(), "oauthAccount"), "billingType");
        known = true;
    }
    return billing;
}

bool save_global_config(const boost::json::object& config, std::string* error) {
    const std::string path = global_config_path();

    // The old file is kept before it is replaced.
    backup_global_config(path);

    return write_private(path, cc::json_dump_pretty(config) + "\n", error);
}

std::map<std::string, std::string> global_config_env() {
    return global_config_env(load_global_config());
}

std::map<std::string, std::string> global_config_env(const boost::json::object& config) {
    std::map<std::string, std::string> out;
    for (const auto& [key, value] : cc::jget_object(config, "env")) {
        if (!value.is_string()) continue;
        out[key] = std::string(value.as_string().c_str());
    }
    return out;
}

void apply_global_config_env() {
    cc::config::apply_env(global_config_env());
}

std::optional<ui::ThemeName> global_theme() {
    return global_theme(load_global_config());
}

std::optional<ui::ThemeName> global_theme(const boost::json::object& config) {
    const std::string theme = cc::jget_string(config, "theme", "dark");
    if (theme == "auto") {
        // "auto" follows the colour scheme of the system, so no theme is
        // pinned.
        return std::nullopt;
    }

    ui::ThemeName name = ui::ThemeName::Dark;
    if (ui::parse_theme_name(theme, name)) return name;
    return ui::ThemeName::Dark;
}

bool global_verbose() {
    return jget_bool(load_global_config(), "verbose", false);
}

std::string global_string(const std::string& key, const std::string& fallback) {
    return jget_string(load_global_config(), key, fallback);
}

void set_global_string(const std::string& key, const std::string& value) {
    // The rest of the configuration file is left alone.
    boost::json::object config = load_global_config();
    config[key] = boost::json::value(value);

    std::string error;
    save_global_config(config, &error);
}

bool global_flag(const std::string& key, bool fallback) {
    return jget_bool(load_global_config(), key, fallback);
}

void set_global_flag(const std::string& key, bool value) {
    // The rest of the configuration file is left alone.
    boost::json::object config = load_global_config();
    config[key] = boost::json::value(value);

    std::string error;
    save_global_config(config, &error);
}

int64_t global_btw_use_count() {
    return jget_int(load_global_config(), "btwUseCount", 0);
}

std::string global_editor_mode() {
    return jget_string(load_global_config(), "editorMode", "normal");
}

void set_global_editor_mode(const std::string& mode) {
    // The rest of the configuration file is left alone.
    boost::json::object config = load_global_config();
    config["editorMode"] = boost::json::value(mode);

    std::string error;
    save_global_config(config, &error);
}

void count_btw_use() {
    // The rest of the configuration file is left alone.
    boost::json::object config = load_global_config();
    config["btwUseCount"] = boost::json::value(global_btw_use_count() + 1);

    std::string error;
    save_global_config(config, &error);
}

/// The use of the skills, as it was read the last time.
///
/// The file is read on every keystroke — the list of the commands is built
/// again and again — so the answer is kept until the file is written. The
/// file is read again when it moves, which is what happens when the
/// directory of the configuration changes.
boost::json::object& skill_usage_cache() {
    static std::string path;
    static boost::json::object usage;

    const std::string current = global_config_path();
    if (path != current) {
        path = current;
        usage = jget_object(load_global_config(), "skillUsage");
    }
    return usage;
}

/// The number of milliseconds two writes of the use of a command are apart.
///
/// The reference waits a minute before it writes the file again: the ranking
/// of the commands only needs the day.
constexpr int64_t kSkillUsageDebounceMs = 60 * 1000;

void record_skill_usage(const std::string& name) {
    if (name.empty()) return;

    // The reference writes the file at most once a minute: the ranking only
    // needs the day, so a write for every use of a command is a waste.
    static std::map<std::string, int64_t> last_write;
    const int64_t now = cc::now_ms();
    const auto written = last_write.find(name);
    if (written != last_write.end() && now - written->second < kSkillUsageDebounceMs) return;
    last_write[name] = now;

    // The rest of the configuration file is left alone.
    boost::json::object config = load_global_config();

    boost::json::object usage = jget_object(config, "skillUsage");
    const boost::json::object entry = jget_object(usage, name);
    const int64_t count = jget_int(entry, "usageCount", 0);

    boost::json::object updated;
    updated["usageCount"] = count + 1;
    updated["lastUsedAt"] = cc::now_ms();

    usage[name] = boost::json::value(std::move(updated));
    config["skillUsage"] = boost::json::value(std::move(usage));

    std::string error;
    save_global_config(config, &error);

    // The file has been written, so the answer that was kept is stale.
    skill_usage_cache() = std::move(usage);
}

double skill_usage_score(const std::string& name) {
    if (name.empty()) return 0.0;

    const boost::json::object entry = jget_object(skill_usage_cache(), name);
    if (entry.empty()) return 0.0;

    const int64_t count = jget_int(entry, "usageCount", 0);
    const int64_t last_used = jget_int(entry, "lastUsedAt", 0);
    if (count <= 0 || last_used <= 0) return 0.0;

    // The score falls by half every seven days; a command that was used a
    // long ago still keeps a tenth of its worth.
    const double days = static_cast<double>(cc::now_ms() - last_used) / (1000.0 * 60.0 * 60.0 * 24.0);
    const double recency = std::pow(0.5, days / 7.0);
    return static_cast<double>(count) * std::max(recency, 0.1);
}

int64_t skill_last_used(const std::string& name) {
    if (name.empty()) return 0;

    const boost::json::object entry = jget_object(skill_usage_cache(), name);
    if (entry.empty()) return 0;
    return jget_int(entry, "lastUsedAt", 0);
}

std::string global_last_release_notes_seen() {
    return jget_string(load_global_config(), "lastReleaseNotesSeen");
}

void remember_release_notes_seen(const std::string& version) {
    // The rest of the configuration file is left alone.
    boost::json::object config = load_global_config();
    config["lastReleaseNotesSeen"] = version;

    std::string error;
    save_global_config(config, &error);
}

bool global_claude_in_chrome_default_enabled() {
    return jget_bool(load_global_config(), "claudeInChromeDefaultEnabled", false);
}

void set_claude_in_chrome_default_enabled(bool enabled) {
    // The rest of the configuration file is left alone.
    boost::json::object config = load_global_config();
    config["claudeInChromeDefaultEnabled"] = enabled;

    std::string error;
    save_global_config(config, &error);
}

bool has_completed_onboarding() {
    return jget_bool(load_global_config(), "hasCompletedOnboarding", false);
}

void set_onboarding_completed() {
    // The rest of the configuration file is left alone.
    boost::json::object config = load_global_config();
    config["hasCompletedOnboarding"] = boost::json::value(true);

    std::string error;
    save_global_config(config, &error);
}

namespace {

/// The keys the user answered for, the way the reference keeps them.
std::vector<std::string> custom_api_key_answers(const boost::json::object& config,
                                                 const std::string& field) {
    std::vector<std::string> keys;
    const boost::json::object responses = jget_object(config, "customApiKeyResponses");
    for (const auto& value : jget_array(responses, field)) {
        if (value.is_string()) keys.push_back(std::string(value.as_string().c_str()));
    }
    return keys;
}

} // namespace

bool custom_api_key_approved(const std::string& key) {
    const auto keys = custom_api_key_answers(load_global_config(), "approved");
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

bool custom_api_key_rejected(const std::string& key) {
    const auto keys = custom_api_key_answers(load_global_config(), "rejected");
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

void remember_custom_api_key(const std::string& key, bool approved) {
    if (key.empty()) return;

    // The rest of the configuration file is left alone.
    boost::json::object config = load_global_config();

    boost::json::object responses = jget_object(config, "customApiKeyResponses");
    const std::string field = approved ? "approved" : "rejected";
    const std::string other = approved ? "rejected" : "approved";

    // The key is taken out of the other list, the way the reference takes it
    // out: a key is either approved or refused, never both.
    boost::json::array kept;
    for (const auto& value : jget_array(responses, other)) {
        if (!value.is_string()) continue;
        if (value.as_string().c_str() == key) continue;
        kept.emplace_back(std::string(value.as_string().c_str()));
    }
    responses[other] = boost::json::value(std::move(kept));

    boost::json::array keys;
    for (const auto& value : jget_array(responses, field)) {
        if (value.is_string()) keys.emplace_back(std::string(value.as_string().c_str()));
    }
    // The key is only remembered once.
    if (std::find(keys.begin(), keys.end(), boost::json::value(key)) == keys.end()) {
        keys.emplace_back(key);
    }

    responses[field] = boost::json::value(std::move(keys));
    config["customApiKeyResponses"] = boost::json::value(std::move(responses));

    std::string error;
    save_global_config(config, &error);
}

} // namespace cc::config
