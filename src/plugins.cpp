#include "plugins.hpp"

#include "config/settings.hpp"
#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/process.hpp"
#include "utils/string.hpp"
#include "utils/time.hpp"

#include <ai/http/client.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace cc::plugins {
namespace {

/// The name of the folder the plugins are kept in.
constexpr std::string_view kPluginsDirectoryName = "plugins";

/// The name of the folder the marketplaces are fetched into.
constexpr std::string_view kMarketplacesDirectoryName = "marketplaces";

/// The name of the folder the installed plugins are cached in.
constexpr std::string_view kCacheDirectoryName = "cache";

/// The name of the folder the data of a plugin is kept in.
constexpr std::string_view kDataDirectoryName = "data";

/// The name of the file the known marketplaces are written to.
constexpr std::string_view kKnownMarketplacesFileName = "known_marketplaces.json";

/// The name of the file the number of the installs is kept in.
constexpr std::string_view kInstallCountsFileName = "install-counts-cache.json";

/// The address the number of the installs comes from.
///
/// The reference takes the numbers from the statistics of the marketplace of
/// Anthropic.
constexpr std::string_view kInstallCountsUrl =
    "https://raw.githubusercontent.com/anthropics/claude-plugins-official/refs/heads/stats/stats/"
    "plugin-installs.json";

/// The version of the file the number of the installs is kept in.
constexpr int64_t kInstallCountsVersion = 1;

/// The time of now, as the reference writes it.
///
/// The stamp of the reference has the milliseconds in it:
/// `2026-01-02T03:04:05.678Z`.
std::string iso_stamp_now() {
    const std::string stamp = cc::format_iso_now();
    if (stamp.size() < 20 || stamp.back() != 'Z') return stamp;

    std::ostringstream out;
    out << stamp.substr(0, stamp.size() - 1) << '.' << std::setw(3) << std::setfill('0')
        << (cc::now_ms() % 1000) << 'Z';
    return out.str();
}

/// The number of the milliseconds a day has.
///
/// The numbers of the installs are good for a day, the way the reference
/// keeps them good for a day.
constexpr int64_t kInstallCountsTtlMs = 24 * 60 * 60 * 1000;

/// The folder of the manifest of a plugin, inside the plugin.
constexpr std::string_view kManifestDirectoryName = ".claude-plugin";

/// The name of the file the manifest of a plugin is written to.
constexpr std::string_view kManifestFileName = "plugin.json";

/// The name of the file the manifest of a marketplace is written to.
constexpr std::string_view kMarketplaceManifestFileName = "marketplace.json";

/// The name of the file the hooks of a plugin are written to.
constexpr std::string_view kPluginHooksFileName = "hooks.json";

/// The environment variable that moves the folder of the plugins.
constexpr std::string_view kCacheDirVariable = "CLAUDE_CODE_PLUGIN_CACHE_DIR";

/// Replaces the characters that cannot be in the name of a folder.
///
/// The reference sanitises the name of a plugin the same way, so that the
/// two agree on the name of the folder it is cached in.
std::string sanitize(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_';
        out += plain ? c : '-';
    }
    return out;
}

/// The settings of the user, as they are on the disk.
boost::json::object user_settings() {
    const std::string path = cc::path_join(cc::claude_config_dir(), "settings.json");
    const auto contents = cc::read_file(path);
    if (!contents) return {};

    const boost::json::value parsed = cc::json_parse(*contents);
    if (!parsed.is_object()) return {};
    return parsed.as_object();
}

/// Writes `value` under `key` in the settings of the user, keeping the rest
/// of the file as it was.
bool write_user_setting(const std::string& key, const boost::json::value& value,
                        std::string* error) {
    const std::string path = cc::path_join(cc::claude_config_dir(), "settings.json");

    boost::json::object root;
    if (const auto text = cc::read_file(path)) {
        const auto parsed = cc::json_parse(*text);
        if (parsed.is_object()) root = parsed.as_object();
    }

    root[key] = value;
    return config::write_settings(path, boost::json::value(std::move(root)), error);
}

/// The source of a marketplace, the way the reference reads it.
///
/// The source is either the address itself or an object that holds it.
std::string source_of(const boost::json::object& marketplace) {
    const boost::json::value* source = nullptr;
    for (const auto& [key, value] : marketplace) {
        if (key == "source") {
            source = &value;
            break;
        }
    }
    if (source == nullptr) return {};

    if (source->is_string()) return source->as_string().c_str();
    if (!source->is_object()) return {};

    // The source of a marketplace is an object that names the kind of it
    // and where it comes from: the reference shows the place, not the kind.
    const auto& object = source->as_object();
    for (const auto& key : {"repo", "url", "path"}) {
        const std::string value = jget_string(object, key);
        if (!value.empty()) return value;
    }
    return jget_string(object, "source");
}

/// The folder a marketplace was fetched into, when the file of the known
/// marketplaces names one.
std::string install_location_of(const boost::json::object& marketplace) {
    return jget_string(marketplace, "installLocation");
}

/// The name of a marketplace that comes from an address.
std::string name_of_source(const std::string& source) {
    std::string name = source;
    while (!name.empty() && (name.back() == '/' || name.back() == '\\')) name.pop_back();

    const size_t slash = name.find_last_of("/\\:");
    if (slash != std::string::npos) name = name.substr(slash + 1);

    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);

    return name;
}

/// The manifest of a plugin, read from the folder of the plugin.
///
/// A plugin without a manifest is still a plugin: the reference gives it the
/// name of the folder it lives in.
boost::json::object read_manifest(const std::string& plugin_path,
                                  const std::string& fallback_name) {
    const std::string path =
        cc::path_join(cc::path_join(plugin_path, kManifestDirectoryName), kManifestFileName);
    const auto contents = cc::read_file(path);
    if (!contents) return {};

    const boost::json::value parsed = cc::json_parse(*contents);
    if (!parsed.is_object()) return {};

    boost::json::object manifest = parsed.as_object();
    if (jget_string(manifest, "name").empty()) manifest["name"] = boost::json::value(fallback_name);
    return manifest;
}

/// The version of a plugin, from its manifest or from the folder it is in.
std::string version_of(const boost::json::object& manifest, const std::string& fallback) {
    const std::string version = jget_string(manifest, "version");
    if (!version.empty()) return version;
    return fallback;
}

