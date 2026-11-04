#include <catch2/catch_test_macros.hpp>

#include "cli/app.hpp"
#include "ui/ansi.hpp"
#include "ui/external_includes_dialog.hpp"
#include "ui/input_dialog.hpp"
#include "ui/permission_prompt.hpp"
#include "ui/question_dialog.hpp"
#include "ui/theme.hpp"
#include "ui/trust_dialog.hpp"

using namespace cc;

namespace {

/// True when the line is painted with the attribute of dimness.
///
/// The attributes of a sequence come before its colours, so the dimness is
/// the first of them.
bool is_dim(const std::string& line) {
    const size_t start = line.find("\x1b[");
    if (start == std::string::npos) return false;
    const size_t end = line.find('m', start);
    if (end == std::string::npos) return false;

    const std::string codes = line.substr(start + 2, end - start - 2);
    return codes == "2" || codes.rfind("2;", 0) == 0;
}

/// True when the line is painted with a colour of its own.
bool has_colour(const std::string& line) { return line.find("38;2;") != std::string::npos; }

/// The lines of a dialog, without the colours.
std::string plain(const std::vector<std::string>& lines) {
    std::string joined;
    for (const auto& line : lines) {
        joined += ui::strip(line);
        joined += "\n";
    }
    return joined;
}

/// A tool of the tests, with the name the test gives it.
class NamedTool : public Tool {
public:
    explicit NamedTool(std::string name) : name_(std::move(name)) {}

    std::string_view name() const override { return name_; }
    std::string description() const override { return {}; }
    boost::json::object input_schema() const override { return {}; }
    ToolResult call(const boost::json::object&, ToolUseContext&) override { return {}; }

private:
    std::string name_;
};

} // namespace

TEST_CASE("The permission dialog asks the question the reference asks") {
    const auto theme = ui::Theme::dark();
    const auto options = ui::permission_options("Bash", ui::PermissionKind::Shell, "",
                                                "Bash(git status:*)", "/tmp", true);
    const ui::PermissionPrompt prompt(theme, "Bash command", "Do you want to proceed?", options);

    const auto lines = prompt.render(60);
    REQUIRE_FALSE(lines.empty());

    // The frame leaves an empty line above the cap, and the cap is a rule
    // across the top without its corners.
    REQUIRE(lines.front().empty());
    REQUIRE(lines[1].find("─") != std::string::npos);
    REQUIRE(lines[1].find("╭") == std::string::npos);
    REQUIRE(lines[1].find("╮") == std::string::npos);

    const std::string joined = plain(lines);

    // The title of the request and the question.
    REQUIRE(joined.find("Bash command") != std::string::npos);
    REQUIRE(joined.find("Do you want to proceed?") != std::string::npos);

    // The choices are numbered, and the first one is marked.
    REQUIRE(joined.find("1. Yes") != std::string::npos);
    REQUIRE(joined.find("2. Yes, and don't ask again for git status") != std::string::npos);
    REQUIRE(joined.find("commands in /tmp") != std::string::npos);
    REQUIRE(joined.find("3. No") != std::string::npos);
    REQUIRE(joined.find("❯ 1. Yes") != std::string::npos);
}

TEST_CASE("The permission dialog names the command of the rule it would save") {
    // The rule of a shell command is a prefix rule: `Bash(git status:*)`.
    // The reference names the command of the rule in the choice, without
    // the wrapper of the tool and without the `:*` of the prefix.
    const auto options = ui::permission_options("Bash", ui::PermissionKind::Shell, "",
                                                "Bash(git status:*)", "/tmp", true);
    REQUIRE(options.size() == 3);

    const std::string label = ui::strip(options[1].label);
    REQUIRE(label.find("Yes, and don't ask again for git status commands in /tmp") !=
            std::string::npos);
    REQUIRE(label.find("Bash(") == std::string::npos);
    REQUIRE(label.find(":*") == std::string::npos);
}

