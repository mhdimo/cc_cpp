#include <catch2/catch_test_macros.hpp>

#include "permissions/content_match.hpp"
#include "permissions/gitignore.hpp"
#include "permissions/permissions.hpp"
#include "permissions/rule_parser.hpp"
#include "permissions/shell_rules.hpp"
#include "tools/impl/bash_tool.hpp"
#include "tools/impl/file_tools.hpp"
#include "tools/tool_use_context.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <boost/json.hpp>

#include <memory>
#include <string>

using namespace cc;

namespace {

/// The rules of one behaviour, as they are written in the settings.
ToolPermissionContext context_with(PermissionBehavior behavior,
                                   const std::vector<std::string>& rules,
                                   PermissionRuleSource source = PermissionRuleSource::UserSettings) {
    ToolPermissionContext context;
    PermissionRulesBySource* target = &context.always_allow_rules;
    switch (behavior) {
    case PermissionBehavior::Allow:
        target = &context.always_allow_rules;
        break;
    case PermissionBehavior::Deny:
        target = &context.always_deny_rules;
        break;
    case PermissionBehavior::Ask:
        target = &context.always_ask_rules;
        break;
    }
    (*target)[source] = rules;
    return context;
}

/// The folders the rules of the paths are resolved against.
permissions::RuleRoots roots_at(const std::string& directory) {
    permissions::RuleRoots roots;
    roots.original_cwd = directory;
    roots.cwd = directory;
    return roots;
}

/// A tool that fetches the pages, the way the reference's WebFetch tool does.
///
/// The port has no such tool yet; this one shows how it would be matched.
class WebFetchTool : public Tool {
public:
    std::string_view name() const override { return "WebFetch"; }
    std::string description() const override { return "Fetch a page"; }
    boost::json::object input_schema() const override { return {}; }
    ToolResult call(const boost::json::object& /*input*/, ToolUseContext& /*context*/) override {
        return {};
    }

    /// The host of the page, the way the reference reads it.
    std::optional<std::string> permission_content(const boost::json::object& input) const override {
        return permissions::hostname_of_url(jget_string(input, "url"));
    }
};

/// The tools the tests are run against.
struct TestTools {
    std::shared_ptr<tools::FileStateTracker> tracker = std::make_shared<tools::FileStateTracker>();
    tools::BashTool bash;
    tools::ReadTool read{tracker};
    tools::WriteTool write{tracker};
    tools::EditTool edit{tracker};
    WebFetchTool web;
};

/// The input of a call of the Bash tool.
boost::json::object bash_input(const std::string& command) {
    boost::json::object input;
    input["command"] = command;
    return input;
}

/// The input of a call of a tool that works on a file.
boost::json::object file_input(const std::string& path) {
    boost::json::object input;
    input["file_path"] = path;
    return input;
}

} // namespace