/// The folder of the plugin of a marketplace entry.
///
/// The source of an entry is either a path relative to the root of the
/// marketplace, or an object that names a kind of source.
std::string resolve_marketplace_plugin_path(const std::string& marketplace_root,
                                             const std::string& source) {
    if (source.empty()) return {};
    if (source.front() == '/') return cc::normalize(source);
    if (source.front() == '~') return cc::normalize(cc::expand_home(source));
    return cc::normalize(cc::path_join(marketplace_root, source));
}

/// The manifest of a marketplace, read from the folder it was fetched into.
///
/// The reference looks for the file `.claude-plugin/marketplace.json` inside
/// the folder of the marketplace; a marketplace fetched from an address is
/// the file itself.
boost::json::object read_marketplace_manifest(const std::string& location) {
    if (location.empty()) return {};

    if (cc::is_file(location)) {
        const auto contents = cc::read_file(location);
        if (!contents) return {};
        const boost::json::value parsed = cc::json_parse(*contents);
        if (!parsed.is_object()) return {};
        return parsed.as_object();
    }

    const std::string path = cc::path_join(
        cc::path_join(location, kManifestDirectoryName), kMarketplaceManifestFileName);
    const auto contents = cc::read_file(path);
    if (!contents) return {};

    const boost::json::value parsed = cc::json_parse(*contents);
    if (!parsed.is_object()) return {};
    return parsed.as_object();
}

/// The folder of a marketplace, given the marketplace itself.
///
/// The folder is the one the file of the known marketplaces names, the one
/// the source names, or the one the marketplaces are fetched into.
std::string marketplace_root(const Marketplace& marketplace) {
    if (!marketplace.install_location.empty()) {
        if (cc::is_directory(marketplace.install_location)) return marketplace.install_location;
    }

    if (!marketplace.source.empty() && !cc::is_url(marketplace.source) &&
        !cc::starts_with(marketplace.source, "git@") && !cc::starts_with(marketplace.source, "git+")) {
        const std::string expanded = cc::expand_home(marketplace.source);
        if (cc::is_directory(expanded)) {
            // The source may name the folder of the marketplace or the file
            // of its manifest.
            if (cc::is_file(expanded)) return cc::dirname(expanded);
            return expanded;
        }
    }

    return cc::path_join(
        cc::path_join(plugins_directory(), kMarketplacesDirectoryName), sanitize(marketplace.name));
}

/// The description of a command of a plugin, when the file names none.
///
/// The reference takes the first line of the file, without the `#` of a
/// heading.
std::string description_from_content(const std::string& content) {
    for (const auto& line : cc::split_lines(content)) {
        const std::string trimmed = cc::trim(line);
        if (trimmed.empty()) continue;

        std::string text = trimmed;
        if (!text.empty() && text.front() == '#') {
            size_t k = 0;
            while (k < text.size() && text[k] == '#') ++k;
            while (k < text.size() && text[k] == ' ') ++k;
            text = text.substr(k);
        }

        if (text.size() > 100) text = text.substr(0, 97) + "...";
        return text;
    }
    return "Plugin command";
}

/// The name a command of a plugin gets.
///
/// The command is named after the plugin: the file `build.md` of the plugin
/// `my-plugin` is the command `my-plugin:build`. A command in a folder of
/// the plugin keeps the folder in its name, the folders joined by colons.
std::string plugin_command_name(const std::string& path, const std::string& base,
                                const std::string& plugin) {
    std::string relative = cc::relative(path, base);
    if (!relative.empty() && relative.front() == '.') relative.erase(0, 1);
    while (!relative.empty() && relative.front() == '/') relative.erase(0, 1);

    if (cc::ends_with(relative, ".md")) relative.erase(relative.size() - 3);

    std::string name = plugin;
    for (const auto& part : cc::split(relative, '/')) {
        if (part.empty()) continue;
        name += ":";
        name += part;
    }
    return name;
}

/// The commands a plugin brings.
///
/// The commands are the markdown files of the folder `commands` of the
/// plugin; a skill is a file named `SKILL.md` in a folder of its own, and
/// the folder names it.
std::vector<PluginCommand> commands_of(const Plugin& plugin) {
    std::vector<PluginCommand> out;

    const std::string commands_path = cc::path_join(plugin.path, "commands");
    for (const std::string& path : cc::walk_files(commands_path)) {
        if (!cc::ends_with(path, ".md")) continue;

        const auto text = cc::read_file(path);
        if (!text) continue;

        PluginCommand command;
        command.plugin = plugin.name;
        command.marketplace = plugin.marketplace;
        command.plugin_root = plugin.path;
        command.path = path;

        // A skill is named after the folder it is in.
        const std::string file_name = cc::basename(path);
        const bool is_skill = cc::iequals(file_name, "skill.md");
        command.is_skill = is_skill;

        std::string base = commands_path;
        if (is_skill) {
            base = cc::dirname(cc::dirname(path));
            command.name = plugin.name + ":" + cc::basename(cc::dirname(path));
        } else {
            command.name = plugin_command_name(path, commands_path, plugin.name);
        }

        // The frontmatter of the file names the description and the hint
        // about the arguments.
        const boost::json::object frontmatter = agents::parse_frontmatter(*text);
        command.description = jget_string(frontmatter, "description");
        command.argument_hint = jget_string(frontmatter, "argument-hint");

        // The body of the file is the prompt; the frontmatter is not part of
        // it.
        const std::vector<std::string> lines = cc::split_lines(*text);
        size_t start = 0;
        if (!lines.empty() && cc::trim(lines.front()) == "---") {
            for (size_t i = 1; i < lines.size(); ++i) {
                if (cc::trim(lines[i]) == "---") {
                    start = i + 1;
                    break;
                }
            }
        }
        std::string body;
        for (size_t i = start; i < lines.size(); ++i) {
            if (!body.empty()) body += "\n";
            body += lines[i];
        }
        command.content = body;

        if (command.description.empty()) command.description = description_from_content(body);

        out.push_back(std::move(command));
    }

    // The skills of the plugin live in a folder of their own.
    const std::string skills_path = cc::path_join(plugin.path, "skills");
    for (const std::string& entry : cc::list_dir(skills_path)) {
        const std::string directory = cc::path_join(skills_path, entry);
        if (!cc::is_directory(directory)) continue;

        const std::string path = cc::path_join(directory, "SKILL.md");
        const auto text = cc::read_file(path);
        if (!text) continue;

        PluginCommand command;
        command.plugin = plugin.name;
        command.marketplace = plugin.marketplace;
        command.plugin_root = plugin.path;
        command.path = path;
        command.is_skill = true;
        command.name = plugin.name + ":" + entry;

        const boost::json::object frontmatter = agents::parse_frontmatter(*text);
        command.description = jget_string(frontmatter, "description");
        command.argument_hint = jget_string(frontmatter, "argument-hint");

        const std::vector<std::string> lines = cc::split_lines(*text);
        size_t start = 0;
        if (!lines.empty() && cc::trim(lines.front()) == "---") {
            for (size_t i = 1; i < lines.size(); ++i) {
                if (cc::trim(lines[i]) == "---") {
                    start = i + 1;
                    break;
                }
            }
        }
        std::string body;
        for (size_t i = start; i < lines.size(); ++i) {
            if (!body.empty()) body += "\n";
            body += lines[i];
        }
        command.content = body;

        if (command.description.empty()) command.description = description_from_content(body);

        out.push_back(std::move(command));
    }

    return out;
}

