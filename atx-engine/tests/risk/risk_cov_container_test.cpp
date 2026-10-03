// atx.cov-container 1.0 (`model.atxcov`, K-P9-12; P9 lane COV, cov-design.md section 4): the
// writer, the memory-mapped reader with Full / Lazy verification, the date-index refusals, the
// prefix property and the zero-copy FactorRiskView. Synthetic containers only (small geometry:
// G = 4 groups, S = 3 dense columns, K = 8).

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/sha256.hpp"
#include "atx/engine/book/risk_target.hpp"
#include "atx/engine/risk/cov_container.hpp"

namespace {
using namespace atx;
namespace er = atx::engine::risk;
namespace cov = atx::engine::risk::cov;
namespace co = atx::core;
namespace fs = std::filesystem;

constexpr i64 day_ns = 86'400'000'000'000LL;
constexpr i64 first_day = 18'262LL * day_ns; // 2020-01-01
constexpr u32 groups = 4, dense = 3, factors = 1 + groups + dense;
constexpr u64 max_bytes = 1ULL << 32;

struct Directory {
  fs::path path;
  Directory() {
    static std::atomic<u64> counter{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() /
           ("atx_cov_" + std::to_string(tick) + "_" + std::to_string(counter.fetch_add(1)));
    if (!fs::create_directory(path)) throw std::runtime_error("fixture directory");
  }
  ~Directory() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
};

std::vector<u8> read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) throw std::runtime_error("read " + path.string());
  std::vector<u8> out(static_cast<usize>(in.tellg()));
  in.seekg(0);
  // SAFETY: char reads the object representation of a byte buffer.
  in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
  return out;
}
void write_file(const fs::path& path, const std::vector<u8>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  // SAFETY: char writes the object representation of a byte buffer.
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!out) throw std::runtime_error("write " + path.string());
}
template <class T>
T get(const std::vector<u8>& bytes, u64 offset) {
  T out{};
  std::memcpy(&out, bytes.data() + offset, sizeof(T));
  return out;
}
template <class T>
void put(std::vector<u8>& bytes, u64 offset, const T& value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}
er::Sha256Bytes sha(std::span<const std::span<const u8>> parts) {
  co::Sha256 h;
  for (const auto p : parts) EXPECT_TRUE(h.update(std::as_bytes(p)));
  const auto raw = h.finalize().value();
  er::Sha256Bytes out{};
  for (usize i = 0; i < out.size(); ++i) out[i] = std::to_integer<u8>(raw[i]);
  return out;
}
// The root of cov-design 4.7 recomputed independently of the writer, stored in the header and the
// trailer (so a test can change one field and leave only that field wrong).
void rehash_root(std::vector<u8>& bytes) {
  const auto h = get<er::CovFileHeader>(bytes, 0);
  const u64 table = h.section_table_offset, entries = h.section_count;
  std::vector<std::span<const u8>> parts{std::span<const u8>(bytes).first(224),
                                         std::span<const u8>(bytes).subspan(table, entries * 64)};
  for (u64 s = 0; s < entries; ++s) {
    const auto e = get<er::CovSectionEntry>(bytes, table + s * 64);
    // A section moved out of the file (a refusal fixture) cannot be hashed: left out here.
    const bool inside = e.offset <= bytes.size() && e.bytes <= bytes.size() - e.offset;
    if ((e.flags & cov::section_in_root) && inside)
      parts.push_back(std::span<const u8>(bytes).subspan(e.offset, e.bytes));
  }
  const auto root = sha(parts);
  put(bytes, 224, root);
  put(bytes, bytes.size() - 64 + 16, root);
}