TEST_CASE("The rules of a shell command are matched against the command") {
    TestTools tools;

    SECTION("The rule of the old shape is a prefix") {
        const auto context = context_with(PermissionBehavior::Allow, {"Bash(git status:*)"});

        // The command is the prefix itself.
        REQUIRE(permissions::match_shell_rules(context, tools.bash, "git status").allow.has_value());
        // The command begins with the prefix, at a word.
        REQUIRE(permissions::match_shell_rules(context, tools.bash, "git status --short").allow.has_value());
        // The prefix has to end at a word.
        REQUIRE_FALSE(permissions::match_shell_rules(context, tools.bash, "git statusfoo").allow.has_value());
        // "xargs git status" runs git status.
        REQUIRE(permissions::match_shell_rules(context, tools.bash, "xargs git status").allow.has_value());
    }

    SECTION("A command of several parts is matched part by part") {
        const auto context = context_with(PermissionBehavior::Allow, {"Bash(git status:*)"});

        // The part that is allowed does not allow the whole command.
        const auto match = permissions::match_shell_rules(context, tools.bash, "git status && rm -rf /");
        REQUIRE_FALSE(match.allow.has_value());
        REQUIRE_FALSE(match.deny.has_value());
        REQUIRE_FALSE(match.ask.has_value());
    }

    SECTION("The rule of a command is the command itself") {
        const auto context = context_with(PermissionBehavior::Allow, {"Bash(npm run test)"});

        REQUIRE(permissions::match_shell_rules(context, tools.bash, "npm run test").allow.has_value());
        REQUIRE_FALSE(permissions::match_shell_rules(context, tools.bash, "npm run test --watch").allow.has_value());
        REQUIRE_FALSE(permissions::match_shell_rules(context, tools.bash, "npm run").allow.has_value());
    }

    SECTION("The rule of a pattern is matched against the parts of the command") {
        const auto context = context_with(PermissionBehavior::Allow, {"Bash(git *)"});

        REQUIRE(permissions::match_shell_rules(context, tools.bash, "git status").allow.has_value());
        REQUIRE(permissions::match_shell_rules(context, tools.bash, "git status --short").allow.has_value());
        REQUIRE_FALSE(permissions::match_shell_rules(context, tools.bash, "ls -la").allow.has_value());
    }

    SECTION("The redirections of the output are not part of the command") {
        const auto context = context_with(PermissionBehavior::Allow, {"Bash(python:*)"});

        REQUIRE(permissions::match_shell_rules(context, tools.bash, "python script.py > output.txt").allow.has_value());
        REQUIRE(permissions::match_shell_rules(context, tools.bash, "python script.py >> output.txt")
                    .allow.has_value());
        REQUIRE(permissions::match_shell_rules(context, tools.bash, "python script.py 2>&1").allow.has_value());
    }
}

TEST_CASE("The rules that deny a command are harder to get around") {
    TestTools tools;

    SECTION("A denied command stays denied behind a variable") {
        const auto context = context_with(PermissionBehavior::Deny, {"Bash(rm:*)"});

        REQUIRE(permissions::match_shell_rules(context, tools.bash, "rm -rf /").deny.has_value());
        REQUIRE(permissions::match_shell_rules(context, tools.bash, "FOO=bar rm -rf /").deny.has_value());
        REQUIRE(permissions::match_shell_rules(context, tools.bash, "timeout 30 rm -rf /").deny.has_value());
        REQUIRE(permissions::match_shell_rules(context, tools.bash, "xargs rm file").deny.has_value());
    }

    SECTION("A denied command stays denied inside a command of several parts") {
        const auto context = context_with(PermissionBehavior::Deny, {"Bash(rm:*)"});

        REQUIRE(permissions::match_shell_rules(context, tools.bash, "echo hello && rm -rf /").deny.has_value());
    }

    SECTION("A rule that allows does not strip the variables that are not safe") {
        // The variable changes the binary that runs, so the rule of the
        // command must not match it.
        const auto context = context_with(PermissionBehavior::Allow, {"Bash(docker ps:*)"});

        REQUIRE(permissions::match_shell_rules(context, tools.bash, "docker ps").allow.has_value());
        REQUIRE_FALSE(
            permissions::match_shell_rules(context, tools.bash, "DOCKER_HOST=tcp://evil docker ps")
                .allow.has_value());
        // A variable that cannot execute code is stripped.
        REQUIRE(permissions::match_shell_rules(context, tools.bash, "NODE_ENV=prod docker ps").allow.has_value());
    }
}

