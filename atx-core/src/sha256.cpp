#include "atx/core/sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace atx::core {
namespace {

constexpr std::array<u32, 64> kConstants{
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

} // namespace

void Sha256::transform(const std::byte *block) noexcept {
  std::array<u32, 64> words{};
  for (usize index = 0; index < 16; ++index) {
    const auto offset = index * 4;
    words[index] = (std::to_integer<u32>(block[offset]) << 24U) |
                   (std::to_integer<u32>(block[offset + 1]) << 16U) |
                   (std::to_integer<u32>(block[offset + 2]) << 8U) |
                   std::to_integer<u32>(block[offset + 3]);
  }
  for (usize index = 16; index < words.size(); ++index) {
    const u32 first = std::rotr(words[index - 15], 7) ^ std::rotr(words[index - 15], 18) ^
                      (words[index - 15] >> 3U);
    const u32 second = std::rotr(words[index - 2], 17) ^ std::rotr(words[index - 2], 19) ^
                       (words[index - 2] >> 10U);
    words[index] = words[index - 16] + first + words[index - 7] + second;
  }
  auto a = state_[0];
  auto b = state_[1];
  auto c = state_[2];
  auto d = state_[3];
  auto e = state_[4];
  auto f = state_[5];
  auto g = state_[6];
  auto h = state_[7];
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
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

Status Sha256::update(std::span<const std::byte> bytes) {
  if (finalized_) {
    return Err(ErrorCode::InvalidArgument, "SHA-256 digest is already finalized");
  }
  if (bytes.size() > (std::numeric_limits<u64>::max() - total_bytes_)) {
    return Err(ErrorCode::OutOfRange, "SHA-256 input is too large");
  }
  total_bytes_ += static_cast<u64>(bytes.size());
  while (!bytes.empty()) {
    const usize count = std::min(buffer_.size() - buffer_size_, bytes.size());
    std::memcpy(buffer_.data() + buffer_size_, bytes.data(), count);
    buffer_size_ += count;
    bytes = bytes.subspan(count);
    if (buffer_size_ == buffer_.size()) {
      transform(buffer_.data());
      buffer_size_ = 0;
    }
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
    transform(buffer_.data());
    buffer_size_ = 0;
  }
  std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_), buffer_.begin() + 56,
            std::byte{});
  for (usize index = 0; index < 8; ++index) {
    buffer_[56 + index] = std::byte{static_cast<unsigned char>(bit_length >> (56U - index * 8U))};
  }
  transform(buffer_.data());
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
  std::array<char, 64U * 1024U> buffer{};
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = stream.gcount();
    if (count > 0) {
      ATX_TRY_VOID(
          digest.update(std::as_bytes(std::span{buffer.data(), static_cast<usize>(count)})));
    }
  }
  if (!stream.eof()) {
    return Err(ErrorCode::IoError, "failed while reading file for SHA-256 digest");
  }
  ATX_TRY(auto finalized, digest.finalize());
  return Ok(to_hex(finalized));
}

} // namespace atx::core
