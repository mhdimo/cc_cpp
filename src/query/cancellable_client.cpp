#include "query/cancellable_client.hpp"

#include <ai/error/api_call_error.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <openssl/ssl.h>

#include <condition_variable>
#include <coroutine>
#include <deque>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace cc::query {
namespace {

namespace beast = boost::beast;
namespace http_ns = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

/// The pieces of an answer, on their way from the thread that reads them to
/// the coroutine that hands them over.
class Channel {
public:
    /// Adds a piece of the answer.
    void push(std::vector<uint8_t> chunk) {
        {
            std::lock_guard lock(mutex_);
            chunks_.push_back(std::move(chunk));
        }
        ready_.notify_one();
    }

    /// Ends the answer.
    void finish() {
        {
            std::lock_guard lock(mutex_);
            finished_ = true;
        }
        ready_.notify_one();
    }

    /// Ends the answer with an error.
    void set_error(std::exception_ptr error) {
        {
            std::lock_guard lock(mutex_);
            error_ = std::move(error);
            finished_ = true;
        }
        ready_.notify_one();
    }

    /// Waits for the next piece of the answer.
    ///
    /// Returns nothing when the answer is over; the error that ended it is
    /// raised, the way the reference raises the error of the API.
    std::optional<std::vector<uint8_t>> pop() {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return !chunks_.empty() || finished_; });

        if (error_) std::rethrow_exception(error_);

        if (chunks_.empty()) return std::nullopt;

        auto chunk = std::move(chunks_.front());
        chunks_.pop_front();
        return chunk;
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::vector<uint8_t>> chunks_;
    std::exception_ptr error_;
    bool finished_ = false;
};

/// The socket of a request, so that the cancellation can close it.
///
/// The socket is put in place by the thread that makes the request, and
/// closed by the thread that cancels it.
class Socket {
public:
    /// Puts the connection of the request in place.
    void set(std::shared_ptr<ai::http::ConnectionPool::Connection> connection) {
        std::lock_guard lock(mutex_);
        connection_ = std::move(connection);
    }

    /// Closes the socket of the request.
    ///
    /// The read that waits for the next piece of the answer ends at once,
    /// with an error that says the request was cut short.
    void close() {
        std::lock_guard lock(mutex_);
        if (!connection_) return;

        boost::system::error_code error;
        if (connection_->is_tls) {
            // The lowest layer of the encrypted stream is the socket.
            connection_->ssl_stream->lowest_layer().close(error);
        } else {
            connection_->tcp_stream->socket().close(error);
        }
    }

private:
    std::mutex mutex_;
    std::shared_ptr<ai::http::ConnectionPool::Connection> connection_;
};

/// Waits for a value that a thread of its own will produce.
///
/// The coroutine suspends; the thread waits for the value and resumes the
/// coroutine when it is there.
template <typename T>
class FutureAwaitable {
public:
    explicit FutureAwaitable(std::future<T> future)
        : future_(std::make_shared<std::future<T>>(std::move(future))) {}

    bool await_ready() const noexcept {
        return future_->wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    void await_suspend(std::coroutine_handle<> handle) const {
        auto future = future_;
        std::thread([future, handle]() {
            future->wait();
            handle.resume();
        }).detach();
    }

    T await_resume() { return future_->get(); }

private:
    std::shared_ptr<std::future<T>> future_;
};

/// The answer of the API, piece by piece.
ai::AsyncGenerator<std::vector<uint8_t>> read_answer(std::shared_ptr<Channel> channel) {
    while (true) {
        auto chunk = channel->pop();
        if (!chunk.has_value()) break;
        co_yield std::move(*chunk);
    }
}

/// Opens the connection of a request, when it is not open already.
void ensure_connected(ai::http::ConnectionPool& pool,
                       ai::http::ConnectionPool::Connection& connection) {
    if (connection.connected && connection.is_open()) return;

    tcp::resolver resolver(pool.io_context());
    auto results = resolver.resolve(connection.host, connection.port);

    if (connection.is_tls) {
        auto& stream = *connection.ssl_stream;

        if (!SSL_set_tlsext_host_name(stream.native_handle(), connection.host.c_str())) {
            throw std::runtime_error("Failed to set SNI hostname");
        }

        auto& lowest = beast::get_lowest_layer(stream);
        lowest.expires_after(std::chrono::seconds(30));
        lowest.connect(results);

        stream.handshake(ssl::stream_base::client);
        lowest.expires_after(std::chrono::seconds(300));
    } else {
        auto& stream = *connection.tcp_stream;
        stream.expires_after(std::chrono::seconds(30));
        stream.connect(results);
        stream.expires_after(std::chrono::seconds(300));
    }

    connection.connected = true;
}

} // namespace

CancellableClient::CancellableClient(boost::asio::io_context& io_context)
    : inner_(io_context), pool_(std::make_shared<ai::http::ConnectionPool>(io_context)) {}

CancellableClient::~CancellableClient() = default;

boost::asio::io_context& CancellableClient::get_io_context() const {
    return inner_.get_io_context();
}

ai::Task<ai::http::HttpResponse> CancellableClient::get(std::string_view url,
                                                        ai::http::Headers headers,
                                                        ai::CancellationToken cancel) {
    return inner_.get(url, std::move(headers), cancel);
}

ai::Task<ai::http::HttpResponse> CancellableClient::post_json(
    std::string_view url, const boost::json::value& body, ai::http::Headers headers,
    ai::CancellationToken cancel) {
    return inner_.post_json(url, body, std::move(headers), cancel);
}

ai::Task<ai::http::HttpResponse> CancellableClient::post_multipart(
    std::string_view url, ai::http::MultipartFormData form, ai::http::Headers headers,
    ai::CancellationToken cancel) {
    return inner_.post_multipart(url, std::move(form), std::move(headers), cancel);
}

