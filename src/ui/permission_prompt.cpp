#include "ui/permission_prompt.hpp"

#include "config/global_config.hpp"
#include "ui/notify.hpp"

#include "utils/path.hpp"
#include "utils/time.hpp"
#include "utils/string.hpp"

namespace cc::ui {
namespace {

/// The time the user has to be away before the terminal is told.
///
/// The reference waits six seconds.
constexpr int64_t kIdleNotificationMs = 6000;

/// The command a scope of a shell tool names.
///
/// The scope of a shell command is the rule it would be saved as,
/// `Bash(git status:*)`; the reference names the command itself in the
/// label of the choice, without the `:*` that makes the rule a prefix.
///
/// The empty string means that the scope does not name a command.
std::string scope_command(const std::string& scope) {
    if (scope.rfind("Bash(", 0) != 0) return {};
    if (scope.size() < 7 || scope.back() != ')') return {};

    std::string command = scope.substr(5, scope.size() - 6);
    // The rule of the old shape, `command:*`, is a prefix: the command is
    // the part before the `:*`.
    if (command.size() > 2 && command.compare(command.size() - 2, 2, ":*") == 0) {
        command.resize(command.size() - 2);
    }
    return command;
}

/// The name of the folder a path is in.
///
/// The reference names the folder the file is in, so that the choice says
/// where the permission of the session reaches.
std::string directory_name(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return "this directory";

    const std::string directory = path.substr(0, slash);
    const size_t cut = directory.find_last_of('/');
    if (cut == std::string::npos) return directory.empty() ? "this directory" : directory;
    return directory.substr(cut + 1);
}

/// True when the path lies inside the folder the session works in.
bool path_is_inside(const std::string& path, const std::string& cwd) {
    if (path.empty() || cwd.empty()) return false;

    const std::string absolute = cc::resolve(path, cwd);
    const std::string base = cc::resolve(cwd, cwd);
    if (absolute == base) return true;
    return cc::starts_with(absolute, base + "/");
}

/// The placeholder of the field the user types the instructions in.
///
/// The reference says what the field is for in the words of the choice it
/// belongs to.
std::string amend_placeholder(const std::string& value) {
    if (value == "no") return "and tell Claude to do something differently";
    return "and tell Claude what to do next";
}

} // namespace

PermissionPrompt::PermissionPrompt(Theme theme, std::string title, std::string question,
                                   std::vector<PermissionOption> options, std::string body,
                                   std::string description, PermissionKind kind)
    : theme_(std::move(theme)),
      title_(std::move(title)),
      question_(std::move(question)),
      body_(std::move(body)),
      description_(std::move(description)),
      kind_(kind),
      options_(std::move(options)),
      amend_(theme_, "") {}

std::vector<std::string> PermissionPrompt::render(int width) const {
    Style rule;
    rule.foreground = theme_.permission;

    Style title_style;
    title_style.bold = true;
    title_style.foreground = theme_.permission;

    Style suggestion;
    suggestion.foreground = theme_.suggestion;

    Style normal;
    normal.foreground = theme_.text;

    // The numbers are dim the way `<Text dimColor>` is: the ordinary
    // colour of the text, made faint with the attribute of dimness.
    Style dim;
    dim.dim = true;

    // The hint of the reference is dim as well.
    Style hint_style;
    hint_style.dim = true;

    std::vector<std::string> lines;

    // The frame of the reference leaves an empty line above the cap.
    lines.push_back("");

    // The cap: the top border of a rounded box, without its corners. The
    // reference leaves the other three sides off as well.
    const size_t rule_width = static_cast<size_t>(std::max(width, 1));
    std::string rule_text;
    rule_text.reserve(rule_width * 3);
    for (size_t i = 0; i < rule_width; ++i) rule_text += "\u2500";
    lines.push_back(styled(rule_text, rule));

    // The title of the request.
    if (!title_.empty()) {
        lines.push_back(" " + styled(title_, title_style));
    }

    // What is being approved. The reference puts it in a block of its own:
    // an empty line above and below it, and the block stands two columns
    // further in than the rest of the dialog.
    const bool file = kind_ == PermissionKind::Read || kind_ == PermissionKind::Write;
    const std::string indent(file ? "  " : "   ");
    Style body_style;
    if (file) {
        // The text of a file is not dim, the way the reference draws it.
        body_style.foreground = theme_.text;
    } else {
        body_style.dim = true;
    }

    if (!body_.empty() || !description_.empty()) {
        lines.push_back("");
        for (const auto& line : cc::split_lines(body_)) {
            lines.push_back(indent + styled(line, body_style));
        }
        if (!description_.empty()) {
            Style description_style;
            description_style.dim = true;
            lines.push_back(indent + styled(description_, description_style));
        }
        lines.push_back("");
    }

    // The question.
    if (!question_.empty()) {
        lines.push_back(" " + styled(question_, normal));
    }

    // The choices.
    for (size_t i = 0; i < options_.size(); ++i) {
        const bool is_selected = i == selected_;
        const std::string number = std::to_string(i + 1) + ".";
        const std::string marker = is_selected ? "\u276f " : "  ";

        // The number is dimmed; the label takes the colour of the choice.
        std::string line = " " + marker + styled(number, dim) + " " +
                           styled(options_[i].label, is_selected ? suggestion : normal);

        // The field the instructions are typed in belongs to the choice
        // that is highlighted.
        if (amending_ && is_selected) {
            const std::string text = amend_.text();
            line += ", " + styled(text.empty() ? amend_placeholder(options_[i].value) : text,
                                  text.empty() ? dim : normal);
            line += styled("█", Style{});
        }
        lines.push_back(std::move(line));
    }

    lines.push_back("");

    // The hint names the keys the user may press. The reference offers the
    // amendment of the choice while the choice is one of the two it can
    // amend.
    const bool amendable = selected_ < options_.size() &&
                           (options_[selected_].value == "yes" || options_[selected_].value == "no");
    std::string hint = " Esc to cancel";
    if (amending_) {
        hint = " Enter to confirm · Esc to cancel";
    } else if (amendable) {
        hint += " · Tab to amend";
    }
    lines.push_back(styled(hint, hint_style));

    return lines;
}

std::string PermissionPrompt::run(Terminal& terminal, std::string* feedback) {
    const int width = terminal.width();

    last_interaction_ms_ = cc::now_ms();

    while (true) {
        terminal.render_dynamic(render(width));

        const auto key = terminal.read_key(100);
        if (!key) {
            // The terminal closed: the view ends rather than wait for a
            // key that will never come.
            if (terminal.closed()) {
                terminal.clear_dynamic();
                return "";
            }

            // The IDE may have answered while the user was reading the
            // question: the change may have been saved there, or rejected
            // there.
            if (answer_provider_) {
                const std::string answer = answer_provider_();
                if (!answer.empty()) {
                    terminal.clear_dynamic();
                    return answer;
                }
            }

            // The user has been away for a while: the terminal is told
            // that the session wants them, the way the reference tells it.
            if (!notified_ && !notification_.empty() &&
                cc::now_ms() - last_interaction_ms_ >= kIdleNotificationMs) {
                notified_ = true;
                notify_terminal(
                    notification_channel(config::global_string("preferredNotifChannel", "auto")),
                    notification_);
            }
            continue;
        }

        // The user is at the keyboard: the terminal is not told.
        last_interaction_ms_ = cc::now_ms();

        // While the user types the instructions, the keys go to the field.
        if (amending_) {
            switch (key->type) {
            case Key::Type::Enter: {
                if (feedback) *feedback = amend_.text();
                const std::string value = options_[selected_].value;
                terminal.clear_dynamic();
                return value;
            }
            case Key::Type::Escape:
                // The reference cancels the whole request when the field is
                // left with Escape.
                terminal.clear_dynamic();
                return {};
            case Key::Type::Backspace:
                amend_.backspace();
                break;
            case Key::Type::Delete:
                amend_.delete_forward();
                break;
            case Key::Type::Left:
                amend_.move_left();
                break;
            case Key::Type::Right:
                amend_.move_right();
                break;
            case Key::Type::Home:
                amend_.move_home();
                break;
            case Key::Type::End:
                amend_.move_end();
                break;
            case Key::Type::Character:
                amend_.insert(key->text);
                break;
            default:
                break;
            }
            continue;
        }

        switch (key->type) {
        case Key::Type::Up:
            // The reference wraps around at the ends of the list.
            if (!options_.empty()) {
                selected_ = selected_ == 0 ? options_.size() - 1 : selected_ - 1;
            }
            break;
        case Key::Type::Down:
            if (!options_.empty()) {
                selected_ = selected_ + 1 < options_.size() ? selected_ + 1 : 0;
            }
            break;
        case Key::Type::Tab:
            // Tab opens the field the instructions are typed in, the way
            // the reference opens it.
            if (selected_ < options_.size() &&
                (options_[selected_].value == "yes" || options_[selected_].value == "no")) {
                amending_ = true;
            }
            break;
        case Key::Type::Enter:
        case Key::Type::Character:
            if (key->type == Key::Type::Character && key->text != "1" && key->text != "2" &&
                key->text != "3") {
                break;
            }
            if (key->type == Key::Type::Character) {
                const size_t index = static_cast<size_t>(key->text[0] - '1');
                if (index < options_.size()) {
                    selected_ = index;
                    terminal.clear_dynamic();
                    return options_[selected_].value;
                }
                break;
            }
            terminal.clear_dynamic();
            return options_[selected_].value;
        case Key::Type::Escape:
        case Key::Type::CtrlC:
            terminal.clear_dynamic();
            return {};
        default:
            break;
        }
    }
}

std::vector<PermissionOption> permission_options(const std::string& user_facing_name,
                                                  PermissionKind kind, const std::string& path,
                                                  const std::string& scope, const std::string& cwd,
                                                  bool always_allow, const Theme& theme) {
    std::vector<PermissionOption> options;
    options.push_back({"Yes", "yes"});

    if (kind == PermissionKind::Shell) {
        if (always_allow) {
            // The reference names the tool and the folder in bold. A shell
            // command is named by the command of the rule it would be saved
            // as: the rule `Bash(git status:*)` is shown as `git status`.
            Style bold;
            bold.bold = true;
            const std::string command = scope_command(scope);
            const std::string named = command.empty() ? user_facing_name : command;
            std::string label = "Yes, and don't ask again for " + styled(named, bold) +
                                " commands in " + styled(cwd, bold);
            options.push_back({std::move(label), "yes-dont-ask-again"});
        }
    } else {
        // A file is allowed for the session alone: the reference writes no
        // rule for it.
        Style bold;
        bold.bold = true;
        const bool inside = path_is_inside(path, cwd);
        std::string label;
        if (inside) {
            if (kind == PermissionKind::Read) {
                label = "Yes, during this session";
            } else {
                label = "Yes, allow all edits during this session " + styled("(shift+tab)", bold);
            }
        } else {
            const std::string directory = directory_name(path);
            if (kind == PermissionKind::Read) {
                label = "Yes, allow reading from " + styled(directory + "/", bold) +
                        " during this session";
            } else {
                label = "Yes, allow all edits in " + styled(directory + "/", bold) +
                        " during this session " + styled("(shift+tab)", bold);
            }
        }
        options.push_back({std::move(label), "yes-session"});
    }

    options.push_back({"No", "no"});
    return options;
}

std::string permission_session_rule(PermissionKind kind, const std::string& path,
                                    const std::string& cwd) {
    if (kind == PermissionKind::Shell) return {};

    // The rule of a read names the tool that reads; the rule of a write
    // names the tool that changes the files, the way the reference does.
    const std::string tool = kind == PermissionKind::Read ? "Read" : "Edit";

    std::string directory = cc::resolve(path, cwd);
    const size_t slash = directory.find_last_of('/');
    if (slash != std::string::npos && slash > 0) directory.resize(slash);

    // The rule begins with two separators, so that it is fixed to the root
    // of the machine and not to the folder of the settings it comes from.
    if (!directory.empty() && directory.front() == '/') directory.erase(0, 1);

    return tool + "(//" + directory + "/**)";
}

} // namespace cc::ui