/// The agents a plugin brings.
///
/// The agents are the markdown files of the folder `agents` of the plugin.
std::vector<agents::Definition> agents_of(const Plugin& plugin) {
    std::vector<agents::Definition> out;

    const std::string agents_path = cc::path_join(plugin.path, "agents");
    for (const std::string& entry : cc::list_dir(agents_path)) {
        if (!cc::ends_with(entry, ".md")) continue;

        const std::string path = cc::path_join(agents_path, entry);
        const auto contents = cc::read_file(path);
        if (!contents) continue;

        const boost::json::object frontmatter = agents::parse_frontmatter(*contents);
        agents::Definition definition;
        definition.name = jget_string(frontmatter, "name");
        definition.description = jget_string(frontmatter, "description");
        if (definition.name.empty()) continue;

        // The body of the file is the prompt of the agent.
        const std::vector<std::string> lines = cc::split_lines(*contents);
        size_t start = 0;
        if (!lines.empty() && cc::trim(lines.front()) == "---") {
            for (size_t i = 1; i < lines.size(); ++i) {
                if (cc::trim(lines[i]) == "---") {
                    start = i + 1;
                    break;
                }
            }
        }
        std::string body;
        for (size_t i = start; i < lines.size(); ++i) {
            if (!body.empty()) body += "\n";
            body += lines[i];
        }
        definition.prompt = cc::trim(body);
        definition.path = path;

        out.push_back(std::move(definition));
    }

    return out;
}

/// The hooks a plugin brings.
///
/// The hooks are the file `hooks/hooks.json` of the plugin; it holds them in
/// the same shape as the settings hold them.
std::vector<hooks::Hook> hooks_of(const Plugin& plugin) {
    std::vector<hooks::Hook> out;

    const std::string path =
        cc::path_join(cc::path_join(plugin.path, "hooks"), kPluginHooksFileName);
    const auto contents = cc::read_file(path);
    if (!contents) return out;

    const boost::json::value parsed = cc::json_parse(*contents);
    if (!parsed.is_object()) return out;

    hooks::Registry registry;
    registry.configure(parsed.as_object());
    for (const auto event : {hooks::Event::PreToolUse, hooks::Event::PostToolUse,
                             hooks::Event::UserPromptSubmit, hooks::Event::Notification,
                             hooks::Event::Stop, hooks::Event::SubagentStop,
                             hooks::Event::PreCompact, hooks::Event::SessionStart,
                             hooks::Event::SessionEnd}) {
        for (auto hook : registry.for_event(event)) {
            // The plugin gives its hooks the place it lives in, the way the
            // reference does.
            hook.command = substitute_plugin_variables(hook.command, plugin.path, plugin.id);
            out.push_back(std::move(hook));
        }
    }

    return out;
}

/// The plugins that are on.
std::vector<Plugin> enabled_plugins() {
    std::vector<Plugin> out;
    for (const Plugin& plugin : installed_plugins()) {
        if (plugin.enabled) out.push_back(plugin);
    }
    return out;
}

/// The name of a markdown file of a folder of a plugin.
///
/// The reference names a command and an agent after the file it was read
/// from, without the `.md` of it.
std::string base_name_of_markdown(const std::string& name) {
    if (!cc::ends_with(name, ".md")) return name;
    return name.substr(0, name.size() - 3);
}

/// The name of a skill of a plugin.
///
/// The skill that is the folder of the skills itself is named after the
/// frontmatter of its file; the name of the plugin is taken off it when it
/// is there, the way the reference takes it off.
std::string skill_name_of(const std::string& file, const std::string& fallback,
                           const std::string& plugin) {
    const auto contents = cc::read_file(file);
    if (!contents) return fallback;

    const boost::json::object frontmatter = agents::parse_frontmatter(*contents);
    std::string name = jget_string(frontmatter, "name");
    if (name.empty()) return fallback;

    const std::string prefix = plugin + ":";
    if (cc::starts_with(name, prefix)) name.erase(0, prefix.size());
    return name;
}

/// The components the plugin in the folder brings.
///
/// The reference reads the commands and the agents from the markdown files
/// of the folders of the plugin, the skills from the folders that hold a
/// file named `SKILL.md`, the hooks from the file `hooks/hooks.json` and
/// the servers of MCP from the file `.mcp.json`.
PluginComponents components_at(const std::string& plugin_path, const std::string& plugin) {
    PluginComponents out;
    if (plugin_path.empty()) return out;

    // The commands are the markdown files of the folder `commands`.
    for (const std::string& entry : cc::list_dir(cc::path_join(plugin_path, "commands"))) {
        if (!cc::ends_with(entry, ".md")) continue;
        out.commands.push_back(base_name_of_markdown(entry));
    }

    // The agents are the markdown files of the folder `agents`.
    for (const std::string& entry : cc::list_dir(cc::path_join(plugin_path, "agents"))) {
        if (!cc::ends_with(entry, ".md")) continue;
        out.agents.push_back(base_name_of_markdown(entry));
    }

    // The skills are the folders of the folder `skills` that hold a file
    // named `SKILL.md`.
    const std::string skills_path = cc::path_join(plugin_path, "skills");
    const std::string skill_file = cc::path_join(skills_path, "SKILL.md");
    if (cc::is_file(skill_file)) {
        // The folder of the skills is a skill of its own.
        out.skills.push_back(skill_name_of(skill_file, cc::basename(skills_path), plugin));
    } else {
        for (const std::string& entry : cc::list_dir(skills_path)) {
            const std::string directory = cc::path_join(skills_path, entry);
            if (!cc::is_directory(directory)) continue;
            if (!cc::is_file(cc::path_join(directory, "SKILL.md"))) continue;
            out.skills.push_back(entry);
        }
    }

    // The hooks are the events of the file `hooks/hooks.json`.
    const std::string hooks_path =
        cc::path_join(cc::path_join(plugin_path, "hooks"), kPluginHooksFileName);
    if (const auto contents = cc::read_file(hooks_path)) {
        const boost::json::value parsed = cc::json_parse(*contents);
        if (parsed.is_object()) {
            for (const auto& [name, matchers] : jget_object(parsed.as_object(), "hooks")) {
                (void)matchers;
                out.hooks.push_back(std::string(name));
            }
        }
    }

    // The servers of MCP are the ones of the file `.mcp.json`.
    if (const auto contents = cc::read_file(cc::path_join(plugin_path, ".mcp.json"))) {
        const boost::json::value parsed = cc::json_parse(*contents);
        if (parsed.is_object()) {
            for (const auto& [name, server] : jget_object(parsed.as_object(), "mcpServers")) {
                (void)server;
                out.mcp_servers.push_back(std::string(name));
            }
        }
    }

    return out;
}

