#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cc::ui {

/// A QR code, the way the reference draws it.
///
/// The code holds the text in the byte mode, with the level of the
/// correction of the errors the reference uses.
class QrCode {
public:
    /// The level of the correction of the errors.
    enum class Level {
        Low,  ///< The level the reference uses.
        Medium,
    };

    /// The code of the text, when the text fits in one.
    ///
    /// The text is held as it is, the way the reference holds it.
    static std::optional<QrCode> encode(std::string_view text, Level level = Level::Low);

    /// The size of the code, in modules.
    int size() const { return static_cast<int>(modules_.size()); }

    /// True when the module at the place is dark.
    bool dark(int x, int y) const {
        if (x < 0 || y < 0 || x >= size() || y >= size()) return false;
        return modules_[static_cast<size_t>(y)][static_cast<size_t>(x)];
    }

    /// The code as the lines of the text the terminal shows.
    ///
    /// Every module is two characters wide, so the code keeps the shape of
    /// the square it is.
    std::vector<std::string> lines() const;

private:
    /// The modules of the code, one row after another.
    std::vector<std::vector<bool>> modules_;
};

} // namespace cc::ui
