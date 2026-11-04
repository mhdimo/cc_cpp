#include "ui/onboarding.hpp"

#include "auth/environment.hpp"
#include "config/global_config.hpp"
#include "ui/figures.hpp"
#include "ui/select.hpp"
#include "ui/theme_picker.hpp"
#include "ui/welcome.hpp"
#include "utils/string.hpp"


namespace cc::ui {
namespace {

/// The line of the notes of the safety that names the guide.
constexpr std::string_view kSecurityGuideUrl = "https://code.claude.com/docs/en/security";

/// The width the notes of the safety are written at.
///
/// The reference writes them in a column of seventy columns.
constexpr size_t kNotesWidth = 70;

/// The line that says that the escape does not end the setup.
constexpr std::string_view kSkipHint = "Enter to confirm · Esc to skip";

} // namespace

Onboarding::Onboarding(Theme theme, ThemeName name, std::string current_theme, std::string version)
    : theme_(std::move(theme)),
      name_(name),
      current_theme_(std::move(current_theme)),
      version_(std::move(version)) {}

std::vector<std::string> Onboarding::welcome() const {
    // The light themes have a picture of their own; the themes that only
    // use the colours of the terminal take the picture of the dark one.
    const bool light = name_ == ThemeName::Light || name_ == ThemeName::LightDaltonized;
    return welcome_lines(theme_, version_, light);
}

std::string Onboarding::api_key_to_approve() {
    const std::string truncated = auth::custom_api_key();
    if (truncated.empty()) return {};

    // The question is asked only once for every key.
    if (config::custom_api_key_approved(truncated)) return {};
    if (config::custom_api_key_rejected(truncated)) return {};

    return truncated;
}

Onboarding::Answers Onboarding::run(Terminal& terminal) {
    Answers answers;

    // The welcome stands above the questions: it is written to the
    // transcript, so that it stays there while they are asked.
    for (const auto& line : welcome()) {
        terminal.write_static_line(line);
    }
    terminal.write_static_line("");

    // The theme of the session. The reference asks the question this way
    // when the picker is part of the setup.
    {
        ThemePickerOptions options;
        options.intro = true;
        options.help_text = "To change this later, run /theme";
        options.hide_hint = true;

        ThemePicker picker(theme_, current_theme_, std::move(options));
        const std::string chosen = picker.run(terminal);
        if (!chosen.empty()) answers.theme = chosen;
    }

    // The key of the API that was found in the environment.
    const std::string key = api_key_to_approve();
    if (!key.empty()) {
        answers.api_key_approved = ask_for_api_key(terminal, key);
        config::remember_custom_api_key(key, answers.api_key_approved);
    }

    // The notes of the safety.
    show_security_notes(terminal);

    // The settings of the terminal.
    answers.terminal_setup = ask_for_terminal_setup(terminal);

    return answers;
}

bool Onboarding::ask_for_api_key(Terminal& terminal, const std::string& key) {
    Style rule;
    rule.foreground = theme_.warning;

    Style title;
    title.bold = true;
    title.foreground = theme_.warning;

    Style bold;
    bold.bold = true;

    Style dim;
    dim.dim = true;

    Style normal;
    normal.foreground = theme_.text;

    // The two answers of the question. The second one is the one the
    // pointer starts on, the way the reference starts on it.
    const std::vector<SelectOption> options = {
        {"Yes", ""},
        {"No (recommended)", ""},
    };
    Select select(theme_, options);
    select.set_selected(1);

    while (true) {
        std::vector<std::string> lines;
        lines.push_back("");
        std::string line;
        for (int i = 0; i < terminal.width(); ++i) line += figures::kHorizontalRule;
        lines.push_back(styled(line, rule));
        lines.push_back(" " + styled("Detected a custom API key in your environment", title));
        lines.push_back("");
        lines.push_back(" " + styled("ANTHROPIC_API_KEY", bold) + styled(": sk-ant-..." + key, normal));
        lines.push_back("");
        lines.push_back(" " + styled("Do you want to use this API key?", normal));
        lines.push_back("");

        for (const auto& line : select.render(terminal.width())) lines.push_back(line);

        lines.push_back("");
        lines.push_back(" " + styled("Enter to confirm · Esc to cancel", dim));

        terminal.render_dynamic(lines);

        const auto pressed = terminal.read_key(100);
        if (!pressed) {
            if (terminal.closed()) {
                terminal.clear_dynamic();
                return false;
            }
            continue;
        }

        if (pressed->type == Key::Type::Up) {
            select.set_selected(select.selected() == 0 ? select.size() - 1 : select.selected() - 1);
            continue;
        }
        if (pressed->type == Key::Type::Down) {
            select.set_selected(select.selected() + 1 >= select.size() ? 0 : select.selected() + 1);
            continue;
        }
        if (pressed->type == Key::Type::Enter) {
            terminal.clear_dynamic();
            return select.selected() == 0;
        }
        if (pressed->type == Key::Type::Escape || pressed->type == Key::Type::CtrlC) {
            terminal.clear_dynamic();
            return false;
        }
    }
}

void Onboarding::show_security_notes(Terminal& terminal) {
    Style bold;
    bold.bold = true;

    Style dim;
    dim.dim = true;

    Style permission;
    permission.foreground = theme_.permission;

    Style bold_permission;
    bold_permission.bold = true;
    bold_permission.foreground = theme_.permission;

    std::vector<std::string> lines;
    lines.push_back(" " + styled("Security notes:", bold));
    lines.push_back("");

    // The notes are a numbered list, the way the reference writes them.
    const std::vector<std::string> titles = {
        "Claude can make mistakes",
        "Due to prompt injection risks, only use it with code you trust",
    };

    for (size_t i = 0; i < titles.size(); ++i) {
        const std::string number = std::to_string(i + 1) + ".";
        lines.push_back(" " + styled(number, dim) + " " + styled(titles[i], Style{}));

        if (i == 0) {
            for (const auto& line :
                 cc::word_wrap("You should always review Claude's responses, especially when running "
                               "code.",
                               kNotesWidth)) {
                lines.push_back("   " + styled(line, dim));
            }
        } else {
            lines.push_back("   " + styled("For more details see:", dim));
            const std::string guide =
                supports_hyperlinks() ? hyperlink(kSecurityGuideUrl, kSecurityGuideUrl)
                                      : std::string(kSecurityGuideUrl);
            lines.push_back("   " + styled(guide, dim));
        }
    }

    lines.push_back("");
    lines.push_back(" " + styled("Press ", permission) + styled("Enter", bold_permission) +
                    styled(" to continue…", permission));

    terminal.render_dynamic(lines);

    // The notes wait for the Enter of the user.
    while (true) {
        const auto pressed = terminal.read_key(100);
        if (!pressed) {
            if (terminal.closed()) {
                terminal.clear_dynamic();
                return;
            }
            continue;
        }
        if (pressed->type == Key::Type::Enter || pressed->type == Key::Type::Escape ||
            pressed->type == Key::Type::CtrlC) {
            terminal.clear_dynamic();
            return;
        }
    }
}

bool Onboarding::ask_for_terminal_setup(Terminal& terminal) {
    Style bold;
    bold.bold = true;

    Style dim;
    dim.dim = true;

    Style normal;
    normal.foreground = theme_.text;

    const std::vector<SelectOption> options = {
        {"Yes, use recommended settings", ""},
        {"No, maybe later with /terminal-setup", ""},
    };
    Select select(theme_, options);

    while (true) {
        std::vector<std::string> lines;
        lines.push_back(" " + styled("Use Claude Code's terminal setup?", bold));
        lines.push_back("");
        for (const auto& line :
             cc::word_wrap("For the optimal coding experience, enable the recommended settings for "
                           "your terminal: Shift+Enter for newlines",
                           kNotesWidth)) {
            lines.push_back(" " + styled(line, normal));
        }
        lines.push_back("");

        for (const auto& line : select.render(terminal.width())) lines.push_back(line);

        lines.push_back("");
        lines.push_back(" " + styled(std::string(kSkipHint), dim));

        terminal.render_dynamic(lines);

        const auto pressed = terminal.read_key(100);
        if (!pressed) {
            if (terminal.closed()) {
                terminal.clear_dynamic();
                return false;
            }
            continue;
        }

        if (pressed->type == Key::Type::Up) {
            select.set_selected(select.selected() == 0 ? select.size() - 1 : select.selected() - 1);
            continue;
        }
        if (pressed->type == Key::Type::Down) {
            select.set_selected(select.selected() + 1 >= select.size() ? 0 : select.selected() + 1);
            continue;
        }
        if (pressed->type == Key::Type::Enter) {
            terminal.clear_dynamic();
            return select.selected() == 0;
        }
        if (pressed->type == Key::Type::Escape || pressed->type == Key::Type::CtrlC) {
            terminal.clear_dynamic();
            return false;
        }
    }
}

} // namespace cc::ui