TEST_CASE("The permission dialog offers a file the permission of the session") {
    // The reference does not write a rule for a file: the middle choice is
    // the permission of the session alone.
    const auto options = ui::permission_options("Read", ui::PermissionKind::Read, "/tmp/project/a.txt",
                                                "Read(/tmp/project/a.txt)", "/tmp/project", true);
    REQUIRE(options.size() == 3);
    REQUIRE(options[0].value == "yes");
    REQUIRE(options[1].value == "yes-session");
    REQUIRE(options[2].value == "no");

    const std::string label = ui::strip(options[1].label);
    REQUIRE(label.find("Yes, during this session") != std::string::npos);
    REQUIRE(label.find("don't ask again") == std::string::npos);
}

TEST_CASE("The permission dialog names the folder a file outside the session is in") {
    const auto options = ui::permission_options("Read", ui::PermissionKind::Read, "/tmp/notes/a.txt",
                                                "Read(/tmp/notes/a.txt)", "/tmp/project", true);
    const std::string label = ui::strip(options[1].label);
    REQUIRE(label.find("Yes, allow reading from notes/ during this session") != std::string::npos);
}

TEST_CASE("The permission dialog offers the edits of the session") {
    const auto options = ui::permission_options("Write", ui::PermissionKind::Write,
                                                "/tmp/project/a.txt", "Write(/tmp/project/a.txt)",
                                                "/tmp/project", true);
    const std::string label = ui::strip(options[1].label);
    REQUIRE(label.find("Yes, allow all edits during this session (shift+tab)") != std::string::npos);
}

TEST_CASE("The rule of the session reaches the folder of the file") {
    const std::string rule =
        ui::permission_session_rule(ui::PermissionKind::Read, "/tmp/project/a.txt", "/tmp/project");
    REQUIRE(rule == "Read(//tmp/project/**)");

    const std::string edits =
        ui::permission_session_rule(ui::PermissionKind::Write, "/tmp/project/a.txt", "/tmp/project");
    REQUIRE(edits == "Edit(//tmp/project/**)");
}

TEST_CASE("The permission dialog says what is being approved") {
    const auto theme = ui::Theme::dark();
    const auto options = ui::permission_options("Bash", ui::PermissionKind::Shell, "",
                                                "Bash(git status:*)", "/tmp", true);
    const ui::PermissionPrompt prompt(theme, "Bash command", "Do you want to proceed?", options,
                                      "Bash(git status)");

    const std::string joined = plain(prompt.render(60));
    REQUIRE(joined.find("Bash(git status)") != std::string::npos);
}

TEST_CASE("The body of the permission dialog stands in its own block") {
    const auto theme = ui::Theme::dark();
    const auto options = ui::permission_options("Bash", ui::PermissionKind::Shell, "",
                                                "Bash(git status:*)", "/tmp", true);
    const ui::PermissionPrompt prompt(theme, "Bash command", "Do you want to proceed?", options,
                                      "git status", "Show the state of the repository");

    const auto lines = prompt.render(60);
    const std::string joined = plain(lines);

    // The body is indented two columns further than the title, and it has
    // an empty line above and below it.
    bool saw_body = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (ui::strip(lines[i]) != "   git status") continue;
        saw_body = true;
        REQUIRE(i > 0);
        REQUIRE(lines[i - 1].empty());
    }
    REQUIRE(saw_body);

    // The description of the tool is under the body, and it is dim.
    bool saw_description = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (ui::strip(lines[i]).find("Show the state of the repository") == std::string::npos) continue;
        saw_description = true;
        REQUIRE(is_dim(lines[i]));
        REQUIRE(ui::strip(lines[i]).rfind("   Show the state", 0) == 0);
        // The block ends with an empty line.
        REQUIRE(i + 1 < lines.size());
        REQUIRE(lines[i + 1].empty());
    }
    REQUIRE(saw_description);
}

TEST_CASE("The permission dialog leaves out the always-allow choice when it cannot") {
    const auto options =
        ui::permission_options("Read", ui::PermissionKind::Read, "", "", "/tmp", false);
    REQUIRE(options.size() == 3);
    REQUIRE(options[0].value == "yes");
    REQUIRE(options[2].value == "no");
}

