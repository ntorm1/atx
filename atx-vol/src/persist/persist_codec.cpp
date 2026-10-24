// PersistFile cold-tier codec (design wave3-persist D9): a Blosc-style byte shuffle by
// element stride, then one zstd frame per section. Hot-tier sections never come here:
// decompression at 1.5-3.9 GB/s is below page-cache bandwidth, so only cold-tier
// (compacted) output is encoded. Deterministic for a given zstd version: one-shot
// ZSTD_compress on one thread, so the same input and level give the same frame.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

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

PersistOutcome decode_zstd(std::span<const std::byte> stored, std::uint32_t stride,
                           PersistFilter filter, std::span<std::byte> out,
                           std::vector<std::byte> &scratch) {
  // Exactly one frame, no trailing bytes, recording exactly the expected size: a frame
  // that decodes "successfully" to another length is a corruption, not data.
  if (ZSTD_findFrameCompressedSize(stored.data(), stored.size()) != stored.size()) {
    return persist_error(PersistStatus::CodecError);
  }
  const unsigned long long content = ZSTD_getFrameContentSize(stored.data(), stored.size());
  if (content == ZSTD_CONTENTSIZE_ERROR || content == ZSTD_CONTENTSIZE_UNKNOWN ||
      content != out.size()) {
    return persist_error(PersistStatus::CodecError, kPersistNone, content);
  }
  if (out.empty()) return {};
  const bool shuffled = filter == PersistFilter::ByteShuffle && stride > 1;
  std::span<std::byte> dst = out;
  if (shuffled) {
    scratch.resize(out.size());
    dst = scratch;
  }
  const std::size_t n = ZSTD_decompress(dst.data(), dst.size(), stored.data(), stored.size());
  if (ZSTD_isError(n) != 0u || n != out.size()) return persist_error(PersistStatus::CodecError);
  if (shuffled) byte_unshuffle(dst, stride, out);
  return {};
}

unsigned zstd_version_number() noexcept { return ZSTD_versionNumber(); }

} // namespace atx::vol::persist_detail
