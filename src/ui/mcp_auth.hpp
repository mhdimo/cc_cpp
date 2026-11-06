#pragma once

#include "mcp/mcp.hpp"
#include "ui/ansi.hpp"
#include "ui/prompt_input.hpp"
#include "ui/spinner.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <memory>
#include <string>
#include <vector>

namespace cc::ui {

/// The dialog that authenticates a server of MCP.
///
/// The dialog is the one the reference shows while the browser signs the user
/// in: the name of the server, the spinner, the address of the browser for
/// the case that it did not open, the field the address of the callback is
/// pasted in, and the way out of the dialog.
///
/// The flow of the authentication runs on a thread of its own, so the dialog
/// may be drawn while the browser is open. The answer of the user is the
/// answer of the flow: the address the user pastes is handed over to it.
class McpAuthDialog {
public:
    McpAuthDialog(Theme theme, std::string server_name, mcp::ServerConfig config);

    /// Renders the dialog.
    std::vector<std::string> render(int width) const;

    /// Runs the dialog until the server is authenticated or the user gives
    /// the flow up.
    ///
    /// Returns true when the server was authenticated. The reason it failed
    /// is given back in `error`, when there is one.
    bool run(Terminal& terminal, std::string* error = nullptr);

private:
    /// The palette of the dialog.
    Theme theme_;

    /// The name of the server.
    std::string server_name_;

    /// The flow of the authentication.
    std::unique_ptr<mcp::AuthFlow> flow_;

    /// The field the address of the callback is pasted in.
    PromptInput input_;

    /// The spinner of the dialog.
    Spinner spinner_;

    /// True when the address of the browser was put in the clipboard.
    bool copied_ = false;

    /// The moment the address was put in the clipboard.
    int64_t copied_at_ = 0;

    /// The reason the flow failed, when it failed.
    std::string failure_;
};

} // namespace cc::ui