ai::Task<ai::http::StreamingResponse> CancellableClient::post_streaming(
    std::string_view url, const boost::json::value& body, ai::http::Headers headers,
    ai::CancellationToken cancel) {
    const ai::http::Url parsed = ai::http::Url::parse(url);
    const std::string body_text = boost::json::serialize(body);

    auto channel = std::make_shared<Channel>();
    auto socket = std::make_shared<Socket>();
    auto headers_ready =
        std::make_shared<std::promise<std::pair<int, ai::http::Headers>>>();

    // The cancellation closes the socket of the request: the read that
    // waits for the next piece of the answer ends at once.
    cancel.on_cancel([socket] { socket->close(); });

    // The request runs in a thread of its own: the answer of the API comes
    // in pieces, and the session goes on while they come. The pool is held
    // by the thread as well, so that it outlives the client.
    auto pool = pool_;
    std::thread([pool, parsed, body_text, headers = std::move(headers), cancel, channel, socket,
                 headers_ready]() mutable {
        try {
            // The request was cut short before it began.
            if (cancel.is_cancelled()) throw ai::OperationCancelled{};

            auto connection = pool->acquire(parsed.host, parsed.port, parsed.is_tls());
            socket->set(connection);

            ensure_connected(*pool, *connection);

            http_ns::request<http_ns::string_body> request{http_ns::verb::post, parsed.path, 11};
            request.set(http_ns::field::host, parsed.host);
            request.set(http_ns::field::content_type, "application/json");
            request.set(http_ns::field::connection, "keep-alive");
            for (const auto& [name, value] : headers) request.set(name, value);
            request.body() = body_text;
            request.prepare_payload();

            beast::flat_buffer buffer;
            http_ns::response_parser<http_ns::buffer_body> parser;
            parser.body_limit(boost::none);

            // The request is the same for a plain connection and for one
            // that is encrypted.
            const auto read = [&]<typename Stream>(Stream& stream) {
                http_ns::write(stream, request);
                http_ns::read_header(stream, buffer, parser);

                const int status = static_cast<int>(parser.get().result_int());
                ai::http::Headers response_headers;
                for (const auto& field : parser.get()) {
                    response_headers[std::string(field.name_string())] = std::string(field.value());
                }
                headers_ready->set_value({status, std::move(response_headers)});

                // The answer of the API is refused with an error before the
                // first piece of it: the body of the refusal is read whole.
                if (status >= 400) {
                    std::string error_body;
                    constexpr size_t kBufferSize = 8192;
                    char buffer_of_error[kBufferSize];
                    while (!parser.is_done()) {
                        parser.get().body().data = buffer_of_error;
                        parser.get().body().size = kBufferSize;

                        beast::error_code error;
                        http_ns::read_some(stream, buffer, parser, error);
                        if (error == http_ns::error::need_buffer) error = {};
                        if (error && error != http_ns::error::end_of_stream) {
                            throw std::runtime_error("Read error: " + error.message());
                        }
                        const size_t bytes_read = kBufferSize - parser.get().body().size;
                        error_body.append(buffer_of_error, bytes_read);
                    }

                    const bool retryable =
                        status == 408 || status == 429 || (status >= 500 && status < 600);
                    throw ai::error::ApiCallError(
                        "API call failed with status " + std::to_string(status), "", status,
                        error_body, {}, retryable);
                }

                // The answer comes in pieces, the way it comes from the API.
                constexpr size_t kChunkSize = 4096;
                char chunk_buffer[kChunkSize];

                while (!parser.is_done()) {
                    parser.get().body().data = chunk_buffer;
                    parser.get().body().size = kChunkSize;

                    beast::error_code error;
                    http_ns::read_some(stream, buffer, parser, error);

                    if (error == http_ns::error::need_buffer) error = {};

                    if (error && error != http_ns::error::end_of_stream) {
                        // The request was cut short: the answer stops the
                        // way the reference stops it.
                        if (cancel.is_cancelled()) throw ai::OperationCancelled{};
                        throw std::runtime_error("Stream read error: " + error.message());
                    }

                    const size_t bytes_read = kChunkSize - parser.get().body().size;
                    if (bytes_read > 0) {
                        channel->push(std::vector<uint8_t>(
                            reinterpret_cast<uint8_t*>(chunk_buffer),
                            reinterpret_cast<uint8_t*>(chunk_buffer) + bytes_read));
                    }

                    if (error == http_ns::error::end_of_stream) break;
                }
            };

            if (connection->is_tls) {
                read(*connection->ssl_stream);
            } else {
                read(*connection->tcp_stream);
            }

            channel->finish();

            // The connection is broken when the request was cut short: it
            // goes back to the pool only when it is whole.
            if (!cancel.is_cancelled()) pool->release(std::move(connection));
        } catch (...) {
            // The request that was cut short ends the answer the way the
            // reference ends it: the answer stops, and the error of the
            // socket that was closed is not shown.
            std::exception_ptr error = std::current_exception();
            if (cancel.is_cancelled()) error = std::make_exception_ptr(ai::OperationCancelled{});

            try {
                headers_ready->set_exception(error);
            } catch (...) {
                // The headers are already there: the error ends the answer.
            }
            channel->set_error(error);
        }
    }).detach();

    // The headers come first: the answer of the API is refused before the
    // first piece of it.
    auto [status_code, response_headers] =
        co_await FutureAwaitable<std::pair<int, ai::http::Headers>>{headers_ready->get_future()};

    ai::http::StreamingResponse response;
    response.status_code = status_code;
    response.headers = std::move(response_headers);
    response.body_stream = read_answer(channel);

    co_return std::move(response);
}

} // namespace cc::query