TEST_CASE("The rules of a path are matched against the path") {
    TestTools tools;

    SECTION("The rule that begins with two separators is fixed to the root") {
        const auto context = context_with(PermissionBehavior::Deny, {"Read(//etc/passwd)"});

        REQUIRE(permissions::match_path_rules(context, tools.read, "/etc/passwd", roots_at("/tmp")).deny
                    .has_value());
        REQUIRE_FALSE(permissions::match_path_rules(context, tools.read, "/etc/passwdx", roots_at("/tmp")).deny
                         .has_value());
    }

    SECTION("The rule that begins with a separator is fixed to the folder of the settings") {
        // The rules of the project are resolved against the folder the
        // session runs in.
        const auto context = context_with(PermissionBehavior::Allow, {"Edit(/src/**)"},
                                          PermissionRuleSource::ProjectSettings);

        REQUIRE(permissions::match_path_rules(context, tools.edit, "/project/src/a/b.cpp", roots_at("/project"))
                    .allow.has_value());
        REQUIRE_FALSE(
            permissions::match_path_rules(context, tools.edit, "/src/a/b.cpp", roots_at("/project"))
                .allow.has_value());
    }

    SECTION("The rule that begins with two separators is fixed to the root of the machine") {
        const auto context = context_with(PermissionBehavior::Allow, {"Edit(//src/**)"},
                                          PermissionRuleSource::ProjectSettings);

        REQUIRE(permissions::match_path_rules(context, tools.edit, "/src/a/b.cpp", roots_at("/project"))
                    .allow.has_value());
    }

    SECTION("The rule that begins with a tilde is fixed to the home of the user") {
        const auto context = context_with(PermissionBehavior::Allow, {"Read(~/notes/**)"});

        const std::string path = cc::path_join(cc::home_dir(), "notes/today.md");
        REQUIRE(permissions::match_path_rules(context, tools.read, path, roots_at("/tmp")).allow.has_value());
        REQUIRE_FALSE(permissions::match_path_rules(context, tools.read, "/notes/today.md", roots_at("/tmp"))
                         .allow.has_value());
    }

    SECTION("The rule that names no folder matches at every level") {
        const auto context = context_with(PermissionBehavior::Deny, {"Read(*.log)"});

        REQUIRE(permissions::match_path_rules(context, tools.read, "/project/a/b/secret.log", roots_at("/project"))
                    .deny.has_value());
        REQUIRE_FALSE(
            permissions::match_path_rules(context, tools.read, "/project/a/b/secret.txt", roots_at("/project"))
                .deny.has_value());
    }

    SECTION("The rules of the files that are changed are the rules of the tool that changes them") {
        const auto context = context_with(PermissionBehavior::Allow, {"Edit(/src/**)"},
                                          PermissionRuleSource::ProjectSettings);

        // The tool that writes the file is allowed by the rule of the tool
        // that changes the files.
        REQUIRE(permissions::match_path_rules(context, tools.write, "/project/src/a/b.cpp", roots_at("/project"))
                    .allow.has_value());
        // The access to a file that may be changed is the access to read it.
        REQUIRE(permissions::match_path_rules(context, tools.read, "/project/src/a/b.cpp", roots_at("/project"))
                    .allow.has_value());
    }

    SECTION("The rules of the files that are read are the rules of the tool that reads them") {
        const auto context = context_with(PermissionBehavior::Deny, {"Read(/private/**)"},
                                          PermissionRuleSource::ProjectSettings);

        REQUIRE(permissions::match_path_rules(context, tools.read, "/project/private/keys.txt", roots_at("/project"))
                    .deny.has_value());
        // The tool that changes the files does not read them.
        REQUIRE_FALSE(
            permissions::match_path_rules(context, tools.write, "/project/private/keys.txt", roots_at("/project"))
                .deny.has_value());
    }
}

TEST_CASE("The rules of a host are matched against the host") {
    TestTools tools;

    const auto context = context_with(PermissionBehavior::Allow, {"WebFetch(domain:example.com)"});

    REQUIRE(permissions::match_domain_rules(context, tools.web, "example.com").allow.has_value());
    REQUIRE_FALSE(permissions::match_domain_rules(context, tools.web, "www.example.com").allow.has_value());
    REQUIRE_FALSE(
        permissions::match_domain_rules(context, tools.web, "example.com.evil.net").allow.has_value());

    // The wildcard of a rule of a host never matches.
    const auto wildcard = context_with(PermissionBehavior::Allow, {"WebFetch(domain:*.example.com)"});
    REQUIRE_FALSE(permissions::match_domain_rules(wildcard, tools.web, "www.example.com").allow.has_value());
}

