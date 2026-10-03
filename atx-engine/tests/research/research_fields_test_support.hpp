#pragma once

// Test support of atx-engine-research-fields-tests: scratch directories, file bytes and a tiny
// synthetic research role written exactly as prepare_research_fields.py's Role reads one.

#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/sha256.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/fields/clock.hpp"

namespace atx::engine::research::fields::test {

namespace fs = std::filesystem;

// A fresh, empty directory under the process-isolated temp path (atx-test-scratch).
inline fs::path scratch(std::string_view name) {
  const fs::path dir = fs::temp_directory_path() / ("atx_research_fields_" + std::string(name));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

inline void write_bytes(const fs::path &path, std::string_view bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

inline std::string read_bytes(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline std::string sha256_of(std::string_view bytes) { return core::sha256_hex(bytes).value(); }

template <class T> std::string bytes_of(const std::vector<T> &values) {
  std::string out(values.size() * sizeof(T), '\0');
  if (!values.empty()) {
    std::memcpy(out.data(), values.data(), out.size());
  }
  return out;
}

inline bool same_bits(f64 a, f64 b) { return std::bit_cast<u64>(a) == std::bit_cast<u64>(b); }

// A role of `days` sessions (calendar days) and `ids`; member / present / volume are date-major.
struct TinyRole {
  std::vector<i64> days;
  std::vector<u64> ids;
  std::vector<u8> member;
  std::vector<u8> present;
  std::vector<f64> volume;
  i64 score_begin{0};
};

// Writes the role payload and its manifest under `dir` (created); returns the manifest's SHA-256.
inline std::string write_role(const fs::path &dir, const TinyRole &role) {
  fs::create_directories(dir);
  std::vector<i64> sessions;
  for (const i64 d : role.days) {
    sessions.push_back(d * kDayNs);
  }
  const std::vector<std::pair<std::string, std::string>> files{
      {"ids.u64", bytes_of(role.ids)},
      {"member.u8", bytes_of(role.member)},
      {"present.u8", bytes_of(role.present)},
      {"sessions.i64", bytes_of(sessions)},
      {"volume.f64", bytes_of(role.volume)}};
  std::string entries;
  for (const auto &[name, blob] : files) {
    write_bytes(dir / name, blob);
    entries += (entries.empty() ? "" : ",") + std::string("\"") + name +
               "\":{\"bytes\":" + std::to_string(blob.size()) + ",\"sha256\":\"" + sha256_of(blob) +
               "\"}";
  }
  const std::string manifest =
      "{\"schema\":\"atx.recent-research-role/v1\",\"status\":\"complete\",\"dates\":" +
      std::to_string(role.days.size()) + ",\"instruments\":" + std::to_string(role.ids.size()) +
      ",\"instrument_namespace\":\"spiderrock.securityID\",\"score_begin\":" +
      std::to_string(role.score_begin) + ",\"score_end\":" + std::to_string(role.days.size()) +
      ",\"source_sha256\":\"" + std::string(64, '0') + "\",\"files\":{" + entries + "}}\n";
  write_bytes(dir / "manifest.json", manifest);
  return sha256_of(manifest);
}

// Consecutive calendar days starting at `first` (weekends included: the field rules here never read
// a calendar).
inline std::vector<i64> consecutive_days(i64 first, usize count) {
  std::vector<i64> out;
  for (usize i = 0; i < count; ++i) {
    out.push_back(first + static_cast<i64>(i));
  }
  return out;
}

} // namespace atx::engine::research::fields::test