/// The kind of a source.
enum class SourceKind {
    /// A folder on this machine.
    Local,
    /// A file that is downloaded.
    Url,
    /// A repository that is cloned.
    Git,
};

/// The source of a marketplace, split into its kind and its location.
struct Source {
    SourceKind kind = SourceKind::Local;
    /// The address, the repository or the folder.
    std::string location;
    /// The branch or the tag of the repository.
    std::string reference;
};

/// Reads the source of a marketplace.
///
/// The source is either the address itself or an object that holds it; the
/// object names the kind of the source.
Source read_source(const boost::json::object& marketplace) {
    const boost::json::value* source = nullptr;
    for (const auto& [key, value] : marketplace) {
        if (key == "source") {
            source = &value;
            break;
        }
    }
    if (source == nullptr) return {};

    if (source->is_string()) {
        Source out;
        out.location = source->as_string().c_str();
        // An address that names a repository is cloned; anything else is a
        // folder of this machine.
        if (cc::is_url(out.location) && !cc::ends_with(out.location, ".json")) {
            out.kind = SourceKind::Git;
        } else if (cc::is_url(out.location)) {
            out.kind = SourceKind::Url;
        }
        return out;
    }

    if (!source->is_object()) return {};

    const boost::json::object& object = source->as_object();
    const std::string kind = jget_string(object, "source");

    Source out;
    if (kind == "url") {
        out.kind = SourceKind::Url;
        out.location = jget_string(object, "url");
        return out;
    }
    if (kind == "github") {
        out.kind = SourceKind::Git;
        const std::string repo = jget_string(object, "repo");
        out.location = repo.empty() ? repo : "https://github.com/" + repo + ".git";
        out.reference = jget_string(object, "ref");
        return out;
    }
    if (kind == "git") {
        out.kind = SourceKind::Git;
        out.location = jget_string(object, "url");
        out.reference = jget_string(object, "ref");
        return out;
    }

    // A source that names neither is a folder of this machine.
    out.kind = SourceKind::Local;
    for (const auto& key : {"path", "repo"}) {
        const std::string value = jget_string(object, key);
        if (!value.empty()) {
            out.location = value;
            break;
        }
    }
    return out;
}

/// The folder the marketplaces are fetched into.
std::string marketplaces_directory() {
    return cc::path_join(plugins_directory(), kMarketplacesDirectoryName);
}

/// The source of a marketplace, read from where the marketplace is written.
///
/// A marketplace is written either in the settings of the user or in the
/// file of the known marketplaces; the settings come first.
Source read_source_of(const std::string& marketplace) {
    const boost::json::object settings = user_settings();
    const boost::json::object entry =
        jget_object(jget_object(settings, "extraKnownMarketplaces"), marketplace);
    if (!entry.empty()) return read_source(entry);

    const std::string path = cc::path_join(plugins_directory(), kKnownMarketplacesFileName);
    const auto contents = cc::read_file(path);
    if (contents) {
        const boost::json::value parsed = cc::json_parse(*contents);
        if (parsed.is_object()) {
            const boost::json::object object = jget_object(parsed.as_object(), marketplace);
            if (!object.empty()) return read_source(object);
        }
    }

    return {};
}

/// Remembers where a marketplace was fetched to.
///
/// The place is written where the marketplace itself is written: the
/// settings of the user when the marketplace is named there, and the file
/// of the known marketplaces otherwise. The rest of the file is left alone.
bool remember_install_location(const std::string& marketplace, const std::string& location,
                                std::string* error) {
    const boost::json::object settings = user_settings();
    boost::json::object entries = jget_object(settings, "extraKnownMarketplaces");

    const auto known = entries.find(marketplace);
    if (known != entries.end()) {
        boost::json::object entry = jget_object(entries, marketplace);
        entry["installLocation"] = boost::json::value(location);
        entries[marketplace] = std::move(entry);
        return write_user_setting("extraKnownMarketplaces",
                                  boost::json::value(std::move(entries)), error);
    }

    // The file of the known marketplaces.
    const std::string path = cc::path_join(plugins_directory(), kKnownMarketplacesFileName);
    boost::json::object root;
    if (const auto contents = cc::read_file(path)) {
        const boost::json::value parsed = cc::json_parse(*contents);
        if (parsed.is_object()) root = parsed.as_object();
    }

    boost::json::object entry = jget_object(root, marketplace);
    if (entry.empty()) return true;
    entry["installLocation"] = boost::json::value(location);
    root[marketplace] = std::move(entry);

    return cc::write_file(path, cc::json_dump_pretty(root) + "\n", error);
}

/// Downloads a file and writes it to `path`.
bool download(boost::asio::io_context& io_context, const std::string& url,
              const std::string& path, std::string* error) {
    ai::http::HttpClient client(io_context);

    ai::http::Headers headers;
    headers["Accept"] = "application/json";
    headers["User-Agent"] = "Claude-Code-Plugin-Manager";

    auto task = client.get(url, headers);
    task.start();
    while (!task.done()) {
        io_context.run_one_for(std::chrono::milliseconds(20));
    }

    try {
        const ai::http::HttpResponse response = task.get();
        if (response.status_code != 200) {
            if (error != nullptr) {
                *error = "The marketplace could not be downloaded: HTTP " +
                         std::to_string(response.status_code);
            }
            return false;
        }
        return cc::write_file(path, response.body, error);
    } catch (const std::exception& failure) {
        if (error != nullptr) {
            std::string message = "The marketplace could not be downloaded: ";
            message += failure.what();
            *error = message;
        }
        return false;
    }
}

