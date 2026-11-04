#include "core/ids.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <random>
#include <string>

namespace cc {
namespace {

std::mt19937_64& rng() {
    static thread_local std::mt19937_64 engine{std::random_device{}()};
    return engine;
}

std::array<uint8_t, 16> random_bytes() {
    std::array<uint8_t, 16> out{};
    auto& e = rng();
    for (size_t i = 0; i < out.size(); i += 8) {
        uint64_t v = e();
        for (size_t j = 0; j < 8 && i + j < out.size(); ++j) {
            out[i + j] = static_cast<uint8_t>((v >> (j * 8)) & 0xFF);
        }
    }
    return out;
}

std::string hex(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0F]);
    }
    return out;
}

constexpr char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/// The longest a project directory name may be.
constexpr size_t kMaxSanitizedLength = 200;

/// The djb2 hash the reference uses, as a signed 32-bit integer.
int32_t djb2_hash(std::string_view text) {
    int32_t hash = 0;
    for (char c : text) {
        hash = (hash << 5) - hash + static_cast<int32_t>(static_cast<unsigned char>(c));
    }
    return hash;
}

/// The djb2 hash of `text`, in base 36.
std::string djb2_base36(std::string_view text) {
    const int64_t magnitude = djb2_hash(text) < 0
                                  ? -static_cast<int64_t>(djb2_hash(text))
                                  : static_cast<int64_t>(djb2_hash(text));

    if (magnitude == 0) return "0";

    static constexpr char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string out;
    int64_t remaining = magnitude;
    while (remaining > 0) {
        out.push_back(kDigits[remaining % 36]);
        remaining /= 36;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

} // namespace

std::string uuid_v4() {
    auto bytes = random_bytes();
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40); // version 4
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80); // variant 10

    std::string h = hex(bytes.data(), bytes.size());
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" +
           h.substr(16, 4) + "-" + h.substr(20, 12);
}

std::string ulid() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    std::array<uint8_t, 16> bytes{};
    for (int i = 0; i < 6; ++i) {
        bytes[i] = static_cast<uint8_t>((static_cast<uint64_t>(ms) >> ((5 - i) * 8)) & 0xFF);
    }
    auto rand_bytes = random_bytes();
    for (size_t i = 0; i < 10; ++i) {
        bytes[6 + i] = rand_bytes[i];
    }

    // 26 * 5 = 130 bits, so the 128-bit value fits with 2 bits to spare.
    std::string encoded(26, '0');
    uint64_t hi = 0;
    uint64_t lo = 0;
    for (int i = 0; i < 8; ++i) hi = (hi << 8) | bytes[i];
    for (int i = 8; i < 16; ++i) lo = (lo << 8) | bytes[i];

    unsigned __int128 v = (static_cast<unsigned __int128>(hi) << 64) | lo;
    for (int i = 25; i >= 0; --i) {
        encoded[static_cast<size_t>(i)] = kCrockford[static_cast<size_t>(v & 0x1F)];
        v >>= 5;
    }
    return encoded;
}

std::string tool_use_id() { return "toolu_" + uuid_v4(); }

std::string tool_result_id() { return "toolr_" + uuid_v4(); }

std::string session_id() { return uuid_v4(); }

bool is_uuid(std::string_view id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

std::string encode_project_dir(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    for (char c : path) {
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9');
        out.push_back(alnum ? c : '-');
    }

    // A long path would exceed the limit on a directory name, so it is
    // truncated and a hash of the path is appended to keep it unique.
    if (out.size() <= kMaxSanitizedLength) {
        return out;
    }
    return out.substr(0, kMaxSanitizedLength) + "-" + djb2_base36(path);
}

} // namespace cc
