#include "session/compact.hpp"

#include "utils/json_util.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace cc::session {
namespace {

/// The number of characters a token is worth, the rough figure Claude Code
/// uses when it cannot ask the API how many tokens a conversation holds.
constexpr int64_t kCharactersPerToken = 4;

/// The number of characters an image counts as.
///
/// Claude Code charges a flat 2000 tokens for an image, which is 8000
/// characters at four characters per token.
constexpr int64_t kImageCharacters = 8000;

/// The preamble that keeps the model from calling a tool.
///
/// The model already has everything it needs in the conversation, and a tool
/// call would waste the one turn the summarisation is allowed.
const char* const kNoToolsPreamble =
    "CRITICAL: Respond with TEXT ONLY. Do NOT call any tools.\n"
    "\n"
    "- Do NOT use Read, Bash, Grep, Glob, Edit, Write, or ANY other tool.\n"
    "- You already have all the context you need in the conversation above.\n"
    "- Tool calls will be REJECTED and will waste your only turn - you will fail the task.\n"
    "- Your entire response must be plain text: an <analysis> block followed by a <summary> block.\n"
    "\n";

/// The reminder that closes the prompt.
const char* const kNoToolsTrailer =
    "\n\nREMINDER: Do NOT call any tools. Respond with plain text only - "
    "an <analysis> block followed by a <summary> block. "
    "Tool calls will be rejected and you will fail the task.";

/// The prompt that asks for a summary of the conversation.
///
/// The wording is the one Claude Code uses: the summary has to carry enough
/// detail - the files that were read, the code that was written, the errors
/// that were hit - for the work to continue without the earlier messages.
const char* const kCompactPrompt = R"PROMPT(Your task is to create a detailed summary of the conversation so far, paying close attention to the user's explicit requests and your previous actions.
This summary should be thorough in capturing technical details, code patterns, and architectural decisions that would be essential for continuing development work without losing context.

Before providing your final summary, wrap your analysis in <analysis> tags to organize your thoughts and ensure you've covered all necessary points. In your analysis process:

1. Chronologically analyze each message and section of the conversation. For each section thoroughly identify:
   - The user's explicit requests and intents
   - Your approach to addressing the user's requests
   - Key decisions, technical concepts and code patterns
   - Specific details like:
     - file names
     - full code snippets
     - function signatures
     - file edits
   - Errors that you ran into and how you fixed them
   - Pay special attention to specific user feedback that you received, especially if the user told you to do something differently.
2. Double-check for technical accuracy and completeness, addressing each required element thoroughly.

Your summary should include the following sections:

1. Primary Request and Intent: Capture all of the user's explicit requests and intents in detail
2. Key Technical Concepts: List all important technical concepts, technologies, and frameworks discussed.
3. Files and Code Sections: Enumerate specific files and code sections examined, modified, or created. Pay special attention to the most recent messages and include full code snippets where applicable and include a summary of why this file read or edit is important.
4. Errors and fixes: List all errors that you ran into, and how you fixed them. Pay special attention to specific user feedback that you received, especially if the user told you to do something differently.
5. Problem Solving: Document problems solved and any ongoing troubleshooting efforts.
6. All user messages: List ALL user messages that are not tool results. These are critical for understanding the users' feedback and changing intent.
7. Pending Tasks: Outline any pending tasks that you have explicitly been asked to work on.
8. Current Work: Describe in detail precisely what was being worked on immediately before this summary request, paying special attention to the most recent messages from both user and assistant. Include file names and code snippets where applicable.
9. Optional Next Step: List the next step that you will take that is related to the most recent work you were doing. IMPORTANT: ensure that this step is DIRECTLY in line with the user's most recent explicit requests, and the task you were working on immediately before this summary request. If your last task was concluded, then only list next steps if they are explicitly in line with the users request. Do not start on tangential requests or really old requests that were already completed without confirming with the user first.
                       If there is a next step, include direct quotes from the most recent conversation showing exactly what task you were working on and where you left off. This should be verbatim to ensure there's no drift in task interpretation.

Here's an example of how your output should be structured:

<example>
<analysis>
[Your thought process, ensuring all points are covered thoroughly and accurately]
</analysis>

<summary>
1. Primary Request and Intent:
   [Detailed description]

2. Key Technical Concepts:
   - [Concept 1]
   - [Concept 2]
   - [...]

3. Files and Code Sections:
   - [File Name 1]
      - [Summary of why this file is important]
      - [Summary of the changes made to this file, if any]
      - [Important Code Snippet]
   - [File Name 2]
      - [Important Code Snippet]
   - [...]

4. Errors and fixes:
    - [Detailed description of error 1]:
      - [How you fixed the error]
      - [User feedback on the error if any]
    - [...]

5. Problem Solving:
    [Description of solved problems and ongoing troubleshooting]

6. All user messages:
    - [Detailed non tool use user message]
    - [...]

7. Pending Tasks:
    - [Task 1]
    - [Task 2]
    - [...]

8. Current Work:
    [Precise description of current work]

9. Optional Next Step:
    [Optional Next step to take]

</summary>
</example>

Please provide your summary based on the conversation so far, following this structure and ensuring precision and thoroughness in your response.

There may be additional summarization instructions provided in the included context. If so, remember to follow these instructions when creating the above summary. Examples of instructions include:
<example>
## Compact Instructions
When summarizing the conversation focus on typescript code changes and also remember the mistakes you made and how you fixed them.
</example>

<example>
# Summary instructions
When you are using compact - please focus on test output and code changes. Include file reads verbatim.
</example>
)PROMPT";