/// Clones a repository into `path`, or fetches it again when it is already
/// there.
bool clone(const std::string& url, const std::string& path, const std::string& reference,
           std::string* error) {
    if (!cc::mkdirp(cc::dirname(path))) {
        if (error != nullptr) *error = "The folder of the marketplaces could not be made";
        return false;
    }

    if (cc::is_directory(cc::path_join(path, ".git"))) {
        const std::string command = reference.empty() ? "git pull --ff-only"
                                                      : "git pull --ff-only origin " + reference;
        const cc::ProcessOutcome outcome = cc::run_process(command, path, "", 120);
        if (outcome.exit_code != 0) {
            if (error != nullptr) {
                *error = "The marketplace could not be fetched: " + cc::trim(outcome.err);
            }
            return false;
        }
        return true;
    }

    cc::remove_dir_all(path);

    std::string command = "git clone --depth 1";
    if (!reference.empty()) command += " --branch " + reference;
    command += " " + url + " " + path;

    const cc::ProcessOutcome outcome = cc::run_process(command, cc::dirname(path), "", 300);
    if (outcome.exit_code != 0) {
        if (error != nullptr) {
            *error = "The marketplace could not be cloned: " + cc::trim(outcome.err);
        }
        return false;
    }
    return true;
}

} // namespace

std::string plugins_directory() {
    if (const char* override_dir = std::getenv(std::string(kCacheDirVariable).c_str())) {
        if (*override_dir != '\0') return cc::expand_home(override_dir);
    }
    return cc::path_join(cc::claude_config_dir(), kPluginsDirectoryName);
}

std::string plugin_cache_directory() {
    return cc::path_join(plugins_directory(), kCacheDirectoryName);
}

std::vector<Marketplace> known_marketplaces() {
    std::vector<Marketplace> marketplaces;

    // The marketplaces of the settings of the user.
    const boost::json::object settings = user_settings();
    const boost::json::object extra = jget_object(settings, "extraKnownMarketplaces");
    for (const auto& [name, value] : extra) {
        if (!value.is_object()) continue;
        Marketplace marketplace;
        marketplace.name = name;
        marketplace.source = source_of(value.as_object());
        marketplace.install_location = install_location_of(value.as_object());
        marketplace.auto_update = jget_bool(value.as_object(), "autoUpdate");
        marketplace.from_settings = true;
        marketplaces.push_back(std::move(marketplace));
    }

    // The marketplaces of the file of the known marketplaces. The ones the
    // settings of the user name are left out: the settings win.
    const std::string path =
        cc::path_join(plugins_directory(), kKnownMarketplacesFileName);
    const auto contents = cc::read_file(path);
    if (contents) {
        const boost::json::value parsed = cc::json_parse(*contents);
        if (parsed.is_object()) {
            for (const auto& [name, value] : parsed.as_object()) {
                if (!value.is_object()) continue;
                const bool known = std::any_of(
                    marketplaces.begin(), marketplaces.end(),
                    [&](const Marketplace& marketplace) { return marketplace.name == name; });
                if (known) continue;

                Marketplace marketplace;
                marketplace.name = name;
                marketplace.source = source_of(value.as_object());
                marketplace.install_location = install_location_of(value.as_object());
                marketplace.last_updated = jget_string(value.as_object(), "lastUpdated");
                marketplace.auto_update = jget_bool(value.as_object(), "autoUpdate");
                marketplaces.push_back(std::move(marketplace));
            }
        }
    }

    return marketplaces;
}

std::vector<MarketplacePlugin> marketplace_plugins(const std::string& marketplace) {
    std::vector<MarketplacePlugin> out;

    const std::vector<Marketplace> known = known_marketplaces();
    const Marketplace* found = nullptr;
    for (const Marketplace& candidate : known) {
        if (candidate.name == marketplace) {
            found = &candidate;
            break;
        }
    }
    if (found == nullptr) return out;

    const std::string root = marketplace_root(*found);
    const boost::json::object manifest = read_marketplace_manifest(
        found->install_location.empty() ? root : found->install_location);
    if (manifest.empty()) return out;

    for (const auto& value : jget_array(manifest, "plugins")) {
        if (!value.is_object()) continue;
        const boost::json::object entry = value.as_object();

        MarketplacePlugin plugin;
        plugin.name = jget_string(entry, "name");
        if (plugin.name.empty()) continue;
        plugin.description = jget_string(entry, "description");
        plugin.version = jget_string(entry, "version");
        plugin.category = jget_string(entry, "category");
        plugin.marketplace = marketplace;

        // The source is either a path or an object that names a kind of
        // source; a path names the folder of the plugin.
        const boost::json::value* source = nullptr;
        for (const auto& [key, member] : entry) {
            if (key == "source") {
                source = &member;
                break;
            }
        }
        if (source != nullptr) {
            if (source->is_string()) {
                plugin.source = source->as_string().c_str();
                plugin.source_kind = "path";
                plugin.path = resolve_marketplace_plugin_path(root, plugin.source);
            } else if (source->is_object()) {
                const boost::json::object& object = source->as_object();
                plugin.source_kind = jget_string(object, "source");

                if (plugin.source_kind == "github") {
                    const std::string repo = jget_string(object, "repo");
                    plugin.source_url = repo.empty() ? repo : "https://github.com/" + repo + ".git";
                } else if (plugin.source_kind == "git" || plugin.source_kind == "url" ||
                           plugin.source_kind == "git-subdir") {
                    plugin.source_url = jget_string(object, "url");
                }

                plugin.source_path = jget_string(object, "path");
                if (plugin.source.empty()) plugin.source = plugin.source_url;
            }
        }

        out.push_back(std::move(plugin));
    }

    return out;
}

