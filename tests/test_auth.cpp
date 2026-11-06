#include <catch2/catch_test_macros.hpp>

#include "auth/listener.hpp"
#include "auth/oauth.hpp"
#include "utils/hash.hpp"

#include <boost/asio.hpp>

#include <chrono>
#include <string>

using namespace cc;

TEST_CASE("The hash of the login is the one the reference uses") {
    REQUIRE(cc::sha256_hex("") ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(cc::sha256_hex("abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // The bytes of the hash are the ones the hexadecimal spells.
    const auto bytes = cc::sha256("abc");
    REQUIRE(bytes.size() == 32);
    REQUIRE(bytes[0] == 0xba);
    REQUIRE(bytes[31] == 0xad);
}

TEST_CASE("The bytes are written the way the reference writes them") {
    const std::vector<uint8_t> bytes = {0xfb, 0xff, 0xbe};
    REQUIRE(cc::base64(bytes) == "+/++");
    REQUIRE(cc::base64url(bytes) == "-_--");
}

TEST_CASE("The values of the login are the ones the reference uses") {
    // The challenge of a verifier is the base64url of its hash.
    const std::string verifier = auth::random_token();
    REQUIRE(verifier.size() == 43);
    REQUIRE(auth::code_challenge(verifier) == cc::base64url(cc::sha256(verifier)));

    // The state is a value of its own.
    REQUIRE(auth::random_token() != auth::random_token());

    // The scopes of the flow are the ones of the reference, the Console
    // scopes first.
    const auto& scopes = auth::all_scopes();
    REQUIRE(scopes.front() == "org:create_api_key");
    REQUIRE(scopes[1] == "user:profile");
    REQUIRE(scopes[2] == "user:inference");
}

TEST_CASE("The URL of the login holds what the provider needs") {
    const auth::OauthConfig config;
    const std::string url = auth::authorization_url(config, "the-challenge", "the-state", 1234,
                                                    /*manual=*/false, /*claude_ai_account=*/true);

    REQUIRE(url.find("https://claude.com/cai/oauth/authorize") == 0);
    REQUIRE(url.find("client_id=9d1c250a-e61b-44d9-88ed-5944d1962f5e") != std::string::npos);
    REQUIRE(url.find("response_type=code") != std::string::npos);
    REQUIRE(url.find("redirect_uri=http%3A%2F%2Flocalhost%3A1234%2Fcallback") != std::string::npos);
    REQUIRE(url.find("code_challenge=the-challenge") != std::string::npos);
    REQUIRE(url.find("code_challenge_method=S256") != std::string::npos);
    REQUIRE(url.find("state=the-state") != std::string::npos);
    REQUIRE(url.find("scope=org%3Acreate_api_key") != std::string::npos);

    // The Console account has its own page.
    const std::string console =
        auth::authorization_url(config, "the-challenge", "the-state", 1234, false, false);
    REQUIRE(console.find("https://platform.claude.com/oauth/authorize") == 0);
}

TEST_CASE("The login server takes the code the browser brings") {
    boost::asio::io_context io_context;
    auth::AuthCodeListener listener(io_context);

    const int port = listener.start();
    REQUIRE(port > 0);

    // The browser is sent back to the local server.
    boost::asio::ip::tcp::socket socket(io_context);
    socket.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));

    const std::string request =
        "GET /callback?code=the-code&state=the-state HTTP/1.1\r\nHost: localhost\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(request));

    // The server is driven until the code has arrived.
    for (int i = 0; i < 200 && !listener.code(); ++i) {
        io_context.run_one_for(std::chrono::milliseconds(10));
    }

    REQUIRE(listener.code().value_or("") == "the-code");
    REQUIRE(listener.state().value_or("") == "the-state");
}
