#pragma once

#include "sandbox/manager.hpp"
#include "ui/settings_tabs.hpp"
#include "ui/theme.hpp"

#include <functional>
#include <string>

namespace cc::ui {

/// The panel of the sandbox.
///
/// The reference shows the sandbox in a panel with the tabs of the mode, of
/// the overrides, of the configuration and of the dependencies; the mode is
/// picked with a list of the three ways the sandbox can run.
class SandboxPanel {
public:
    /// The panel of the sandbox.
    ///
    /// The changes are written by `apply`, which is given the new settings
    /// of the sandbox.
    SandboxPanel(Theme theme, sandbox::Manager& manager,
                 std::function<void(const sandbox::Settings&)> apply);

    /// Runs the panel until the user closes it.
    ///
    /// Returns the message of what happened, or an empty string when the
    /// panel was closed without a change.
    std::string run(Terminal& terminal);

private:
    Theme theme_;
    sandbox::Manager& manager_;
    std::function<void(const sandbox::Settings&)> apply_;
};

} // namespace cc::ui
