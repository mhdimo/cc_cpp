#include "tools/registry.hpp"

#include "tools/impl/bash_tool.hpp"
#include "tools/impl/common.hpp"
#include "tools/impl/file_tools.hpp"
#include "tools/impl/misc_tools.hpp"
#include "tools/impl/search_tools.hpp"
#include "tools/tool_names.hpp"
#include "tools/tool_use_context.hpp"
#include "types/permissions.hpp"

namespace cc::tools {
namespace {

/// The prompt the reference sends with the tool that enters the plan mode.
const std::string& enter_plan_mode_prompt() {
    static const std::string prompt = R"(Use this tool proactively when you're about to start a non-trivial implementation task. Getting user sign-off on your approach before writing code prevents wasted effort and ensures alignment. This tool transitions you into plan mode where you can explore the codebase and design an implementation approach for user approval.

## When to Use This Tool

**Prefer using EnterPlanMode** for implementation tasks unless they're simple. Use it when ANY of these conditions apply:

1. **New Feature Implementation**: Adding meaningful new functionality
   - Example: "Add a logout button" - where should it go? What should happen on click?
   - Example: "Add form validation" - what rules? What error messages?

2. **Multiple Valid Approaches**: The task can be solved in several different ways
   - Example: "Add caching to the API" - could use Redis, in-memory, file-based, etc.
   - Example: "Improve performance" - many optimization strategies possible

3. **Code Modifications**: Changes that affect existing behavior or structure
   - Example: "Update the login flow" - what exactly should change?
   - Example: "Refactor this component" - what's the target architecture?

4. **Architectural Decisions**: The task requires choosing between patterns or technologies
   - Example: "Add real-time updates" - WebSockets vs SSE vs polling
   - Example: "Implement state management" - Redux vs Context vs custom solution

5. **Multi-File Changes**: The task will likely touch more than 2-3 files
   - Example: "Refactor the authentication system"
   - Example: "Add a new API endpoint with tests"

6. **Unclear Requirements**: You need to explore before understanding the full scope
   - Example: "Make the app faster" - need to profile and identify bottlenecks
   - Example: "Fix the bug in checkout" - need to investigate root cause

7. **User Preferences Matter**: The implementation could reasonably go multiple ways
   - If you would use AskUserQuestion to clarify the approach, use EnterPlanMode instead
   - Plan mode lets you explore first, then present options with context

## When NOT to Use This Tool

Only skip EnterPlanMode for simple tasks:
- Single-line or few-line fixes (typos, obvious bugs, small tweaks)
- Adding a single function with clear requirements
- Tasks where the user has given very specific, detailed instructions
- Pure research/exploration tasks (use the Agent tool with explore agent instead)

## What Happens in Plan Mode

In plan mode, you'll:
1. Thoroughly explore the codebase using Glob, Grep, and Read tools
2. Understand existing patterns and architecture
3. Design an implementation approach
4. Present your plan to the user for approval
5. Use AskUserQuestion if you need to clarify approaches
6. Exit plan mode with ExitPlanMode when ready to implement

## Examples

### GOOD - Use EnterPlanMode:
User: "Add user authentication to the app"
- Requires architectural decisions (session vs JWT, where to store tokens, middleware structure)

User: "Optimize the database queries"
- Multiple approaches possible, need to profile first, significant impact

User: "Implement dark mode"
- Architectural decision on theme system, affects many components

User: "Add a delete button to the user profile"
- Seems simple but involves: where to place it, confirmation dialog, error handling, state updates

User: "Update the error handling in the API"
- Affects multiple files, user should approve the approach

### BAD - Don't use EnterPlanMode:
User: "Fix the typo in the README"
- Straightforward, no planning needed

User: "Add a console.log to debug this function"
- Simple, obvious implementation

User: "What files handle routing?"
- Research task, not implementation planning

## Important Notes

- This tool REQUIRES user approval - they must consent to entering plan mode
- If unsure whether to use it, err on the side of planning - it's better to get alignment upfront than to redo work
- Users appreciate being consulted before significant changes are made to their codebase
)";
    return prompt;
}

/// The tool that puts the session into the plan mode.
///
/// The reference lets the model ask for the plan mode, and the mode is
/// switched as soon as the tool runs.
class EnterPlanModeTool : public Tool {
public:
    std::string_view name() const override { return ENTER_PLAN_MODE; }

    std::string description() const override {
        return "Requests permission to enter plan mode for complex tasks requiring exploration and "
               "design";
    }

    std::string prompt() const override { return enter_plan_mode_prompt(); }

    boost::json::object input_schema() const override { return schema_object({}, {}); }

    ToolResult call(const boost::json::object& input, ToolUseContext& context) override;

    bool is_read_only(const boost::json::object& input) const override { return true; }
};

ToolResult EnterPlanModeTool::call(const boost::json::object& input, ToolUseContext& context) {
    // The session goes into the plan mode, the way the reference goes into
    // it.
    context.permission_context.mode = PermissionMode::Plan;

    // The reference tells the model what it is expected to do now.
    const std::string message =
        "Entered plan mode. You should now focus on exploring the codebase and designing an "
        "implementation approach.";

    ToolResult result;
    result.content = message + "\n"
                                 "\n"
                                 "In plan mode, you should:\n"
                                 "1. Thoroughly explore the codebase to understand existing patterns\n"
                                 "2. Identify similar features and architectural approaches\n"
                                 "3. Consider multiple approaches and their trade-offs\n"
                                 "4. Use AskUserQuestion if you need to clarify the approach\n"
                                 "5. Design a concrete implementation strategy\n"
                                 "6. When ready, use ExitPlanMode to present your plan for approval\n"
                                 "\n"
                                 "Remember: DO NOT write or edit any files yet. This is a read-only "
                                 "exploration and planning phase.";

    // The mode that was entered travels with the result, so that the UI can
    // show it.
    boost::json::object payload;
    payload["message"] = message;
    result.tool_use_result = boost::json::value(std::move(payload));

    return result;
}

} // namespace

Registry::Registry() : tracker_(std::make_shared<FileStateTracker>()) {
    tools_.push_back(std::make_shared<ReadTool>(tracker_));
    tools_.push_back(std::make_shared<WriteTool>(tracker_));
    tools_.push_back(std::make_shared<EditTool>(tracker_));
    tools_.push_back(std::make_shared<GlobTool>());
    tools_.push_back(std::make_shared<GrepTool>());
    bash_ = std::make_shared<BashTool>();
    tools_.push_back(bash_);
    tools_.push_back(std::make_shared<TodoWriteTool>());
    tools_.push_back(std::make_shared<AskUserQuestionTool>());
    tools_.push_back(std::make_shared<EnterPlanModeTool>());
    tools_.push_back(std::make_shared<ExitPlanModeTool>());

    read_ = std::dynamic_pointer_cast<ReadTool>(tools_.front());
}

std::vector<std::string> Registry::names() const {
    std::vector<std::string> result;
    result.reserve(tools_.size());
    for (const auto& tool : tools_) {
        if (tool) result.emplace_back(tool->name());
    }
    return result;
}

void Registry::set_sandbox(sandbox::Manager* manager) {
    if (bash_) bash_->set_sandbox(manager);
}

void Registry::set_shell_session(ShellSession* session) {
    if (bash_) bash_->set_shell_session(session);
}

void Registry::set_pdf_supported(bool supported) {
    if (read_) read_->set_pdf_supported(supported);
}

void Registry::set_include_git_instructions(bool included) {
    if (bash_) bash_->set_include_git_instructions(included);
}

} // namespace cc::tools
