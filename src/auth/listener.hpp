#pragma once

#include <boost/asio.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>

namespace cc::auth {

/// The server that waits for the authorization code of the OAuth flow.
///
/// The browser is sent back to `http://localhost:<port>/callback` with the
/// code and the state; the server takes them and answers with a short page
/// that says the login is done.
class AuthCodeListener {
public:
    explicit AuthCodeListener(boost::asio::io_context& io_context);
    ~AuthCodeListener();

    AuthCodeListener(const AuthCodeListener&) = delete;
    AuthCodeListener& operator=(const AuthCodeListener&) = delete;

    /// Starts listening on a free port and returns it.
    ///
    /// Zero is returned when no port could be taken.
    int start();

    /// The code the browser brought, once it has arrived.
    std::optional<std::string> code() const { return code_; }

    /// The state the browser brought.
    std::optional<std::string> state() const { return state_; }

    /// True when the browser has been answered.
    bool answered() const { return answered_; }

private:
    /// Waits for the next connection.
    void accept();

    /// Reads the request of the browser.
    void read();

    /// Answers the browser and closes the connection.
    void respond(bool success);

    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::ip::tcp::socket socket_;

    /// The buffer the request is read into; it has to live as long as the
    /// read that is running.
    std::shared_ptr<std::array<char, 4096>> read_buffer_ =
        std::make_shared<std::array<char, 4096>>();

    /// The answer that is written to the browser.
    std::string response_;

    std::string buffer_;
    std::optional<std::string> code_;
    std::optional<std::string> state_;
    bool answered_ = false;
};

} // namespace cc::auth
