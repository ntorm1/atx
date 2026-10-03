// atx::engine::risk -- the atx.cov-container 1.0 load path (P9 lane COV, cov-design 4.12): open
// (Lazy: header, table, root over the metadata, index; Full: every block hashed on the
// deterministic pool), first touch (lazy verify + view of one block), a verified view, and the
// writer (excluding estimation), on synthetic containers of K 62 (G 50, S 11), D 1,000 sessions,
// N 3,000 and 5,627 rows, f32 and f64 exposures. BM_LegacyStore* is an EMULATION of
// spo::RiskStore::open / read on the same synthetic data (whole-file SHA-256 of the four legacy
// payloads; four ifstream open + seek + read per row), reported for the ratio only. Every
// benchmark's label names the SHA-256 backend in use. One synthetic container (and one legacy
// set) lives in the temp directory at a time and is removed when the next is built or at exit.
//
// Root runs it on the `rel` preset with ATX_BUILD_BENCH=ON:
//   atx-engine-bench --benchmark_filter=CovContainer --benchmark_repetitions=5

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/sha256.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/risk/cov_container.hpp"

namespace {
namespace er = atx::engine::risk;
namespace fs = std::filesystem;
using atx::f32;
using atx::f64;
using atx::i64;
using atx::u32;
using atx::u64;
using atx::u8;
using atx::usize;

constexpr u32 groups = 50, dense = 11, factors = 1 + groups + dense;
constexpr usize dates = 1000;
constexpr i64 day_ns = 86'400'000'000'000LL;
constexpr u64 max_bytes = 64ULL << 30;

std::string backend_label() {
  return std::string("sha256 ") +
         std::string(atx::core::sha256_backend_name(atx::core::sha256_default_backend()));
}

// Deterministic synthetic rows: 90% priced (group 1..G, specific > 0), the rest unpriced.
struct Synthetic {
  usize rows{};
  bool f32_exposures{};
  std::vector<u64> ids;
  std::vector<u32> role, group;
  std::vector<f32> x32;
  std::vector<f64> x64, specific, cov;
  u32 priced{};
  Synthetic(usize n, bool narrow) : rows(n), f32_exposures(narrow) {
    priced = static_cast<u32>(n * 9 / 10);
    u64 state = 0x9E3779B97F4A7C15ULL ^ n;
    const auto next = [&state] {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      return static_cast<f64>(state >> 11U) * 0x1.0p-53 - 0.5;
    };
    for (usize r = 0; r < n; ++r) {
      ids.push_back(1000 + r);
      role.push_back(static_cast<u32>(r));
      const bool p = r < priced;
      group.push_back(p ? static_cast<u32>(1 + r % groups) : 0U);
      specific.push_back(p ? 1e-4 * (1.0 + next()) : std::numeric_limits<f64>::quiet_NaN());
      for (u32 s = 0; s < dense; ++s) {
        const f64 z = 3.0 * next();
        x32.push_back(static_cast<f32>(z));
        x64.push_back(z);
      }
    }
    cov.assign(static_cast<usize>(factors) * factors, 0.0);
    for (u32 k = 0; k < factors; ++k) cov[k * factors + k] = 1e-5 * (1.0 + 0.01 * k);
  }
  [[nodiscard]] er::CovBlockInput block(usize t) const {
    er::CovBlockInput b;
    b.as_of_session_ns = b.data_cutoff_ns = b.history_first_ns =
        1'577'836'800'000'000'000LL + static_cast<i64>(t) * day_ns;
    b.role_date_index = static_cast<u32>(t);
    b.flags = er::cov::date_forecast;
    b.priced_rows = priced;
    b.ids = ids; b.role_index = role; b.group = group; b.specific = specific; b.factor_cov = cov;
    if (f32_exposures) b.exposures_f32 = x32; else b.exposures_f64 = x64;
    return b;
  }
};
er::CovContainerSpec spec_of(bool narrow) {
  er::CovContainerSpec spec;
  spec.groups = groups;
  spec.dense = dense;
  spec.f32_exposures = narrow;
  spec.recipe_json = "{\"model\":\"bench\"}";
  spec.inputs_json = "{}";
  spec.meta_json = "{\"format\":\"atx.cov-container/1.0\"}";
  for (u32 k = 0; k < factors; ++k) spec.factor_names.push_back("f" + std::to_string(k));
  return spec;
}
void remove_file(const fs::path& p) {
  std::error_code ec;
  fs::remove(p, ec);
  fs::remove(fs::path(p.string() + ".partial"), ec);
}
// Writes the D-block container of (n, narrow) at `path`; false on any error.
bool write_container(const fs::path& path, const Synthetic& s, bool narrow) {
  remove_file(path);
  auto w = er::CovContainerWriter::create(path.string(), spec_of(narrow));
  if (!w) return false;
  for (usize t = 0; t < dates; ++t)
    if (!w->append(s.block(t))) return false;
  return static_cast<bool>(w->finalize());
}

// The one container on disk (rebuilt when the arguments change), removed at exit.
struct Fixture {
  usize n{};
  bool narrow{};
  fs::path path, legacy;
  std::optional<Synthetic> synthetic;
  ~Fixture() { clear(); }
  void clear() {
    if (!path.empty()) remove_file(path);
    if (!legacy.empty()) {
      std::error_code ec;
      fs::remove_all(legacy, ec);
    }
    path.clear();
    legacy.clear();
  }
  bool ensure(usize rows, bool f32_rows) {
    if (synthetic && n == rows && narrow == f32_rows && !path.empty()) return true;
    clear();
    n = rows; narrow = f32_rows;
    synthetic.emplace(rows, f32_rows);
    path = fs::temp_directory_path() /
           ("atx_cov_bench_" + std::to_string(rows) + (f32_rows ? "_f32" : "_f64") + ".atxcov");
    return write_container(path, *synthetic, f32_rows);
  }
  // The legacy per-session files of the same data (dates x ...), written on first use.
  bool ensure_legacy() {
    if (!legacy.empty()) return true;
    legacy = fs::temp_directory_path() / ("atx_cov_bench_legacy_" + std::to_string(n));
    std::error_code ec;
    fs::create_directories(legacy, ec);
    const auto& s = *synthetic;
    std::ofstream cov(legacy / "factor_covariance.f64", std::ios::binary);
    std::ofstream spec(legacy / "specific_variance.f64", std::ios::binary);
    std::ofstream styles(legacy / "style_exposures.f32", std::ios::binary);
    std::ofstream slots(legacy / "industry_slot.u8", std::ios::binary);
    std::vector<u8> slot(n);
    for (usize i = 0; i < n; ++i) slot[i] = s.group[i] ? static_cast<u8>(s.group[i] - 1) : u8{255};
    const auto put = [](std::ofstream& f, const auto& v) {
      // SAFETY: char writes the object representation of a contiguous arithmetic vector.
      f.write(reinterpret_cast<const char*>(v.data()),
              static_cast<std::streamsize>(v.size() * sizeof(v[0])));
    };
    for (usize t = 0; t < dates; ++t) {
      put(cov, s.cov); put(spec, s.specific); put(styles, s.x32); put(slots, slot);
    }
    return static_cast<bool>(cov) && static_cast<bool>(spec) && static_cast<bool>(styles) &&
           static_cast<bool>(slots);
  }
};
Fixture& fixture() {
  static Fixture f;
  return f;
}
bool prepare(benchmark::State& state) {
  if (fixture().ensure(static_cast<usize>(state.range(0)), state.range(1) != 0)) return true;
  state.SkipWithError("synthetic container write failed");
  return false;
}

void BM_CovContainerWrite(benchmark::State& state) {
  const auto n = static_cast<usize>(state.range(0));
  const bool narrow = state.range(1) != 0;
  const Synthetic s(n, narrow);
  const fs::path path = fs::temp_directory_path() / "atx_cov_bench_write.atxcov";
  for (auto _ : state)
    if (!write_container(path, s, narrow)) state.SkipWithError("write failed");
  std::error_code ec;
  const auto bytes = fs::file_size(path, ec);
  remove_file(path);
  state.SetBytesProcessed(static_cast<i64>(state.iterations()) * static_cast<i64>(bytes));
  state.SetLabel(backend_label());
}
void BM_CovContainerOpenLazy(benchmark::State& state) {
  if (!prepare(state)) return;
  for (auto _ : state) {
    auto f = er::CovModelFile::open(fixture().path.string(), std::nullopt, er::VerifyMode::Lazy,
                                    max_bytes);
    benchmark::DoNotOptimize(f);
  }
  state.SetLabel(backend_label());
}
void BM_CovContainerOpenFull(benchmark::State& state) {
  if (!prepare(state)) return;
  const auto workers = static_cast<usize>(state.range(2));
  std::error_code ec;
  const auto bytes = fs::file_size(fixture().path, ec);
  for (auto _ : state) {
    auto f = er::CovModelFile::open(fixture().path.string(), std::nullopt, er::VerifyMode::Full,
                                    max_bytes, workers);
    if (!f) state.SkipWithError("open failed");
    benchmark::DoNotOptimize(f);
  }
  state.SetBytesProcessed(static_cast<i64>(state.iterations()) * static_cast<i64>(bytes));
  state.SetLabel(backend_label() + (workers == 1 ? ", 1 worker" : ", default workers"));
}
void BM_CovContainerFirstTouch(benchmark::State& state) {
  if (!prepare(state)) return;
  usize t = 0;
  for (auto _ : state) {
    state.PauseTiming();
    auto f = er::CovModelFile::open(fixture().path.string(), std::nullopt, er::VerifyMode::Lazy,
                                    max_bytes);
    state.ResumeTiming();
    if (!f) { state.SkipWithError("open failed"); break; }
    auto v = f->view(t);
    benchmark::DoNotOptimize(v);
    t = (t + 397) % dates;
  }
  state.SetLabel(backend_label());
}
void BM_CovContainerView(benchmark::State& state) {
  if (!prepare(state)) return;
  auto f = er::CovModelFile::open(fixture().path.string(), std::nullopt, er::VerifyMode::Full,
                                  max_bytes);
  if (!f) { state.SkipWithError("open failed"); return; }
  usize t = 0;
  for (auto _ : state) {
    auto v = f->view(t);
    benchmark::DoNotOptimize(v);
    t = (t + 1) % dates;
  }
  state.SetLabel(backend_label());
}
// EMULATION of spo::RiskStore::open: whole-file SHA-256 of the four legacy payloads.
void BM_CovContainerLegacyStoreOpen(benchmark::State& state) {
  if (!prepare(state) || !fixture().ensure_legacy()) return;
  const auto& dir = fixture().legacy;
  for (auto _ : state)
    for (const char* name : {"factor_covariance.f64", "specific_variance.f64",
                             "style_exposures.f32", "industry_slot.u8"}) {
      auto digest = atx::core::sha256_file((dir / name).string());
      benchmark::DoNotOptimize(digest);
    }
  state.SetLabel(backend_label() + ", emulation");
}
// EMULATION of spo::RiskStore::read: four ifstream open + seek + read of one row.
void BM_CovContainerLegacyStoreRead(benchmark::State& state) {
  if (!prepare(state) || !fixture().ensure_legacy()) return;
  const auto& dir = fixture().legacy;
  const usize n = fixture().n;
  std::vector<f64> cov(static_cast<usize>(factors) * factors), spec(n);
  std::vector<f32> styles(n * dense);
  std::vector<u8> slots(n);
  usize t = 0;
  const auto read = [&](const char* name, u64 offset, void* out, u64 bytes) {
    std::ifstream f(dir / name, std::ios::binary);
    f.seekg(static_cast<std::streamoff>(offset));
    // SAFETY: char reads into the caller's contiguous arithmetic buffer of `bytes` bytes.
    f.read(static_cast<char*>(out), static_cast<std::streamsize>(bytes));
  };
  for (auto _ : state) {
    read("factor_covariance.f64", t * cov.size() * 8, cov.data(), cov.size() * 8);
    read("specific_variance.f64", t * n * 8, spec.data(), n * 8);
    read("style_exposures.f32", t * n * dense * 4, styles.data(), n * dense * 4);
    read("industry_slot.u8", t * n, slots.data(), n);
    benchmark::DoNotOptimize(spec.data());
    t = (t + 1) % dates;
  }
  state.SetLabel(backend_label() + ", emulation");
}

} // namespace

