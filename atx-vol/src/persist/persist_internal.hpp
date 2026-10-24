#pragma once

// Private helpers shared by the PersistFile translation units (persist_file.cpp,
// persist_codec.cpp, persist_directory.cpp, the typed stores). Not installed.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "atx/vol/api/persist/persist_format.hpp"

namespace atx::vol::persist_detail {

// One 64-byte block: a std::vector<Block64> is a 64-byte aligned owned buffer (the
// decoded-section and Read-mode backing).
struct alignas(64) Block64 {
  std::byte b[64];
};
static_assert(sizeof(Block64) == 64);

// Blosc-style byte shuffle by element stride: out[b * n + i] = in[i * stride + b] for
// n = in.size() / stride elements; a tail shorter than one element is copied as is.
// `out` must be as long as `in`. Stride 1 is the identity.
void byte_shuffle(std::span<const std::byte> in, std::uint32_t stride,
                  std::span<std::byte> out) noexcept;
void byte_unshuffle(std::span<const std::byte> in, std::uint32_t stride,
                    std::span<std::byte> out) noexcept;

// Encode `raw` as ONE zstd frame (content size recorded) at `level`, after an optional
// shuffle. Single-threaded and deterministic for a given zstd version. CodecError
// when zstd refuses.
[[nodiscard]] PersistResult<std::vector<std::byte>> encode_zstd(std::span<const std::byte> raw,
                                                                std::uint32_t stride,
                                                                PersistFilter filter, int level);

// The largest decoded (raw) length of one codec section, writer and reader alike. A
// reader allocates the decoded buffer from the section table, so the table's raw_length
// must be bounded before any allocation (a resealed table can claim 2^44 or 2^64 - 128).
inline constexpr std::uint64_t kMaxCodecRawBytes = std::uint64_t{1} << 32;

// CodecError unless `stored` is exactly one zstd frame (no trailing bytes) whose
// recorded content size is exactly `raw_length`. Reads the frame header only: the
// check a reader makes BEFORE allocating `raw_length` bytes.
[[nodiscard]] PersistOutcome check_zstd_frame(std::span<const std::byte> stored,
                                              std::uint64_t raw_length) noexcept;

// Decode one zstd frame into `out` (exactly out.size() bytes), undoing the shuffle.
// CodecError unless `stored` is exactly one frame whose recorded content size is
// out.size() and which decodes to that many bytes. `scratch` is grown as needed.
[[nodiscard]] PersistOutcome decode_zstd(std::span<const std::byte> stored, std::uint32_t stride,
                                         PersistFilter filter, std::span<std::byte> out,
                                         std::vector<std::byte> &scratch);

// The zstd library version the codec was built against (bench / inspect output).
[[nodiscard]] unsigned zstd_version_number() noexcept;

// Write `bytes` to `path` through a unique same-directory temp, one flush and a
// rename that REPLACES an existing `path` (the manifest's publish). IoError on failure.
[[nodiscard]] PersistOutcome replace_file(std::string_view path, std::span<const std::byte> bytes);

} // namespace atx::vol::persist_detail
