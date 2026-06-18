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

atx::u64 fnv1a64(const void* data, atx::usize len) noexcept {
    atx::u64 h = 0xcbf29ce484222325ULL;
    const auto* p = static_cast<const unsigned char*>(data);
    for (atx::usize i = 0; i < len; ++i) {
        h = (h ^ static_cast<atx::u64>(p[i])) * 0x100000001b3ULL;
    }
    return h;
}

} // namespace atx::impl