TEST_CASE("The numbers of the choices are dim") {
    const auto theme = ui::Theme::dark();
    const auto options = ui::permission_options("Bash", ui::PermissionKind::Shell, "",
                                                "Bash(git status:*)", "/tmp", true);
    const ui::PermissionPrompt prompt(theme, "Bash command", "Do you want to proceed?", options);

    bool saw_number = false;
    for (const auto& line : prompt.render(60)) {
        if (ui::strip(line).find("1. Yes") == std::string::npos) continue;
        saw_number = true;
        // The number is dim, and it carries no colour of its own.
        REQUIRE(is_dim(line));
    }
    REQUIRE(saw_number);
}

TEST_CASE("The hint of the permission dialog is dim") {
    const auto theme = ui::Theme::dark();
    const auto options = ui::permission_options("Bash", ui::PermissionKind::Shell, "",
                                                "Bash(git status:*)", "/tmp", true);
    const ui::PermissionPrompt prompt(theme, "Bash command", "Do you want to proceed?", options);

    const auto lines = prompt.render(60);
    const std::string hint = lines.back();

    // The reference paints the hint with `<Text dimColor>`: the ordinary
    // colour of the text, made faint with the attribute of dimness.
    REQUIRE(is_dim(hint));
    REQUIRE_FALSE(has_colour(hint));
}

TEST_CASE("The hint of the permission dialog offers the amendment of the choice") {
    const auto theme = ui::Theme::dark();
    const auto options = ui::permission_options("Bash", ui::PermissionKind::Shell, "",
                                                "Bash(git status:*)", "/tmp", true);
    const ui::PermissionPrompt prompt(theme, "Bash command", "Do you want to proceed?", options);

    const std::string hint = ui::strip(prompt.render(60).back());
    REQUIRE(hint.find("Esc to cancel · Tab to amend") != std::string::npos);
}

TEST_CASE("The hint of the question dialog is dim") {
    const auto theme = ui::Theme::dark();

    UserQuestion question;
    question.text = "Which library should we use?";
    question.options.push_back(UserQuestionOption{"date-fns", "The modern one", {}});

    const ui::QuestionDialog dialog(theme, question);
    const auto lines = dialog.render(60);
    const std::string hint = lines.back();

    // The reference keeps the colour of the inactive text and adds the
    // attribute of dimness on top of it.
    REQUIRE(is_dim(hint));
    REQUIRE(hint.find("38;2;153;153;153") != std::string::npos);
}

TEST_CASE("The question dialog shows the bar of the questions") {
    const auto theme = ui::Theme::dark();

    UserQuestion first;
    first.text = "Which library should we use?";
    first.header = "Database";
    first.options.push_back(UserQuestionOption{"date-fns", "The modern one", {}});

    UserQuestion second;
    second.text = "Which language?";
    second.header = "Language";
    second.options.push_back(UserQuestionOption{"C++", "The one of the port", {}});

    ui::QuestionDialog dialog(theme, first);
    dialog.set_questions({first, second}, 0);

    const std::string joined = plain(dialog.render(60));

    // The bar holds the header of every question and the chip that hands
    // the answers over.
    REQUIRE(joined.find("←") != std::string::npos);
    REQUIRE(joined.find("☐ Database") != std::string::npos);
    REQUIRE(joined.find("☐ Language") != std::string::npos);
    REQUIRE(joined.find("✔ Submit") != std::string::npos);
    REQUIRE(joined.find("→") != std::string::npos);
}

TEST_CASE("The choices of the question are written the way the reference writes them") {
    const auto theme = ui::Theme::dark();

    UserQuestion question;
    question.text = "Which library should we use?";
    question.options.push_back(UserQuestionOption{"date-fns", "The modern one", {}});

    const ui::QuestionDialog dialog(theme, question);
    const std::string joined = plain(dialog.render(60));

    // The label of the choice follows the number without a space between
    // them, the way the reference writes it.
    REQUIRE(joined.find("❯ 1.date-fns") != std::string::npos);
}

TEST_CASE("The question dialog offers the answer of the user and the way out") {
    const auto theme = ui::Theme::dark();

    UserQuestion question;
    question.text = "Which library should we use?";
    question.options.push_back(UserQuestionOption{"date-fns", "The modern one", {}});
    question.options.push_back(UserQuestionOption{"dayjs", "The small one", {}});

    const ui::QuestionDialog dialog(theme, question);
    const std::string joined = plain(dialog.render(60));

    // The answer of the user is the last choice of the list, and the way
    // out of the dialog is under a rule of its own.
    REQUIRE(joined.find("❯ 1.date-fns") != std::string::npos);
    REQUIRE(joined.find("3.Other") != std::string::npos);
    REQUIRE(joined.find("4. Chat about this") != std::string::npos);
}

