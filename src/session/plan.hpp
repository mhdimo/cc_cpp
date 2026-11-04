#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace cc::session {

/// The folder the plans of the sessions are kept in.
///
/// The reference keeps them in `~/.claude/plans`.
std::string plans_directory();

/// The slug of the plan of a session.
///
/// The slug is made of three words — an adjective, a verb and a noun — and
/// is kept for the session, so that the plan of the session is found again.
/// A slug whose file is already there is tried again, the way the reference
/// does it.
std::string plan_slug(const std::string& session_id);

/// The file the plan of a session is written to.
std::string plan_path(const std::string& session_id);

/// The plan of a session, when there is one.
std::optional<std::string> read_plan(const std::string& session_id);

/// Writes the plan of a session.
///
/// The folder of the plans is made when it is not there yet.
bool write_plan(const std::string& session_id, std::string_view content, std::string* error);

/// Forgets the slug of a session, so that the next plan of the session gets
/// a name of its own.
void forget_plan_slug(const std::string& session_id);

} // namespace cc::session
