#pragma once

// Private helpers shared by the PersistFile translation units (persist_file.cpp,
// persist_codec.cpp, persist_directory.cpp, the typed stores). Not installed.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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

// The zstd FORMAT's own expansion ceiling: the densest block is RLE, a 3-byte header and
// one byte regenerating at most 128 KiB, so no valid frame decodes to more than 32768x
// its size. A lower, data-derived cap would refuse legitimate sections: measured
// 2026-10-24, MarketState columns peak near 300:1 but a constant 16 MiB column (an
// all-zero risk-run column, say) reaches 31596:1, pinned by the test
// PersistFile.ConstantColumnAtTheCodecRatioCeilingRoundTrips. The precise check is the
// block walk below.
inline constexpr std::uint64_t kMaxCodecRatio = 32768;

// An upper bound on what the frame in `stored` can decode to, from its block headers
// alone (raw and RLE blocks exactly, compressed blocks at the frame's block maximum);
// nullopt when the frame or its block chain is malformed. A self-consistent frame
// header can claim any content size; its blocks cannot.
[[nodiscard]] std::optional<std::uint64_t>
zstd_content_bound(std::span<const std::byte> stored) noexcept;

// CodecError unless `stored` is exactly one zstd frame (no trailing bytes) whose
// recorded content size is exactly `raw_length`, `raw_length` is within
// kMaxCodecRawBytes and kMaxCodecRatio x the stored size, and the block headers can
// produce it (zstd_content_bound). Reads headers only: the check a reader makes BEFORE
// allocating `raw_length` bytes.
[[nodiscard]] PersistOutcome check_zstd_frame(std::span<const std::byte> stored,
                                              std::uint64_t raw_length) noexcept;

// Grow-only scratch that is never zero-filled (it is always fully overwritten).
struct CodecScratch {
  std::unique_ptr<std::byte[]> data;
  std::size_t size{0};
  [[nodiscard]] std::span<std::byte> get(std::size_t n) {
    if (n > size) {
      data = std::make_unique_for_overwrite<std::byte[]>(n);
      size = n;
    }
    return {data.get(), n};
  }
};

// Decode one zstd frame into `out` (exactly out.size() bytes), undoing the shuffle.
// CodecError unless `stored` is exactly one frame whose recorded content size is
// out.size() and which decodes to that many bytes. `scratch` is grown as needed.
[[nodiscard]] PersistOutcome decode_zstd(std::span<const std::byte> stored, std::uint32_t stride,
                                         PersistFilter filter, std::span<std::byte> out,
                                         CodecScratch &scratch);

// The zstd library version the codec was built against (bench / inspect output).
[[nodiscard]] unsigned zstd_version_number() noexcept;

// Write `bytes` to `path` through a unique same-directory temp, one flush and a
// rename that REPLACES an existing `path` (the manifest's publish). IoError on failure.
[[nodiscard]] PersistOutcome replace_file(std::string_view path, std::span<const std::byte> bytes);

} // namespace atx::vol::persist_detail
