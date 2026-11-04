#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cc {

/// The SHA-256 of the text, as bytes.
///
/// The reference names the backups of the files it keeps with the hash of
/// their path, so the hash has to be the same one for the two to agree.
std::vector<uint8_t> sha256(std::string_view text);

/// The SHA-256 of the text, in hexadecimal.
std::string sha256_hex(std::string_view text);

/// The bytes in the base64url of the standard.
///
/// The padding is left out and the two characters that are not safe in a
/// URL are replaced, the way the reference encodes the values of its OAuth
/// flow.
std::string base64url(const std::vector<uint8_t>& bytes);

/// The bytes in the base64 of the standard.
std::string base64(const std::vector<uint8_t>& bytes);

} // namespace cc
