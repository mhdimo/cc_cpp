#pragma once

#include <string>
#include <string_view>

namespace cc {

/// A random UUID (version 4, RFC 4122) rendered in the canonical
/// 8-4-4-4-12 lowercase hex form. Used for session ids, tool-use ids and
/// message ids.
std::string uuid_v4();

/// A monotonic, lexicographically sortable identifier (ULID-shaped): a
/// 48-bit millisecond timestamp followed by 80 bits of randomness, encoded in
/// Crockford base32. Session transcripts sort by these.
std::string ulid();

/// Claude Code uses the `toolu_` prefix for assistant tool-use ids and
/// `toolr_` for tool-result ids. These helpers mint ids in that shape.
std::string tool_use_id();
std::string tool_result_id();

/// Session ids in Claude Code are UUIDs; transcripts are stored under
/// `~/.claude/projects/<encoded-cwd>/<session-id>.jsonl`.
std::string session_id();

/// True when `id` looks like a UUID produced by `uuid_v4`.
bool is_uuid(std::string_view id);

/// Encodes an arbitrary path into the directory-name form Claude Code uses
/// for project folders: every non-alphanumeric character becomes a dash.
std::string encode_project_dir(std::string_view path);

} // namespace cc
