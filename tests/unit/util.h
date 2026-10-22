// Helpers shared by the unit tests.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "casement/text.h"
#include "x2go/kdrive.h"

namespace testing {

inline std::vector<std::uint8_t> bytes(const char* base64) {
    auto v = casement::base64_decode(base64);
    return v ? *v : std::vector<std::uint8_t>{};
}

inline std::vector<std::uint8_t> bytes(std::string_view text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

inline std::vector<std::uint8_t> as_vector(const x2go::kdrive::Event& e) {
    return std::vector<std::uint8_t>(e.begin(), e.end());
}

inline std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
           static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}

}  // namespace testing
