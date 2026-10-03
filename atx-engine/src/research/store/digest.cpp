// DigestStream over atx::core::Sha256 (digest.hpp holds the encoding contract).

#include "atx/engine/research/store/digest.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "atx/core/sha256.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::store {
namespace {

constexpr std::string_view kHexDigits = "0123456789abcdef";

// 16 lower-case hex digits, zero padded (most significant first).
[[nodiscard]] std::array<char, 16> hex16(u64 value) noexcept {
  std::array<char, 16> out{};
  for (usize i = 0; i < out.size(); ++i) {
    out[out.size() - 1 - i] = kHexDigits[(value >> (4U * i)) & 0xFU];
  }
  return out;
}

// Decimal text of an integer in a stack buffer (std::to_chars: no locale, no allocation).
template <class Int> struct Decimal {
  std::array<char, 24> buffer{};
  usize size{};

  explicit Decimal(Int value) noexcept {
    const std::to_chars_result result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    // SAFETY: 24 bytes hold any 64-bit integer in decimal (at most 20 digits and a sign).
    assert(result.ec == std::errc{});
    size = static_cast<usize>(result.ptr - buffer.data());
  }
  [[nodiscard]] std::string_view view() const noexcept { return {buffer.data(), size}; }
};

} // namespace

DigestStream::DigestStream(Kind kind) {
  write(kind == Kind::Record ? kRecordDigestDomain : kCatalogDigestDomain);
  write("\n");
}

void DigestStream::write(std::string_view bytes) {
  assert(!finished_ && "DigestStream used after finish()");
  if (bytes.empty()) {
    return;
  }
  // Sha256::update fails only after finalize(), which finished_ excludes; a failure is
  // recorded and turns finish() into "" (a digest that matches nothing).
  if (!sha_.update(std::as_bytes(std::span<const char>{bytes.data(), bytes.size()}))) {
    failed_ = true;
  }
}

void DigestStream::write_column(std::string_view column, char tag) {
  write(column);
  const std::array<char, 2> prefix{'=', tag};
  write(std::string_view{prefix.data(), prefix.size()});
}

void DigestStream::begin_row(std::string_view table, i32 version) {
  write("row ");
  write(table);
  write("@");
  write(Decimal<i32>{version}.view());
  write("\n");
}

void DigestStream::add_null(std::string_view column) {
  write(column);
  write("=~\n");
}

void DigestStream::add_int(std::string_view column, i64 value) {
  write_column(column, 'i');
  write(Decimal<i64>{value}.view());
  write("\n");
}

void DigestStream::add_bool(std::string_view column, bool value) {
  write_column(column, 'b');
  write(value ? "1\n" : "0\n");
}

void DigestStream::add_real(std::string_view column, f64 value) {
  write_column(column, 'r');
  const std::array<char, 16> hex = hex16(std::bit_cast<u64>(value));
  write(std::string_view{hex.data(), hex.size()});
  write("\n");
}

void DigestStream::add_u64(std::string_view column, u64 value) {
  write_column(column, 'u');
  const std::array<char, 16> hex = hex16(value);
  write(std::string_view{hex.data(), hex.size()});
  write("\n");
}

void DigestStream::add_text(std::string_view column, std::string_view value) {
  write_column(column, 't');
  write(Decimal<usize>{value.size()}.view());
  write(":");
  write(value);
  write("\n");
}

void DigestStream::add_blob(std::string_view column, std::span<const std::byte> value) {
  write_column(column, 'x');
  write(Decimal<usize>{value.size()}.view());
  write(":");
  if (!value.empty()) {
    // SAFETY: viewing raw bytes as char for the byte-exact write (char may alias any object).
    write(std::string_view{reinterpret_cast<const char *>(value.data()), value.size()});
  }
  write("\n");
}

void DigestStream::add_entry(std::string_view table, std::string_view row_digest) {
  write(table);
  write(" ");
  write(row_digest);
  write("\n");
}

std::string DigestStream::finish() {
  assert(!finished_ && "DigestStream::finish() called twice");
  finished_ = true;
  auto digest = sha_.finalize();
  if (!digest || failed_) {
    assert(false && "DigestStream: SHA-256 update or finalize failed");
    return {};
  }
  std::string out;
  out.reserve(64);
  for (const std::byte b : *digest) {
    const auto v = static_cast<u8>(b);
    out += kHexDigits[v >> 4U];
    out += kHexDigits[v & 0xFU];
  }
  return out;
}

} // namespace atx::engine::research::store
