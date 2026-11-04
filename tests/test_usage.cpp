#include <catch2/catch_test_macros.hpp>

#include "ui/ansi.hpp"
#include "ui/settings_tabs.hpp"
#include "ui/theme.hpp"
#include "utils/time.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace cc;

namespace {

/// The text of the lines, without the colours.
std::string plain(const std::vector<std::string>& lines) {
    std::string joined;
    for (const auto& line : lines) {
        joined += ui::strip(line);
        joined += "\n";
    }
    return joined;
}

} // namespace

TEST_CASE("The limit of a plan that resets in more than a day carries the date") {
    const int64_t now = cc::now_ms();
    const int64_t in_two_days = now + 2 * 24 * 60 * 60 * 1000;

    const std::string text = cc::format_reset_time(in_two_days);

    // The day of the reset is written, the way the reference writes it.
    REQUIRE(text.find(",") != std::string::npos);
    REQUIRE(text.size() > 8);

    // A limit that resets within the day carries the clock only.
    const std::string soon = cc::format_reset_time(now + 60 * 60 * 1000);
    REQUIRE(soon.find(",") == std::string::npos);
}

TEST_CASE("The time of a reset may be left out") {
    const int64_t now = cc::now_ms();
    const std::string text = cc::format_reset_time(now + 2 * 24 * 60 * 60 * 1000, false);

    // The date is there, the clock is not.
    REQUIRE(text.find(",") == std::string::npos);
    REQUIRE(!text.empty());
}

TEST_CASE("The extra usage of the plan is shown under the limits") {
    const auto theme = ui::Theme::dark();

    ui::UsageTab::ExtraUsage extra;
    extra.is_enabled = true;
    extra.monthly_limit = 5000.0;
    extra.used_credits = 123.0;
    extra.utilization = 2.46;

    const ui::UsageTab tab(theme, {}, extra);
    const std::string text = plain(tab.render(70));

    REQUIRE(text.find("Extra usage") != std::string::npos);
    REQUIRE(text.find("$1.23 / $50.00 spent") != std::string::npos);
    REQUIRE(text.find("2% used") != std::string::npos);
}

TEST_CASE("An extra usage that is not turned on says so") {
    const auto theme = ui::Theme::dark();

    ui::UsageTab::ExtraUsage extra;
    extra.is_enabled = false;

    const ui::UsageTab tab(theme, {}, extra);
    const std::string text = plain(tab.render(70));

    REQUIRE(text.find("Extra usage not enabled · /extra-usage to enable") != std::string::npos);
}

TEST_CASE("An extra usage without a limit is unlimited") {
    const auto theme = ui::Theme::dark();

    ui::UsageTab::ExtraUsage extra;
    extra.is_enabled = true;

    const ui::UsageTab tab(theme, {}, extra);
    const std::string text = plain(tab.render(70));

    REQUIRE(text.find("Unlimited") != std::string::npos);
}

TEST_CASE("The extra usage of the plan keeps the spending in the narrow shape") {
    const auto theme = ui::Theme::dark();

    ui::UsageTab::ExtraUsage extra;
    extra.is_enabled = true;
    extra.monthly_limit = 5000.0;
    extra.used_credits = 123.0;
    extra.utilization = 2.46;

    const ui::UsageTab tab(theme, {}, extra);

    // A narrow terminal has three lines: the title carries the spending and
    // the time the extra usage is given up on.
    const std::vector<std::string> lines = tab.render(40);
    std::string text;
    for (const std::string& line : lines) text += ui::strip(line) + "\n";

    REQUIRE(text.find("Extra usage · $1.23 / $50.00 spent · Resets") != std::string::npos);
    REQUIRE(text.find("2% used") != std::string::npos);
}

TEST_CASE("A reset that is less than a day away keeps its time") {
    const int64_t now = cc::now_ms();

    // The flag of the reference only touches the resets that are more than a
    // day away: a reset that is nearer is told in its time alone.
    const std::string text = cc::format_reset_time(now + 60 * 60 * 1000, false);

    REQUIRE(text.find(",") == std::string::npos);
    REQUIRE((text.find("am") != std::string::npos || text.find("pm") != std::string::npos));
}

TEST_CASE("The zone of a reset is named the way the reference names it") {
    // The name of the zone of the machine, as the system keeps it.
    std::string zone;
    if (const char* tz = std::getenv("TZ")) {
        zone = tz;
        if (!zone.empty() && zone.front() == ':') zone.erase(0, 1);
        if (zone.find('/') == std::string::npos) zone.clear();
    }
    if (zone.empty()) {
        std::error_code ec;
        const std::filesystem::path link = std::filesystem::read_symlink("/etc/localtime", ec);
        if (ec) SKIP("the system does not name its zone");
        const std::string name = link.string();
        const size_t at = name.find("zoneinfo/");
        if (at == std::string::npos) SKIP("the system does not name its zone");
        zone = name.substr(at + 9);
    }

    const int64_t now = cc::now_ms();
    const std::string text = cc::format_reset_time(now + 60 * 60 * 1000);

    // The zone is in the brackets, and it is the name of the zone and not
    // its abbreviation.
    REQUIRE(text.find("(" + zone + ")") != std::string::npos);
}
