#include "ui/sandbox_panel.hpp"

#include "ui/ansi.hpp"
#include "ui/figures.hpp"
#include "utils/string.hpp"

#include <algorithm>

namespace cc::ui {
namespace {

/// The ways the sandbox can run.
enum class Mode {
    AutoAllow,
    Regular,
    Disabled,
};

/// The mode the sandbox is in now.
Mode current_mode(const sandbox::Manager& manager) {
    if (!manager.is_sandboxing_enabled()) return Mode::Disabled;
    if (manager.is_auto_allow_bash_if_sandboxed_enabled()) return Mode::AutoAllow;
    return Mode::Regular;
}

/// Wraps the body of a tab of the sandbox in the padding of the reference.
///
/// The reference wraps the body of every tab of the sandbox in a box with a
/// row of padding above it and below it, so that the body sits one row
/// further from the strip of the tabs. A body that ends with an empty line
/// keeps it: that line is the row of padding below it.
std::vector<std::string> padded(std::vector<std::string> body) {
    const bool ends_with_padding = !body.empty() && body.back().empty();
    body.insert(body.begin(), "");
    if (!ends_with_padding) body.push_back("");
    return body;
}

/// The row of one of the options of a list.
///
/// The reference writes the number of the option before its label, padded
/// to the width of the largest number plus two, and the pointer and the
/// label are one space apart. The label of an option that is not focused
/// keeps the colour of the text.
std::string option_row(const Theme& theme, const std::string& label, size_t index, size_t count,
                        bool focused, bool chosen) {
    Style suggestion;
    suggestion.foreground = theme.suggestion;

    Style dim;
    dim.foreground = theme.inactive;

    Style success;
    success.foreground = theme.success;

    // The number of the option is padded to the width of the largest one.
    const size_t width = std::to_string(count).size();
    std::string number = std::to_string(index + 1) + ".";
    if (number.size() < width + 2) number += std::string(width + 2 - number.size(), ' ');

    std::string text = focused ? styled(std::string(figures::kPointer), suggestion) : std::string(" ");
    text += " ";
    text += styled(number, dim);
    text += focused ? styled(label, suggestion) : label;
    if (chosen) text += " " + styled("(current)", success);
    return text;
}

/// The tab of the mode.
///
/// The three ways the sandbox can run are offered, and the one that is in
/// use is marked.
class SandboxModeTab : public SettingsTab {
public:
    SandboxModeTab(Theme theme, sandbox::Manager& manager,
            std::function<void(const sandbox::Settings&)> apply)
        : theme_(theme), manager_(manager), apply_(std::move(apply)) {}

    std::string title() const override { return "Mode"; }

    std::vector<std::string> render(int width, int height = 0) const override {
        std::vector<std::string> out;

        const Mode current = current_mode(manager_);

        Style label_style;
        label_style.bold = true;

        Style dim;
        dim.foreground = theme_.inactive;

        // The sockets cannot be blocked when a dependency is missing.
        if (manager_.cannot_block_unix_sockets()) {
            Style warning;
            warning.foreground = theme_.warning;
            out.push_back(styled("Cannot block unix domain sockets (see Dependencies tab)",
                                 warning));
            out.push_back("");
        }

        out.push_back(styled("Configure Mode:", label_style));
        out.push_back("");

        // The pointer is drawn on the row the cursor is on, and only there.
        const auto option = [&](const char* label, Mode mode, size_t index) {
            const bool chosen = current == mode;
            const bool focused = index == selected_;
            out.push_back(option_row(theme_, label, index, 3, focused, chosen));
        };

        option("Sandbox BashTool, with auto-allow", Mode::AutoAllow, 0);
        option("Sandbox BashTool, with regular permissions", Mode::Regular, 1);
        option("No Sandbox", Mode::Disabled, 2);

        out.push_back("");
        // The label of the explanation is dim, the way the reference writes it.
        Style dim_label;
        dim_label.bold = true;
        dim_label.foreground = theme_.inactive;
        out.push_back(styled("Auto-allow mode:", dim_label) +
                      styled(
                          " Commands will try to run in the sandbox automatically, and attempts to "
                          "run outside of the sandbox fallback to regular permissions. Explicit "
                          "ask/deny rules are always respected.",
                          dim));
        out.push_back("");
        out.push_back(styled("Learn more: ", dim) +
                      hyperlink("code.claude.com/docs/en/sandboxing",
                                "https://code.claude.com/docs/en/sandboxing"));

        return padded(out);
    }