TEST_CASE("The numbers of the choices of the question are dim") {
    const auto theme = ui::Theme::dark();

    UserQuestion question;
    question.text = "Which library should we use?";
    question.options.push_back(UserQuestionOption{"date-fns", "The modern one", {}});

    const ui::QuestionDialog dialog(theme, question);
    for (const auto& line : dialog.render(60)) {
        if (ui::strip(line).find("1.date-fns") == std::string::npos) continue;
        REQUIRE(is_dim(line));
    }
}

TEST_CASE("The hint of the trust dialog is dim") {
    const auto theme = ui::Theme::dark();

    const ui::TrustDialog dialog(theme, "/tmp/project");
    const auto lines = dialog.render(70);
    const std::string hint = lines.back();

    REQUIRE(is_dim(hint));
    REQUIRE_FALSE(has_colour(hint));

    // The link to the guide is dim as well.
    bool saw_guide = false;
    for (const auto& line : lines) {
        if (ui::strip(line).find("Security guide") == std::string::npos) continue;
        saw_guide = true;
        REQUIRE(is_dim(line));
        REQUIRE_FALSE(has_colour(line));
    }
    REQUIRE(saw_guide);
}

TEST_CASE("The numbers of the choices of the trust dialog are dim") {
    const auto theme = ui::Theme::dark();

    const ui::TrustDialog dialog(theme, "/tmp/project");
    for (const auto& line : dialog.render(70)) {
        if (ui::strip(line).find("1. Yes, I trust this folder") == std::string::npos) continue;
        REQUIRE(is_dim(line));
    }
}

TEST_CASE("The dialog of the files outside the folder says what the reference says") {
    const auto theme = ui::Theme::dark();
    const ui::ExternalIncludesDialog dialog(theme, {"/tmp/notes.md"});

    const std::vector<std::string> lines = dialog.render(80);
    const std::string text = plain(lines);

    // The text of the dialog is wrapped, so the words are put back
    // together before they are looked for.
    std::string together;
    for (const auto& line : lines) {
        const std::string text_of_line = ui::strip(line);
        const size_t first = text_of_line.find_first_not_of(' ');
        if (first == std::string::npos) continue;
        if (!together.empty()) together += " ";
        together += text_of_line.substr(first);
    }

    REQUIRE(text.find("Allow external CLAUDE.md file imports?") != std::string::npos);
    REQUIRE(together.find("This project's CLAUDE.md imports files outside the current working "
                          "directory. Never allow this for third-party repositories.") !=
            std::string::npos);
    REQUIRE(text.find("External imports:") != std::string::npos);
    REQUIRE(text.find("/tmp/notes.md") != std::string::npos);
    REQUIRE(text.find("Yes, allow external imports") != std::string::npos);
    REQUIRE(text.find("No, disable external imports") != std::string::npos);
}

TEST_CASE("The dialog of the files outside the folder leaves out its frame in the panel") {
    const auto theme = ui::Theme::dark();
    const ui::ExternalIncludesDialog standalone(theme, {}, true);
    const ui::ExternalIncludesDialog submenu(theme, {}, false);

    // The dialog that stands on its own is drawn with the rule of the
    // frame; the one that is a panel of the configuration is not.
    REQUIRE(standalone.render(80).size() > submenu.render(80).size());

    const std::string text = plain(submenu.render(80));
    REQUIRE(text.find("Enter to confirm") == std::string::npos);
}

TEST_CASE("The hint of the input dialog is dim") {
    const auto theme = ui::Theme::dark();

    const ui::InputDialog dialog(theme, "Cleanup period", "How many days?");
    const auto lines = dialog.render(60);
    const std::string hint = lines.back();

    REQUIRE(is_dim(hint));
    REQUIRE_FALSE(has_colour(hint));
}

