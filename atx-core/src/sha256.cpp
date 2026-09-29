#include "atx/core/sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// x86-64 clang (clang-cl included): the SHA-NI kernel is compiled per function
// with [[gnu::target]], so this TU keeps the baseline ISA and the scalar path
// runs on any x86-64 CPU; the kernel runs only after CPUID reports its features.
// clang-cl's <immintrin.h> declares the SSSE3, SSE4.1 and SHA intrinsics only
// when the matching -m flags are on the command line, so their headers are
// included directly (their guards require <immintrin.h> first, which it is).
#if defined(__clang__) && defined(__x86_64__)
#define ATX_SHA256_X86_SHANI 1
#include <immintrin.h>
#include <tmmintrin.h>
#include <smmintrin.h>
#include <shaintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#else
#define ATX_SHA256_X86_SHANI 0
#endif

namespace atx::core {
namespace {

constexpr usize kBlockBytes = 64;
constexpr usize kFileChunkBytes = usize{1} << 20U;

// alignas(16): the SHA-NI kernel loads four constants at a time.
alignas(16) constexpr std::array<u32, 64> kConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

[[nodiscard]] std::string to_hex(const std::array<std::byte, 32> &digest) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (const auto byte : digest) {
    const auto value = std::to_integer<unsigned>(byte);
    result.push_back(hex[value >> 4U]);
    result.push_back(hex[value & 0x0fU]);
  }
  return result;
}

// The FIPS 180-4 reference compression, one 64-byte block at a time.
void compress_scalar(std::array<u32, 8> &state, const std::byte *blocks, usize count) noexcept {
  for (usize block = 0; block < count; ++block) {
    const std::byte *data = blocks + block * kBlockBytes;
    std::array<u32, 64> words{};
    for (usize index = 0; index < 16; ++index) {
      const auto offset = index * 4;
      words[index] = (std::to_integer<u32>(data[offset]) << 24U) |
                     (std::to_integer<u32>(data[offset + 1]) << 16U) |
                     (std::to_integer<u32>(data[offset + 2]) << 8U) |
                     std::to_integer<u32>(data[offset + 3]);
    }
    for (usize index = 16; index < words.size(); ++index) {
      const u32 first = std::rotr(words[index - 15], 7) ^ std::rotr(words[index - 15], 18) ^
                        (words[index - 15] >> 3U);
      const u32 second = std::rotr(words[index - 2], 17) ^ std::rotr(words[index - 2], 19) ^
                         (words[index - 2] >> 10U);
      words[index] = words[index - 16] + first + words[index - 7] + second;
    }
    auto a = state[0];
    auto b = state[1];
    auto c = state[2];
    auto d = state[3];
    auto e = state[4];
    auto f = state[5];
    auto g = state[6];
    auto h = state[7];
    for (usize index = 0; index < words.size(); ++index) {
      const u32 sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const u32 choose = (e & f) ^ ((~e) & g);
      const u32 first = h + sum1 + choose + kConstants[index] + words[index];
      const u32 sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const u32 majority = (a & b) ^ (a & c) ^ (b & c);
      const u32 second = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + first;
      d = c;
      c = b;
      b = a;
      a = first + second;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  }
}

#if ATX_SHA256_X86_SHANI
// Four rounds t..t+3; `wk` = W[t..t+3] + K[t..t+3]. The state is kept in the
// SHA-NI layout: abef = (A,B,E,F) and cdgh = (C,D,G,H), high lane first. Each
// SHA256RNDS2 does two rounds and returns the new ABEF, whose predecessor is
// the new CDGH, so the two halves swap roles between the calls.
[[gnu::target("sha,sse4.1")]] void shani_rounds(__m128i &abef, __m128i &cdgh,
                                                __m128i wk) noexcept {
  cdgh = _mm_sha256rnds2_epu32(cdgh, abef, wk);
  abef = _mm_sha256rnds2_epu32(abef, cdgh, _mm_shuffle_epi32(wk, 0x0E));
}

// Four big-endian message words from 16 bytes of a block.
[[gnu::target("sha,sse4.1")]] __m128i shani_load_words(const std::byte *data,
                                                       __m128i byte_swap) noexcept {
  // SAFETY: unaligned 16-byte load inside the caller's 64-byte block.
  return _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i *>(data)), byte_swap);
}