// One synthetic session: rows [0, P) priced (group 1..G, specific > 0, even role indexes), the rest
// unpriced (group 0 or a NaN specific with a payload, odd role indexes).
struct Owned {
  std::vector<u64> ids;
  std::vector<u32> role, group;
  std::vector<f32> x32;
  std::vector<f64> x64, specific, cov, fret, diag, gamma;
  std::vector<u8> fflags, rflags;
  i64 as_of{}, cutoff{}, first{};
  u32 priced{}, date{}, flags{};
  [[nodiscard]] er::CovBlockInput input(bool f32, bool optional = true) const {
    er::CovBlockInput b;
    b.as_of_session_ns = as_of; b.data_cutoff_ns = cutoff; b.history_first_ns = first;
    b.role_date_index = date; b.flags = flags; b.priced_rows = priced;
    b.ids = ids; b.role_index = role; b.group = group; b.specific = specific; b.factor_cov = cov;
    if (f32) b.exposures_f32 = x32; else b.exposures_f64 = x64;
    if (optional) {
      b.factor_return = fret; b.factor_flags = fflags; b.row_flags = rflags; b.diag = diag;
      b.eigen_gamma = gamma;
    }
    return b;
  }
};
u64 mix(u64 x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31U);
}
f64 uniform(u64& s) { s = mix(s); return static_cast<f64>(s >> 11U) * 0x1.0p-53; }
constexpr u64 nan_payload = 0x7FF80000DEADBEEFULL;
constexpr u32 nan_payload32 = 0x7FC0BEEFU;
Owned make_block(u32 t, u32 rows, u64 seed, bool complete = true) {
  Owned o;
  u64 s = mix(seed * 1000 + t);
  o.as_of = first_day + static_cast<i64>(t) * day_ns;
  o.cutoff = o.as_of;
  o.first = first_day;
  o.date = t;
  o.priced = rows * 2 / 3;
  for (u32 r = 0; r < rows; ++r) {
    const bool p = r < o.priced;
    const u32 role = p ? 2 * r : 2 * (r - o.priced) + 1;
    o.ids.push_back(1000 + role);
    o.role.push_back(role);
    o.group.push_back(p ? 1 + r % groups : (r % 2 ? 0 : 1 + r % groups));
    o.specific.push_back(p ? 1e-4 + 1e-4 * uniform(s) : std::bit_cast<f64>(nan_payload + t));
    o.rflags.push_back(static_cast<u8>(p ? cov::row_eligible : 0));
    for (u32 c = 0; c < dense; ++c) {
      const f64 z = 6.0 * uniform(s) - 3.0;
      o.x64.push_back(z);
      o.x32.push_back(r == rows - 1 && c == 0 ? std::bit_cast<f32>(nan_payload32)
                                              : static_cast<f32>(z));
    }
  }
  std::vector<f64> v(factors);
  for (auto& x : v) x = 1e-3 * (uniform(s) - 0.5);
  for (u32 a = 0; a < factors; ++a)
    for (u32 b = 0; b < factors; ++b) o.cov.push_back(v[a] * v[b] + (a == b ? 1e-5 : 0.0));
  if (!complete) {
    const u32 k = factors - 1; // the last dense factor unforecast: its row and column NaN
    for (u32 a = 0; a < factors; ++a) {
      o.cov[a * factors + k] = std::bit_cast<f64>(nan_payload);
      o.cov[k * factors + a] = std::bit_cast<f64>(nan_payload);
    }
  }
  for (u32 k = 0; k < factors; ++k) {
    o.fret.push_back(1e-3 * (uniform(s) - 0.5));
    o.fflags.push_back(static_cast<u8>(std::isfinite(o.cov[k * factors + k]) ? cov::factor_forecast : 0));
    o.gamma.push_back(1.0 + 0.1 * uniform(s));
  }
  for (usize d = 0; d < cov::diag_count; ++d) o.diag.push_back(d < 4 ? uniform(s) : std::bit_cast<f64>(nan_payload));
  o.flags = cov::date_forecast | cov::date_vra;
  return o;
}
er::CovContainerSpec make_spec(bool f32) {
  er::CovContainerSpec s;
  s.groups = groups; s.dense = dense; s.f32_exposures = f32;
  for (u32 k = 0; k < factors; ++k) s.factor_names.push_back("f" + std::to_string(k));
  s.role_sha256.fill(0xAB);
  s.recipe_json = R"({"model":"synthetic"})";
  s.inputs_json = R"({"fields":"synthetic"})";
  s.meta_json = R"({"inputs":{"fields":"synthetic"},"recipe":{"model":"synthetic"}})";
  return s;
}
std::vector<Owned> make_blocks(u32 count, u64 seed, u32 rows = 12) {
  std::vector<Owned> out;
  for (u32 t = 0; t < count; ++t) out.push_back(make_block(t, rows + t % 3, seed, t % 2 == 0));
  return out;
}
// Writes the blocks to `path`; returns the root.
er::Sha256Bytes write(const fs::path& path, const std::vector<Owned>& blocks, bool f32) {
  auto w = er::CovContainerWriter::create(path.string(), make_spec(f32));
  EXPECT_TRUE(w) << w.error().to_string();
  for (const auto& b : blocks) {
    const auto s = w->append(b.input(f32));
    EXPECT_TRUE(s) << s.error().to_string();
  }
  auto root = w->finalize();
  EXPECT_TRUE(root) << root.error().to_string();
  return *root;
}
co::Result<er::CovModelFile> open(const fs::path& path, er::VerifyMode mode = er::VerifyMode::Full,
                                  std::optional<er::Sha256Bytes> pin = std::nullopt,
                                  usize workers = 0) {
  return er::CovModelFile::open(path.string(), pin, mode, max_bytes, workers);
}
template <class T>
std::vector<u64> bits_of(std::span<const T> v) {
  std::vector<u64> out;
  for (const T x : v) {
    if constexpr (sizeof(T) == 8) out.push_back(std::bit_cast<u64>(x));
    else if constexpr (sizeof(T) == 4) out.push_back(std::bit_cast<u32>(x));
    else out.push_back(static_cast<u64>(x));
  }
  return out;
}
template <class T>
std::vector<u64> bits_of(const std::vector<T>& v) { return bits_of(std::span<const T>(v)); }
// The data offset of block t's array `id` (from its array table).
u64 array_offset(const std::vector<u8>& bytes, const er::CovDateIndexEntry& e, cov::ArrayId id) {
  const auto h = get<er::CovBlockHeader>(bytes, e.block_offset);
  for (u32 a = 0; a < h.array_count; ++a) {
    const auto entry = get<er::CovArrayEntry>(bytes, e.block_offset + 48 + 24ULL * a);
    if (entry.id == static_cast<u32>(id)) return e.block_offset + entry.offset_in_block;
  }
  throw std::runtime_error("array not found");
}
std::vector<er::CovDateIndexEntry> index_of(const std::vector<u8>& bytes) {
  const auto h = get<er::CovFileHeader>(bytes, 0);
  for (u32 s = 0; s < h.section_count; ++s) {
    const auto e = get<er::CovSectionEntry>(bytes, h.section_table_offset + 64ULL * s);
    if (e.kind != static_cast<u32>(cov::SectionKind::DateIndex)) continue;
    std::vector<er::CovDateIndexEntry> out(e.count);
    std::memcpy(out.data(), bytes.data() + e.offset, e.bytes);
    return out;
  }
  throw std::runtime_error("no index");
}
template <class T>
bool parse_error(const co::Result<T>& r) {
  return !r && r.error().code() == co::ErrorCode::ParseError;
}

