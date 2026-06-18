#include "artifacts.hpp"

#include <array>
#include <cstdio>

namespace atx::impl {

std::string to_hex16(atx::u64 value) {
    std::array<char, 17> buf{};
    // snprintf writes at most 17 bytes (16 hex chars + NUL).
    std::snprintf(buf.data(), buf.size(), "%016llx",
                  static_cast<unsigned long long>(value));
    return std::string(buf.data(), 16);
}

} // namespace atx::impl
