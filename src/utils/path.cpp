#include "utils/path.hpp"

#include "utils/string.hpp"

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace cc {
namespace {

/// The path the program was started with.
std::string g_program_path;

} // namespace

std::string home_dir() {
    if (const char* home = std::getenv("HOME"); home && *home) return home;
    std::error_code ec;
    const auto p = fs::current_path(ec);
    return ec ? std::string() : p.string();
}

std::string cwd() {
    std::error_code ec;
    const auto p = fs::current_path(ec);
    return ec ? std::string() : p.string();
}

std::string claude_config_dir() {
    if (const char* dir = std::getenv("CLAUDE_CONFIG_DIR"); dir && *dir) return dir;
    return path_join(home_dir(), ".claude");
}

void set_program_path(const std::string& path) {
    if (path.empty()) return;

    // A path that is only a name is looked for in the path of the system,
    // the way a shell looks for it.
    std::error_code ec;
    if (fs::path(path).has_parent_path()) {
        g_program_path = fs::absolute(path, ec).lexically_normal().string();
        if (ec) g_program_path = path;
        return;
    }

    const char* raw_path = std::getenv("PATH");
    const std::string search = raw_path != nullptr ? raw_path : "";
    for (const std::string& directory : split(search, ':')) {
        if (directory.empty()) continue;
        const fs::path candidate = fs::path(directory) / path;
        if (fs::exists(candidate, ec) && !ec) {
            g_program_path = fs::absolute(candidate, ec).lexically_normal().string();
            return;
        }
    }

    g_program_path = path;
}

std::string program_path() { return g_program_path; }

std::string program_directory() {
    if (g_program_path.empty()) return {};
    return dirname(g_program_path);
}

std::string path_join(std::string_view a, std::string_view b) {
    if (a.empty()) return std::string(b);
    if (b.empty()) return std::string(a);
    if (is_absolute(b)) return std::string(b);
    std::string out(a);
    if (out.back() != '/') out.push_back('/');
    size_t start = 0;
    while (start < b.size() && b[start] == '/') ++start;
    out.append(b.substr(start));
    return out;
}

std::string dirname(std::string_view path) {
    const size_t pos = path.find_last_of('/');
    if (pos == std::string_view::npos) return "";
    if (pos == 0) return "/";
    return std::string(path.substr(0, pos));
}

std::string basename(std::string_view path) {
    std::string_view p = path;
    while (p.size() > 1 && p.back() == '/') p.remove_suffix(1);
    const size_t pos = p.find_last_of('/');
    if (pos == std::string_view::npos) return std::string(p);
    return std::string(p.substr(pos + 1));
}

std::string stem(std::string_view path) {
    const std::string base = basename(path);
    const size_t pos = base.find_last_of('.');
    if (pos == std::string::npos || pos == 0) return base;
    return base.substr(0, pos);
}

std::string extension(std::string_view path) {
    const std::string base = basename(path);
    const size_t pos = base.find_last_of('.');
    if (pos == std::string::npos || pos == 0) return "";
    return base.substr(pos + 1);
}

std::vector<std::string> path_components(std::string_view path) {
    std::vector<std::string> out;
    for (const auto& piece : split(path, '/')) {
        if (piece.empty() || piece == ".") continue;
        out.push_back(piece);
    }
    return out;
}

std::string normalize(std::string_view path) {
    if (path.empty()) return "";
    const bool absolute = path[0] == '/';

    std::vector<std::string> stack;
    for (const auto& piece : split(path, '/')) {
        if (piece.empty() || piece == ".") continue;
        if (piece == "..") {
            if (!stack.empty() && stack.back() != "..") {
                stack.pop_back();
            } else if (!absolute) {
                stack.push_back("..");
            }
            continue;
        }
        stack.push_back(piece);
    }

    std::string out = absolute ? "/" : "";
    out += join(stack, "/");
    if (out.empty()) out = absolute ? "/" : ".";
    return out;
}

bool is_absolute(std::string_view path) { return !path.empty() && path[0] == '/'; }

std::string normalize_path_for_config_key(std::string_view path) {
    std::string out = normalize(path);
    // The keys of the global configuration are written with the separators
    // of the reference, so that the two agree on the name of a bucket.
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    return out;
}

std::string resolve(std::string_view path, std::string_view base) {
    std::string raw(path);
    if (!raw.empty() && raw[0] == '~') raw = expand_home(raw);
    if (is_absolute(raw)) return normalize(raw);
    const std::string root = base.empty() ? cwd() : std::string(base);
    return normalize(path_join(root, raw));
}

std::string relative(std::string_view path, std::string_view base) {
    const std::string p = normalize(path);
    const std::string b = normalize(base);
    if (b.empty()) return p;
    if (p == b) return ".";
    if (!starts_with(p, b)) return p;
    if (b == "/") return p.substr(1);
    if (p.size() > b.size() && p[b.size()] == '/') return p.substr(b.size() + 1);
    return p;
}

/// The path of `path` relative to `base`.
///
/// A path that lies outside of `base` is written with the `..` of the way
/// back to it, the way Node writes it.
std::string relative_from(std::string_view base, std::string_view path) {
    const std::string b = normalize(base);
    const std::string p = normalize(path);
    if (b.empty() || p.empty()) return p;

    // The root of the filesystem: the path is what stands after it.
    if (b == "/") return p.size() > 1 ? p.substr(1) : "";

    const std::vector<std::string> from = cc::split(b, '/');
    const std::vector<std::string> to = cc::split(p, '/');
    size_t common = 0;
    while (common < from.size() && common < to.size() && from[common] == to[common]) ++common;

    std::string out;
    for (size_t i = common; i < from.size(); ++i) {
        if (!out.empty()) out += '/';
        out += "..";
    }
    for (size_t i = common; i < to.size(); ++i) {
        if (!out.empty()) out += '/';
        out += to[i];
    }
    return out.empty() ? "." : out;
}

bool is_inside(std::string_view path, std::string_view base) {    const std::string p = normalize(path);
    const std::string b = normalize(base);
    if (b.empty()) return false;
    if (p == b) return true;
    if (b == "/") return starts_with(p, "/");
    return starts_with(p, b) && p.size() > b.size() && p[b.size()] == '/';
}

std::string with_extension(std::string_view path, std::string_view ext) {
    const std::string base = basename(path);
    const size_t pos = base.find_last_of('.');
    std::string dir = dirname(path);
    std::string stem_part = base;
    if (pos != std::string::npos && pos != 0) stem_part = base.substr(0, pos);
    std::string new_base = stem_part;
    if (!ext.empty()) {
        new_base += ".";
        new_base += ext;
    }
    if (dir.empty()) return new_base;
    return path_join(dir, new_base);
}

std::string tilde_path(std::string_view path) {
    const std::string home = home_dir();
    if (home.empty()) return std::string(path);
    if (path == home) return "~";
    if (starts_with(path, home) && path.size() > home.size() && path[home.size()] == '/') {
        return "~" + std::string(path.substr(home.size()));
    }
    return std::string(path);
}

std::string path_label(std::string_view path) {
    const std::string base = basename(path);
    const std::string dir = basename(dirname(path));
    if (base.empty()) return std::string(path);
    if (dir.empty() || dir == "/" || dir == ".") return base;
    return dir + "/" + base;
}

bool is_url(std::string_view path) {
    const size_t pos = path.find("://");
    if (pos == std::string_view::npos || pos == 0) return false;
    for (size_t i = 0; i < pos; ++i) {
        const char c = path[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

} // namespace cc