TEST(RiskCovContainer, RoundTripIsBitExact) {
  for (const bool f32 : {true, false}) {
    SCOPED_TRACE(f32 ? "f32 exposures" : "f64 exposures");
    const Directory dir;
    const auto blocks = make_blocks(5, 11);
    const auto root = write(dir.path / "model.atxcov", blocks, f32);
    const auto file = open(dir.path / "model.atxcov");
    ASSERT_TRUE(file) << file.error().to_string();
    EXPECT_EQ(file->root_sha256(), root);
    EXPECT_EQ(file->date_count(), blocks.size());
    EXPECT_EQ(file->f32_exposures(), f32);
    EXPECT_EQ(file->factor_names().size(), factors);
    EXPECT_EQ(file->factor_names()[3], "f3");
    EXPECT_EQ(file->meta_json(), make_spec(f32).meta_json);
    for (usize t = 0; t < blocks.size(); ++t) {
      SCOPED_TRACE(t);
      const auto& b = blocks[t];
      const auto v = file->view(t);
      ASSERT_TRUE(v) << v.error().to_string();
      EXPECT_EQ(v->as_of, b.as_of);
      EXPECT_EQ(v->cutoff, b.cutoff);
      EXPECT_EQ(v->priced, b.priced);
      EXPECT_EQ(bits_of(v->ids), bits_of(b.ids));
      EXPECT_EQ(bits_of(v->role_index), bits_of(b.role));
      EXPECT_EQ(bits_of(v->group), bits_of(b.group));
      EXPECT_EQ(bits_of(v->specific), bits_of(b.specific)); // NaN payloads included
      EXPECT_EQ(bits_of(v->factor_cov), bits_of(b.cov));
      if (f32) EXPECT_EQ(bits_of(v->exposures_f32), bits_of(b.x32));
      else EXPECT_EQ(bits_of(v->exposures_f64), bits_of(b.x64));
      EXPECT_EQ(bits_of(v->factor_return), bits_of(b.fret));
      EXPECT_EQ(bits_of(v->diag), bits_of(b.diag));
      EXPECT_EQ(bits_of(v->eigen_gamma), bits_of(b.gamma));
      EXPECT_EQ(bits_of(v->factor_flags), bits_of(b.fflags));
      EXPECT_EQ(bits_of(v->row_flags), bits_of(b.rflags));
      const auto& e = file->index()[t];
      EXPECT_EQ(e.role_date_index, b.date);
      EXPECT_EQ(e.history_first_ns, b.first);
      // date_factor_complete is the writer's, from the covariance (even blocks are complete).
      EXPECT_EQ((e.flags & cov::date_factor_complete) != 0, t % 2 == 0);
      EXPECT_EQ(e.flags & ~cov::date_factor_complete, b.flags);
      EXPECT_EQ(e.unforecast_factors, t % 2 == 0 ? 0U : 1U);
    }
  }
}

TEST(RiskCovContainer, SameInputsSameBytes) {
  const Directory dir;
  const auto blocks = make_blocks(4, 13);
  const auto a = write(dir.path / "a.atxcov", blocks, true);
  const auto b = write(dir.path / "b.atxcov", blocks, true);
  EXPECT_EQ(a, b);
  EXPECT_EQ(read_file(dir.path / "a.atxcov"), read_file(dir.path / "b.atxcov"));
  std::vector<u8> encoded_a, encoded_b;
  ASSERT_TRUE(er::encode_cov_block(make_spec(true), blocks[1].input(true), encoded_a));
  ASSERT_TRUE(er::encode_cov_block(make_spec(true), blocks[1].input(true), encoded_b));
  EXPECT_EQ(encoded_a, encoded_b);
  EXPECT_FALSE(fs::exists(dir.path / "a.atxcov.partial"));
}