std::vector<Plugin> installed_plugins() {
    std::vector<Plugin> out;

    const boost::json::object settings = user_settings();
    const boost::json::object enabled = jget_object(settings, "enabledPlugins");

    const std::string cache = plugin_cache_directory();
    if (!cc::is_directory(cache)) return out;

    // The cache holds the plugins in the folders of their marketplace, of
    // their name and of their version.
    for (const std::string& marketplace : cc::list_dir(cache)) {
        const std::string marketplace_path = cc::path_join(cache, marketplace);
        if (!cc::is_directory(marketplace_path)) continue;

        for (const std::string& name : cc::list_dir(marketplace_path)) {
            const std::string plugin_path = cc::path_join(marketplace_path, name);
            if (!cc::is_directory(plugin_path)) continue;

            for (const std::string& version : cc::list_dir(plugin_path)) {
                const std::string version_path = cc::path_join(plugin_path, version);
                if (!cc::is_directory(version_path)) continue;

                const boost::json::object manifest = read_manifest(version_path, name);

                Plugin plugin;
                plugin.name = jget_string(manifest, "name");
                if (plugin.name.empty()) plugin.name = name;
                plugin.marketplace = marketplace;
                plugin.id = plugin.name + "@" + marketplace;
                plugin.version = version_of(manifest, version);
                plugin.description = jget_string(manifest, "description");
                plugin.path = version_path;

                const auto setting = enabled.find(plugin.id);
                plugin.enabled =
                    setting != enabled.end() ? setting->value().is_bool() && setting->value().as_bool()
                                             : false;

                out.push_back(std::move(plugin));
            }
        }
    }

    std::sort(out.begin(), out.end(), [](const Plugin& a, const Plugin& b) {
        if (a.marketplace != b.marketplace) return a.marketplace < b.marketplace;
        return a.name < b.name;
    });

    return out;
}

std::vector<PluginOption> plugin_options(const Plugin& plugin) {
    std::vector<PluginOption> options;

    const std::string path =
        cc::path_join(cc::path_join(plugin.path, kManifestDirectoryName), kManifestFileName);
    const auto contents = cc::read_file(path);
    if (!contents) return options;

    const boost::json::value parsed = cc::json_parse(*contents);
    if (!parsed.is_object()) return options;

    for (const auto& [key, value] : jget_object(parsed.as_object(), "userConfig")) {
        if (!value.is_object()) continue;
        const boost::json::object object = value.as_object();

        PluginOption option;
        option.key = std::string(key);
        option.type = jget_string(object, "type", "string");
        option.title = jget_string(object, "title");
        option.description = jget_string(object, "description");
        option.required = jget_bool(object, "required", false);
        option.multiple = jget_bool(object, "multiple", false);
        option.sensitive = jget_bool(object, "sensitive", false);

        // The iterator of the member is compared with the end of the object:
        // the end is a pointer of its own, so it is never null.
        const auto default_value = object.find("default");
        if (default_value != object.end()) {
            if (default_value->value().is_string()) {
                option.default_value = default_value->value().as_string().c_str();
            } else if (default_value->value().is_bool()) {
                option.default_value = default_value->value().as_bool() ? "true" : "false";
            } else if (default_value->value().is_int64()) {
                option.default_value = std::to_string(default_value->value().as_int64());
            }
        }

        options.push_back(std::move(option));
    }

    return options;
}

boost::json::object load_plugin_options(const std::string& plugin_id) {
    const boost::json::object settings = user_settings();
    const boost::json::object configs = jget_object(settings, "pluginConfigs");
    const boost::json::object entry = jget_object(configs, plugin_id);
    return jget_object(entry, "options");
}

bool save_plugin_options(const std::string& plugin_id, const boost::json::object& values,
                         std::string* error) {
    const boost::json::object settings = user_settings();
    boost::json::object configs = jget_object(settings, "pluginConfigs");

    boost::json::object entry = jget_object(configs, plugin_id);
    entry["options"] = boost::json::value(values);
    configs[plugin_id] = std::move(entry);

    return write_user_setting("pluginConfigs", boost::json::value(std::move(configs)), error);
}

PluginComponents plugin_components(const std::string& plugin_id) {
    for (const Plugin& plugin : installed_plugins()) {
        if (plugin.id != plugin_id) continue;
        return components_at(plugin.path, plugin.name);
    }
    return {};
}

PluginComponents marketplace_plugin_components(const std::string& marketplace,
                                                const std::string& plugin) {
    for (const MarketplacePlugin& entry : marketplace_plugins(marketplace)) {
        if (entry.name != plugin) continue;

        PluginComponents out = components_at(entry.path, entry.name);

        // A plugin that the marketplace does not hold on this machine has
        // components that are found when it is installed.
        if (entry.path.empty() || !cc::is_directory(entry.path)) out.unknown = true;
        return out;
    }
    return {};
}

std::vector<PluginCommand> plugin_commands() {
    std::vector<PluginCommand> out;
    for (const Plugin& plugin : enabled_plugins()) {
        for (PluginCommand& command : commands_of(plugin)) out.push_back(std::move(command));
    }
    return out;
}

std::vector<agents::Definition> plugin_agents() {
    std::vector<agents::Definition> out;
    for (const Plugin& plugin : enabled_plugins()) {
        for (agents::Definition& definition : agents_of(plugin)) out.push_back(std::move(definition));
    }
    return out;
}

std::vector<hooks::Hook> plugin_hooks() {
    std::vector<hooks::Hook> out;
    for (const Plugin& plugin : enabled_plugins()) {
        for (hooks::Hook& hook : hooks_of(plugin)) out.push_back(std::move(hook));
    }
    return out;
}

boost::json::object plugin_mcp_servers() {
    boost::json::object out;

    for (const Plugin& plugin : enabled_plugins()) {
        const std::string path = cc::path_join(plugin.path, ".mcp.json");
        const auto contents = cc::read_file(path);
        if (!contents) continue;

        const boost::json::value parsed = cc::json_parse(*contents);
        if (!parsed.is_object()) continue;

        for (const auto& [name, server] : jget_object(parsed.as_object(), "mcpServers")) {
            if (!server.is_object()) continue;

            // The servers of a plugin are named after it, so that two
            // plugins cannot name a server the same way.
            out["plugin:" + plugin.name + ":" + std::string(name)] = server;
        }
    }

    return out;
}

std::optional<std::map<std::string, int64_t>> install_counts() {
    const std::string path = cc::path_join(plugins_directory(), kInstallCountsFileName);
    const auto contents = cc::read_file(path);
    if (!contents) return std::nullopt;

    const boost::json::value parsed = cc::json_parse(*contents);
    if (!parsed.is_object()) return std::nullopt;
    const boost::json::object root = parsed.as_object();

    // The file of another version is not read: its shape is not the one the
    // reference knows.
    if (jget_int(root, "version") != kInstallCountsVersion) return std::nullopt;

    // The numbers are good for a day; the ones that are older are fetched
    // again.
    const int64_t fetched_at = cc::parse_timestamp(jget_string(root, "fetchedAt"));
    if (fetched_at <= 0) return std::nullopt;
    if (cc::now_ms() - fetched_at > kInstallCountsTtlMs) return std::nullopt;

    std::map<std::string, int64_t> out;
    for (const boost::json::value& value : jget_array(root, "counts")) {
        if (!value.is_object()) continue;
        const boost::json::object entry = value.as_object();
        const std::string plugin = jget_string(entry, "plugin");
        if (plugin.empty()) continue;
        out[plugin] = jget_int(entry, "unique_installs");
    }
    if (out.empty()) return std::nullopt;

    return out;
}