    TabResult handle(const Key& key) override {
        TabResult result;
        result.used = true;

        switch (key.type) {
        case Key::Type::Up:
            selected_ = selected_ == 0 ? 2 : selected_ - 1;
            break;
        case Key::Type::Down:
            selected_ = selected_ + 1 < 3 ? selected_ + 1 : 0;
            break;
        case Key::Type::Enter: {
            sandbox::Settings settings = manager_.settings();
            switch (selected_) {
            case 0:
                settings.enabled = true;
                settings.auto_allow_bash_if_sandboxed = true;
                result.picked = "✓ Sandbox enabled with auto-allow for bash commands";
                break;
            case 1:
                settings.enabled = true;
                settings.auto_allow_bash_if_sandboxed = false;
                result.picked = "✓ Sandbox enabled with regular bash permissions";
                break;
            default:
                settings.enabled = false;
                settings.auto_allow_bash_if_sandboxed = false;
                result.picked = "○ Sandbox disabled";
                break;
            }
            apply_(settings);
            result.close = true;
            break;
        }
        case Key::Type::Escape:
        case Key::Type::CtrlC:
            result.close = true;
            break;
        default:
            result.used = false;
            break;
        }

        return result;
    }

private:
    Theme theme_;
    sandbox::Manager& manager_;
    std::function<void(const sandbox::Settings&)> apply_;
    size_t selected_ = 0;
};

/// The tab of the overrides.
///
/// The user chooses whether a command may ask to run outside the sandbox.
class SandboxOverridesTab : public SettingsTab {
public:
    SandboxOverridesTab(Theme theme, sandbox::Manager& manager,
                 std::function<void(const sandbox::Settings&)> apply)
        : theme_(theme), manager_(manager), apply_(std::move(apply)) {}

    std::string title() const override { return "Overrides"; }

    std::vector<std::string> render(int width, int height = 0) const override {
        std::vector<std::string> out;

        Style dim;
        dim.foreground = theme_.inactive;

        if (!manager_.is_sandboxing_enabled()) {
            Style subtle;
            subtle.foreground = theme_.subtle;
            out.push_back(styled(
                "Sandbox is not enabled. Enable sandbox to configure override settings.", subtle));
            return padded(out);
        }

        const bool allowed = manager_.are_unsandboxed_commands_allowed();

        // The settings of the overrides are held by a policy, so they cannot
        // be changed here.
        if (manager_.are_settings_locked_by_policy()) {
            Style subtle;
            subtle.foreground = theme_.subtle;
            out.push_back(styled(
                "Override settings are managed by a higher-priority configuration and cannot be "
                "changed locally.",
                subtle));
            out.push_back("");
            out.push_back(styled(
                std::string("Current setting: ") +
                    (allowed ? "Allow unsandboxed fallback" : "Strict sandbox mode"),
                dim));
            return padded(out);
        }

        Style label_style;
        label_style.bold = true;

        // The labels of the explanations are dim, the way the reference writes
        // them.
        Style dim_label;
        dim_label.bold = true;
        dim_label.foreground = theme_.inactive;

        // The pointer is drawn on the row the cursor is on, and only there.
        const auto option = [&](const char* label, bool chosen, size_t index) {
            const bool focused = index == selected_;
            out.push_back(option_row(theme_, label, index, 2, focused, chosen));
        };

        out.push_back(styled("Configure Overrides:", label_style));
        out.push_back("");

        option("Allow unsandboxed fallback", allowed, 0);
        option("Strict sandbox mode", !allowed, 1);

        out.push_back("");
        out.push_back(styled("Allow unsandboxed fallback:", dim_label) +
                      styled(
                          " When a command fails due to sandbox restrictions, Claude can retry "
                          "with dangerouslyDisableSandbox to run outside the sandbox (falling back "
                          "to default permissions).",
                          dim));
        out.push_back("");
        out.push_back(styled("Strict sandbox mode:", dim_label) +
                      styled(
                          " All bash commands invoked by the model must run in the sandbox unless "
                          "they are explicitly listed in excludedCommands.",
                          dim));
        out.push_back("");
        out.push_back(styled("Learn more: ", dim) +
                      hyperlink("code.claude.com/docs/en/sandboxing#configure-sandboxing",
                                "https://code.claude.com/docs/en/sandboxing#configure-sandboxing"));

        return padded(out);
    }

