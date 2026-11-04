#include "utils/model.hpp"

#include "auth/storage.hpp"
#include "utils/string.hpp"

namespace cc {
namespace {

/// True when `model` names one of the given public model ids.
bool is_model(std::string_view model, std::string_view id) { return model == id; }

/// One of the public model ids Claude Code knows by name.
struct KnownModel {
    /// The id of the model.
    std::string_view id;
    /// The name the model is shown with.
    std::string_view name;
};

/// The public model ids Claude Code knows by name.
constexpr KnownModel kKnownModels[] = {
    {"claude-opus-4-6", "Opus 4.6"},
    {"claude-opus-4-5", "Opus 4.5"},
    {"claude-opus-4-1", "Opus 4.1"},
    {"claude-opus-4-0", "Opus 4"},
    {"claude-sonnet-4-6", "Sonnet 4.6"},
    {"claude-sonnet-4-5", "Sonnet 4.5"},
    {"claude-sonnet-4-0", "Sonnet 4"},
    {"claude-sonnet-3-7", "Sonnet 3.7"},
    {"claude-sonnet-3-5", "Sonnet 3.5"},
    {"claude-haiku-4-5", "Haiku 4.5"},
    {"claude-haiku-3-5", "Haiku 3.5"},
};

/// True when the name is one of the names the reference gives its models.
bool is_known_name(std::string_view name) {
    for (const KnownModel& model : kKnownModels) {
        if (model.name == name) return true;
    }
    return false;
}

} // namespace

std::string model_public_name(std::string_view model) {
    const std::string name = model_display_name(model);

    // A model the reference knows by name is written with the name of the
    // product in front of it.
    if (is_known_name(name)) return "Claude " + name;
    return "Claude (" + name + ")";
}

std::string model_display_name(std::string_view model) {
    const KnownModel* known = kKnownModels;
    const size_t count = sizeof(kKnownModels) / sizeof(kKnownModels[0]);

    // The tag of the context window is a part of the name the reference
    // shows: `claude-sonnet-4-6[1m]` is `Sonnet 4.6 (1M context)`.
    std::string_view base = model;
    bool wide = false;
    if (ends_with(base, "[1m]")) {
        base = base.substr(0, base.size() - 4);
        wide = true;
    }

    for (size_t i = 0; i < count; ++i) {
        const KnownModel& entry = known[i];
        if (is_model(base, entry.id)) {
            return std::string(entry.name) + (wide ? " (1M context)" : "");
        }
        // The ids carry a date suffix, e.g. `claude-sonnet-4-5-20250929`.
        if (starts_with(base, entry.id) && base.size() > entry.id.size() &&
            base[entry.id.size()] == '-') {
            return std::string(entry.name) + (wide ? " (1M context)" : "");
        }
    }

    // The aliases the user may type.
    if (base == "opus") return "Opus";
    if (base == "sonnet") return "Sonnet";
    if (base == "haiku") return "Haiku";
    if (base == "opusplan") return "Opus Plan";

    return std::string(model);
}

std::string billing_label() {
    // The reference names the subscription the account has, and writes
    // "API Usage Billing" when the session does not have one.
    const auto tokens = auth::current_tokens();
    if (!tokens) return "API Usage Billing";

    const std::string& type = tokens->subscription_type;
    if (type == "enterprise") return "Claude Enterprise";
    if (type == "team") return "Claude Team";
    if (type == "max") return "Claude Max";
    if (type == "pro") return "Claude Pro";
    return "Claude API";
}

} // namespace cc
