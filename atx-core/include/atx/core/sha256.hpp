#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::core {

// Incremental SHA-256 for durable content manifests. The object is single-use:
// update() rejects calls after finalize(), and finalize() rejects repetition.
class Sha256 {
public:
  [[nodiscard]] Status update(std::span<const std::byte> bytes);
  [[nodiscard]] Result<std::array<std::byte, 32>> finalize();

private:
  void transform(const std::byte *block) noexcept;

  std::array<u32, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<std::byte, 64> buffer_{};
  u64 total_bytes_{};
  usize buffer_size_{};
  bool finalized_{};
};

[[nodiscard]] Result<std::string> sha256_hex(std::span<const std::byte> bytes);
[[nodiscard]] Result<std::string> sha256_hex(std::string_view text);
[[nodiscard]] Result<std::string> sha256_file(std::string_view path);

} // namespace atx::core
