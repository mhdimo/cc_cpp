#pragma once

#include "core/message.hpp"

#include <functional>
#include <string>
#include <vector>

namespace cc::session {

/// How a conversation is summarised.
struct CompactionOptions {
    /// The instructions the user gave, if any.
    std::string instructions;
    /// The number of tokens the context window holds.
    int context_window_tokens = 200000;
    /// The fraction of the window at which the conversation is compacted.
    double compact_threshold = 0.8;
    /// The number of messages that are kept verbatim at the end.
    int keep_recent_messages = 6;
};

/// The summariser, which asks the model for a summary of the conversation.
///
/// It receives the messages to summarise and the user's instructions, and
/// returns the summary text.
using Summarizer = std::function<std::string(const std::vector<Message>& messages,
                                              const std::string& instructions)>;

/// The number of tokens the conversation is estimated to use.
///
/// The estimate is the character count of the conversation divided by four,
/// the same rough figure the reference uses.
int estimate_tokens(const std::vector<Message>& messages);

/// True when the conversation has grown past the compaction threshold.
bool should_compact(const std::vector<Message>& messages, const CompactionOptions& options);

/// Compacts the conversation.
///
/// The result holds the messages that follow the summary: the summary
/// message, marked with `is_compact_summary`, followed by the most recent
/// messages that are kept verbatim.
std::vector<Message> compact(const std::vector<Message>& messages, const CompactionOptions& options,
                             const Summarizer& summarizer);

/// The summary of a compacted conversation, as it is written to the
/// transcript.
std::string summary_text(const std::vector<Message>& messages);

/// The text of the summary message, as it is written to the transcript.
///
/// The reference says that the session is continued from a previous
/// conversation and puts the summary under it.
std::string summary_message_text(const std::string& summary, bool keeps_recent);

/// The prompt that asks the model for a summary of the conversation.
///
/// The prompt asks for a summary that is detailed enough to continue the
/// work without the earlier messages, and for no tool calls: the model
/// already has everything it needs in the conversation.
std::string compact_prompt(const std::string& instructions);

} // namespace cc::session
