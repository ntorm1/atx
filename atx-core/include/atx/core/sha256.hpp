#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::core {

// Block-compression kernel. Every backend computes the identical FIPS 180-4
// function, so digests never depend on it; backends differ only in speed.
// Scalar is portable and always available. X86ShaNi uses the SHA extensions
// (SHA256RNDS2/MSG1/MSG2 plus SSSE3/SSE4.1 shuffles) and is selected only when
// CPUID reports all three on an x86-64 clang build.
enum class Sha256Backend : u8 { Scalar, X86ShaNi };

// The fastest backend this host supports, resolved once per process (thread-safe).
[[nodiscard]] Sha256Backend sha256_default_backend() noexcept;
// True iff `backend` can run on this host with this build.
[[nodiscard]] bool sha256_backend_available(Sha256Backend backend) noexcept;
[[nodiscard]] std::string_view sha256_backend_name(Sha256Backend backend) noexcept;

// Incremental SHA-256 for durable content manifests. The object is single-use:
// update() rejects calls after finalize(), and finalize() rejects repetition.
class Sha256 {
public:
  // Uses sha256_default_backend().
  Sha256() noexcept;
  // Uses `backend` when available on this host, else Scalar (see backend()).
  // Tests and diagnostics pin a backend; production code uses the default.
  explicit Sha256(Sha256Backend backend) noexcept;

  [[nodiscard]] Status update(std::span<const std::byte> bytes);
  [[nodiscard]] Result<std::array<std::byte, 32>> finalize();
  // The backend this object actually runs.
  [[nodiscard]] Sha256Backend backend() const noexcept { return backend_; }

private:
  // Compresses `count` consecutive 64-byte blocks into state_.
  void compress(const std::byte *blocks, usize count) noexcept;

  std::array<u32, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<std::byte, 64> buffer_{};
  u64 total_bytes_{};
  usize buffer_size_{};
  Sha256Backend backend_{Sha256Backend::Scalar};
  bool finalized_{};
};

[[nodiscard]] Result<std::string> sha256_hex(std::span<const std::byte> bytes);
[[nodiscard]] Result<std::string> sha256_hex(std::string_view text);
// Streams the file in 1 MiB reads (one heap buffer per call).
[[nodiscard]] Result<std::string> sha256_file(std::string_view path);

} // namespace atx::core
