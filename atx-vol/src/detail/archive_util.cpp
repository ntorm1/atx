#include "atx/vol/detail/archive_util.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#define ATX_ARCH_X86 1
#include <intrin.h>
#else
#define ATX_ARCH_X86 0
#endif

namespace atx::vol::detail {

namespace {

// ── CRC-32C (Castagnoli, reflected poly 0x82F63B78) ──────────────────────
//
// Two implementations with bit-identical output: a table-driven fallback and a
// hardware SSE4.2 `_mm_crc32` path (8 bytes/step), runtime-dispatched by CPUID.
// The running-state semantics (no init/final XOR inside `_update`) match, so the
// one-shot `crc32c` = update(0xFFFFFFFF) ^ 0xFFFFFFFF regardless of path.

[[nodiscard]] constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t n = 0; n < 256; ++n) {
    std::uint32_t c = n;
    for (int k = 0; k < 8; ++k) {
      c = ((c & 1u) != 0u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
    }
    table[n] = c;
  }
  return table;
}

inline constexpr std::array<std::uint32_t, 256> kCrc32cTable = make_crc32c_table();

[[nodiscard]] std::uint32_t crc32c_update_table(std::uint32_t crc, const std::byte *p,
                                                std::size_t n) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    const auto b = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[i]));
    crc = kCrc32cTable[(crc ^ b) & 0xFFu] ^ (crc >> 8);
  }
  return crc;
}

#if ATX_ARCH_X86
[[nodiscard]] bool detect_sse42() noexcept {
  int regs[4] = {0, 0, 0, 0};
  __cpuid(regs, 1);
  return (regs[2] & (1 << 20)) != 0; // ECX bit 20 = SSE4.2 (CRC32)
}
// Resolved once at load; the dispatch below is a predictable branch.
const bool kHasSse42 = detect_sse42();

// The SSE4.2 CRC32 intrinsics require the `crc32` target feature to be emitted.
// The build compiles the TU without it (baseline x86-64), so mark just this
// function with the target attribute; it is only ever *called* behind the runtime
// CPUID gate (kHasSse42), so no unsupported instruction is executed on an old CPU.
#if defined(__clang__)
__attribute__((target("sse4.2")))
#endif
std::uint32_t
crc32c_update_hw(std::uint32_t crc, const std::byte *p, std::size_t n) noexcept {
  std::uint64_t c = crc;
  while (n >= 8) {
    std::uint64_t v = 0;
    std::memcpy(&v, p, 8);
    c = _mm_crc32_u64(c, v);
    p += 8;
    n -= 8;
  }
  auto c32 = static_cast<std::uint32_t>(c);
  while (n != 0) {
    c32 = _mm_crc32_u8(c32, std::to_integer<std::uint8_t>(*p));
    ++p;
    --n;
  }
  return c32;
}
#endif

} // namespace

// Continue a CRC-32C over [p, p+n). `crc` is the running (un-finalized) state.
[[nodiscard]] std::uint32_t crc32c_update(std::uint32_t crc, const std::byte *p,
                                          std::size_t n) noexcept {
#if ATX_ARCH_X86
  if (kHasSse42) {
    return crc32c_update_hw(crc, p, n);
  }
#endif
  return crc32c_update_table(crc, p, n);
}

// One-shot CRC-32C with the standard init/final XOR applied.
[[nodiscard]] std::uint32_t crc32c(const std::byte *p, std::size_t n) noexcept {
  return crc32c_update(0xFFFFFFFFu, p, n) ^ 0xFFFFFFFFu;
}

// ASCII upper-case + truncate to max_len. Returns exactly the canonical bytes
// (no zero-pad tail): callers that need a fixed-width buffer size it to the
// returned std::string's length, matching the archive's prior fixed-array
// semantics where only the first `n` bytes were ever hashed/compared/copied.
[[nodiscard]] std::string canonicalize_symbol(std::string_view s, std::size_t max_len) {
  const std::size_t n = std::min(s.size(), max_len);
  std::string dst(n, '\0');
  for (std::size_t i = 0; i < n; ++i) {
    char c = s[i];
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
    dst[i] = c;
  }
  return dst;
}

} // namespace atx::vol::detail