    TabResult handle(const Key& key) override {
        TabResult result;
        result.used = true;

        switch (key.type) {
        case Key::Type::Up:
            selected_ = selected_ == 0 ? 1 : selected_ - 1;
            break;
        case Key::Type::Down:
            selected_ = selected_ + 1 < 2 ? selected_ + 1 : 0;
            break;
        case Key::Type::Enter: {
            if (!manager_.is_sandboxing_enabled()) {
                result.close = true;
                break;
            }
            sandbox::Settings settings = manager_.settings();
            settings.allow_unsandboxed_commands = selected_ == 0;
            apply_(settings);
            result.picked =
                selected_ == 0
                    ? "✓ Unsandboxed fallback allowed - commands can run outside sandbox when "
                      "necessary"
                    : "✓ Strict sandbox mode - all commands must run in sandbox or be excluded "
                      "via the `excludedCommands` option";
            result.close = true;
            break;
        }
        case Key::Type::Escape:
        case Key::Type::CtrlC:
            result.close = true;
            break;
        default:
            result.used = false;
            break;
        }

        return result;
    }

private:
    Theme theme_;
    sandbox::Manager& manager_;
    std::function<void(const sandbox::Settings&)> apply_;
    size_t selected_ = 0;
};

/// The tab of the configuration.
///
/// The restrictions of the sandbox are listed, the way the reference lists
/// them.
class SandboxConfigTab : public SettingsTab {
public:
    SandboxConfigTab(Theme theme, const sandbox::Manager& manager) : theme_(theme), manager_(manager) {}

    std::string title() const override { return "Config"; }