TEST(RiskCovContainer, HeaderLayoutIsPinned) {
  EXPECT_EQ(offsetof(er::CovFileHeader, flags), 20U);
  EXPECT_EQ(offsetof(er::CovFileHeader, root_sha256), 224U);
  EXPECT_EQ(offsetof(er::CovDateIndexEntry, rows), 40U);
  EXPECT_EQ(offsetof(er::CovBlockHeader, array_count), 44U);
  EXPECT_EQ(offsetof(er::CovTrailer, root_sha256), 16U);
  const Directory dir;
  const auto blocks = make_blocks(3, 17);
  const auto root = write(dir.path / "m.atxcov", blocks, true);
  const auto bytes = read_file(dir.path / "m.atxcov");
  const std::array<u8, 8> magic{0x89, 0x43, 0x4F, 0x56, 0x0D, 0x0A, 0x1A, 0x0A};
  EXPECT_TRUE(std::equal(magic.begin(), magic.end(), bytes.begin()));
  EXPECT_EQ(get<u16>(bytes, 8), 1U);   // major
  EXPECT_EQ(get<u16>(bytes, 10), 0U);  // minor
  EXPECT_EQ(get<u32>(bytes, 12), 4096U);
  EXPECT_EQ(bytes[16], 0x04); EXPECT_EQ(bytes[17], 0x03); EXPECT_EQ(bytes[18], 0x02);
  EXPECT_EQ(bytes[19], 0x01); // endian tag 0x01020304, little-endian on disk
  EXPECT_EQ(get<u32>(bytes, 20), cov::flag_complete | cov::flag_f32_exposures);
  EXPECT_EQ(get<u64>(bytes, 24), bytes.size());
  EXPECT_EQ(get<u32>(bytes, 32), 5U); // sections
  EXPECT_EQ(get<u32>(bytes, 36), 3U); // dates
  const u64 table = get<u64>(bytes, 40);
  EXPECT_EQ(table % 4096, 0U);
  EXPECT_EQ(get<u32>(bytes, 48), factors);
  EXPECT_EQ(get<u32>(bytes, 52), groups);
  EXPECT_EQ(get<u32>(bytes, 56), dense);
  EXPECT_EQ(get<u32>(bytes, 60), 14U); // max rows (12 + 2)
  EXPECT_EQ(get<i64>(bytes, 64), blocks.front().as_of);
  EXPECT_EQ(get<i64>(bytes, 72), blocks.back().as_of);
  EXPECT_EQ(get<er::Sha256Bytes>(bytes, 112), make_spec(true).role_sha256);
  EXPECT_EQ(get<er::Sha256Bytes>(bytes, 224), root);
  for (usize i = 176; i < 224; ++i) EXPECT_EQ(bytes[i], 0U) << i;   // reserved
  for (usize i = 256; i < 4096; ++i) ASSERT_EQ(bytes[i], 0U) << i; // header padding
  // The recipe and inputs digests are SHA-256 of the canonical texts.
  const std::string recipe = make_spec(true).recipe_json;
  const std::array<std::span<const u8>, 1> parts{
      std::span<const u8>(reinterpret_cast<const u8*>(recipe.data()), recipe.size())};
  EXPECT_EQ(get<er::Sha256Bytes>(bytes, 80), sha(parts));
  // Section table: kinds 1..5 ascending, 64 bytes each; META etc. 64-aligned; BLOCKS at 4096.
  for (u32 s = 0; s < 5; ++s) {
    const auto e = get<er::CovSectionEntry>(bytes, table + 64ULL * s);
    EXPECT_EQ(e.kind, s + 1);
    EXPECT_EQ(e.offset % (e.kind == 5 ? 4096U : 64U), 0U);
    if (e.kind == 5) { EXPECT_EQ(e.offset, 4096U); EXPECT_EQ(e.offset + e.bytes, table); }
  }
  // Block 0: magic, header fields, the array table (11 arrays: 48 + 264 = 312 -> 320).
  EXPECT_EQ(std::string(bytes.begin() + 4096, bytes.begin() + 4104), "COVBLK01");
  EXPECT_EQ(get<i64>(bytes, 4096 + 8), blocks[0].as_of);
  EXPECT_EQ(get<u32>(bytes, 4096 + 24), static_cast<u32>(blocks[0].ids.size()));
  EXPECT_EQ(get<u32>(bytes, 4096 + 44), 11U);
  EXPECT_EQ(get<u32>(bytes, 4096 + 48), 1U);          // IDS first
  EXPECT_EQ(get<u64>(bytes, 4096 + 48 + 8), 320U);    // ... at 320
  EXPECT_EQ(get<u32>(bytes, 4096 + 48 + 24 * 6), 7U | cov::array_optional); // FACTOR_RETURN
  // Trailer.
  const u64 end = bytes.size();
  EXPECT_EQ(std::string(bytes.begin() + static_cast<std::ptrdiff_t>(end - 64),
                        bytes.begin() + static_cast<std::ptrdiff_t>(end - 56)), "COVEND01");
  EXPECT_EQ(get<u64>(bytes, end - 56), end);
  EXPECT_EQ(get<er::Sha256Bytes>(bytes, end - 48), root);
  auto copy = bytes;
  rehash_root(copy);
  EXPECT_EQ(copy, bytes); // the documented root definition is the writer's
}

TEST(RiskCovContainer, ArraysAre64ByteAlignedAndBlocksPageAligned) {
  const Directory dir;
  write(dir.path / "m.atxcov", make_blocks(6, 19, 37), false);
  const auto file = open(dir.path / "m.atxcov");
  ASSERT_TRUE(file);
  const auto aligned = [](const void* p, usize a) {
    return reinterpret_cast<std::uintptr_t>(p) % a == 0;
  };
  for (usize t = 0; t < file->date_count(); ++t) {
    const auto& e = file->index()[t];
    EXPECT_EQ(e.block_offset % 4096, 0U);
    EXPECT_EQ(e.block_bytes % 4096, 0U);
    const auto v = file->view(t);
    ASSERT_TRUE(v);
    for (const void* p : {static_cast<const void*>(v->ids.data()), static_cast<const void*>(v->role_index.data()),
                          static_cast<const void*>(v->group.data()), static_cast<const void*>(v->exposures_f64.data()),
                          static_cast<const void*>(v->specific.data()), static_cast<const void*>(v->factor_cov.data()),
                          static_cast<const void*>(v->factor_return.data()), static_cast<const void*>(v->factor_flags.data()),
                          static_cast<const void*>(v->row_flags.data()), static_cast<const void*>(v->diag.data()),
                          static_cast<const void*>(v->eigen_gamma.data())})
      EXPECT_TRUE(aligned(p, 64)) << t;
  }
}

TEST(RiskCovContainer, RefusesTruncatedFile) {
  const Directory dir;
  write(dir.path / "m.atxcov", make_blocks(4, 23), true);
  const auto bytes = read_file(dir.path / "m.atxcov");
  for (const u64 cut : {u64{1}, u64{64}, u64{4096}, static_cast<u64>(bytes.size()) - 5000}) {
    SCOPED_TRACE(cut);
    std::vector<u8> shorter(bytes.begin(), bytes.end() - static_cast<std::ptrdiff_t>(cut));
    write_file(dir.path / "short.atxcov", shorter);
    EXPECT_TRUE(parse_error(open(dir.path / "short.atxcov")));
  }
}

