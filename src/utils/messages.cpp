#include "utils/messages.hpp"

namespace cc::messages {

std::string auto_reject_message(const std::string& tool_name) {
    return "Permission to use " + tool_name + " has been denied. " +
           std::string(kDenialWorkaroundGuidance);
}

std::string dont_ask_reject_message(const std::string& tool_name) {
    return "Permission to use " + tool_name +
           " has been denied because Claude Code is running in don't ask mode. " +
           std::string(kDenialWorkaroundGuidance);
}

std::string build_yolo_rejection_message(const std::string& reason) {
    return std::string(kAutoModeRejectionPrefix) + reason + ". " +
           "If you have other tasks that don't depend on this action, continue working on those. " +
           std::string(kDenialWorkaroundGuidance) +
           " To allow this type of action in the future, the user can add a Bash permission rule to "
           "their settings.";
}

std::string build_classifier_unavailable_message(const std::string& tool_name,
                                                  const std::string& classifier_model) {
    return classifier_model +
           " is temporarily unavailable, so auto mode cannot determine the safety of " + tool_name +
           " right now. Wait briefly and then try this action again. If it keeps failing, continue "
           "with other tasks that don't require this action and come back to it later. Note: reading "
           "files, searching code, and other read-only operations do not require the classifier and "
           "can still be used.";
}

bool is_classifier_denial(const std::string& content) {
    return content.rfind(kAutoModeRejectionPrefix, 0) == 0;
}

} // namespace cc::messages
