#include "ide_diff.hpp"

#include "utils/fs.hpp"
#include "utils/hash.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"
#include "utils/time.hpp"

#include <atomic>

namespace cc {
namespace {

/// The word the extension of the IDE answers with when the file was saved.
constexpr std::string_view kSaved = "FILE_SAVED";

/// The word the extension of the IDE answers with when the tab was closed.
constexpr std::string_view kClosed = "TAB_CLOSED";

/// The word the extension of the IDE answers with when the change was
/// rejected.
constexpr std::string_view kRejected = "DIFF_REJECTED";

/// The number of characters of the number that names the tab.
///
/// The reference takes the first six characters of a number it makes up.
constexpr size_t kTabNumber = 6;

} // namespace

std::string ide_tab_name(const std::string& path) {
    // The number is made up, the way the reference makes it up: the tab of
    // every change is one of its own. The counter keeps two changes that are
    // made in the same moment apart.
    static std::atomic<int64_t> counter{0};
    const std::string seed = path + ":" + std::to_string(cc::now_ms()) + ":" +
                             std::to_string(counter.fetch_add(1));
    const std::string hex = sha256_hex(seed).substr(0, kTabNumber);

    return "✻ [Claude Code] " + cc::basename(path) + " (" + hex + ") ⧉";
}

std::optional<std::string> contents_after_edit(const std::string& path,
                                               const boost::json::object& input) {
    // The notebooks are not shown in the IDE, the way the reference does not
    // show them.
    if (cc::ends_with(path, ".ipynb")) return std::nullopt;

    // A file that is written whole holds what the tool writes.
    const std::string content = cc::jget_string(input, "content");
    if (!content.empty()) return content;

    // A file that is edited holds the old text with the new text in its
    // place.
    const std::string old_string = cc::jget_string(input, "old_string");
    const std::string new_string = cc::jget_string(input, "new_string");
    if (old_string.empty()) return std::nullopt;

    const auto contents = cc::read_file(path);
    if (!contents) return std::nullopt;

    const size_t at = contents->find(old_string);
    if (at == std::string::npos) return std::nullopt;

    std::string out = *contents;
    if (cc::jget_bool(input, "replace_all", false)) {
        out = cc::replace_all(out, old_string, new_string);
    } else {
        out.replace(at, old_string.size(), new_string);
    }

    return out;
}

IdeDiffAnswer ide_answer(const std::string& text) {
    if (text.empty()) return IdeDiffAnswer::Waiting;

    // The extension answers with the word of the answer, and the contents of
    // the file follow it when the file was saved.
    if (cc::starts_with(text, kSaved)) return IdeDiffAnswer::Accepted;
    if (cc::starts_with(text, kClosed)) return IdeDiffAnswer::Accepted;
    if (cc::starts_with(text, kRejected)) return IdeDiffAnswer::Refused;

    return IdeDiffAnswer::Waiting;
}

} // namespace cc