TEST(RiskCovContainer, RefusesBadMagicOrMajorAndAcceptsHigherMinor) {
  const Directory dir;
  write(dir.path / "m.atxcov", make_blocks(3, 29), true);
  const auto bytes = read_file(dir.path / "m.atxcov");
  auto magic = bytes;
  magic[1] = 'X';
  rehash_root(magic);
  write_file(dir.path / "magic.atxcov", magic);
  EXPECT_TRUE(parse_error(open(dir.path / "magic.atxcov")));
  auto major = bytes;
  put<u16>(major, 8, 2);
  rehash_root(major);
  write_file(dir.path / "major.atxcov", major);
  const auto refused = open(dir.path / "major.atxcov");
  ASSERT_TRUE(parse_error(refused));
  EXPECT_NE(refused.error().message().find("major"), std::string::npos);
  auto minor = bytes;
  put<u16>(minor, 10, 7);
  rehash_root(minor); // a later 1.x writer computes its root over its own header
  write_file(dir.path / "minor.atxcov", minor);
  const auto accepted = open(dir.path / "minor.atxcov");
  ASSERT_TRUE(accepted) << accepted.error().to_string();
  EXPECT_EQ(accepted->header().minor, 7U);
  // Minor changed but root not recomputed: the metadata no longer matches its root.
  auto stale = bytes;
  put<u16>(stale, 10, 7);
  write_file(dir.path / "stale.atxcov", stale);
  EXPECT_TRUE(parse_error(open(dir.path / "stale.atxcov")));
}

TEST(RiskCovContainer, RefusesIncompleteFlag) {
  const Directory dir;
  write(dir.path / "m.atxcov", make_blocks(3, 31), true);
  auto bytes = read_file(dir.path / "m.atxcov");
  put<u32>(bytes, 20, get<u32>(bytes, 20) & ~cov::flag_complete);
  rehash_root(bytes);
  write_file(dir.path / "incomplete.atxcov", bytes);
  const auto r = open(dir.path / "incomplete.atxcov");
  ASSERT_TRUE(parse_error(r));
  EXPECT_NE(r.error().message().find("incomplete"), std::string::npos);
  // A writer dropped before finalize leaves only its .partial, which is refused.
  {
    auto w = er::CovContainerWriter::create((dir.path / "dropped.atxcov").string(), make_spec(true));
    ASSERT_TRUE(w);
    const auto blocks = make_blocks(2, 31);
    ASSERT_TRUE(w->append(blocks[0].input(true)));
  }
  EXPECT_FALSE(fs::exists(dir.path / "dropped.atxcov"));
  ASSERT_TRUE(fs::exists(dir.path / "dropped.atxcov.partial"));
  EXPECT_TRUE(parse_error(open(dir.path / "dropped.atxcov.partial")));
}

TEST(RiskCovContainer, FullVerifyRefusesAFlippedBlockByte) {
  const Directory dir;
  write(dir.path / "m.atxcov", make_blocks(4, 37), true);
  auto bytes = read_file(dir.path / "m.atxcov");
  const auto idx = index_of(bytes);
  bytes[array_offset(bytes, idx[1], cov::ArrayId::Specific) + 3] ^= 0x01;
  write_file(dir.path / "flipped.atxcov", bytes);
  const auto r = open(dir.path / "flipped.atxcov");
  ASSERT_TRUE(parse_error(r));
  EXPECT_NE(r.error().message().find("block 1 "), std::string::npos) << r.error().to_string();
}

TEST(RiskCovContainer, LazyVerifyRefusesAFlippedBlockByteOnFirstTouch) {
  const Directory dir;
  write(dir.path / "m.atxcov", make_blocks(4, 37), true);
  auto bytes = read_file(dir.path / "m.atxcov");
  const auto idx = index_of(bytes);
  bytes[array_offset(bytes, idx[2], cov::ArrayId::FactorCov) + 9] ^= 0x40;
  write_file(dir.path / "flipped.atxcov", bytes);
  const auto file = open(dir.path / "flipped.atxcov", er::VerifyMode::Lazy);
  ASSERT_TRUE(file) << file.error().to_string(); // lazy: nothing hashed yet
  EXPECT_TRUE(file->view(0));
  EXPECT_TRUE(file->view(1));
  EXPECT_TRUE(parse_error(file->view(2)));
  EXPECT_TRUE(parse_error(file->view(2))); // sticky
  EXPECT_TRUE(file->view(3));
  EXPECT_TRUE(parse_error(file->exact(file->index()[2].as_of_session_ns)));
}

TEST(RiskCovContainer, RefusesRootPinMismatch) {
  const Directory dir;
  const auto root = write(dir.path / "m.atxcov", make_blocks(3, 41), true);
  auto wrong = root;
  wrong[0] ^= 0xFF;
  const auto refused = open(dir.path / "m.atxcov", er::VerifyMode::Full, wrong);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), co::ErrorCode::InvalidArgument);
  EXPECT_TRUE(open(dir.path / "m.atxcov", er::VerifyMode::Full, root));
  EXPECT_EQ(er::cov_parse_hex(er::cov_hex(root)), root);
  EXPECT_FALSE(er::cov_parse_hex("ABC"));
}

TEST(RiskCovContainer, RefusesOverlappingOrMisalignedSections) {
  const Directory dir;
  write(dir.path / "m.atxcov", make_blocks(3, 43), true);
  const auto bytes = read_file(dir.path / "m.atxcov");
  const u64 table = get<u64>(bytes, 40);
  const auto meta = get<er::CovSectionEntry>(bytes, table);
  const auto names = get<er::CovSectionEntry>(bytes, table + 64);
  auto overlap = bytes;
  auto moved = names;
  moved.offset = meta.offset; // names over META
  put(overlap, table + 64, moved);
  rehash_root(overlap);
  write_file(dir.path / "overlap.atxcov", overlap);
  EXPECT_TRUE(parse_error(open(dir.path / "overlap.atxcov")));
  auto misaligned = bytes;
  moved = names;
  moved.offset += 8;
  put(misaligned, table + 64, moved);
  rehash_root(misaligned);
  write_file(dir.path / "misaligned.atxcov", misaligned);
  EXPECT_TRUE(parse_error(open(dir.path / "misaligned.atxcov")));
  auto outside = bytes;
  moved = names;
  moved.offset = bytes.size(); // past the trailer
  put(outside, table + 64, moved);
  rehash_root(outside);
  write_file(dir.path / "outside.atxcov", outside);
  EXPECT_TRUE(parse_error(open(dir.path / "outside.atxcov")));
}

