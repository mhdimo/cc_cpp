#include "config/claude_md.hpp"

#include "config/project_key.hpp"
#include "utils/fs.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <cctype>
#include <set>

namespace cc::config {
namespace {

/// How many levels of `@path` imports are followed.
constexpr int kMaxIncludeDepth = 5;

/// The names of the memory files looked for in every project directory.
///
/// The memory files the reference reads in every directory, in order.
constexpr const char* kProjectMemoryNames[] = {"CLAUDE.md", ".claude/CLAUDE.md"};

/// The name of the memory file of the user.
constexpr const char* kUserMemoryName = "CLAUDE.md";

/// The name of the memory file of the folder, the one that is not written
/// into the repository.
constexpr const char* kLocalMemoryName = "CLAUDE.local.md";

/// The `@path` imports in `contents`.
///
/// An import is an `@` at the start of a line or after whitespace, followed by
/// a path. The path may be absolute, relative to the including file, or start
/// with `~/`.
std::vector<std::string> extract_imports(const std::string& contents) {
    std::vector<std::string> imports;

    for (const auto& line : cc::split_lines(contents)) {
        size_t pos = 0;
        while (pos < line.size()) {
            size_t at = std::string::npos;
            for (size_t i = pos; i < line.size(); ++i) {
                if (line[i] != '@') continue;
                if (i > 0 && !std::isspace(static_cast<unsigned char>(line[i - 1]))) continue;
                at = i;
                break;
            }
            if (at == std::string::npos) break;

            size_t end = at + 1;
            while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end]))) {
                ++end;
            }
            std::string path = line.substr(at + 1, end - at - 1);
            pos = end;

            // A trailing `#fragment` selects a section of the file, not the
            // file itself.
            const size_t hash = path.find('#');
            if (hash != std::string::npos) path = path.substr(0, hash);
            if (path.empty()) continue;

            const bool looks_like_path =
                cc::starts_with(path, "./") || cc::starts_with(path, "~/") ||
                (path[0] == '/' && path != "/") ||
                std::isalnum(static_cast<unsigned char>(path[0])) || path[0] == '.' ||
                path[0] == '_';
            if (!looks_like_path) continue;

            imports.push_back(std::move(path));
        }
    }

    return imports;
}

/// The path an import of `from` refers to.
std::string resolve_import(const std::string& from, const std::string& import_path) {
    if (import_path[0] == '/') return cc::normalize(import_path);
    return cc::resolve(import_path, cc::dirname(from));
}

/// True when `path` lies in the folder `root`.
///
/// The root is empty when there is none, and then every path is inside it.
bool inside_directory(const std::string& root, const std::string& path) {
    if (root.empty()) return true;
    const std::string resolved = cc::resolve(path);
    return resolved == root || cc::starts_with(resolved, root + "/");
}

/// Adds the files imported by `contents` to `files`.
///
/// Imports are followed recursively, up to `kMaxIncludeDepth` levels deep.
/// `visited` holds the files that were already added, so that a cycle in the
/// imports terminates.
///
/// A file that lies outside `root` is only read when `include_external` is
/// true, the way the reference reads it: the user is asked about the files
/// that a project pulls in from outside its folder, and they are read only
/// when the user says yes.
void collect_imports(const std::string& from, const std::string& contents, int depth,
                      bool is_global, bool include_external, const std::string& root,
                      std::set<std::string>& visited, std::vector<MemoryFile>& files) {
    if (depth >= kMaxIncludeDepth) return;

    for (const auto& import_path : extract_imports(contents)) {
        const std::string path = resolve_import(from, import_path);
        if (path.empty()) continue;
        if (!include_external && !inside_directory(root, path)) continue;
        if (!visited.insert(path).second) continue;

        const auto text = cc::read_file(path);
        if (!text || cc::trim(*text).empty()) continue;

        MemoryFile file;
        file.path = path;
        file.contents = *text;
        file.directory = cc::dirname(path);
        // A file that the memory of the user pulls in belongs to the memory
        // of the user, the way the reference reads it.
        file.is_global = is_global;
        // The file that pulled this one in is remembered, the way the
        // reference remembers it.
        file.parent = from;
        files.push_back(file);

        collect_imports(path, *text, depth + 1, is_global, include_external, root, visited, files);
    }
}

/// True when the frontmatter of the file names the paths the rule applies
/// to.
///
/// A rule that names paths is a conditional rule; the reference reads those
/// when a file of the session matches them, and not with the other rules.
bool is_conditional_rule(const std::string& contents) {
    const auto lines = cc::split_lines(contents);
    if (lines.empty() || cc::trim(lines.front()) != "---") return false;

    for (size_t i = 1; i < lines.size(); ++i) {
        const std::string line = cc::trim(lines[i]);
        if (line == "---") break;
        if (cc::starts_with(line, "paths:")) return true;
    }
    return false;
}

