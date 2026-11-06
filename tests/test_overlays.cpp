#include <catch2/catch_test_macros.hpp>

#include "tools/tool_names.hpp"
#include "ui/ansi.hpp"
#include "ui/help_overlay.hpp"
#include "ui/question_dialog.hpp"
#include "ui/select.hpp"
#include "ui/theme.hpp"
#include "ui/tool_display.hpp"
#include "ui/trust_dialog.hpp"

using namespace cc;

TEST_CASE("The picker marks the focused entry") {
    const auto theme = ui::Theme::dark();

    const std::vector<ui::SelectOption> options = {
        {"Opus", "The big one"},
        {"Sonnet", "The fast one"},
    };
    const ui::Select select(theme, options);

    const auto lines = select.render(60);
    REQUIRE(lines.size() == 2);

    const std::string first = ui::strip(lines[0]);
    REQUIRE(first.find("❯ 1. Opus") != std::string::npos);

    // The description sits in a column of its own.
    REQUIRE(first.find("The big one") != std::string::npos);

    const std::string second = ui::strip(lines[1]);
    REQUIRE(second.find("2. Sonnet") != std::string::npos);
    REQUIRE(second.find("The fast one") != std::string::npos);
    REQUIRE(second.find("❯") == std::string::npos);
}

TEST_CASE("The question dialog asks the question and offers the choices") {
    const auto theme = ui::Theme::dark();

    UserQuestion question;
    question.text = "Which library should we use?";
    question.header = "Library";
    question.options.push_back(UserQuestionOption{"date-fns", "The modern one", {}});
    question.options.push_back(UserQuestionOption{"moment", "The old one", {}});

    const ui::QuestionDialog dialog(theme, question);
    const auto lines = dialog.render(60);

    std::string joined;
    for (const auto& line : lines) {
        joined += ui::strip(line);
        joined += "\n";
    }

    REQUIRE(joined.find("Which library should we use?") != std::string::npos);
    // The label of the choice follows the number without a space between
    // them, the way the reference writes it.
    REQUIRE(joined.find("❯ 1.date-fns") != std::string::npos);
    REQUIRE(joined.find("The modern one") != std::string::npos);
    REQUIRE(joined.find("2.moment") != std::string::npos);
    REQUIRE(joined.find("Enter to select") != std::string::npos);
}

TEST_CASE("The trust dialog asks whether the folder is trusted") {
    const auto theme = ui::Theme::dark();

    const ui::TrustDialog dialog(theme, "/tmp/project");
    const auto lines = dialog.render(70);

    std::string joined;
    for (const auto& line : lines) {
        joined += ui::strip(line);
        joined += "\n";
    }

    REQUIRE(joined.find("Accessing workspace:") != std::string::npos);
    REQUIRE(joined.find("/tmp/project") != std::string::npos);
    REQUIRE(joined.find("Quick safety check") != std::string::npos);
    REQUIRE(joined.find("Yes, I trust this folder") != std::string::npos);
    REQUIRE(joined.find("No, exit") != std::string::npos);
    REQUIRE(joined.find("Enter to confirm") != std::string::npos);
}

TEST_CASE("The help overlay shows the tabs and the shortcuts") {
    const auto theme = ui::Theme::dark();

    const std::vector<ui::HelpCommand> commands = {
        {"/clear", "Clear conversation history"},
        {"/compact", "Clear conversation history but keep a summary"},
    };
    const ui::HelpOverlay overlay(theme, commands, {});

    // The pane of the help is at most half the screen tall, so the screen
    // has to be tall enough for the whole of the tab.
    const auto lines = overlay.render(80, 80);

    std::string joined;
    for (const auto& line : lines) {
        joined += ui::strip(line);
        joined += "\n";
    }

    REQUIRE(joined.find("Claude Code v") != std::string::npos);
    REQUIRE(joined.find("general") != std::string::npos);
    REQUIRE(joined.find("commands") != std::string::npos);
    REQUIRE(joined.find("custom-commands") != std::string::npos);
    REQUIRE(joined.find("Shortcuts") != std::string::npos);
    REQUIRE(joined.find("For more help") != std::string::npos);
    // The name of the key that dismisses the pane is written the way the
    // reference writes it.
    REQUIRE(joined.find("Esc to cancel") != std::string::npos);
}

TEST_CASE("The agent tool reports what the sub-agent did") {
    const auto theme = ui::Theme::dark();

    boost::json::object payload;
    payload["totalToolUseCount"] = 3;
    payload["totalTokens"] = 1321;
    payload["totalDurationMs"] = 5000;

    const auto lines = ui::render_tool_result_lines(tools::AGENT, "", boost::json::value(payload),
                                                    "/tmp", theme, 60, false);

    REQUIRE(lines.size() == 2);
    const std::string text = ui::strip(lines[0]);
    INFO("rendered: [" << text << "]");
    REQUIRE(text.find("Done (3 tool uses · 1.3k tokens · 5s)") != std::string::npos);
    REQUIRE(ui::strip(lines[1]).find("(ctrl+o to expand)") != std::string::npos);
}