TEST_CASE("The rules of the content of a rule are read by the pipeline") {
    tools::BashTool tool;

    ToolUseContext context;
    context.options.cwd = "/tmp/project";
    context.original_cwd = "/tmp/project";

    SECTION("A rule that denies the command denies the call") {
        context.permission_context = context_with(PermissionBehavior::Deny, {"Bash(rm:*)"});

        const auto result = permissions::has_permissions_to_use_tool(tool, bash_input("rm -rf /"), context);
        REQUIRE(result.is_deny());
        REQUIRE(result.decision_reason.has_value());
        REQUIRE(result.decision_reason->type == DecisionReasonType::Rule);
    }

    SECTION("A rule that allows the command allows the call") {
        context.permission_context = context_with(PermissionBehavior::Allow, {"Bash(git status:*)"});

        const auto result = permissions::has_permissions_to_use_tool(tool, bash_input("git status"), context);
        REQUIRE(result.is_allow());
    }

    SECTION("A rule that asks for the command asks the user") {
        context.permission_context = context_with(PermissionBehavior::Ask, {"Bash(npm publish:*)"});

        const auto result = permissions::has_permissions_to_use_tool(tool, bash_input("npm publish"), context);
        REQUIRE(result.is_ask());
    }

    SECTION("A rule that denies the command is not bypassed") {
        context.permission_context = context_with(PermissionBehavior::Deny, {"Bash(rm:*)"});
        context.permission_context.mode = PermissionMode::BypassPermissions;

        const auto result = permissions::has_permissions_to_use_tool(tool, bash_input("rm -rf /"), context);
        REQUIRE(result.is_deny());
    }

    SECTION("A rule that asks for the command is not bypassed") {
        context.permission_context = context_with(PermissionBehavior::Ask, {"Bash(npm publish:*)"});
        context.permission_context.mode = PermissionMode::BypassPermissions;

        const auto result = permissions::has_permissions_to_use_tool(tool, bash_input("npm publish"), context);
        REQUIRE(result.is_ask());
    }

    SECTION("A rule that allows the command is bypassed by the mode of the session") {
        context.permission_context = context_with(PermissionBehavior::Allow, {"Bash(git status:*)"});
        context.permission_context.mode = PermissionMode::BypassPermissions;

        const auto result = permissions::has_permissions_to_use_tool(tool, bash_input("git status"), context);
        REQUIRE(result.is_allow());
    }

    SECTION("A command that no rule names asks the user") {
        context.permission_context = context_with(PermissionBehavior::Allow, {"Bash(git status:*)"});

        const auto result = permissions::has_permissions_to_use_tool(tool, bash_input("ls -la"), context);
        REQUIRE(result.is_ask());
    }
}

TEST_CASE("The rules of the files are read by the pipeline") {
    auto tracker = std::make_shared<tools::FileStateTracker>();
    tools::ReadTool read(tracker);
    tools::EditTool edit(tracker);

    ToolUseContext context;
    context.options.cwd = "/tmp/project";
    context.original_cwd = "/tmp/project";
    context.permission_context = context_with(PermissionBehavior::Deny, {"Read(//etc/passwd)"});

    const auto denied = permissions::has_permissions_to_use_tool(read, file_input("/etc/passwd"), context);
    REQUIRE(denied.is_deny());

    const auto other = permissions::has_permissions_to_use_tool(read, file_input("/etc/passwdx"), context);
    REQUIRE_FALSE(other.is_deny());

    SECTION("The rules of the files that are changed are read as well") {
        context.permission_context =
            context_with(PermissionBehavior::Allow, {"Edit(/src/**)"},
                         PermissionRuleSource::ProjectSettings);

        const auto result = permissions::has_permissions_to_use_tool(edit, file_input("/tmp/project/src/a.cpp"),
                                                                    context);
        REQUIRE(result.is_allow());
    }
}