/// The rule files of `directory`.
///
/// The reference reads the markdown files of the directory and of the
/// directories below it, the way it reads the other markdown files.
std::vector<std::string> rule_files(const std::string& directory) {
    std::vector<std::string> out;

    for (const auto& name : cc::list_dir(directory)) {
        const std::string path = cc::path_join(directory, name);
        if (cc::is_directory(path)) {
            for (const auto& nested : rule_files(path)) out.push_back(nested);
            continue;
        }
        if (!cc::ends_with(name, ".md")) continue;
        out.push_back(path);
    }

    return out;
}

} // namespace

std::vector<std::string> external_includes(const std::string& cwd) {
    std::vector<std::string> paths;

    const std::string root = cc::resolve(cwd);
    // The files are looked for with the permission granted: the question is
    // asked about the files that would be read, not about the ones that were
    // read already.
    for (const auto& file : load_memory_files(cwd, true)) {
        // The files that are read on their own are not external, and the
        // file of the user is never one.
        if (file.parent.empty() || file.is_global) continue;

        // A file that lies in the folder of the session is not external
        // either.
        if (inside_directory(root, file.path)) continue;

        paths.push_back(file.path);
    }

    return paths;
}

std::optional<std::string> user_memory() {
    const std::string path = cc::path_join(cc::claude_config_dir(), kUserMemoryName);
    if (!cc::file_exists(path)) return std::nullopt;
    return cc::read_file(path);
}

std::vector<MemoryFile> load_memory_files(const std::string& cwd, bool include_external) {
    std::vector<MemoryFile> files;
    std::set<std::string> visited;

    // The user memory file comes first: it holds the instructions that apply
    // to every project.
    const std::string user_path = cc::path_join(cc::claude_config_dir(), kUserMemoryName);
    if (const auto contents = user_memory()) {
        MemoryFile file;
        file.path = user_path;
        file.contents = *contents;
        file.directory = cc::claude_config_dir();
        file.is_global = true;
        files.push_back(file);
        visited.insert(cc::normalize(file.path));
    }

    // Then the rules of the user, which the reference reads as a part of the
    // memory of the user.
    const std::string user_rules = cc::path_join(cc::claude_config_dir(), "rules");
    for (const auto& path : rule_files(user_rules)) {
        if (visited.count(cc::normalize(path))) continue;

        const auto contents = cc::read_file(path);
        if (!contents || cc::trim(*contents).empty()) continue;
        // A rule that names the paths it applies to is only read when a file
        // of the session matches them.
        if (is_conditional_rule(*contents)) continue;

        MemoryFile file;
        file.path = path;
        file.contents = *contents;
        file.directory = cc::claude_config_dir();
        file.is_global = true;
        files.push_back(file);
        visited.insert(cc::normalize(path));
    }

    // Then the project files, from the repository root down to the working
    // directory.
    for (const auto& directory : project_directories(cwd)) {
        for (const auto* name : kProjectMemoryNames) {
            const std::string path = cc::path_join(directory, name);
            if (!cc::file_exists(path)) continue;
            // A file that was already read is not read again: the memory of
            // the user lives in a folder of the tree as well.
            if (visited.count(cc::normalize(path))) continue;

            const auto contents = cc::read_file(path);
            if (!contents || cc::trim(*contents).empty()) continue;

            MemoryFile file;
            file.path = path;
            file.contents = *contents;
            file.directory = directory;
            file.is_global = false;
            files.push_back(file);
            visited.insert(cc::normalize(path));
        }

        // The rules of the folder are read as a part of the memory of the
        // folder.
        const std::string rules = cc::path_join(cc::path_join(directory, ".claude"), "rules");
        for (const auto& path : rule_files(rules)) {
            if (visited.count(cc::normalize(path))) continue;

            const auto contents = cc::read_file(path);
            if (!contents || cc::trim(*contents).empty()) continue;
            if (is_conditional_rule(*contents)) continue;

            MemoryFile file;
            file.path = path;
            file.contents = *contents;
            file.directory = directory;
            file.is_global = false;
            files.push_back(file);
            visited.insert(cc::normalize(path));
        }

        // The memory file of the folder is the one that is not written into
        // the repository.
        const std::string local_path = cc::path_join(directory, kLocalMemoryName);
        if (cc::file_exists(local_path) && !visited.count(cc::normalize(local_path))) {
            const auto contents = cc::read_file(local_path);
            if (contents && !cc::trim(*contents).empty()) {
                MemoryFile file;
                file.path = local_path;
                file.contents = *contents;
                file.directory = directory;
                file.is_global = false;
                files.push_back(file);
                visited.insert(cc::normalize(local_path));
            }
        }
    }

    // Finally the files the memory files pull in with `@path`.
    std::vector<MemoryFile> imports;
    const std::string root = cc::resolve(cwd);
    for (const auto& file : files) {
        // The memory of the user may always pull in files from outside the
        // folder of the session, the way the reference reads it; the memory
        // of the project may not, until the user says so.
        const bool allow_external = file.is_global || include_external;
        collect_imports(file.path, file.contents, 0, file.is_global, allow_external, root, visited,
                        imports);
    }
    files.insert(files.end(), imports.begin(), imports.end());

    return files;
}

} // namespace cc::config