    std::vector<std::string> render(int width, int height = 0) const override {
        std::vector<std::string> out;

        // The titles of the sections are painted with the colour of the
        // permission, the way the reference paints them.
        Style label_style;
        label_style.bold = true;
        label_style.foreground = theme_.permission;

        Style dim;
        dim.foreground = theme_.inactive;

        Style subtle;
        subtle.foreground = theme_.subtle;

        if (!manager_.is_sandboxing_enabled()) {
            out.push_back(styled("Sandbox is not enabled", subtle));
            const std::vector<std::string> warnings = manager_.dependency_warnings();
            if (!warnings.empty()) {
                // The warnings of the dependencies are a row apart from the
                // line that says that the sandbox is not enabled.
                out.push_back("");
                for (const std::string& warning : warnings) {
                    out.push_back(styled(warning, dim));
                }
            }
            return padded(out);
        }

        // The sections of the tab are one row apart, the way the reference
        // lays them out.
        const auto section = [&](const std::string& title, const std::string& value) {
            out.push_back(styled(title, label_style));
            for (const std::string& line : cc::split_lines(value)) {
                out.push_back(styled(line, dim));
            }
            out.push_back("");
        };

        const std::vector<std::string>& excluded = manager_.excluded_commands();
        section("Excluded Commands:",
                excluded.empty() ? "None" : cc::join(excluded, ", "));

        // The restrictions of the filesystem hold the paths of the settings
        // and the paths of the rules of the permissions of the session.
        const sandbox::FsReadConfig read = manager_.get_fs_read_config();
        if (!read.deny_only.empty()) {
            section("Filesystem Read Restrictions:", "Denied: " + cc::join(read.deny_only, ", "));
        }
        if (!read.allow_within_deny.empty()) {
            out.push_back(styled("Allowed within denied: " + cc::join(read.allow_within_deny, ", "), dim));
            out.push_back("");
        }

        const sandbox::FsWriteConfig write = manager_.get_fs_write_config();
        if (!write.allow_only.empty()) {
            section("Filesystem Write Restrictions:",
                    "Allowed: " + cc::join(write.allow_only, ", "));
        }
        if (!write.deny_within_allow.empty()) {
            out.push_back(styled("Denied within allowed: " + cc::join(write.deny_within_allow, ", "), dim));
            out.push_back("");
        }

        const sandbox::NetworkRestrictionConfig network = manager_.get_network_restriction_config();
        if (!network.allowed_hosts.empty() || !network.denied_hosts.empty()) {
            std::string value;
            if (!network.allowed_hosts.empty()) {
                value += "Allowed: " + cc::join(network.allowed_hosts, ", ");
            }
            if (!network.denied_hosts.empty()) {
                if (!value.empty()) value += "\n";
                value += "Denied: " + cc::join(network.denied_hosts, ", ");
            }
            const std::string title =
                manager_.settings().network.allow_managed_domains_only
                    ? "Network Restrictions (Managed):"
                    : "Network Restrictions:";
            section(title, value);
        }

        const std::vector<std::string> sockets = manager_.get_allow_unix_sockets();
        if (!sockets.empty()) {
            section("Allowed Unix Sockets:", cc::join(sockets, ", "));
        }

        // The wildcards of the paths are ignored on the systems whose
        // runtime does not know them.
        const std::vector<std::string> globs = manager_.get_linux_glob_pattern_warnings();
        if (!globs.empty()) {
            Style warning;
            warning.bold = true;
            warning.foreground = theme_.warning;
            out.push_back(styled("⚠ Warning: Glob patterns not fully supported on Linux", warning));

            std::string text = "The following patterns will be ignored: ";
            const size_t shown = std::min<size_t>(3, globs.size());
            text += cc::join(std::vector<std::string>(globs.begin(), globs.begin() + shown), ", ");
            if (globs.size() > 3) {
                text += " (" + std::to_string(globs.size() - 3) + " more)";
            }
            out.push_back(styled(text, dim));
            out.push_back("");
        }

        for (const std::string& warning : manager_.dependency_warnings()) {
            out.push_back(styled(warning, dim));
            out.push_back("");
        }

        return padded(out);
    }

private:
    Theme theme_;
    const sandbox::Manager& manager_;
};

/// The tab of the dependencies.
class SandboxDependenciesTab : public SettingsTab {
public:
    SandboxDependenciesTab(Theme theme, const sandbox::Manager& manager)
        : theme_(theme), manager_(manager) {}

    std::string title() const override { return "Dependencies"; }

