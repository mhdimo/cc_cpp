#include "ui/mcp_auth.hpp"

#include "utils/string.hpp"
#include "utils/time.hpp"

#include <chrono>

namespace cc::ui {
namespace {

/// How long the message of the copy stays on the screen.
constexpr int64_t kCopyTimeoutMs = 2000;

} // namespace

McpAuthDialog::McpAuthDialog(Theme theme, std::string server_name, mcp::ServerConfig config)
    : theme_(theme),
      server_name_(std::move(server_name)),
      flow_(std::make_unique<mcp::AuthFlow>(std::move(config))),
      input_(theme, ""),
      spinner_(theme) {}

std::vector<std::string> McpAuthDialog::render(int width) const {
    std::vector<std::string> out;

    // The content of the dialog stands two columns in, the way the reference
    // stands it in.
    const std::string indent = "  ";

    Style title;
    title.foreground = theme_.claude;

    Style dim;
    dim.dim = true;

    Style success;
    success.foreground = theme_.success;

    // The reason the flow failed, when it failed.
    if (!failure_.empty()) {
        out.push_back(indent + styled("Authentication failed", title));
        out.push_back(indent + styled(failure_, dim));
        out.push_back("");
        out.push_back(indent + styled("Press Enter to go back", dim));
        return out;
    }

    // The name of the server, and the news that a browser is opening.
    out.push_back(indent + styled("Authenticating with " + server_name_ + "…", title));
    out.push_back(indent + spinner_.line() + "  A browser window will open for authentication");

    // The address of the browser, for the case that it did not open.
    if (const auto url = flow_->authorization_url()) {
        out.push_back("");
        out.push_back(indent +
                      styled("If your browser doesn't open automatically, copy this URL manually ", dim) +
                      (copied_ ? styled("(Copied!)", success) : styled("(c to copy)", dim)));
        out.push_back(indent + hyperlink(*url, *url));

        // The address of the callback, for the case that the browser could
        // not reach the local server of the port.
        out.push_back("");
        out.push_back(indent +
                      styled("If the redirect page shows a connection error, paste the URL from your "
                             "browser's address bar:",
                             dim));
        out.push_back(indent + styled("URL > ", dim) + styled(input_.text(), Style{}) +
                      styled("█", Style{}));
    }

    // The way out of the dialog.
    out.push_back("");
    out.push_back(indent +
                  styled("Return here after authenticating in your browser. Press Esc to go back.", dim));

    return out;
}

bool McpAuthDialog::run(Terminal& terminal, std::string* error) {
    if (error != nullptr) error->clear();

    flow_->start();

    while (true) {
        // The message of the copy goes away after a moment.
        if (copied_ && cc::now_ms() - copied_at_ > kCopyTimeoutMs) copied_ = false;

        // The flow may have ended while the dialog was drawn.
        if (flow_->finished()) {
            const auto result = flow_->result();
            if (result && result->ok) {
                terminal.clear_dynamic();
                return true;
            }

            // The flow failed: the reason is shown, and the dialog waits for
            // the user to have read it.
            failure_ = result && !result->error.empty() ? result->error
                                                         : "the server could not be authenticated";
            if (error != nullptr) *error = failure_;

            while (true) {
                terminal.render_dynamic(render(terminal.width()));
                const auto key = terminal.read_key(100);
                if (!key) {
                    if (terminal.closed()) {
                        terminal.clear_dynamic();
                        return false;
                    }
                    continue;
                }
                if (key->type == Key::Type::Enter || key->type == Key::Type::Escape ||
                    key->type == Key::Type::CtrlC) {
                    break;
                }
            }

            terminal.clear_dynamic();
            return false;
        }

        spinner_.tick();
        terminal.render_dynamic(render(terminal.width()));

        const auto key = terminal.read_key(100);
        if (!key) {
            // The terminal closed: the dialog ends rather than wait for a
            // key that will never come.
            if (terminal.closed()) {
                flow_->cancel();
                terminal.clear_dynamic();
                if (error != nullptr && error->empty()) *error = "the authentication was cancelled";
                return false;
            }
            continue;
        }

        switch (key->type) {
        case Key::Type::Escape:
        case Key::Type::CtrlC: {
            flow_->cancel();
            terminal.clear_dynamic();
            if (error != nullptr && error->empty()) *error = "the authentication was cancelled";
            return false;
        }
        case Key::Type::Enter: {
            // The address the user pasted is handed over to the flow.
            const std::string pasted = cc::trim(input_.text());
            if (!pasted.empty()) {
                flow_->submit_callback_url(pasted);
                input_.clear();
            }
            break;
        }
        case Key::Type::Backspace:
            input_.backspace();
            break;
        case Key::Type::Delete:
            input_.delete_forward();
            break;
        case Key::Type::Left:
            input_.move_left();
            break;
        case Key::Type::Right:
            input_.move_right();
            break;
        case Key::Type::Home:
            input_.move_home();
            break;
        case Key::Type::End:
            input_.move_end();
            break;
        case Key::Type::CtrlU:
            input_.kill_to_start();
            break;
        case Key::Type::CtrlK:
            input_.kill_to_end();
            break;
        case Key::Type::Paste:
            input_.insert(key->text);
            break;
        case Key::Type::Character: {
            // The key that copies the address of the browser is the one the
            // reference uses.
            if (key->text == "c") {
                if (const auto url = flow_->authorization_url()) {
                    copy_to_clipboard(*url);
                    copied_ = true;
                    copied_at_ = cc::now_ms();
                    break;
                }
            }
            input_.insert(key->text);
            break;
        }
        default:
            break;
        }
    }
}

} // namespace cc::ui
