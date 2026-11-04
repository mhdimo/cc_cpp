#pragma once

#include "plugins.hpp"
#include "ui/ansi.hpp"
#include "ui/prompt_input.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <boost/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace cc::ui {

/// The dialog that asks the user for the options a plugin declares.
///
/// A plugin declares the values it needs in the `userConfig` of its
/// manifest; the user is asked for them when the plugin is turned on, the
/// way the reference asks for them. The values are kept in the settings of
/// the user, under `pluginConfigs[<plugin>].options`, and the plugin sees
/// them as `${user_config.KEY}`.
///
/// The dialog is drawn the way the reference draws it: the rule across the
/// top, the title, the subtitle, one row for every option — the name of
/// the option, the mark of the ones that must be given and the value —
/// the row that hands the values over, the line that names the error or
/// the explanation of the option that is asked, and the line that names
/// the keys.
///
/// A value of the kind `boolean` is a choice between "true" and "false",
/// the way the reference asks for it; every other value is written in a
/// field. A value that is secret is masked, and its field is never filled
/// in with the value that is already saved: the field says "(unchanged)"
/// in its place.
///
/// The session asks for the values of a plugin like this:
///
/// ```cpp
/// ui::PluginConfigure dialog(theme, "Configure " + plugin.name, "Plugin options");
/// const auto values = dialog.run(plugins::plugin_options(plugin),
///                                plugins::load_plugin_options(plugin.id),
///                                ui::Terminal::instance());
/// if (values) plugins::save_plugin_options(plugin.id, *values, &error);
/// ```
class PluginConfigure {
public:
    /// The screen the dialog is drawn on and the keys it reads.
    ///
    /// The session gives it the terminal of the session; the tests give it
    /// a screen of their own, so that the dialog can be driven without a
    /// terminal.
    class Screen {
    public:
        virtual ~Screen() = default;

        /// Draws the lines of the dialog.
        virtual void draw(const std::vector<std::string>& lines) = 0;

        /// Takes the dialog off the screen.
        virtual void clear() = 0;

        /// The width of the screen, in columns.
        virtual int width() const = 0;

        /// The next key the user pressed, waiting at most `timeout_ms`.
        ///
        /// Nothing comes back when there is no key yet; `closed` says then
        /// whether one will ever come.
        virtual std::optional<Key> read_key(int timeout_ms) = 0;

        /// True when the input has closed and no more keys will come.
        virtual bool closed() const = 0;
    };

    /// The screen of the terminal of the session.
    class TerminalScreen : public Screen {
    public:
        explicit TerminalScreen(Terminal& terminal) : terminal_(terminal) {}

        void draw(const std::vector<std::string>& lines) override {
            terminal_.render_dynamic(lines);
        }

        void clear() override { terminal_.clear_dynamic(); }

        int width() const override { return terminal_.width(); }

        std::optional<Key> read_key(int timeout_ms) override {
            return terminal_.read_key(timeout_ms);
        }

        bool closed() const override { return terminal_.closed(); }

    private:
        Terminal& terminal_;
    };

    /// The dialog, with the title and the subtitle it is drawn with.
    ///
    /// The reference titles the dialog "Configure <plugin>" and gives it
    /// the subtitle "Plugin options".
    PluginConfigure(Theme theme, std::string title, std::string subtitle);

    /// Sets the options of the plugin and the values that are already saved.
    ///
    /// The dialog fills its fields in with the values that are saved; the
    /// field of a value that is secret is left empty, the way the reference
    /// leaves it.
    void set_options(std::vector<plugins::PluginOption> options, boost::json::object values);

    /// The lines of the dialog.
    std::vector<std::string> render(int width) const;

    /// Runs the dialog until the user has given the values.
    ///
    /// Returns the values the user gave, keyed by the name of the option.
    /// Nothing comes back when the user gave the dialog up.
    std::optional<boost::json::object> run(std::vector<plugins::PluginOption> options,
                                          boost::json::object values, Terminal& terminal);

    /// Runs the dialog until the user has given the values.
    ///
    /// The screen is the one the dialog is drawn on and the one it reads
    /// its keys from.
    std::optional<boost::json::object> run(std::vector<plugins::PluginOption> options,
                                          boost::json::object values, Screen& screen);

    /// The values the user gave, once the dialog is over.
    const boost::json::object& values() const { return values_; }

    /// The values of the options that are secret.
    ///
    /// The reference keeps these in the keychain of the machine and not in
    /// the settings of the user; the port has no such place yet, so the
    /// caller is the one who decides where they go.
    static boost::json::object secrets_of(const std::vector<plugins::PluginOption>& options,
                                          const boost::json::object& values);

private:
    /// One option of the plugin, as the dialog shows it.
    struct Field {
        /// The name of the option; the plugin sees it as `${user_config.KEY}`.
        std::string key;

        /// The label of the field: the title of the option, or its name.
        std::string label;

        /// The explanation that stands under the label.
        std::string description;

        /// The kind of the value: `string`, `number`, `boolean`, `directory`
        /// or `file`.
        std::string type;

        /// True when the value must be given.
        bool required = false;

        /// True when the value is a secret.
        bool sensitive = false;

        /// True when the value is a choice between "true" and "false".
        bool choice = false;
    };

    /// The row of the button that hands the values over.
    ///
    /// The button stands after the options.
    size_t submit_row() const { return fields_.size(); }

    /// The option that is asked, when one is.
    const Field* current_field() const;

    /// The line an option is drawn on.
    std::string render_field(size_t index) const;

    /// The line the button that hands the values over is drawn on.
    std::string render_submit() const;

    /// The line under the button: the error of the option that is asked,
    /// or its explanation.
    ///
    /// The reference names the error of a number that is not a number
    /// there; the error of an option that must be given stands beside the
    /// button.
    std::string render_note() const;

    /// The line that names the keys.
    std::string render_keys() const;

    /// The text the field of an option holds.
    const std::string& field_text(size_t index) const;

    /// The error of an option, when it has one.
    ///
    /// The error of a number that is not a number says that it takes a
    /// number; it stands under the option that is asked.
    std::string error_of(size_t index) const;

    /// The first error of the dialog, when there is one.
    ///
    /// The error of an option that must be given and is empty says that it
    /// is required; it stands beside the button that hands the values over.
    std::string first_error() const;

    /// The values the user gave, in the shape the settings keep them.
    boost::json::object build_values() const;

    /// Moves the pointer to another row.
    void move_to(size_t row);

    /// Handles one key press.
    ///
    /// Returns true when the dialog is over.
    bool handle_key(const Key& key);

    /// Hands the values over, when every one of them is there.
    bool submit();

    /// The theme the dialog is drawn with.
    Theme theme_;

    /// The title of the dialog.
    std::string title_;

    /// The subtitle of the dialog.
    std::string subtitle_;

    /// The options of the plugin, as the dialog shows them.
    std::vector<Field> fields_;

    /// The field every value is written in.
    std::vector<PromptInput> inputs_;

    /// The values that were saved when the dialog opened.
    boost::json::object saved_;

    /// The values the user gave.
    boost::json::object values_;

    /// The row that is asked.
    size_t current_ = 0;

    /// The width of the widest label of the options.
    size_t label_width_ = 0;

    /// True when the user gave the dialog up.
    bool cancelled_ = false;
};

} // namespace cc::ui