/// The number of characters a content block holds.
int64_t block_characters(const ContentBlock& block) {
    if (const auto* text = as_text(block)) return static_cast<int64_t>(text->text.size());
    if (const auto* thinking = as_thinking(block)) {
        return static_cast<int64_t>(thinking->thinking.size());
    }
    if (const auto* tool_use = as_tool_use(block)) {
        return static_cast<int64_t>(tool_use->name.size() + json_dump(tool_use->input).size());
    }
    if (const auto* tool_result = as_tool_result(block)) {
        return static_cast<int64_t>(tool_result->content.size());
    }
    if (const auto* image = as_image(block)) {
        return kImageCharacters + static_cast<int64_t>(image->media_type.size());
    }
    return 0;
}

/// Removes the drafting scratchpad from a summary.
///
/// Claude Code asks the model to think in an `<analysis>` block before it
/// writes the summary. The block sharpens the summary but has no value of
/// its own once the summary is written.
std::string strip_analysis(const std::string& text) {
    const std::string open = "<analysis>";
    const std::string close = "</analysis>";

    const size_t start = text.find(open);
    if (start == std::string::npos) return text;
    const size_t end = text.find(close, start);
    if (end == std::string::npos) return text;

    return text.substr(0, start) + text.substr(end + close.size());
}

/// Turns the `<summary>` block of a summary into a readable header.
std::string format_summary(const std::string& text) {
    const std::string open = "<summary>";
    const std::string close = "</summary>";

    const size_t start = text.find(open);
    if (start == std::string::npos) return text;
    const size_t end = text.find(close, start);
    if (end == std::string::npos) return text;

    const std::string content = trim(text.substr(start + open.size(), end - start - open.size()));
    return text.substr(0, start) + "Summary:\n" + content + text.substr(end + close.size());
}

} // namespace

/// The text of the summary message, as it is written to the transcript.
std::string summary_message_text(const std::string& summary, bool keeps_recent) {
    std::string text =
        "This session is being continued from a previous conversation that ran out of context. "
        "The summary below covers the earlier portion of the conversation.\n\n";
    text += format_summary(strip_analysis(summary));

    // Collapse the blank lines the formatting leaves behind.
    while (text.find("\n\n\n") != std::string::npos) {
        text = replace_all(text, "\n\n\n", "\n\n");
    }
    text = trim(text);

    if (keeps_recent) text += "\n\nRecent messages are preserved verbatim.";
    return text;
}

int estimate_tokens(const std::vector<Message>& messages) {
    int64_t characters = 0;
    for (const auto& message : messages) {
        for (const auto& block : message.content) {
            characters += block_characters(block);
        }
    }
    return static_cast<int>(characters / kCharactersPerToken);
}

bool should_compact(const std::vector<Message>& messages, const CompactionOptions& options) {
    if (options.context_window_tokens <= 0) return false;

    const double threshold =
        static_cast<double>(options.context_window_tokens) * options.compact_threshold;
    return estimate_tokens(messages) >= threshold;
}

std::vector<Message> compact(const std::vector<Message>& messages, const CompactionOptions& options,
                             const Summarizer& summarizer) {
    if (messages.empty()) return messages;
    if (!should_compact(messages, options)) return messages;

    size_t keep = options.keep_recent_messages > 0 ? static_cast<size_t>(options.keep_recent_messages)
                                                   : 0;
    if (keep >= messages.size()) {
        // The conversation is shorter than the number of messages that are
        // kept, so there is nothing to keep: the whole conversation is
        // summarised.
        keep = 0;
    }

    const auto split = static_cast<std::ptrdiff_t>(messages.size() - keep);
    const std::vector<Message> dropped(messages.begin(), messages.begin() + split);
    const std::vector<Message> kept(messages.begin() + split, messages.end());

    const std::string summary =
        summarizer ? summarizer(dropped, options.instructions) : std::string();

    Message marker;
    marker.role = Role::User;
    marker.is_meta = true;
    marker.is_compact_summary = true;
    marker.content.emplace_back(TextBlock{summary_message_text(summary, !kept.empty()), false});

    std::vector<Message> compacted;
    compacted.reserve(kept.size() + 1);
    compacted.push_back(std::move(marker));
    compacted.insert(compacted.end(), kept.begin(), kept.end());
    return compacted;
}

std::string summary_text(const std::vector<Message>& messages) {
    std::string summary;
    for (const auto& message : messages) {
        if (!message.is_compact_summary) continue;
        summary = message.text();
    }
    return summary;
}

std::string compact_prompt(const std::string& instructions) {
    std::string prompt = kNoToolsPreamble + std::string(kCompactPrompt);

    if (!trim(instructions).empty()) {
        prompt += "\n\nAdditional Instructions:\n" + instructions;
    }

    prompt += kNoToolsTrailer;
    return prompt;
}

} // namespace cc::session