// Rounds 4g..4g+3 with the message words W[4g..4g+3] in `words`.
[[gnu::target("sha,sse4.1")]] void shani_group(__m128i &abef, __m128i &cdgh, __m128i words,
                                               usize group) noexcept {
  // SAFETY: kConstants is 16-byte aligned and 4 * group + 3 < 64 (group < 16).
  const __m128i constants =
      _mm_load_si128(reinterpret_cast<const __m128i *>(kConstants.data() + 4U * group));
  shani_rounds(abef, cdgh, _mm_add_epi32(words, constants));
}

// W[t..t+3] for t = 4g from the previous sixteen words, oldest first:
// oldest = W[t-16..t-13], older = W[t-12..t-9], previous2 = W[t-8..t-5],
// previous = W[t-4..t-1]. SHA256MSG1 adds sigma0(W[t-15]); ALIGNR supplies
// W[t-7..t-4]; SHA256MSG2 adds sigma1(W[t-2]) including the in-group chain.
[[gnu::target("sha,sse4.1")]] __m128i shani_schedule(__m128i oldest, __m128i older,
                                                     __m128i previous2,
                                                     __m128i previous) noexcept {
  const __m128i partial = _mm_add_epi32(_mm_sha256msg1_epu32(oldest, older),
                                        _mm_alignr_epi8(previous, previous2, 4));
  return _mm_sha256msg2_epu32(partial, previous);
}

// The same function as compress_scalar (the unit tests pin equality on random data).
[[gnu::target("sha,sse4.1")]] void compress_shani(std::array<u32, 8> &state,
                                                  const std::byte *blocks, usize count) noexcept {
  // Reverses the bytes of each 32-bit lane: message words are big-endian.
  const __m128i byte_swap = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);
  // SAFETY: loadu/storeu are unaligned 128-bit accesses of the 8 contiguous u32 words.
  __m128i ordered = _mm_loadu_si128(reinterpret_cast<const __m128i *>(state.data()));
  __m128i cdgh = _mm_loadu_si128(reinterpret_cast<const __m128i *>(state.data() + 4));
  ordered = _mm_shuffle_epi32(ordered, 0xB1);       // C D A B
  cdgh = _mm_shuffle_epi32(cdgh, 0x1B);             // E F G H
  __m128i abef = _mm_alignr_epi8(ordered, cdgh, 8); // A B E F
  cdgh = _mm_blend_epi16(cdgh, ordered, 0xF0);      // C D G H
  for (usize block = 0; block < count; ++block) {
    const std::byte *data = blocks + block * kBlockBytes;
    const __m128i abef_saved = abef;
    const __m128i cdgh_saved = cdgh;
    // w0..w3 rotate: before group g, w[g % 4] holds W[4g-16..4g-13].
    __m128i w0 = shani_load_words(data, byte_swap);
    __m128i w1 = shani_load_words(data + 16, byte_swap);
    __m128i w2 = shani_load_words(data + 32, byte_swap);
    __m128i w3 = shani_load_words(data + 48, byte_swap);
    shani_group(abef, cdgh, w0, 0);
    shani_group(abef, cdgh, w1, 1);
    shani_group(abef, cdgh, w2, 2);
    shani_group(abef, cdgh, w3, 3);
    for (usize group = 4; group < 16; group += 4) {
      w0 = shani_schedule(w0, w1, w2, w3);
      shani_group(abef, cdgh, w0, group);
      w1 = shani_schedule(w1, w2, w3, w0);
      shani_group(abef, cdgh, w1, group + 1U);
      w2 = shani_schedule(w2, w3, w0, w1);
      shani_group(abef, cdgh, w2, group + 2U);
      w3 = shani_schedule(w3, w0, w1, w2);
      shani_group(abef, cdgh, w3, group + 3U);
    }
    abef = _mm_add_epi32(abef, abef_saved);
    cdgh = _mm_add_epi32(cdgh, cdgh_saved);
  }
  const __m128i feba = _mm_shuffle_epi32(abef, 0x1B); // F E B A
  cdgh = _mm_shuffle_epi32(cdgh, 0xB1);               // D C H G
  const __m128i dcba = _mm_blend_epi16(feba, cdgh, 0xF0);
  const __m128i hgfe = _mm_alignr_epi8(cdgh, feba, 8);
  _mm_storeu_si128(reinterpret_cast<__m128i *>(state.data()), dcba);
  _mm_storeu_si128(reinterpret_cast<__m128i *>(state.data() + 4), hgfe);
}