TEST(RiskCovContainer, RefusesNonAscendingSessionsAndCutoffAfterAsOf) {
  const Directory dir;
  const auto blocks = make_blocks(3, 47);
  {
    auto w = er::CovContainerWriter::create((dir.path / "w.atxcov").string(), make_spec(true));
    ASSERT_TRUE(w);
    ASSERT_TRUE(w->append(blocks[1].input(true)));
    const auto same = w->append(blocks[1].input(true));
    ASSERT_FALSE(same);
    EXPECT_EQ(same.error().code(), co::ErrorCode::InvalidArgument);
    EXPECT_FALSE(w->append(blocks[0].input(true))); // earlier session
    auto late = blocks[2];
    late.cutoff = late.as_of + day_ns; // data after the stamp
    EXPECT_FALSE(w->append(late.input(true)));
    auto history = blocks[2];
    history.first = history.cutoff + 1;
    EXPECT_FALSE(w->append(history.input(true)));
    EXPECT_TRUE(w->append(blocks[2].input(true)));
    EXPECT_EQ(w->blocks(), 2U);
    ASSERT_TRUE(w->finalize());
    EXPECT_FALSE(w->finalize());
  }
  // A reader refuses an index whose sessions do not ascend or whose cutoff passes as_of.
  write(dir.path / "m.atxcov", blocks, true);
  const auto bytes = read_file(dir.path / "m.atxcov");
  const u64 table = get<u64>(bytes, 40);
  const auto index = get<er::CovSectionEntry>(bytes, table + 128);
  ASSERT_EQ(index.kind, 3U);
  auto swapped = bytes;
  auto e1 = get<er::CovDateIndexEntry>(bytes, index.offset + 64);
  e1.as_of_session_ns = blocks[0].as_of;
  e1.data_cutoff_ns = blocks[0].as_of;
  put(swapped, index.offset + 64, e1);
  rehash_root(swapped);
  write_file(dir.path / "swapped.atxcov", swapped);
  EXPECT_TRUE(parse_error(open(dir.path / "swapped.atxcov")));
  auto cutoff = bytes;
  auto e2 = get<er::CovDateIndexEntry>(bytes, index.offset + 128);
  e2.data_cutoff_ns = e2.as_of_session_ns + 1;
  put(cutoff, index.offset + 128, e2);
  rehash_root(cutoff);
  write_file(dir.path / "cutoff.atxcov", cutoff);
  EXPECT_TRUE(parse_error(open(dir.path / "cutoff.atxcov")));
}

// The writer's tail rebuilt with one extra section (kind 9) after BLOCK_HASHES: the table grows to
// six entries and the metadata sections are moved after it (64-aligned), then the root.
std::vector<u8> with_extra_section(const std::vector<u8>& bytes, u32 flags) {
  auto h = get<er::CovFileHeader>(bytes, 0);
  const u64 table = h.section_table_offset;
  std::vector<er::CovSectionEntry> entries;
  for (u32 s = 0; s < h.section_count; ++s)
    entries.push_back(get<er::CovSectionEntry>(bytes, table + 64ULL * s));
  er::CovSectionEntry extra{};
  extra.kind = 9; extra.flags = flags; extra.bytes = 16; extra.count = 16;
  extra.dtype = static_cast<u32>(cov::Dtype::U8); extra.align = 64;
  entries.push_back(extra);
  std::vector<u8> out(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(table));
  out.resize(table + 64 * entries.size(), 0);
  std::vector<std::vector<u8>> payloads;
  for (auto& e : entries) {
    if (e.kind == 5) { payloads.emplace_back(); continue; }
    payloads.emplace_back(e.kind == 9 ? std::vector<u8>(16, 0x5A)
                                      : std::vector<u8>(bytes.begin() + static_cast<std::ptrdiff_t>(e.offset),
                                                        bytes.begin() + static_cast<std::ptrdiff_t>(e.offset + e.bytes)));
    e.offset = cov::align_up(out.size(), 64);
    out.resize(e.offset, 0);
    out.insert(out.end(), payloads.back().begin(), payloads.back().end());
  }
  out.resize(cov::align_up(out.size(), 64) + 64, 0);
  for (usize s = 0; s < entries.size(); ++s) put(out, table + 64 * s, entries[s]);
  h.section_count = static_cast<u32>(entries.size());
  h.file_bytes = out.size();
  put(out, 0, h);
  er::CovTrailer t{};
  t.magic = cov::trailer_magic; t.file_bytes = out.size();
  put(out, out.size() - 64, t);
  rehash_root(out);
  return out;
}
TEST(RiskCovContainer, IgnoresUnknownOptionalSectionRefusesUnknownRequired) {
  const Directory dir;
  const auto blocks = make_blocks(3, 53);
  write(dir.path / "m.atxcov", blocks, true);
  const auto bytes = read_file(dir.path / "m.atxcov");
  write_file(dir.path / "optional.atxcov", with_extra_section(bytes, cov::section_in_root));
  const auto ok = open(dir.path / "optional.atxcov");
  ASSERT_TRUE(ok) << ok.error().to_string();
  const auto v = ok->view(2);
  ASSERT_TRUE(v);
  EXPECT_EQ(bits_of(v->specific), bits_of(blocks[2].specific));
  write_file(dir.path / "required.atxcov", with_extra_section(bytes, cov::section_required));
  const auto refused = open(dir.path / "required.atxcov");
  ASSERT_TRUE(parse_error(refused));
  EXPECT_NE(refused.error().message().find("unknown required"), std::string::npos);
  // Arrays follow the same rule: an unknown optional array is ignored by parse_cov_block, an
  // unknown required one refused.
  std::vector<u8> block;
  ASSERT_TRUE(er::encode_cov_block(make_spec(true), blocks[0].input(true), block));
  auto unknown = block;
  put<u32>(unknown, 48 + 24 * 10, 12U | cov::array_optional); // EIGEN_GAMMA's slot -> id 12
  const auto parsed = er::parse_cov_block(unknown, factors, groups, dense, true);
  ASSERT_TRUE(parsed) << parsed.error().to_string();
  EXPECT_TRUE(parsed->eigen_gamma.empty());
  put<u32>(unknown, 48 + 24 * 10, 12U);
  EXPECT_TRUE(parse_error(er::parse_cov_block(unknown, factors, groups, dense, true)));
}

