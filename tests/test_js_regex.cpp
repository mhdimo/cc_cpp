#include <catch2/catch_test_macros.hpp>

#include "hooks/hooks.hpp"
#include "utils/js_regex.hpp"

#include <boost/json.hpp>

#include <string>

using namespace cc;

namespace {

/// True when the pattern holds a match of the text.
bool matches(const std::string& pattern, const std::string& text) {
    const auto compiled = JsRegex::compile(pattern, nullptr);
    if (!compiled) return false;
    return compiled->search(text);
}

/// True when the pattern is one the port understands.
bool compiles(const std::string& pattern) {
    return JsRegex::compile(pattern, nullptr).has_value();
}

} // namespace

TEST_CASE("The patterns are read the way JavaScript reads them") {
    // The plain patterns.
    REQUIRE(matches("Write", "Write"));
    REQUIRE(matches("^Write.*", "WriteFile"));
    REQUIRE(matches("^(Write|Edit)$", "Edit"));
    REQUIRE(matches(".*", "anything"));
    REQUIRE_FALSE(matches("^Write$", "NotebookWrite"));

    // The classes.
    REQUIRE(matches("[A-Z][a-z]+", "Write"));
    REQUIRE_FALSE(matches("[A-Z][a-z]+", "WRITE"));
    REQUIRE(matches("\\d+", "42"));
    REQUIRE(matches("\\w+", "a_1"));
    REQUIRE(matches("[^\\d]", "a"));
    REQUIRE_FALSE(matches("[^\\d]", "1"));

    // The counts.
    REQUIRE(matches("a{2,3}", "aa"));
    REQUIRE(matches("a+?b", "ab"));
    REQUIRE(matches("(ab)+", "abab"));

    // The anchors and the boundaries.
    REQUIRE(matches("\\bword\\b", "a word here"));
    REQUIRE(matches("^a$", "a"));
}

TEST_CASE("The groups of a pattern may be named") {
    // A group with a name is a group, the way JavaScript reads it.
    REQUIRE(matches("(?<name>\\w+)", "Write"));
    REQUIRE(matches("(?<name>\\w+)=(?<value>\\w+)", "name=value"));
    REQUIRE(matches("(?:Write|Edit)", "Edit"));

    // The name may not be used twice in the same path.
    REQUIRE_FALSE(compiles("(?<n>a)(?<n>b)"));
    // But it may be used in two branches of the same alternation.
    REQUIRE(compiles("(?<n>a)|(?<n>b)"));
}

TEST_CASE("The pattern may look ahead and behind") {
    REQUIRE(matches("foo(?=bar)", "foobar"));
    REQUIRE_FALSE(matches("foo(?=bar)", "foobaz"));
    REQUIRE(matches("foo(?!bar)", "foobaz"));
    REQUIRE(matches("(?<=foo)bar", "foobar"));
    REQUIRE_FALSE(matches("(?<!foo)bar", "foobar"));
}

TEST_CASE("The escapes of the properties are read the way JavaScript reads them") {
    // Without the flag of the Unicode, the escape of a property is the
    // letter itself, the way JavaScript reads it.
    REQUIRE(matches("\\p{L}", "p{L}"));
    REQUIRE_FALSE(matches("\\p{L}", "Write"));

    // With the flag, it is the property of the Unicode standard.
    std::string error;
    const auto property = JsRegex::compile("\\p{L}+", "u", &error);
    REQUIRE(property.has_value());
    REQUIRE(property->search("Write"));
    REQUIRE_FALSE(property->search("123"));

    const auto upper = JsRegex::compile("\\p{Lu}", "u", nullptr);
    REQUIRE(upper.has_value());
    REQUIRE(upper->search("Write"));
    REQUIRE_FALSE(upper->search("write"));
}

TEST_CASE("The flags of JavaScript are honoured") {
    const auto ignore_case = JsRegex::compile("write", "i", nullptr);
    REQUIRE(ignore_case.has_value());
    REQUIRE(ignore_case->search("WRITE"));

    const auto across_lines = JsRegex::compile("^b", "m", nullptr);
    REQUIRE(across_lines.has_value());
    REQUIRE(across_lines->search("a\nb"));

    const auto dot_all = JsRegex::compile("a.b", "s", nullptr);
    REQUIRE(dot_all.has_value());
    REQUIRE(dot_all->search("a\nb"));
}

TEST_CASE("The references to a group are the text the group took") {
    REQUIRE(matches("(a)\\1", "aa"));
    REQUIRE_FALSE(matches("(a)\\1", "ab"));
    REQUIRE(matches("(?<letter>a)\\k<letter>", "aa"));
}

TEST_CASE("The patterns that are not one the port knows are reported") {
    std::string error;
    REQUIRE_FALSE(JsRegex::compile("(unclosed", &error).has_value());
    REQUIRE_FALSE(error.empty());

    REQUIRE_FALSE(JsRegex::compile("[unclosed", &error).has_value());
    REQUIRE_FALSE(JsRegex::compile("*nothing", &error).has_value());
    REQUIRE_FALSE(JsRegex::compile("$*", &error).has_value());
}

TEST_CASE("The matcher of a hook takes the patterns of JavaScript") {
    boost::json::object settings;
    boost::json::array hooks;
    boost::json::object group;
    group["matcher"] = boost::json::value("(?<tool>Write|Edit)");
    group["hooks"] = boost::json::array({boost::json::object{{"type", "command"},
                                                            {"command", "echo one"}}});
    hooks.emplace_back(std::move(group));

    boost::json::object named;
    named["matcher"] = boost::json::value("(?<tool>Write)");
    named["hooks"] = boost::json::array({boost::json::object{{"type", "command"},
                                                            {"command", "echo two"}}});
    hooks.emplace_back(std::move(named));

    boost::json::object events;
    events["PreToolUse"] = std::move(hooks);
    settings["hooks"] = std::move(events);

    hooks::Registry registry;
    registry.configure(settings);

    // The matcher of the hook selects the tools it names.
    const auto selected = registry.matching(hooks::Event::PreToolUse, "Write");
    REQUIRE(selected.size() == 2);

    const auto other = registry.matching(hooks::Event::PreToolUse, "Bash");
    REQUIRE(other.empty());
}