struct CpuidRegisters {
  u32 eax{};
  u32 ebx{};
  u32 ecx{};
  u32 edx{};
};

[[nodiscard]] CpuidRegisters cpuid(u32 leaf, u32 subleaf) noexcept {
#if defined(_MSC_VER)
  std::array<int, 4> registers{};
  __cpuidex(registers.data(), static_cast<int>(leaf), static_cast<int>(subleaf));
  return {static_cast<u32>(registers[0]), static_cast<u32>(registers[1]),
          static_cast<u32>(registers[2]), static_cast<u32>(registers[3])};
#else
  unsigned eax = 0;
  unsigned ebx = 0;
  unsigned ecx = 0;
  unsigned edx = 0;
  __cpuid_count(leaf, subleaf, eax, ebx, ecx, edx);
  return {eax, ebx, ecx, edx};
#endif
}

[[nodiscard]] bool bit_set(u32 value, unsigned position) noexcept {
  return ((value >> position) & 1U) != 0U;
}

// SSSE3 = CPUID.1:ECX[9], SSE4.1 = CPUID.1:ECX[19], SHA = CPUID.(7,0):EBX[29].
// XMM state is always OS-enabled on x86-64, so no XGETBV check is needed.
[[nodiscard]] bool host_has_shani() noexcept {
  if (cpuid(0, 0).eax < 7U) {
    return false;
  }
  const auto features = cpuid(1, 0);
  return bit_set(features.ecx, 9U) && bit_set(features.ecx, 19U) &&
         bit_set(cpuid(7, 0).ebx, 29U);
}
#endif

} // namespace

bool sha256_backend_available(Sha256Backend backend) noexcept {
  switch (backend) {
  case Sha256Backend::Scalar:
    return true;
  case Sha256Backend::X86ShaNi: {
#if ATX_SHA256_X86_SHANI
    static const bool available = host_has_shani();
    return available;
#else
    return false;
#endif
  }
  }
  return false;
}

Sha256Backend sha256_default_backend() noexcept {
  static const Sha256Backend backend = sha256_backend_available(Sha256Backend::X86ShaNi)
                                           ? Sha256Backend::X86ShaNi
                                           : Sha256Backend::Scalar;
  return backend;
}

std::string_view sha256_backend_name(Sha256Backend backend) noexcept {
  switch (backend) {
  case Sha256Backend::Scalar:
    return "scalar";
  case Sha256Backend::X86ShaNi:
    return "x86-sha-ni";
  }
  return "unknown";
}

Sha256::Sha256() noexcept : backend_{sha256_default_backend()} {}

Sha256::Sha256(Sha256Backend backend) noexcept
    : backend_{sha256_backend_available(backend) ? backend : Sha256Backend::Scalar} {}

void Sha256::compress(const std::byte *blocks, usize count) noexcept {
#if ATX_SHA256_X86_SHANI
  if (backend_ == Sha256Backend::X86ShaNi) {
    compress_shani(state_, blocks, count);
    return;
  }
#endif
  compress_scalar(state_, blocks, count);
}