bool fetch_install_counts(boost::asio::io_context& io_context) {
    // The fetch is given ten seconds, the way the reference gives it.
    ai::http::HttpClientConfig config;
    config.connect_timeout = std::chrono::seconds(10);
    config.read_timeout = std::chrono::seconds(10);
    ai::http::HttpClient client(io_context, config);

    ai::http::Headers headers;
    headers["Accept"] = "application/json";
    headers["User-Agent"] = "Claude-Code-Plugin-Manager";

    // The address is kept in a string of its own: the call is a coroutine,
    // and the text it is given has to live longer than the call.
    const std::string url(kInstallCountsUrl);
    auto task = client.get(url, headers);
    task.start();
    while (!task.done()) {
        io_context.run_one_for(std::chrono::milliseconds(20));
    }

    std::string body;
    try {
        const ai::http::HttpResponse response = task.get();
        if (response.status_code != 200) return false;
        body = response.body;
    } catch (const std::exception&) {
        // A network that is not there is not an error: the numbers that are
        // on the disk are kept.
        return false;
    }

    const boost::json::value parsed = cc::json_parse(body);
    if (!parsed.is_object()) return false;

    boost::json::array counts;
    for (const boost::json::value& value : jget_array(parsed.as_object(), "plugins")) {
        if (!value.is_object()) continue;
        const boost::json::object entry = value.as_object();
        const std::string plugin = jget_string(entry, "plugin");
        if (plugin.empty()) continue;

        boost::json::object item;
        item["plugin"] = boost::json::value(plugin);
        item["unique_installs"] = boost::json::value(jget_int(entry, "unique_installs"));
        counts.emplace_back(std::move(item));
    }
    if (counts.empty()) return false;

    boost::json::object root;
    root["version"] = boost::json::value(kInstallCountsVersion);
    root["fetchedAt"] = boost::json::value(iso_stamp_now());
    root["counts"] = boost::json::value(std::move(counts));

    // The file is written in one go and put in its place afterwards, so
    // that a file that is being written is never read.
    const std::string path = cc::path_join(plugins_directory(), kInstallCountsFileName);
    const std::string temporary = path + ".tmp";
    if (!cc::write_file(temporary, cc::json_dump_pretty(root) + "\n")) return false;

    std::error_code code;
    std::filesystem::rename(temporary, path, code);
    if (code) {
        cc::remove_file(temporary);
        return false;
    }
    return true;
}

std::string format_install_count(int64_t count) {
    if (count < 1000) return std::to_string(count);

    // The number is written with one decimal, and the zero of it is left
    // out, the way the reference writes it.
    const auto one_decimal = [](double value) {
        const double rounded = std::round(value * 10.0) / 10.0;
        std::ostringstream out;
        out << std::fixed << std::setprecision(1) << rounded;
        std::string text = out.str();
        if (text.size() > 2 && text.compare(text.size() - 2, 2, ".0") == 0) {
            text.erase(text.size() - 2);
        }
        return text;
    };

    if (count < 1000000) return one_decimal(static_cast<double>(count) / 1000.0) + "K";
    return one_decimal(static_cast<double>(count) / 1000000.0) + "M";
}

