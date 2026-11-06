#include <catch2/catch_test_macros.hpp>

#include "ui/ansi.hpp"
#include "ui/plugin_configure.hpp"
#include "ui/theme.hpp"

#include <optional>
#include <string>
#include <vector>

using namespace cc;

namespace {

/// A screen of the tests: it keeps the frames that were drawn and hands
/// the keys the test wrote down.
class FakeScreen : public ui::PluginConfigure::Screen {
public:
    void draw(const std::vector<std::string>& lines) override { frames.push_back(lines); }

    void clear() override { cleared = true; }

    int width() const override { return 80; }

    std::optional<ui::Key> read_key(int) override {
        if (keys.empty()) return std::nullopt;
        ui::Key key = keys.front();
        keys.erase(keys.begin());
        return key;
    }

    /// The input of the tests is closed when the keys run out, so that a
    /// test that forgets a key ends instead of waiting for ever.
    bool closed() const override { return keys.empty(); }

    /// Presses a key.
    void press(ui::Key::Type type, std::string text = {}) {
        ui::Key key;
        key.type = type;
        key.text = std::move(text);
        keys.push_back(std::move(key));
    }

    /// Writes a text, one key at a time.
    void type(const std::string& text) {
        for (const char c : text) press(ui::Key::Type::Character, std::string(1, c));
    }

    /// The last frame that was drawn, without the colours.
    std::string last_frame() const {
        if (frames.empty()) return {};
        std::string joined;
        for (const auto& line : frames.back()) {
            joined += ui::strip(line);
            joined += "\n";
        }
        return joined;
    }

    /// Every frame that was drawn, without the colours.
    std::string every_frame() const {
        std::string joined;
        for (const auto& frame : frames) {
            for (const auto& line : frame) {
                joined += ui::strip(line);
                joined += "\n";
            }
            joined += "---\n";
        }
        return joined;
    }

    std::vector<std::vector<std::string>> frames;
    std::vector<ui::Key> keys;
    bool cleared = false;
};

/// The option of a plugin that carries an API token.
plugins::PluginOption token_option() {
    plugins::PluginOption option;
    option.key = "token";
    option.type = "string";
    option.title = "API token";
    option.description = "The token of the service.";
    option.required = true;
    return option;
}

/// The option of a plugin that names the region it talks to.
plugins::PluginOption region_option() {
    plugins::PluginOption option;
    option.key = "region";
    option.type = "string";
    option.title = "Region";
    return option;
}

} // namespace

TEST_CASE("The plugin dialog asks for the option that must be given") {
    FakeScreen screen;
    screen.type("secret-value");
    screen.press(ui::Key::Type::Enter);

    ui::PluginConfigure dialog(ui::Theme::dark(), "Configure demo", "Plugin options");
    const auto given = dialog.run({token_option()}, boost::json::object(), screen);

    // The dialog asked for the option and gave back what the user wrote.
    REQUIRE(given.has_value());
    REQUIRE(given->find("token") != given->end());
    REQUIRE(given->at("token").as_string() == "secret-value");
}

TEST_CASE("The plugin dialog names the option it asks for") {
    FakeScreen screen;
    screen.type("secret-value");
    screen.press(ui::Key::Type::Enter);

    ui::PluginConfigure dialog(ui::Theme::dark(), "Configure demo", "Plugin options");
    dialog.run({token_option()}, boost::json::object(), screen);

    const std::string frames = screen.every_frame();

    // The title of the dialog and its subtitle.
    REQUIRE(frames.find("Configure demo") != std::string::npos);
    REQUIRE(frames.find("Plugin options") != std::string::npos);

    // The name of the option and the mark of the one that must be given.
    REQUIRE(frames.find("API token") != std::string::npos);
    REQUIRE(frames.find("*") != std::string::npos);

    // The explanation of the option stands under it.
    REQUIRE(frames.find("The token of the service.") != std::string::npos);

    // The button that hands the values over.
    REQUIRE(frames.find("Save configuration") != std::string::npos);

    // The keys are named at the bottom.
    REQUIRE(frames.find("↑/↓ to move") != std::string::npos);
    REQUIRE(frames.find("Enter to continue") != std::string::npos);
    REQUIRE(frames.find("Esc to cancel") != std::string::npos);
}

TEST_CASE("The plugin dialog refuses to hand the values over while one is missing") {
    FakeScreen screen;
    // The pointer goes to the button that hands the values over; the
    // option must be given, but nothing is written in it.
    screen.press(ui::Key::Type::Down);
    screen.press(ui::Key::Type::Enter);

    ui::PluginConfigure dialog(ui::Theme::dark(), "Configure demo", "Plugin options");
    const auto given = dialog.run({token_option()}, boost::json::object(), screen);

    // The dialog did not give the values over: the run ends when the keys
    // of the test run out.
    REQUIRE_FALSE(given.has_value());

    // The error is named beside the button.
    const std::string frames = screen.every_frame();
    REQUIRE(frames.find("Save configuration") != std::string::npos);
    REQUIRE(frames.find("API token is required") != std::string::npos);
}

