#include "config/environment.hpp"

#include <cctype>
#include <cstdlib>
#include <set>

namespace cc::config {
namespace {

/// The variables that may be set before the folder is trusted.
///
/// The list is the one of the reference. The variables that are missing from
/// it are the ones that could send the session somewhere else — the address
/// of the API, the key, the proxies, the certificates — so they wait for the
/// trust.
const std::set<std::string>& safe_env_vars() {
    static const std::set<std::string> vars = {
        "ANTHROPIC_CUSTOM_HEADERS",
        "ANTHROPIC_CUSTOM_MODEL_OPTION",
        "ANTHROPIC_CUSTOM_MODEL_OPTION_DESCRIPTION",
        "ANTHROPIC_CUSTOM_MODEL_OPTION_NAME",
        "ANTHROPIC_DEFAULT_HAIKU_MODEL",
        "ANTHROPIC_DEFAULT_HAIKU_MODEL_DESCRIPTION",
        "ANTHROPIC_DEFAULT_HAIKU_MODEL_NAME",
        "ANTHROPIC_DEFAULT_HAIKU_MODEL_SUPPORTED_CAPABILITIES",
        "ANTHROPIC_DEFAULT_OPUS_MODEL",
        "ANTHROPIC_DEFAULT_OPUS_MODEL_DESCRIPTION",
        "ANTHROPIC_DEFAULT_OPUS_MODEL_NAME",
        "ANTHROPIC_DEFAULT_OPUS_MODEL_SUPPORTED_CAPABILITIES",
        "ANTHROPIC_DEFAULT_SONNET_MODEL",
        "ANTHROPIC_DEFAULT_SONNET_MODEL_DESCRIPTION",
        "ANTHROPIC_DEFAULT_SONNET_MODEL_NAME",
        "ANTHROPIC_DEFAULT_SONNET_MODEL_SUPPORTED_CAPABILITIES",
        "ANTHROPIC_FOUNDRY_API_KEY",
        "ANTHROPIC_MODEL",
        "ANTHROPIC_SMALL_FAST_MODEL_AWS_REGION",
        "ANTHROPIC_SMALL_FAST_MODEL",
        "AWS_DEFAULT_REGION",
        "AWS_PROFILE",
        "AWS_REGION",
        "BASH_DEFAULT_TIMEOUT_MS",
        "BASH_MAX_OUTPUT_LENGTH",
        "BASH_MAX_TIMEOUT_MS",
        "CLAUDE_BASH_MAINTAIN_PROJECT_WORKING_DIR",
        "CLAUDE_CODE_API_KEY_HELPER_TTL_MS",
        "CLAUDE_CODE_DISABLE_EXPERIMENTAL_BETAS",
        "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC",
        "CLAUDE_CODE_DISABLE_TERMINAL_TITLE",
        "CLAUDE_CODE_ENABLE_TELEMETRY",
        "CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS",
        "CLAUDE_CODE_IDE_SKIP_AUTO_INSTALL",
        "CLAUDE_CODE_MAX_OUTPUT_TOKENS",
        "CLAUDE_CODE_SKIP_BEDROCK_AUTH",
        "CLAUDE_CODE_SKIP_FOUNDRY_AUTH",
        "CLAUDE_CODE_SKIP_VERTEX_AUTH",
        "CLAUDE_CODE_SUBAGENT_MODEL",
        "CLAUDE_CODE_USE_BEDROCK",
        "CLAUDE_CODE_USE_FOUNDRY",
        "CLAUDE_CODE_USE_VERTEX",
        "DISABLE_AUTOUPDATER",
        "DISABLE_BUG_COMMAND",
        "DISABLE_COST_WARNINGS",
        "DISABLE_ERROR_REPORTING",
        "DISABLE_FEEDBACK_COMMAND",
        "DISABLE_TELEMETRY",
        "ENABLE_TOOL_SEARCH",
        "MAX_MCP_OUTPUT_TOKENS",
        "MAX_THINKING_TOKENS",
        "MCP_TIMEOUT",
        "MCP_TOOL_TIMEOUT",
        "OTEL_EXPORTER_OTLP_HEADERS",
        "OTEL_EXPORTER_OTLP_LOGS_HEADERS",
        "OTEL_EXPORTER_OTLP_LOGS_PROTOCOL",
        "OTEL_EXPORTER_OTLP_METRICS_CLIENT_CERTIFICATE",
        "OTEL_EXPORTER_OTLP_METRICS_CLIENT_KEY",
        "OTEL_EXPORTER_OTLP_METRICS_HEADERS",
        "OTEL_EXPORTER_OTLP_METRICS_PROTOCOL",
        "OTEL_EXPORTER_OTLP_PROTOCOL",
        "OTEL_EXPORTER_OTLP_TRACES_HEADERS",
        "OTEL_LOG_TOOL_DETAILS",
        "OTEL_LOG_USER_PROMPTS",
        "OTEL_LOGS_EXPORT_INTERVAL",
        "OTEL_LOGS_EXPORTER",
        "OTEL_METRIC_EXPORT_INTERVAL",
        "OTEL_METRICS_EXPORTER",
        "OTEL_METRICS_INCLUDE_ACCOUNT_UUID",
        "OTEL_METRICS_INCLUDE_SESSION_ID",
        "OTEL_METRICS_INCLUDE_VERSION",
        "OTEL_RESOURCE_ATTRIBUTES",
        "USE_BUILTIN_RIPGREP",
        "VERTEX_REGION_CLAUDE_3_5_HAIKU",
        "VERTEX_REGION_CLAUDE_3_5_SONNET",
        "VERTEX_REGION_CLAUDE_3_7_SONNET",
        "VERTEX_REGION_CLAUDE_4_0_OPUS",
        "VERTEX_REGION_CLAUDE_4_0_SONNET",
        "VERTEX_REGION_CLAUDE_4_1_OPUS",
        "VERTEX_REGION_CLAUDE_4_5_SONNET",
        "VERTEX_REGION_CLAUDE_4_6_SONNET",
        "VERTEX_REGION_CLAUDE_HAIKU_4_5",
    };
    return vars;
}

/// The name of a variable, in capitals.
///
/// The names of the variables of the environment are not case sensitive on
/// the platforms the port runs on, so the list is compared in one case.
std::string upper(const std::string& key) {
    std::string out = key;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

bool is_safe_env_var(const std::string& key) {
    return safe_env_vars().count(upper(key)) > 0;
}

void apply_env(const std::map<std::string, std::string>& variables) {
    for (const auto& [key, value] : variables) {
        if (key.empty()) continue;
        ::setenv(key.c_str(), value.c_str(), 1);
    }
}

} // namespace cc::config