// Args: rows N, f32 exposures (1) or f64 (0)[, Full-open workers: 0 = default pool, 1 = one].
#define ATX_COV_SHAPES Args({3000, 1})->Args({5627, 1})->Args({3000, 0})->Args({5627, 0})
BENCHMARK(BM_CovContainerWrite)->ATX_COV_SHAPES->Unit(benchmark::kMillisecond)->Iterations(2);
BENCHMARK(BM_CovContainerOpenLazy)->ATX_COV_SHAPES->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_CovContainerOpenFull)
    ->Args({3000, 1, 0})->Args({3000, 1, 1})->Args({5627, 1, 0})->Args({5627, 1, 1})
    ->Args({3000, 0, 0})->Args({3000, 0, 1})->Args({5627, 0, 0})->Args({5627, 0, 1})
    ->Unit(benchmark::kMillisecond);
BENCHMARK(BM_CovContainerFirstTouch)->ATX_COV_SHAPES->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_CovContainerView)->ATX_COV_SHAPES->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_CovContainerLegacyStoreOpen)->ATX_COV_SHAPES->Unit(benchmark::kMillisecond);
BENCHMARK(BM_CovContainerLegacyStoreRead)->ATX_COV_SHAPES->Unit(benchmark::kMicrosecond);
#undef ATX_COV_SHAPES