TEST_CASE("The patterns of the paths are read the way the rules of a gitignore are") {
    SECTION("The star does not cross the separator of the folders") {
        REQUIRE(permissions::gitignore_matches("src/*.cpp", "src/main.cpp"));
        REQUIRE_FALSE(permissions::gitignore_matches("src/*.cpp", "src/deep/main.cpp"));
    }

    SECTION("The two stars cross the separator of the folders") {
        REQUIRE(permissions::gitignore_matches("src/**/*.cpp", "src/deep/main.cpp"));
        REQUIRE(permissions::gitignore_matches("src/**/*.cpp", "src/main.cpp"));
    }

    SECTION("The question mark matches one character") {
        REQUIRE(permissions::gitignore_matches("file?.txt", "file1.txt"));
        REQUIRE_FALSE(permissions::gitignore_matches("file?.txt", "file12.txt"));
    }

    SECTION("A pattern that names a folder takes everything inside it") {
        REQUIRE(permissions::gitignore_matches("build", "build/deep/file.o"));
        REQUIRE_FALSE(permissions::gitignore_matches("build", "builds/file.o"));
    }

    SECTION("A pattern that is negated takes the file back") {
        const std::vector<std::string> patterns = {"*.log", "!keep.log"};
        REQUIRE_FALSE(permissions::gitignore_match(patterns, "keep.log").has_value());
        REQUIRE(permissions::gitignore_match(patterns, "other.log").has_value());
    }

    SECTION("A pattern that names no folder matches at every level") {
        REQUIRE(permissions::gitignore_matches("*.log", "a/b/c.log"));
        REQUIRE(permissions::gitignore_matches("*.log", "c.log"));
    }
}

TEST_CASE("The rules that can never be reached are found") {
    SECTION("A rule that denies the whole tool hides the rules of its content") {
        ToolPermissionContext context;
        context.always_allow_rules[PermissionRuleSource::UserSettings] = {"Bash(ls:*)"};
        context.always_deny_rules[PermissionRuleSource::UserSettings] = {"Bash"};

        const auto unreachable = permissions::detect_unreachable_rules(context);
        REQUIRE(unreachable.size() == 1);
        REQUIRE(unreachable[0].shadow_type == PermissionBehavior::Deny);
        REQUIRE(unreachable[0].rule.rule_value.rule_content.value_or("") == "ls:*");
        REQUIRE(unreachable[0].shadowed_by.rule_value.tool_name == "Bash");
        REQUIRE(unreachable[0].reason.find("deny rule") != std::string::npos);
        REQUIRE(unreachable[0].fix.find("deny rule") != std::string::npos);
    }

    SECTION("A rule that asks for the whole tool hides the rules of its content") {
        ToolPermissionContext context;
        context.always_allow_rules[PermissionRuleSource::UserSettings] = {"Bash(ls:*)"};
        context.always_ask_rules[PermissionRuleSource::UserSettings] = {"Bash"};

        const auto unreachable = permissions::detect_unreachable_rules(context);
        REQUIRE(unreachable.size() == 1);
        REQUIRE(unreachable[0].shadow_type == PermissionBehavior::Ask);
    }

    SECTION("The rule of the whole tool cannot be hidden") {
        ToolPermissionContext context;
        context.always_allow_rules[PermissionRuleSource::UserSettings] = {"Bash"};
        context.always_deny_rules[PermissionRuleSource::UserSettings] = {"Bash"};

        REQUIRE(permissions::detect_unreachable_rules(context).empty());
    }

    SECTION("The rule of another tool does not hide the rule of this one") {
        ToolPermissionContext context;
        context.always_allow_rules[PermissionRuleSource::UserSettings] = {"Bash(ls:*)"};
        context.always_deny_rules[PermissionRuleSource::UserSettings] = {"Read"};

        REQUIRE(permissions::detect_unreachable_rules(context).empty());
    }
}

