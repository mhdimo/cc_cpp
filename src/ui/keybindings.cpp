#include "ui/keybindings.hpp"

#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>

namespace cc::ui::keybindings {
namespace {

/// A key with its modifiers.
struct Keystroke {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool meta = false;
    bool cmd = false;
    std::string key;

    friend bool operator==(const Keystroke& a, const Keystroke& b) {
        return a.ctrl == b.ctrl && a.alt == b.alt && a.shift == b.shift && a.meta == b.meta &&
               a.cmd == b.cmd && a.key == b.key;
    }
};

/// The name of a key of the port, in the terms of the reference.
const char* name_of_type(Key::Type type) {
    switch (type) {
    case Key::Type::Enter: return "enter";
    case Key::Type::Backspace: return "backspace";
    case Key::Type::Delete: return "delete";
    case Key::Type::Tab: return "tab";
    case Key::Type::ShiftTab: return "shift+tab";
    case Key::Type::Escape: return "escape";
    case Key::Type::Up: return "up";
    case Key::Type::Down: return "down";
    case Key::Type::ShiftUp: return "shift+up";
    case Key::Type::ShiftDown: return "shift+down";
    case Key::Type::Left: return "left";
    case Key::Type::Right: return "right";
    case Key::Type::Home: return "home";
    case Key::Type::End: return "end";
    case Key::Type::PageUp: return "pageup";
    case Key::Type::PageDown: return "pagedown";
    case Key::Type::CtrlC: return "ctrl+c";
    case Key::Type::CtrlD: return "ctrl+d";
    case Key::Type::CtrlA: return "ctrl+a";
    case Key::Type::CtrlE: return "ctrl+e";
    case Key::Type::CtrlK: return "ctrl+k";
    case Key::Type::CtrlU: return "ctrl+u";
    case Key::Type::CtrlW: return "ctrl+w";
    case Key::Type::CtrlL: return "ctrl+l";
    case Key::Type::CtrlR: return "ctrl+r";
    case Key::Type::CtrlO: return "ctrl+o";
    case Key::Type::CtrlG: return "ctrl+g";
    case Key::Type::CtrlF: return "ctrl+f";
    case Key::Type::CtrlB: return "ctrl+b";
    case Key::Type::CtrlZ: return "ctrl+z";
    case Key::Type::CtrlY: return "ctrl+y";
    case Key::Type::CtrlX: return "ctrl+x";
    case Key::Type::CtrlT: return "ctrl+t";
    case Key::Type::CtrlS: return "ctrl+s";
    case Key::Type::F1: return "f1";
    case Key::Type::F2: return "f2";
    case Key::Type::F3: return "f3";
    case Key::Type::F4: return "f4";
    case Key::Type::F5: return "f5";
    case Key::Type::F6: return "f6";
    case Key::Type::F7: return "f7";
    case Key::Type::F8: return "f8";
    case Key::Type::F9: return "f9";
    case Key::Type::F10: return "f10";
    case Key::Type::F11: return "f11";
    case Key::Type::F12: return "f12";
    default: return "";
    }
}

/// Parses the name of a key, the way the reference parses it.
///
/// The modifiers may be written as `ctrl`, `control`, `alt`, `opt`,
/// `option`, `meta`, `cmd`, `command`, `super` or `win`.
Keystroke parse(const std::string& text) {
    Keystroke keystroke;

    size_t start = 0;
    while (start <= text.size()) {
        const size_t plus = text.find('+', start);
        const std::string part =
            cc::to_lower(text.substr(start, plus == std::string::npos ? std::string::npos : plus - start));

        if (part == "ctrl" || part == "control") {
            keystroke.ctrl = true;
        } else if (part == "alt" || part == "opt" || part == "option") {
            keystroke.alt = true;
        } else if (part == "shift") {
            keystroke.shift = true;
        } else if (part == "meta") {
            // The terminal cannot tell the meta key from the alt one.
            keystroke.alt = true;
        } else if (part == "cmd" || part == "command" || part == "super" || part == "win") {
            keystroke.cmd = true;
        } else if (part == "esc") {
            keystroke.key = "escape";
        } else if (part == "return") {
            keystroke.key = "enter";
        } else if (part == "space") {
            keystroke.key = " ";
        } else if (part == "↑") {
            keystroke.key = "up";
        } else if (part == "↓") {
            keystroke.key = "down";
        } else if (part == "←") {
            keystroke.key = "left";
        } else if (part == "→") {
            keystroke.key = "right";
        } else if (!part.empty()) {
            keystroke.key = part;
        }

        if (plus == std::string::npos) break;
        start = plus + 1;
    }

    return keystroke;
}

} // namespace

std::string name_of(const Key& key) {
    if (key.type == Key::Type::Character) {
        std::string name = key.text;
        if (name.size() != 1) return {};
        if (key.ctrl) name = "ctrl+" + name;
        if (key.alt) name = "alt+" + name;
        return name;
    }

    const char* name = name_of_type(key.type);
    if (name == nullptr || *name == '\0') return {};

    // The modifiers of the key that are not part of its name.
    std::string prefix;
    if (key.ctrl && !cc::starts_with(name, "ctrl+")) prefix += "ctrl+";
    if (key.alt) prefix += "alt+";
    if (key.cmd) prefix += "cmd+";
    return prefix + name;
}

void Bindings::load(std::string_view path) {
    const std::string file =
        path.empty() ? cc::path_join(cc::claude_config_dir(), "keybindings.json") : std::string(path);
    const auto contents = cc::read_file(file);
    if (!contents) return;

    const auto parsed = cc::json_parse(*contents, boost::json::value(boost::json::object{}));
    if (!parsed.is_object()) return;

    const auto& root = parsed.as_object();
    const auto bindings = root.find("bindings");
    if (bindings == root.end() || !bindings->value().is_array()) return;

    for (const auto& block : bindings->value().as_array()) {
        if (!block.is_object()) continue;
        const auto& object = block.as_object();

        const auto context = object.find("context");
        if (context == object.end() || !context->value().is_string()) continue;

        const auto keys = object.find("bindings");
        if (keys == object.end() || !keys->value().is_object()) continue;

        std::vector<std::pair<std::string, std::string>> entries;
        for (const auto& entry : keys->value().as_object()) {
            const std::string key(entry.key());
            std::string action;
            if (entry.value().is_string()) {
                action = std::string(entry.value().as_string().c_str());
            } else if (entry.value().is_null()) {
                // A key that is bound to nothing is unbound.
                action.clear();
            } else {
                continue;
            }
            entries.emplace_back(key, action);
        }

        blocks_.emplace_back(std::string(context->value().as_string().c_str()), std::move(entries));
    }
}

std::string Bindings::action_for(const Key& key, std::string_view context) const {
    const std::string name = name_of(key);
    if (name.empty()) return {};

    const Keystroke keystroke = parse(name);

    for (const auto& block : blocks_) {
        if (block.first != context) continue;
        for (const auto& entry : block.second) {
            if (parse(entry.first) == keystroke) return entry.second;
        }
    }
    return {};
}

} // namespace cc::ui::keybindings