TEST(RiskCovContainer, AtOrBeforeNeverReturnsAFutureCutoff) {
  const Directory dir;
  // Sessions on days 0, 2, 3, 7 (gaps); block 2 is lagged: cutoff one day before as_of.
  std::vector<Owned> blocks;
  for (const u32 day : {0U, 2U, 3U, 7U}) {
    auto b = make_block(day, 10, 59);
    if (day == 3) b.cutoff = b.as_of - day_ns;
    blocks.push_back(b);
  }
  write(dir.path / "m.atxcov", blocks, true);
  const auto file = open(dir.path / "m.atxcov");
  ASSERT_TRUE(file);
  for (i64 s = first_day - 2 * day_ns; s <= first_day + 9 * day_ns; s += day_ns / 2) {
    const auto got = file->at_or_before(s);
    // The latest block whose as_of (hence cutoff) is at or before s, by brute force.
    std::optional<usize> expect;
    for (usize t = 0; t < blocks.size(); ++t)
      if (blocks[t].as_of <= s && blocks[t].cutoff <= s) expect = t;
    if (!expect) {
      EXPECT_FALSE(got) << s;
      EXPECT_EQ(got.error().code(), co::ErrorCode::InvalidArgument);
      continue;
    }
    ASSERT_TRUE(got) << s;
    EXPECT_LE(got->cutoff, s);
    EXPECT_LE(got->as_of, s);
    EXPECT_EQ(got->as_of, blocks[*expect].as_of) << s;
    const auto exact = file->exact(s);
    const bool listed = std::any_of(blocks.begin(), blocks.end(), [&](const Owned& b) { return b.as_of == s; });
    EXPECT_EQ(static_cast<bool>(exact), listed) << s;
  }
}

TEST(RiskCovContainer, PrefixRunsShareBlockHashes) {
  const Directory dir;
  const auto blocks = make_blocks(7, 61);
  const std::vector<Owned> prefix(blocks.begin(), blocks.begin() + 4);
  const auto full_root = write(dir.path / "full.atxcov", blocks, true);
  const auto cut_root = write(dir.path / "cut.atxcov", prefix, true);
  EXPECT_NE(full_root, cut_root); // a different file (index, root)
  const auto full = open(dir.path / "full.atxcov");
  const auto cut = open(dir.path / "cut.atxcov");
  ASSERT_TRUE(full && cut);
  for (usize t = 0; t < prefix.size(); ++t) EXPECT_EQ(*full->block_sha256(t), *cut->block_sha256(t)) << t;
  const auto same = er::first_difference(*full, *cut, blocks[3].as_of);
  EXPECT_FALSE(same.differs);
  const auto beyond = er::first_difference(*full, *cut, blocks[5].as_of);
  ASSERT_TRUE(beyond.differs);
  EXPECT_EQ(beyond.index, 4U);
  EXPECT_EQ(beyond.session, blocks[4].as_of);
  EXPECT_EQ(beyond.what, "missing block");
  // A changed block 2 is the first difference.
  auto changed = blocks;
  changed[2].specific[0] *= 2.0;
  write(dir.path / "changed.atxcov", changed, true);
  const auto other = open(dir.path / "changed.atxcov");
  ASSERT_TRUE(other);
  const auto diff = er::first_difference(*full, *other, blocks.back().as_of);
  ASSERT_TRUE(diff.differs);
  EXPECT_EQ(diff.index, 2U);
  EXPECT_EQ(diff.what, "block");
  EXPECT_FALSE(er::first_difference(*full, *other, blocks[1].as_of).differs);
}

