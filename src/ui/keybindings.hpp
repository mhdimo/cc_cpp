#pragma once

#include "ui/terminal.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cc::ui::keybindings {

/// The name a key is known by.
///
/// The names are the ones the reference uses: `ctrl+o`, `shift+up`,
/// `escape`, `enter`, `a`.
std::string name_of(const Key& key);

/// The keybindings the user wrote in `~/.claude/keybindings.json`.
///
/// The file holds a list of blocks, each of them a context and the keys
/// that do something in it:
///
/// ```json
/// { "bindings": [ { "context": "Global", "bindings": { "ctrl+g": "app:toggleTranscript" } } ] }
/// ```
///
/// A key that is bound to `null` is unbound.
class Bindings {
public:
    /// Reads the keybindings of the user.
    ///
    /// A file that cannot be read or understood is left alone. The path is
    /// the file of the user unless another one is named.
    void load(std::string_view path = {});

    /// True when the user has written no keybindings.
    bool empty() const { return blocks_.empty(); }

    /// The action a key does in a context, or an empty string.
    ///
    /// The contexts are the ones the reference names: `Global`, `Chat`,
    /// `Select`, `Help` and the rest.
    std::string action_for(const Key& key, std::string_view context) const;

private:
    /// The blocks of the file: a context and the keys of it.
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> blocks_;
};

} // namespace cc::ui::keybindings