TEST_CASE("The caret of the input dialog is a block") {
    const auto theme = ui::Theme::dark();

    const ui::InputDialog dialog(theme, "Cleanup period", "How many days?");
    bool saw_caret = false;
    for (const auto& line : dialog.render(60)) {
        if (ui::strip(line).find("› ") == std::string::npos) continue;
        saw_caret = true;
        // The reference draws a solid block, not an inverted space.
        REQUIRE(line.find("█") != std::string::npos);
    }
    REQUIRE(saw_caret);
}

TEST_CASE("The input dialog says which field it is and how it is saved") {
    const auto theme = ui::Theme::dark();

    ui::InputDialog dialog(theme, "Configure my-plugin", "Plugin options");
    dialog.set_field("The port", "The port of the plugin", 0, 3);

    const std::string joined = plain(dialog.render(60));

    REQUIRE(joined.find("Configure my-plugin") != std::string::npos);
    REQUIRE(joined.find("The port") != std::string::npos);
    REQUIRE(joined.find("The port of the plugin") != std::string::npos);
    REQUIRE(joined.find("Field 1 of 3") != std::string::npos);
    REQUIRE(joined.find("Tab: Next field · Enter: Save and continue") != std::string::npos);
}

TEST_CASE("The last field of the input dialog is saved with the enter key") {
    const auto theme = ui::Theme::dark();

    ui::InputDialog dialog(theme, "Configure my-plugin", "Plugin options");
    dialog.set_field("The port", "The port of the plugin", 2, 3);

    const std::string joined = plain(dialog.render(60));
    REQUIRE(joined.find("Field 3 of 3") != std::string::npos);
    REQUIRE(joined.find("Enter: Save configuration") != std::string::npos);
    REQUIRE(joined.find("Tab: Next field") == std::string::npos);
}

TEST_CASE("The dialog of an MCP server warns of what the servers may do") {
    const auto theme = ui::Theme::dark();

    const ui::McpServerDialog dialog(theme, "my-server");
    const std::string joined = plain(dialog.render(80));

    REQUIRE(joined.find("New MCP server found in .mcp.json: my-server") != std::string::npos);
    REQUIRE(joined.find("MCP servers may execute code or access system resources.") !=
            std::string::npos);
    REQUIRE(joined.find("Use this and all future MCP servers in this project") != std::string::npos);
    REQUIRE(joined.find("Use this MCP server") != std::string::npos);
    REQUIRE(joined.find("Continue without using this MCP server") != std::string::npos);
}

TEST_CASE("The title of the permission dialog is the title of the tool") {
    boost::json::object input;
    input["file_path"] = boost::json::value("/tmp/project/a.txt");

    NamedTool read("Read");
    REQUIRE(cli::permission_title(read, input, "/tmp/project") == "Read file");

    NamedTool edit("Edit");
    REQUIRE(cli::permission_title(edit, input, "/tmp/project") == "Edit file");

    NamedTool write("Write");
    REQUIRE(cli::permission_title(write, input, "/tmp/project") == "Create file");

    NamedTool notebook("NotebookEdit");
    REQUIRE(cli::permission_title(notebook, input, "/tmp/project") == "Edit notebook");

    NamedTool skill("Skill");
    boost::json::object skill_input;
    skill_input["skill"] = boost::json::value("my-skill");
    REQUIRE(cli::permission_title(skill, skill_input, "/tmp/project") == "Use skill \"my-skill\"?");

    NamedTool bash("Bash");
    REQUIRE(cli::permission_title(bash, input, "/tmp/project") == "Bash command");

    NamedTool other("Something");
    REQUIRE(cli::permission_title(other, input, "/tmp/project") == "Tool use");
}

TEST_CASE("The question of the permission dialog is the question of the tool") {
    boost::json::object input;
    input["file_path"] = boost::json::value("/tmp/project/a.txt");

    NamedTool edit("Edit");
    REQUIRE(ui::strip(cli::permission_question(edit, input, "/tmp/project")) ==
            "Do you want to make this edit to a.txt?");

    NamedTool write("Write");
    REQUIRE(ui::strip(cli::permission_question(write, input, "/tmp/project")) ==
            "Do you want to create a.txt?");

    NamedTool bash("Bash");
    REQUIRE(cli::permission_question(bash, input, "/tmp/project") == "Do you want to proceed?");
}
