#include "utils/hash.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace cc {
namespace {

/// The round constants of SHA-256.
constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

/// The first sixteen bits of the cube roots of the first eighty primes.
constexpr uint32_t kInitialState[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

/// The right rotation of a word.
uint32_t rotate_right(uint32_t value, uint32_t count) {
    return (value >> count) | (value << (32 - count));
}

} // namespace

std::vector<uint8_t> sha256(std::string_view text) {
    // The message is padded to a multiple of sixty-four bytes.
    std::vector<uint8_t> message(text.begin(), text.end());
    const uint64_t length = static_cast<uint64_t>(message.size()) * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56) message.push_back(0);
    for (int i = 7; i >= 0; --i) {
        message.push_back(static_cast<uint8_t>((length >> (i * 8)) & 0xff));
    }

    uint32_t state[8];
    for (size_t i = 0; i < 8; ++i) state[i] = kInitialState[i];

    for (size_t block = 0; block < message.size(); block += 64) {
        uint32_t w[64];
        for (size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(message[block + i * 4]) << 24) |
                   (static_cast<uint32_t>(message[block + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(message[block + i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(message[block + i * 4 + 3]);
        }
        for (size_t i = 16; i < 64; ++i) {
            const uint32_t s0 = rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = state[0];
        uint32_t b = state[1];
        uint32_t c = state[2];
        uint32_t d = state[3];
        uint32_t e = state[4];
        uint32_t f = state[5];
        uint32_t g = state[6];
        uint32_t h = state[7];

        for (size_t i = 0; i < 64; ++i) {
            const uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
            const uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    std::vector<uint8_t> out;
    out.reserve(32);
    for (const uint32_t word : state) {
        for (int i = 3; i >= 0; --i) {
            out.push_back(static_cast<uint8_t>((word >> (i * 8)) & 0xff));
        }
    }
    return out;
}

std::string sha256_hex(std::string_view text) {
    const std::vector<uint8_t> digest = sha256(text);

    std::string out;
    out.reserve(64);
    char buffer[3];
    for (const uint8_t byte : digest) {
        std::snprintf(buffer, sizeof(buffer), "%02x", byte);
        out += buffer;
    }
    return out;
}

std::string base64(const std::vector<uint8_t>& bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);

    for (size_t i = 0; i < bytes.size(); i += 3) {
        const size_t left = bytes.size() - i;
        const uint32_t chunk = (static_cast<uint32_t>(bytes[i]) << 16) |
                               (left > 1 ? static_cast<uint32_t>(bytes[i + 1]) << 8 : 0) |
                               (left > 2 ? static_cast<uint32_t>(bytes[i + 2]) : 0);

        out.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
        out.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
        out.push_back(left > 1 ? kAlphabet[(chunk >> 6) & 0x3f] : '=');
        out.push_back(left > 2 ? kAlphabet[chunk & 0x3f] : '=');
    }
    return out;
}

std::string base64url(const std::vector<uint8_t>& bytes) {
    std::string out = base64(bytes);
    for (char& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    // The padding is left out.
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

} // namespace cc