TEST(RiskCovContainer, FactorRiskViewIsZeroCopyOnPricedRows) {
  const Directory dir;
  const auto blocks = make_blocks(2, 67, 20); // block 0 complete, block 1 not
  write(dir.path / "f64.atxcov", blocks, false);
  write(dir.path / "f32.atxcov", blocks, true);
  const auto f64_file = open(dir.path / "f64.atxcov");
  const auto f32_file = open(dir.path / "f32.atxcov");
  ASSERT_TRUE(f64_file && f32_file);
  er::CovViewScratch scratch;
  const auto v = f64_file->view(0);
  ASSERT_TRUE(v);
  const auto m = er::factor_risk_view(*v, scratch);
  EXPECT_EQ(m.groups, groups);
  EXPECT_EQ(m.styles, dense);
  EXPECT_EQ(m.group.size(), v->priced);
  EXPECT_EQ(m.group.data(), v->group.data());
  EXPECT_EQ(m.specific.data(), v->specific.data());
  EXPECT_EQ(m.exposures.data(), v->exposures_f64.data());
  EXPECT_EQ(m.covariance.data(), v->factor_cov.data());
  EXPECT_TRUE(scratch.exposures.empty() && scratch.covariance.empty());
  // The kernel prices a book over the view as over the dense rows.
  std::vector<f64> w(v->priced);
  for (usize i = 0; i < w.size(); ++i) w[i] = (i % 2 ? 1.0 : -1.0) / static_cast<f64>(w.size());
  const auto variance = atx::engine::book::factor_variance(m, w);
  ASSERT_TRUE(variance) << variance.error().to_string();
  std::vector<f64> e(factors, 0.0);
  f64 specific = 0;
  for (usize i = 0; i < w.size(); ++i) {
    e[0] += w[i];
    e[v->group[i]] += w[i];
    for (u32 c = 0; c < dense; ++c) e[1 + groups + c] += w[i] * v->exposures_f64[i * dense + c];
    specific += v->specific[i] * w[i] * w[i];
  }
  f64 systematic = 0;
  for (u32 a = 0; a < factors; ++a)
    for (u32 b = 0; b < factors; ++b) systematic += e[a] * v->factor_cov[a * factors + b] * e[b];
  EXPECT_NEAR(*variance, systematic + specific, 1e-15);
  // f32: exposures widened into scratch; an incomplete covariance copied with NaN -> 0.
  const auto v32 = f32_file->view(1);
  ASSERT_TRUE(v32);
  const auto m32 = er::factor_risk_view(*v32, scratch);
  EXPECT_EQ(m32.exposures.data(), scratch.exposures.data());
  EXPECT_EQ(m32.covariance.data(), scratch.covariance.data());
  EXPECT_EQ(m32.group.data(), v32->group.data());
  for (usize i = 0; i < m32.exposures.size(); ++i)
    EXPECT_EQ(m32.exposures[i], static_cast<f64>(v32->exposures_f32[i]));
  for (usize c = 0; c < m32.covariance.size(); ++c)
    EXPECT_EQ(m32.covariance[c], std::isfinite(v32->factor_cov[c]) ? v32->factor_cov[c] : 0.0);
}

TEST(RiskCovContainer, WriterRefusesOutOfOrderAppendAndBadRowOrder) {
  const Directory dir;
  const auto path = (dir.path / "w.atxcov").string();
  auto w = er::CovContainerWriter::create(path, make_spec(true));
  ASSERT_TRUE(w);
  EXPECT_FALSE(er::CovContainerWriter::create(path, make_spec(true))); // .partial exists
  const auto good = make_block(1, 12, 71);
  auto unpriced_first = good; // the last priced row's specific turned NaN: an unpriced row in [0, P)
  unpriced_first.specific[good.priced - 1] = std::numeric_limits<f64>::quiet_NaN();
  EXPECT_FALSE(w->append(unpriced_first.input(true)));
  auto priced_late = good; // an unpriced row made priced after P
  priced_late.group[good.priced] = 1;
  priced_late.specific[good.priced] = 1e-4;
  EXPECT_FALSE(w->append(priced_late.input(true)));
  auto descending = good;
  std::swap(descending.role[0], descending.role[1]);
  EXPECT_FALSE(w->append(descending.input(true)));
  auto big_group = good;
  big_group.group[0] = groups + 1;
  EXPECT_FALSE(w->append(big_group.input(true)));
  auto short_cov = good;
  short_cov.cov.pop_back();
  EXPECT_FALSE(w->append(short_cov.input(true)));
  auto wrong_dtype = good;
  EXPECT_FALSE(w->append(wrong_dtype.input(false))); // f64 exposures into an f32 file
  auto flags = good;
  flags.flags |= 1U << 9U;
  EXPECT_FALSE(w->append(flags.input(true)));
  EXPECT_EQ(w->blocks(), 0U);
  ASSERT_TRUE(w->append(good.input(true)));
  EXPECT_FALSE(w->append(make_block(0, 12, 71).input(true))); // out of order
  EXPECT_EQ(w->blocks(), 1U);
  ASSERT_TRUE(w->finalize());
  EXPECT_TRUE(fs::exists(path));
  EXPECT_FALSE(fs::exists(path + ".partial"));
  EXPECT_FALSE(er::CovContainerWriter::create(path, make_spec(true))); // the final file exists
  auto empty = er::CovContainerWriter::create((dir.path / "empty.atxcov").string(), make_spec(true));
  ASSERT_TRUE(empty);
  EXPECT_FALSE(empty->finalize()); // no block
  auto names = make_spec(true);
  names.factor_names.pop_back();
  EXPECT_FALSE(er::CovContainerWriter::create((dir.path / "names.atxcov").string(), names));
}

TEST(RiskCovContainer, FullVerifyIsWorkerCountInvariant) {
  const Directory dir;
  write(dir.path / "m.atxcov", make_blocks(9, 73), true);
  for (const usize workers : {1U, 2U, 3U, 4U, 8U, 16U})
    EXPECT_TRUE(open(dir.path / "m.atxcov", er::VerifyMode::Full, std::nullopt, workers)) << workers;
  auto bytes = read_file(dir.path / "m.atxcov");
  const auto idx = index_of(bytes);
  bytes[array_offset(bytes, idx[7], cov::ArrayId::Ids)] ^= 0x10;
  bytes[array_offset(bytes, idx[3], cov::ArrayId::Group)] ^= 0x10;
  write_file(dir.path / "bad.atxcov", bytes);
  for (const usize workers : {1U, 2U, 3U, 4U, 8U, 16U}) {
    const auto r = open(dir.path / "bad.atxcov", er::VerifyMode::Full, std::nullopt, workers);
    ASSERT_TRUE(parse_error(r)) << workers;
    EXPECT_NE(r.error().message().find("block 3 "), std::string::npos) << workers << ' '
                                                                       << r.error().to_string();
  }
}
} // namespace
