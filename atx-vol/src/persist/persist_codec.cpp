// PersistFile cold-tier codec (design wave3-persist D9): a Blosc-style byte shuffle by
// element stride, then one zstd frame per section. Hot-tier sections never come here:
// decompression at 1.5-3.9 GB/s is below page-cache bandwidth, so only cold-tier
// (compacted) output is encoded. Deterministic for a given zstd version: one-shot
// ZSTD_compress on one thread, so the same input and level give the same frame.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

// ZSTD_getFrameHeader / ZSTD_FrameHeader (static-linking-only API; the vcpkg zstd 1.5.7
// DLL exports it).
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include "persist/persist_internal.hpp"

namespace atx::vol::persist_detail {

void byte_shuffle(std::span<const std::byte> in, std::uint32_t stride,
                  std::span<std::byte> out) noexcept {
  const std::size_t s = stride == 0 ? 1 : stride;
  const std::size_t n = in.size() / s;
  const std::size_t body = n * s;
  if (s == 1 || n == 0) {
    if (!in.empty()) std::memcpy(out.data(), in.data(), in.size());
    return;
  }
  for (std::size_t b = 0; b < s; ++b) {
    std::byte *dst = out.data() + b * n;
    const std::byte *src = in.data() + b;
    for (std::size_t i = 0; i < n; ++i) dst[i] = src[i * s];
  }
  if (body < in.size()) std::memcpy(out.data() + body, in.data() + body, in.size() - body);
}

void byte_unshuffle(std::span<const std::byte> in, std::uint32_t stride,
                    std::span<std::byte> out) noexcept {
  const std::size_t s = stride == 0 ? 1 : stride;
  const std::size_t n = in.size() / s;
  const std::size_t body = n * s;
  if (s == 1 || n == 0) {
    if (!in.empty()) std::memcpy(out.data(), in.data(), in.size());
    return;
  }
  for (std::size_t b = 0; b < s; ++b) {
    const std::byte *src = in.data() + b * n;
    std::byte *dst = out.data() + b;
    for (std::size_t i = 0; i < n; ++i) dst[i * s] = src[i];
  }
  if (body < in.size()) std::memcpy(out.data() + body, in.data() + body, in.size() - body);
}

PersistResult<std::vector<std::byte>> encode_zstd(std::span<const std::byte> raw,
                                                  std::uint32_t stride, PersistFilter filter,
                                                  int level) {
  std::vector<std::byte> shuffled;
  std::span<const std::byte> src = raw;
  if (filter == PersistFilter::ByteShuffle && stride > 1 && !raw.empty()) {
    shuffled.resize(raw.size());
    byte_shuffle(raw, stride, shuffled);
    src = shuffled;
  }
  std::vector<std::byte> out(ZSTD_compressBound(src.size()));
  const std::size_t n = ZSTD_compress(out.data(), out.size(), src.data(), src.size(), level);
  if (ZSTD_isError(n) != 0u) return persist_error(PersistStatus::CodecError);
  out.resize(n);
  return out;
}

std::optional<std::uint64_t> zstd_content_bound(std::span<const std::byte> stored) noexcept {
  ZSTD_FrameHeader h{};
  if (ZSTD_getFrameHeader(&h, stored.data(), stored.size()) != 0 || h.frameType != ZSTD_frame ||
      h.headerSize > stored.size()) {
    return std::nullopt;
  }
  // RFC 8878 3.1.1.2: a 3-byte little-endian block header (bit 0 last block, bits 1-2
  // type, bits 3-23 size), then the block content.
  const std::uint64_t block_max = h.blockSizeMax;
  std::size_t at = h.headerSize;
  std::uint64_t bound = 0;
  for (;;) {
    if (stored.size() - at < 3) return std::nullopt;
    const auto byte_at = [&](std::size_t k) {
      return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(stored[at + k]));
    };
    const std::uint32_t header = byte_at(0) | (byte_at(1) << 8) | (byte_at(2) << 16);
    at += 3;
    const bool last = (header & 1u) != 0u;
    const std::uint32_t type = (header >> 1) & 3u;
    const std::uint64_t size = header >> 3;
    const std::size_t left = stored.size() - at;
    switch (type) {
    case 0: // raw: `size` bytes stored and produced
      if (size > block_max || size > left) return std::nullopt;
      bound += size;
      at += static_cast<std::size_t>(size);
      break;
    case 1: // RLE: one byte stored, `size` produced
      if (size > block_max || left < 1) return std::nullopt;
      bound += size;
      at += 1;
      break;
    case 2: // compressed: `size` bytes stored, at most the block maximum produced
      if (size > left) return std::nullopt;
      bound += block_max;
      at += static_cast<std::size_t>(size);
      break;
    default: return std::nullopt; // reserved
    }
    if (last) break;
  }
  return bound;
}

PersistOutcome check_zstd_frame(std::span<const std::byte> stored,
                                std::uint64_t raw_length) noexcept {
  if (raw_length > kMaxCodecRawBytes ||
      raw_length / kMaxCodecRatio > static_cast<std::uint64_t>(stored.size())) {
    return persist_error(PersistStatus::CodecError, kPersistNone, raw_length);
  }
  // Exactly one frame, no trailing bytes, recording exactly the expected size: a frame
  // that decodes "successfully" to another length is a corruption, not data.
  if (ZSTD_findFrameCompressedSize(stored.data(), stored.size()) != stored.size()) {
    return persist_error(PersistStatus::CodecError);
  }
  const unsigned long long content = ZSTD_getFrameContentSize(stored.data(), stored.size());
  if (content == ZSTD_CONTENTSIZE_ERROR || content == ZSTD_CONTENTSIZE_UNKNOWN ||
      content != raw_length) {
    return persist_error(PersistStatus::CodecError, kPersistNone, content);
  }
  const std::optional<std::uint64_t> bound = zstd_content_bound(stored);
  if (!bound || raw_length > *bound) {
    return persist_error(PersistStatus::CodecError, kPersistNone, bound.value_or(0));
  }
  return {};
}

PersistOutcome decode_zstd(std::span<const std::byte> stored, std::uint32_t stride,
                           PersistFilter filter, std::span<std::byte> out,
                           CodecScratch &scratch) {
  if (PersistOutcome ok = check_zstd_frame(stored, out.size()); !ok) return ok;
  if (out.empty()) return {};
  const bool shuffled = filter == PersistFilter::ByteShuffle && stride > 1;
  std::span<std::byte> dst = out;
  if (shuffled) dst = scratch.get(out.size());
  const std::size_t n = ZSTD_decompress(dst.data(), dst.size(), stored.data(), stored.size());
  if (ZSTD_isError(n) != 0u || n != out.size()) return persist_error(PersistStatus::CodecError);
  if (shuffled) byte_unshuffle(dst, stride, out);
  return {};
}

unsigned zstd_version_number() noexcept { return ZSTD_versionNumber(); }

} // namespace atx::vol::persist_detail