    std::vector<std::string> render(int width, int height = 0) const override {
        std::vector<std::string> out;

        Style dim;
        dim.foreground = theme_.inactive;

        Style success;
        success.foreground = theme_.success;

        Style error;
        error.foreground = theme_.error;

        Style warning;
        warning.foreground = theme_.warning;

        const std::vector<std::string> errors = manager_.dependency_errors();
        const std::vector<std::string> warnings = manager_.dependency_warnings();

        const bool mac = sandbox::Manager::platform() == "macos";

        // The errors that have a row of their own are shown by the row; the
        // others are shown as they are.
        const auto names = [&](const std::string& text) {
            return text.find("ripgrep") != std::string::npos ||
                   text.find("bwrap") != std::string::npos ||
                   text.find("socat") != std::string::npos;
        };

        bool rg_missing = false;
        bool bwrap_missing = false;
        bool socat_missing = false;
        bool seccomp_missing = false;
        for (const std::string& text : errors) {
            if (text.find("ripgrep") != std::string::npos) rg_missing = true;
            if (text.find("bwrap") != std::string::npos) bwrap_missing = true;
            if (text.find("socat") != std::string::npos) socat_missing = true;
        }
        for (const std::string& text : warnings) {
            if (text.find("ripgrep") != std::string::npos) rg_missing = true;
            if (text.find("seccomp") != std::string::npos) seccomp_missing = true;
        }

        // Every block of the tab is a row apart from the next one, the way
        // the reference lays them out.
        std::vector<std::vector<std::string>> blocks;

        // The seatbelt of macOS is part of the system.
        if (mac) {
            blocks.push_back({"seatbelt: " + styled("built-in (macOS)", success)});
        }

        {
            std::vector<std::string> block;
            block.push_back(std::string("ripgrep (rg): ") +
                            styled(rg_missing ? "not found" : "found", rg_missing ? error : success));
            if (rg_missing) {
                block.push_back(
                    styled(mac ? "  · brew install ripgrep" : "  · apt install ripgrep", dim));
            }
            blocks.push_back(std::move(block));
        }

        if (!mac) {
            std::vector<std::string> block;
            block.push_back(std::string("bubblewrap (bwrap): ") +
                            styled(bwrap_missing ? "not installed" : "installed",
                                   bwrap_missing ? error : success));
            if (bwrap_missing) block.push_back(styled("  · apt install bubblewrap", dim));
            blocks.push_back(std::move(block));

            std::vector<std::string> socat_block;
            socat_block.push_back(std::string("socat: ") +
                                  styled(socat_missing ? "not installed" : "installed",
                                         socat_missing ? error : success));
            if (socat_missing) socat_block.push_back(styled("  · apt install socat", dim));
            blocks.push_back(std::move(socat_block));

            // The filter of seccomp is only needed to block the sockets.
            std::vector<std::string> seccomp_block;
            if (seccomp_missing) {
                seccomp_block.push_back(std::string("seccomp filter: ") +
                                        styled("not installed", warning) +
                                        styled(" (required to block unix domain sockets)", dim));
                seccomp_block.push_back(styled("  · npm install -g @anthropic-ai/sandbox-runtime", dim));
                seccomp_block.push_back(
                    styled("  · or copy vendor/seccomp/* from sandbox-runtime and set", dim));
                seccomp_block.push_back(
                    styled("    sandbox.seccomp.bpfPath and applyPath in settings.json", dim));
            } else {
                seccomp_block.push_back(std::string("seccomp filter: ") + styled("installed", success));
            }
            blocks.push_back(std::move(seccomp_block));
        }

        for (const std::string& text : errors) {
            if (names(text)) continue;
            blocks.push_back({styled(text, error)});
        }

        for (size_t i = 0; i < blocks.size(); ++i) {
            if (i > 0) out.push_back("");
            for (const std::string& line : blocks[i]) out.push_back(line);
        }

        return padded(out);
    }

private:
    Theme theme_;
    const sandbox::Manager& manager_;
};

} // namespace

/// The panel of the settings of the sandbox.
///
/// The reference opens the sandbox in a panel with the tabs of the mode, of
/// the overrides and of the configuration. The dependencies get a tab of
/// their own when one of them is missing, and the panel shows only that tab
/// when the sandbox cannot run at all.
std::unique_ptr<SettingsTabs> sandbox_settings_panel(Theme theme, sandbox::Manager& manager,
                                                      std::function<void(const sandbox::Settings&)> apply) {
    std::vector<std::unique_ptr<SettingsTab>> tabs;

    const std::vector<std::string> errors = manager.dependency_errors();
    const std::vector<std::string> warnings = manager.dependency_warnings();

    // A dependency that is missing keeps the sandbox from running at all, so
    // the panel shows only the tab of the dependencies.
    if (!errors.empty()) {
        tabs.push_back(std::make_unique<SandboxDependenciesTab>(theme, manager));
    } else {
        tabs.push_back(std::make_unique<SandboxModeTab>(theme, manager, apply));
        if (!warnings.empty()) {
            tabs.push_back(std::make_unique<SandboxDependenciesTab>(theme, manager));
        }
        tabs.push_back(std::make_unique<SandboxOverridesTab>(theme, manager, apply));
        tabs.push_back(std::make_unique<SandboxConfigTab>(theme, manager));
    }

    auto panel = std::make_unique<SettingsTabs>(theme, std::move(tabs));
    panel->set_title("Sandbox:");
    panel->set_pane_color(theme.permission);
    return panel;
}

SandboxPanel::SandboxPanel(Theme theme, sandbox::Manager& manager,
                           std::function<void(const sandbox::Settings&)> apply)
    : theme_(theme), manager_(manager), apply_(std::move(apply)) {}

std::string SandboxPanel::run(Terminal& terminal) {
    const auto panel = sandbox_settings_panel(theme_, manager_, apply_);
    return panel->run(terminal);
}

} // namespace cc::ui