TEST_CASE("The plugin dialog fills its fields in with the values that are saved") {
    boost::json::object saved;
    saved["token"] = boost::json::value("saved-token");

    FakeScreen screen;
    screen.press(ui::Key::Type::Enter);

    ui::PluginConfigure dialog(ui::Theme::dark(), "Configure demo", "Plugin options");
    const auto given = dialog.run({token_option()}, saved, screen);

    REQUIRE(given.has_value());
    REQUIRE(given->find("token") != given->end());
    REQUIRE(given->at("token").as_string() == "saved-token");

    // The value that is saved stands in the field.
    REQUIRE(screen.last_frame().find("saved-token") != std::string::npos);
}

TEST_CASE("The plugin dialog masks the value that is secret") {
    plugins::PluginOption secret = token_option();
    secret.sensitive = true;

    boost::json::object saved;
    saved["token"] = boost::json::value("saved-secret");

    FakeScreen screen;
    screen.press(ui::Key::Type::Enter);

    ui::PluginConfigure dialog(ui::Theme::dark(), "Configure demo", "Plugin options");
    dialog.run({secret}, saved, screen);

    // The secret that is saved is not shown again: the field says that the
    // value is unchanged.
    const std::string frame = screen.last_frame();
    REQUIRE(frame.find("saved-secret") == std::string::npos);
    REQUIRE(frame.find("(unchanged)") != std::string::npos);
}

TEST_CASE("The plugin dialog keeps the secret that is saved when the field is left empty") {
    plugins::PluginOption secret = token_option();
    secret.sensitive = true;

    boost::json::object saved;
    saved["token"] = boost::json::value("saved-secret");

    FakeScreen screen;
    screen.press(ui::Key::Type::Enter);

    ui::PluginConfigure dialog(ui::Theme::dark(), "Configure demo", "Plugin options");
    const auto given = dialog.run({secret}, saved, screen);

    // The secret is left out of the values: the one that is saved is kept.
    REQUIRE(given.has_value());
    REQUIRE(given->find("token") == given->end());
}

TEST_CASE("The plugin dialog asks for a boolean with a choice") {
    plugins::PluginOption flag;
    flag.key = "enabled";
    flag.type = "boolean";
    flag.title = "Enabled";

    FakeScreen screen;
    // The choice is changed with the arrow on its side.
    screen.press(ui::Key::Type::Right);
    screen.press(ui::Key::Type::Enter);
    screen.press(ui::Key::Type::Enter);

    ui::PluginConfigure dialog(ui::Theme::dark(), "Configure demo", "Plugin options");
    const auto given = dialog.run({flag}, boost::json::object(), screen);

    REQUIRE(given.has_value());
    REQUIRE(given->find("enabled") != given->end());
    REQUIRE(given->at("enabled").as_bool() == true);
}

TEST_CASE("The plugin dialog asks for a number") {
    plugins::PluginOption count;
    count.key = "count";
    count.type = "number";
    count.title = "Count";
    count.required = true;

    FakeScreen screen;
    screen.type("42");
    screen.press(ui::Key::Type::Enter);

    ui::PluginConfigure dialog(ui::Theme::dark(), "Configure demo", "Plugin options");
    const auto given = dialog.run({count}, boost::json::object(), screen);

    REQUIRE(given.has_value());
    REQUIRE(given->find("count") != given->end());
    REQUIRE(given->at("count").as_int64() == 42);
}

TEST_CASE("The plugin dialog leaves out the option that was not given") {
    FakeScreen screen;
    screen.type("secret-value");
    screen.press(ui::Key::Type::Enter);
    screen.press(ui::Key::Type::Enter);

    ui::PluginConfigure dialog(ui::Theme::dark(), "Configure demo", "Plugin options");
    const auto given = dialog.run({token_option(), region_option()}, boost::json::object(), screen);

    REQUIRE(given.has_value());
    REQUIRE(given->find("token") != given->end());
    REQUIRE(given->find("region") == given->end());
}

TEST_CASE("The plugin dialog gives nothing back when the user leaves it") {
    FakeScreen screen;
    screen.press(ui::Key::Type::Escape);

    ui::PluginConfigure dialog(ui::Theme::dark(), "Configure demo", "Plugin options");
    const auto given = dialog.run({token_option()}, boost::json::object(), screen);

    REQUIRE_FALSE(given.has_value());
    REQUIRE(screen.cleared);
}

TEST_CASE("The plugin dialog names the values that are secret") {
    plugins::PluginOption secret = token_option();
    secret.sensitive = true;
    plugins::PluginOption plain = region_option();

    boost::json::object values;
    values["token"] = boost::json::value("a-secret");
    values["region"] = boost::json::value("eu-west-1");

    const boost::json::object secrets = ui::PluginConfigure::secrets_of({secret, plain}, values);

    REQUIRE(secrets.find("token") != secrets.end());
    REQUIRE(secrets.find("region") == secrets.end());
}