bool install_plugin(const std::string& marketplace, const std::string& plugin,
                    std::string* error) {
    const auto offered = marketplace_plugins(marketplace);
    const MarketplacePlugin* entry = nullptr;
    for (const MarketplacePlugin& candidate : offered) {
        if (candidate.name == plugin) {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr) {
        if (error != nullptr) {
            *error = "No plugin named " + plugin + " in the marketplace " + marketplace;
        }
        return false;
    }

    if (entry->path.empty() || !cc::is_directory(entry->path)) {
        if (error != nullptr) {
            *error = "The plugin " + plugin + " is not held by the marketplace " + marketplace +
                     " on this machine";
        }
        return false;
    }

    const std::string version = entry->version.empty() ? std::string("unknown") : entry->version;
    const std::string destination = cc::path_join(
        cc::path_join(cc::path_join(plugin_cache_directory(), sanitize(marketplace)),
                      sanitize(plugin)),
        sanitize(version));

    if (cc::is_directory(destination)) {
        if (!cc::remove_dir_all(destination)) {
            if (error != nullptr) *error = "The plugin " + plugin + " could not be replaced";
            return false;
        }
    }

    // The cache holds the plugins in a folder for every marketplace, for
    // every plugin and for every version.
    if (!cc::mkdirp(cc::dirname(destination))) {
        if (error != nullptr) *error = "The cache of the plugins could not be made";
        return false;
    }

    if (!cc::copy_recursive(entry->path, destination, error)) {
        if (error != nullptr && error->empty()) {
            *error = "The plugin " + plugin + " could not be copied into the cache";
        }
        return false;
    }

    // The plugin is on once it is installed.
    return set_plugin_enabled(plugin + "@" + marketplace, true, error);
}

bool uninstall_plugin(const std::string& id, std::string* error) {
    const size_t at = id.find('@');
    const std::string name = at == std::string::npos ? id : id.substr(0, at);
    const std::string marketplace = at == std::string::npos ? std::string() : id.substr(at + 1);

    bool removed = false;
    for (const Plugin& plugin : installed_plugins()) {
        if (plugin.name != name) continue;
        if (!marketplace.empty() && plugin.marketplace != marketplace) continue;

        if (!cc::remove_dir_all(plugin.path) && cc::is_directory(plugin.path)) {
            if (error != nullptr) *error = "The plugin " + plugin.id + " could not be removed";
            return false;
        }
        removed = true;
    }

    if (!removed) {
        if (error != nullptr) *error = "No plugin named " + id + " is installed";
        return false;
    }

    // The plugin is forgotten by the settings as well.
    const boost::json::object settings = user_settings();
    boost::json::object enabled = jget_object(settings, "enabledPlugins");
    enabled.erase(id);
    return write_user_setting("enabledPlugins", boost::json::value(std::move(enabled)), error);
}

bool set_plugin_enabled(const std::string& plugin, bool enabled, std::string* error) {
    const boost::json::object settings = user_settings();
    boost::json::object enabled_plugins = jget_object(settings, "enabledPlugins");
    enabled_plugins[plugin] = enabled;

    return write_user_setting("enabledPlugins", boost::json::value(std::move(enabled_plugins)),
                              error);
}

bool add_marketplace(const std::string& name, const std::string& source, std::string* error) {
    const std::string marketplace_name = name.empty() ? name_of_source(source) : name;
    if (marketplace_name.empty()) {
        if (error != nullptr) *error = "The marketplace has no name";
        return false;
    }

    const boost::json::object settings = user_settings();
    boost::json::object marketplaces = jget_object(settings, "extraKnownMarketplaces");

    boost::json::object entry;
    entry["source"] = boost::json::value(source);
    marketplaces[marketplace_name] = std::move(entry);

    return write_user_setting("extraKnownMarketplaces",
                              boost::json::value(std::move(marketplaces)), error);
}

bool remove_marketplace(const std::string& name, std::string* error) {
    const boost::json::object settings = user_settings();
    boost::json::object marketplaces = jget_object(settings, "extraKnownMarketplaces");

    if (marketplaces.find(name) == marketplaces.end()) {
        if (error != nullptr) *error = "No marketplace named " + name;
        return false;
    }
    marketplaces.erase(name);

    return write_user_setting("extraKnownMarketplaces",
                              boost::json::value(std::move(marketplaces)), error);
}

bool set_marketplace_auto_update(const std::string& name, bool enabled, std::string* error) {
    // The wish is kept where the marketplace itself is written: the settings
    // of the user when the marketplace is named there, and the file of the
    // known marketplaces otherwise. The reference writes it in both places.
    const boost::json::object settings = user_settings();
    boost::json::object marketplaces = jget_object(settings, "extraKnownMarketplaces");

    bool written = false;
    if (marketplaces.find(name) != marketplaces.end()) {
        boost::json::object entry = jget_object(marketplaces, name);
        entry["autoUpdate"] = boost::json::value(enabled);
        marketplaces[name] = std::move(entry);
        written = write_user_setting("extraKnownMarketplaces",
                                      boost::json::value(std::move(marketplaces)), error);
    }

    const std::string path = cc::path_join(plugins_directory(), kKnownMarketplacesFileName);
    boost::json::object root;
    if (const auto contents = cc::read_file(path)) {
        const boost::json::value parsed = cc::json_parse(*contents);
        if (parsed.is_object()) root = parsed.as_object();
    }

    boost::json::object entry = jget_object(root, name);
    if (!entry.empty()) {
        entry["autoUpdate"] = boost::json::value(enabled);
        root[name] = std::move(entry);
        if (!cc::write_file(path, cc::json_dump_pretty(root) + "\n", error)) return false;
        written = true;
    }

    if (!written) {
        if (error != nullptr) *error = "No marketplace named " + name;
        return false;
    }

    return true;
}

/// Remembers the time a marketplace was fetched at.
///
/// The reference keeps the time in the file of the known marketplaces.
bool remember_last_updated(const std::string& marketplace, const std::string& stamp,
                           std::string* error) {
    const std::string path = cc::path_join(plugins_directory(), kKnownMarketplacesFileName);
    boost::json::object root;
    if (const auto contents = cc::read_file(path)) {
        const boost::json::value parsed = cc::json_parse(*contents);
        if (parsed.is_object()) root = parsed.as_object();
    }

    boost::json::object entry = jget_object(root, marketplace);
    if (entry.empty()) return true;
    entry["lastUpdated"] = boost::json::value(stamp);
    root[marketplace] = std::move(entry);

    return cc::write_file(path, cc::json_dump_pretty(root) + "\n", error);
}

bool refresh_marketplace(boost::asio::io_context& io_context, const std::string& marketplace,
                          std::string* error) {
    const std::vector<Marketplace> known = known_marketplaces();
    const Marketplace* found = nullptr;
    for (const Marketplace& candidate : known) {
        if (candidate.name == marketplace) {
            found = &candidate;
            break;
        }
    }
    if (found == nullptr) {
        if (error != nullptr) *error = "No marketplace named " + marketplace;
        return false;
    }

    const Source source = read_source_of(marketplace);
    if (source.location.empty()) {
        if (error != nullptr) *error = "The marketplace " + marketplace + " has no source";
        return false;
    }

    if (source.kind == SourceKind::Url) {
        // The marketplace is a file that is downloaded.
        const std::string path =
            cc::path_join(marketplaces_directory(), sanitize(marketplace) + ".json");
        if (!download(io_context, source.location, path, error)) return false;
        if (!remember_install_location(marketplace, path, error)) return false;
        return remember_last_updated(marketplace, cc::format_iso_now(), error);
    }

    if (source.kind == SourceKind::Git) {
        // The marketplace is a repository that is cloned.
        const std::string path = cc::path_join(marketplaces_directory(), sanitize(marketplace));
        if (!clone(source.location, path, source.reference, error)) return false;
        if (!remember_install_location(marketplace, path, error)) return false;
        return remember_last_updated(marketplace, cc::format_iso_now(), error);
    }

    // A marketplace that is a folder of this machine is already there.
    const std::string expanded = cc::expand_home(source.location);
    if (!cc::is_directory(expanded)) {
        if (error != nullptr) {
            *error = "The marketplace " + marketplace + " is not on this machine";
        }
        return false;
    }
    return remember_last_updated(marketplace, cc::format_iso_now(), error);
}

std::string substitute_plugin_variables(std::string_view content,
                                        const std::string& plugin_root,
                                        const std::string& plugin_id) {
    if (content.empty()) return std::string(content);

    std::string text(content);
    text = cc::replace_all(text, "${CLAUDE_PLUGIN_ROOT}", plugin_root);

    // The data of the plugin lives in a folder of its own, next to the cache.
    // The reference leaves the variable as it is when the plugin has no name
    // of its own.
    if (!plugin_id.empty()) {
        const std::string data = cc::path_join(
            cc::path_join(plugins_directory(), kDataDirectoryName), sanitize(plugin_id));
        text = cc::replace_all(text, "${CLAUDE_PLUGIN_DATA}", data);

        // The values the user gave the plugin are named in its text.
        const boost::json::object values = load_plugin_options(plugin_id);
        for (const auto& [key, value] : values) {
            std::string shown;
            if (value.is_string()) {
                shown = value.as_string().c_str();
            } else if (value.is_bool()) {
                shown = value.as_bool() ? "true" : "false";
            } else if (value.is_int64()) {
                shown = std::to_string(value.as_int64());
            } else {
                continue;
            }
            text = cc::replace_all(text, "${user_config." + std::string(key) + "}", shown);
        }
    }

    return text;
}

} // namespace cc::plugins
