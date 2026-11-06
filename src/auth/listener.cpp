#include "auth/listener.hpp"

#include <array>

namespace cc::auth {
namespace {

/// The page the browser is answered with.
const char* const kDonePage =
    "<!doctype html><html><head><title>Claude Code</title></head>"
    "<body><p>Login successful. You can close this tab and go back to Claude Code.</p>"
    "</body></html>";

/// The page the browser is answered with when something went wrong.
const char* const kFailedPage =
    "<!doctype html><html><head><title>Claude Code</title></head>"
    "<body><p>The login did not finish. You can close this tab and go back to Claude Code.</p>"
    "</body></html>";

/// The path of the callback.
constexpr std::string_view kCallbackPath = "/callback";

/// The query of a request line.
std::string_view query_of(const std::string& request) {
    const size_t space = request.find(' ');
    if (space == std::string::npos) return {};

    const size_t start = space + 1;
    size_t end = request.find(' ', start);
    if (end == std::string::npos) end = request.size();

    const std::string_view target = std::string_view(request).substr(start, end - start);
    const size_t mark = target.find('?');
    if (mark == std::string_view::npos) return {};
    return target.substr(mark + 1);
}

/// The value of a parameter of a query string.
std::string parameter(std::string_view query, const std::string& name) {
    size_t at = 0;
    while (at < query.size()) {
        size_t end = query.find('&', at);
        if (end == std::string_view::npos) end = query.size();

        const std::string_view pair = query.substr(at, end - at);
        const size_t equals = pair.find('=');
        if (equals != std::string_view::npos && pair.substr(0, equals) == name) {
            return std::string(pair.substr(equals + 1));
        }
        at = end + 1;
    }
    return {};
}

} // namespace

AuthCodeListener::AuthCodeListener(boost::asio::io_context& io_context)
    : acceptor_(io_context), socket_(io_context) {}

AuthCodeListener::~AuthCodeListener() {
    boost::system::error_code error;
    acceptor_.close(error);
    socket_.close(error);
}

int AuthCodeListener::start() {
    // The server is only reached from the machine of the user; the code of
    // the login must not be offered to the network.
    const boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::address_v4::loopback(), 0);

    boost::system::error_code error;
    acceptor_.open(endpoint.protocol(), error);
    if (error) return 0;

    acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), error);
    acceptor_.bind(endpoint, error);
    if (error) return 0;

    acceptor_.listen(boost::asio::socket_base::max_listen_connections, error);
    if (error) return 0;

    accept();
    return acceptor_.local_endpoint(error).port();
}

void AuthCodeListener::accept() {
    acceptor_.async_accept(socket_, [this](const boost::system::error_code& error) {
        if (error) return;
        read();
    });
}

void AuthCodeListener::read() {
    socket_.async_read_some(boost::asio::buffer(*read_buffer_),
                            [this](const boost::system::error_code& error, const size_t transferred) {
        if (error) return;

        buffer_.append(read_buffer_->data(), transferred);
        const size_t end = buffer_.find("\r\n\r\n");
        if (end == std::string::npos) {
            read();
            return;
        }

        // The request line holds the method, the path and the version; the
        // path is the second word.
        const std::string line = buffer_.substr(0, buffer_.find("\r\n"));
        const size_t first_space = line.find(' ');
        const size_t second_space =
            first_space == std::string::npos ? std::string::npos : line.find(' ', first_space + 1);
        if (first_space == std::string::npos || second_space == std::string::npos) {
            respond(false);
            return;
        }
        const std::string path = line.substr(first_space + 1, second_space - first_space - 1);

        bool success = false;
        if (path.rfind(std::string(kCallbackPath), 0) == 0) {
            const std::string_view query = query_of(line);
            const std::string code = parameter(query, "code");
            const std::string state = parameter(query, "state");
            if (!code.empty()) {
                code_ = code;
                state_ = state;
                success = true;
            }
        }

        respond(success);
    });
}

void AuthCodeListener::respond(bool success) {
    answered_ = true;

    const std::string body = success ? kDonePage : kFailedPage;
    response_ = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                "Content-Length: " +
                std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;

    boost::asio::async_write(socket_, boost::asio::buffer(response_),
                              [this](const boost::system::error_code&, const size_t) {
                                  boost::system::error_code error;
                                  socket_.close(error);
                              });
}

} // namespace cc::auth
