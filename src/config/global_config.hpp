#pragma once

#include "ui/theme.hpp"

#include <boost/json.hpp>

#include <map>
#include <optional>
#include <string>

namespace cc::config {

/// The directory that holds the global configuration file.
///
/// The reference takes the variable `CLAUDE_CONFIG_DIR` when it is set, and
/// the home directory of the user otherwise — the file itself is named
/// `.claude.json` and lies in that directory.
std::string global_config_dir();

/// The path of the user's global configuration file (`~/.claude.json`).
///
/// The legacy file (`~/.claude/.config.json`) is used instead when it is
/// there, the way the reference does it.
std::string global_config_path();

/// The path of the legacy global configuration file (`~/.claude/.config.json`).
std::string legacy_global_config_path();

/// The directory the backups of the configuration files live in
/// (`~/.claude/backups`).
std::string config_backup_dir();

/// Reads the global configuration.
///
/// The file may not exist, and it may not hold an object; an empty object
/// is returned then. A file that cannot be parsed is backed up, and the
/// most recent backup is read in its place.
boost::json::object load_global_config();

/// Writes the global configuration, keeping a backup of the old one.
///
/// The backup is written before the file is replaced, the way the reference
/// does it: a copy with the time in its name lives in the backup
/// directory, and the plain `<file>.backup` lies next to the file.
bool save_global_config(const boost::json::object& config, std::string* error = nullptr);

/// The environment variables of the global configuration file.
///
/// The reference applies them before the folder is trusted, because the
/// user wrote them.
std::map<std::string, std::string> global_config_env();

/// The environment variables of a configuration that was already read.
std::map<std::string, std::string> global_config_env(const boost::json::object& config);

/// Applies the environment variables of the global configuration file.
void apply_global_config_env();

/// The theme of the user, as it is written in the global configuration file.
///
/// The reference keeps the theme in the global configuration file and not in
/// the settings file; `dark` is the theme of a user who chose none, and
/// `auto` follows the colour scheme of the system.
std::optional<ui::ThemeName> global_theme();

/// The theme of the user, as it is written in a configuration that was
/// already read.
std::optional<ui::ThemeName> global_theme(const boost::json::object& config);

/// True when the user asked for the detailed output.
///
/// The reference keeps this in the global configuration file, next to the
/// acceptance of the folder's trust.
bool global_verbose();

/// A string of the global configuration file.
///
/// The reference keeps the choices of the user in it, the channel of the
/// notifications among them.
std::string global_string(const std::string& key, const std::string& fallback = {});

/// Remembers a string of the global configuration file.
///
/// The rest of the file is left alone.
void set_global_string(const std::string& key, const std::string& value);

/// A flag of the global configuration file.
///
/// The reference keeps the flags of the session in it: the auto-compact,
/// the checkpoints of the code, the bar of the terminal and so on.
bool global_flag(const std::string& key, bool fallback = false);

/// Remembers a flag of the global configuration file.
///
/// The rest of the file is left alone.
void set_global_flag(const std::string& key, bool value);

/// How many times the user has used the `/btw` command.
///
/// The reference leaves out the tip about it once the user has used it.
int64_t global_btw_use_count();

/// Remembers that a command of the user was used.
///
/// The reference keeps the use of every skill in the global configuration
/// file, so that the ones that are used often float to the top of the list.
void record_skill_usage(const std::string& name);

/// The score of a command of the user, the way the reference scores it.
///
/// The score falls by half every seven days, so that a command that was used
/// long ago does not stay at the top of the list for ever.
double skill_usage_score(const std::string& name);

/// The time a command of the user was used last.
///
/// Zero is returned when the command was never used.
int64_t skill_last_used(const std::string& name);

/// Remembers that the user has used the `/btw` command once more.
void count_btw_use();

/// The way the keys of the prompt box are read.
///
/// The reference keeps this in the global configuration file: `vim` for the
/// keys of vim and `normal` for the usual ones.
std::string global_editor_mode();

/// Remembers the way the keys of the prompt box are read.
void set_global_editor_mode(const std::string& mode);

/// The billing of the account of the user.
///
/// The kind of the billing is read from `oauthAccount.billingType` in the
/// global configuration file. The answer is kept, because the file is read
/// on every keystroke otherwise; it is read again when the file is
/// written.
std::string global_account_billing_type();

/// The version of the changelog the user has seen.
///
/// The notes of the versions that came after it are shown at the start of
/// the session.
std::string global_last_release_notes_seen();

/// Remembers the version of the changelog the user has seen.
void remember_release_notes_seen(const std::string& version);

/// True when the browser of Claude is on by default.
///
/// The reference keeps this in the global configuration file.
bool global_claude_in_chrome_default_enabled();

/// Turns the browser of Claude on or off by default.
void set_claude_in_chrome_default_enabled(bool enabled);

/// True when the setup of the session was done already.
///
/// The reference keeps the flag in the global configuration file, so that
/// the questions of the setup are asked only once.
bool has_completed_onboarding();

/// Remembers that the setup of the session was done.
void set_onboarding_completed();

/// True when the user approved the key of the API.
///
/// The reference keeps the keys that were approved and the ones that were
/// refused in the global configuration file, so that the question is asked
/// only once for every key.
bool custom_api_key_approved(const std::string& key);

/// True when the user refused the key of the API.
bool custom_api_key_rejected(const std::string& key);

/// Remembers the answer of the user about a key of the API.
void remember_custom_api_key(const std::string& key, bool approved);

} // namespace cc::config
