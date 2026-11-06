#pragma once

#include <string>

namespace cc::config {

/// True when the user has already trusted `path`, or one of the folders
/// above it.
///
/// Claude Code asks once per folder tree; accepting the question is
/// remembered so the question is not asked again.
bool trust_accepted(const std::string& path);

/// Records that the user trusts `path`.
///
/// The rest of the global configuration file is left alone.
void remember_trust(const std::string& path);

/// True when the user allowed the files the project pulls in from outside
/// its folder.
///
/// The reference keeps the answer in the bucket of the project, the way it
/// keeps the acceptance of the folder.
bool external_includes_approved(const std::string& path);

/// True when the user was already asked about the files the project pulls
/// in from outside its folder.
///
/// The reference asks the question once: the answer may be no, and the
/// question is not asked again.
bool external_includes_warning_shown(const std::string& path);

/// Remembers whether the user allowed the files the project pulls in from
/// outside its folder.
///
/// The question is remembered as asked, the way the reference remembers it.
void remember_external_includes(const std::string& path, bool approved);

} // namespace cc::config
