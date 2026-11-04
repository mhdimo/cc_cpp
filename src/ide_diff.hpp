#pragma once

#include <optional>
#include <string>

#include <boost/json.hpp>

namespace cc {

/// The answer of the IDE about a change that was shown in it.
enum class IdeDiffAnswer {
    Waiting,  ///< The IDE has not answered yet.
    Accepted, ///< The change was saved in the IDE, or its tab was closed.
    Refused,  ///< The change was rejected in the IDE.
};

/// The name of the tab the change is shown in.
///
/// The reference names the tab after the file and after a short number, so
/// that every change has a tab of its own.
std::string ide_tab_name(const std::string& path);

/// What the file will hold once the change is made.
///
/// The contents of the file are read, and the change of the tool is applied
/// to them. The value is empty when the change cannot be worked out: the
/// file cannot be read, or the tool does not change a file.
std::optional<std::string> contents_after_edit(const std::string& path,
                                               const boost::json::object& input);

/// The answer of the IDE, as it is read out of the reply of the extension.
///
/// The extension answers with the text `FILE_SAVED`, `TAB_CLOSED` or
/// `DIFF_REJECTED`; the answer of the session follows.
IdeDiffAnswer ide_answer(const std::string& text);

} // namespace cc
