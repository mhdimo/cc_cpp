#pragma once

#include <ai/http/client.hpp>
#include <ai/http/connection_pool.hpp>
#include <ai/util/cancellation.hpp>

#include <boost/asio.hpp>
#include <boost/json.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace cc::query {

/// The client of the API whose answer can be cut short.
///
/// The client of the SDK reads the answer in a thread of its own and only
/// notices the cancellation between two pieces of the answer: the request
/// of the API goes on for as long as the server keeps it open, and the
/// first piece of the answer of a model that thinks for a while comes
/// late. The reference aborts the request itself, so that the answer
/// stops the moment the user presses Ctrl-C.
///
/// This client keeps the socket of the request at hand, and the
/// cancellation closes it: the read that waits for the next piece of the
/// answer ends at once, and the answer is cut short.
class CancellableClient : public ai::http::IHttpClient {
public:
    explicit CancellableClient(boost::asio::io_context& io_context);
    ~CancellableClient() override;

    ai::Task<ai::http::HttpResponse> get(std::string_view url, ai::http::Headers headers,
                                         ai::CancellationToken cancel = {}) override;

    ai::Task<ai::http::HttpResponse> post_json(std::string_view url, const boost::json::value& body,
                                                ai::http::Headers headers,
                                                ai::CancellationToken cancel = {}) override;

    ai::Task<ai::http::StreamingResponse> post_streaming(std::string_view url,
                                                         const boost::json::value& body,
                                                         ai::http::Headers headers,
                                                         ai::CancellationToken cancel = {}) override;

    ai::Task<ai::http::HttpResponse> post_multipart(std::string_view url,
                                                     ai::http::MultipartFormData form,
                                                     ai::http::Headers headers,
                                                     ai::CancellationToken cancel = {}) override;

    boost::asio::io_context& get_io_context() const override;

private:
    /// The client of the SDK, which does the requests that are not streamed.
    ai::http::HttpClient inner_;

    /// The pool the connections of the streams come from. The pool is held
    /// by a pointer, so that it outlives the client when a request is still
    /// running when the session ends.
    std::shared_ptr<ai::http::ConnectionPool> pool_;
};

} // namespace cc::query