TEST_CASE("The host of a URL is read the way the reference reads it") {
    REQUIRE(permissions::hostname_of_url("https://example.com") == "example.com");
    REQUIRE(permissions::hostname_of_url("https://example.com/path?q=1") == "example.com");
    REQUIRE(permissions::hostname_of_url("https://www.example.com:8080/path") == "www.example.com");
    REQUIRE(permissions::hostname_of_url("not a url").empty());
}

TEST_CASE("The rules of the shell are read the way the reference reads them") {
    SECTION("The prefix of the old shape is taken off the rule") {
        const auto prefix = permissions::extract_prefix("npm:*");
        REQUIRE(prefix.has_value());
        REQUIRE(*prefix == "npm");
        REQUIRE_FALSE(permissions::extract_prefix("npm").has_value());
    }

    SECTION("A wildcard that is escaped is not a wildcard") {
        REQUIRE(permissions::has_wildcards("git *"));
        REQUIRE_FALSE(permissions::has_wildcards("git \\*"));
        REQUIRE_FALSE(permissions::has_wildcards("npm:*"));
    }

    SECTION("The command of a rule of a pattern is the whole command") {
        REQUIRE(permissions::matches_wildcard("git *", "git status"));
        REQUIRE(permissions::matches_wildcard("git *", "git"));
        REQUIRE_FALSE(permissions::matches_wildcard("git *", "ls -la"));
    }

    SECTION("The command is split at the operators") {
        const auto parts = permissions::split_command("git status && rm -rf /");
        REQUIRE(parts.size() == 2);
        REQUIRE(cc::trim(parts[0]) == "git status");
        REQUIRE(cc::trim(parts[1]) == "rm -rf /");
    }

    SECTION("The wrappers are taken off the command") {
        REQUIRE(permissions::strip_safe_wrappers("timeout 10 npm install foo") == "npm install foo");
        REQUIRE(permissions::strip_safe_wrappers("timeout --kill-after=5 10 ls") == "ls");
        REQUIRE(permissions::strip_safe_wrappers("time ls") == "ls");
        REQUIRE(permissions::strip_safe_wrappers("nice -n 5 ls -la") == "ls -la");
        REQUIRE(permissions::strip_safe_wrappers("stdbuf -o0 ls") == "ls");
        REQUIRE(permissions::strip_safe_wrappers("nohup -- rm -- -/../foo") == "rm -- -/../foo");
        // A wrapper that is not one of them stays.
        REQUIRE(permissions::strip_safe_wrappers("env ls") == "env ls");
    }

    SECTION("Only the variables that are safe to strip are taken off the command") {
        REQUIRE(permissions::strip_safe_wrappers("NODE_ENV=prod npm run build") == "npm run build");
        REQUIRE(permissions::strip_safe_wrappers("GOOS=linux go build") == "go build");
        // The variable names the binary that runs, so it stays.
        REQUIRE(permissions::strip_safe_wrappers("DOCKER_HOST=tcp://evil docker ps") ==
                "DOCKER_HOST=tcp://evil docker ps");
    }

    SECTION("Every variable is taken off the command of a rule that denies") {
        REQUIRE(permissions::strip_all_leading_env_vars("FOO=bar rm -rf /") == "rm -rf /");
        REQUIRE(permissions::strip_all_leading_env_vars("FOO=a=b rm") == "rm");
        REQUIRE(permissions::strip_all_leading_env_vars("FOO='bar baz' rm") == "rm");
        // The name of the variable is not quoted, so this is not a variable.
        REQUIRE(permissions::strip_all_leading_env_vars("'FOO=bar' rm") == "'FOO=bar' rm");
    }

    SECTION("The redirections of the output are taken off the command") {
        REQUIRE(permissions::strip_output_redirections("python script.py > output.txt") ==
                "python script.py");
        REQUIRE(permissions::strip_output_redirections("ls 2>&1") == "ls");
        REQUIRE(permissions::strip_output_redirections("echo hi > out.txt && ls") == "echo hi && ls");
        // The redirections of the input stay.
        REQUIRE(permissions::strip_output_redirections("cat < input.txt") == "cat < input.txt");
    }
}