Status Sha256::update(std::span<const std::byte> bytes) {
  if (finalized_) {
    return Err(ErrorCode::InvalidArgument, "SHA-256 digest is already finalized");
  }
  if (bytes.size() > (std::numeric_limits<u64>::max() - total_bytes_)) {
    return Err(ErrorCode::OutOfRange, "SHA-256 input is too large");
  }
  if (bytes.empty()) {
    return Ok();
  }
  total_bytes_ += static_cast<u64>(bytes.size());
  // Top up a partial block first; whole blocks then compress straight from the
  // caller's bytes (no copy), and only the tail waits in buffer_.
  if (buffer_size_ > 0) {
    const usize count = std::min(buffer_.size() - buffer_size_, bytes.size());
    std::memcpy(buffer_.data() + buffer_size_, bytes.data(), count);
    buffer_size_ += count;
    bytes = bytes.subspan(count);
    if (buffer_size_ < buffer_.size()) {
      return Ok();
    }
    compress(buffer_.data(), 1);
    buffer_size_ = 0;
  }
  const usize blocks = bytes.size() / kBlockBytes;
  if (blocks > 0) {
    compress(bytes.data(), blocks);
    bytes = bytes.subspan(blocks * kBlockBytes);
  }
  if (!bytes.empty()) {
    std::memcpy(buffer_.data(), bytes.data(), bytes.size());
    buffer_size_ = bytes.size();
  }
  return Ok();
}

Result<std::array<std::byte, 32>> Sha256::finalize() {
  if (finalized_) {
    return Err(ErrorCode::InvalidArgument, "SHA-256 digest is already finalized");
  }
  if (total_bytes_ > std::numeric_limits<u64>::max() / 8U) {
    return Err(ErrorCode::OutOfRange, "SHA-256 bit length overflow");
  }
  finalized_ = true;
  const u64 bit_length = total_bytes_ * 8U;
  buffer_[buffer_size_++] = std::byte{0x80};
  if (buffer_size_ > 56) {
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_), buffer_.end(),
              std::byte{});
    compress(buffer_.data(), 1);
    buffer_size_ = 0;
  }
  std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_), buffer_.begin() + 56,
            std::byte{});
  for (usize index = 0; index < 8; ++index) {
    buffer_[56 + index] = std::byte{static_cast<unsigned char>(bit_length >> (56U - index * 8U))};
  }
  compress(buffer_.data(), 1);
  std::array<std::byte, 32> digest{};
  for (usize word = 0; word < state_.size(); ++word) {
    for (usize byte = 0; byte < 4; ++byte) {
      digest[word * 4 + byte] =
          std::byte{static_cast<unsigned char>(state_[word] >> (24U - byte * 8U))};
    }
  }
  return Ok(digest);
}

Result<std::string> sha256_hex(std::span<const std::byte> bytes) {
  Sha256 digest;
  ATX_TRY_VOID(digest.update(bytes));
  ATX_TRY(auto finalized, digest.finalize());
  return Ok(to_hex(finalized));
}

Result<std::string> sha256_hex(std::string_view text) {
  return sha256_hex(std::as_bytes(std::span{text.data(), text.size()}));
}

Result<std::string> sha256_file(std::string_view path) {
  if (path.empty()) {
    return Err(ErrorCode::InvalidArgument, "SHA-256 file path is empty");
  }
  std::ifstream stream{std::string{path}, std::ios::binary};
  if (!stream) {
    return Err(ErrorCode::IoError, "cannot open file for SHA-256 digest");
  }
  Sha256 digest;
  // Large reads keep per-call stream overhead negligible next to the hash;
  // for_overwrite: read() writes every byte before it is hashed.
  const auto buffer = std::make_unique_for_overwrite<char[]>(kFileChunkBytes);
  while (stream) {
    stream.read(buffer.get(), static_cast<std::streamsize>(kFileChunkBytes));
    const auto count = stream.gcount();
    if (count > 0) {
      ATX_TRY_VOID(
          digest.update(std::as_bytes(std::span{buffer.get(), static_cast<usize>(count)})));
    }
  }
  if (!stream.eof()) {
    return Err(ErrorCode::IoError, "failed while reading file for SHA-256 digest");
  }
  ATX_TRY(auto finalized, digest.finalize());
  return Ok(to_hex(finalized));
}

} // namespace atx::core
