#include "query/errors.hpp"

#include "utils/json_util.hpp"
#include "utils/string.hpp"

#include <ai/error/api_call_error.hpp>

#include <boost/asio/ssl.hpp>
#include <boost/json.hpp>
#include <boost/system/system_error.hpp>

namespace cc::query {
namespace {

/// The message the API put in its answer.
///
/// The body of an error is JSON of the shape `{"error":{"message":…}}`; the
/// message is looked for there, and in the two other places the reference
/// looks for it.
std::string message_from_body(const std::string& body) {
    if (body.empty()) return {};

    const boost::json::value parsed = cc::json_parse(body, boost::json::value(nullptr));
    if (!parsed.is_object()) return cc::trim(body);

    const boost::json::object& object = parsed.as_object();

    // The iterator of a member is compared with the end of the object: the
    // end is a pointer of its own, so it is never null.
    const auto error = object.find("error");
    if (error != object.end() && error->value().is_object()) {
        const boost::json::object& inner = error->value().as_object();

        const auto message = inner.find("message");
        if (message != inner.end() && message->value().is_string()) {
            return std::string(message->value().as_string().c_str());
        }

        // The shape of the errors of the gateway that speaks the API of
        // Bedrock: the message is one level deeper.
        const auto deeper = inner.find("error");
        if (deeper != inner.end() && deeper->value().is_object()) {
            const boost::json::object& deepest = deeper->value().as_object();
            const auto deepest_message = deepest.find("message");
            if (deepest_message != deepest.end() && deepest_message->value().is_string()) {
                return std::string(deepest_message->value().as_string().c_str());
            }
        }
    }

    const auto message = object.find("message");
    if (message != object.end() && message->value().is_string()) {
        return std::string(message->value().as_string().c_str());
    }

    return {};
}

/// The message of a connection that failed, the way the reference writes it.
///
/// The reference does not show the text of the exception — it names the
/// failure: a timeout, a certificate that cannot be verified, or a
/// connection that was refused. An empty string means that the failure is
/// not one of those.
std::string connection_error_message(const std::exception& error) {
    // A system error carries the code of the failure.
    const auto* system = dynamic_cast<const boost::system::system_error*>(&error);
    if (system == nullptr) return {};

    const boost::system::error_code& code = system->code();

    // The certificate could not be verified.
    if (code.category() == boost::asio::error::get_ssl_category()) {
        return "Unable to connect to API: SSL certificate verification failed. Check your proxy or "
               "corporate SSL certificates";
    }

    // The request took too long.
    if (code == boost::system::errc::timed_out) {
        return "Request timed out. Check your internet connection and proxy settings";
    }

    // The connection was refused, or the host could not be found.
    if (code == boost::system::errc::connection_refused ||
        code == boost::system::errc::host_unreachable ||
        code == boost::system::errc::network_unreachable ||
        code == boost::asio::error::host_not_found) {
        return "Unable to connect to API. Check your internet connection";
    }

    // Any other failure of the system is a connection that failed.
    if (code.category() == boost::system::generic_category()) {
        return "Unable to connect to API. Check your internet connection";
    }

    return {};
}

} // namespace

std::string api_error_message(const std::exception& error) {
    const auto* api = dynamic_cast<const ai::error::ApiCallError*>(&error);
    if (api == nullptr) {
        // A connection that failed is named, and not shown as the text of
        // the exception.
        const std::string connection = connection_error_message(error);
        if (!connection.empty()) return connection;
        return error.what();
    }

    const std::string message = message_from_body(api->response_body());
    if (message.empty()) return error.what();

    // The reference writes the errors of the API with this prefix, and
    // without another one.
    return "API Error: " + std::to_string(api->status_code()) + " " + message;
}

} // namespace cc::query
