#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/engine/combine/marginal_rank_ic.hpp"
#include "strategy_marginal_ic.hpp"

// platform v8 F-2: `atx-equity-strategy-ic marginal` end to end on a synthetic role, candidate
// cache and saved combined signal (contract K6). The statistical thresholds were checked for this
// seed against an independent numpy replica of the same world before the tests were fixed.
namespace {
using namespace atx;
using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace st = atx::impl::strategy;
namespace cb = atx::engine::combine;
constexpr usize D = 700, N = 80, score_begin = 383, rows = D - score_begin - 22;
constexpr i64 day = 86'400'000'000'000LL;
constexpr f64 missing = std::numeric_limits<f64>::quiet_NaN();

struct Directory {
  fs::path path;
  Directory() {
    static std::atomic<unsigned> sequence{};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned a = 0; a < 32; ++a) {
      auto candidate = fs::temp_directory_path() /
          ("atx-marginal-ic-" + std::to_string(stamp) + "-" + std::to_string(sequence.fetch_add(1)));
      if (fs::create_directory(candidate)) { path = std::move(candidate); break; }
    }
  }
  ~Directory() { if (!path.empty()) { std::error_code ec; fs::remove_all(path, ec); } }
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
};
template<class T> bool payload(const fs::path& dir, Json& files, const std::string& name, const std::vector<T>& data) {
  std::ofstream f(dir / name, std::ios::binary); const auto bytes = std::as_bytes(std::span(data));
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); f.close();
  if (!f) return false;
  auto sha = core::sha256_file((dir / name).string()); if (!sha) return false;
  files[name] = {{"bytes", bytes.size()}, {"sha256", *sha}}; return true;
}
bool json_file(const fs::path& path, const Json& j, std::string& sha) {
  const auto text = j.dump(2) + "\n";
  std::ofstream out(path, std::ios::binary); out << text; out.close();
  auto digest = core::sha256_hex(text); if (!out || !digest) return false; sha = *digest; return true;
}
Json read_json(const fs::path& path) { std::ifstream in(path); return Json::parse(in); }
struct Rng {
  std::uint64_t s;
  f64 uni() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (static_cast<f64>(s >> 11U) + 0.5) / 9007199254740992.0;
  }
  f64 gauss() {
    const f64 u1 = uni(); const f64 u2 = uni();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
};
std::vector<f64> draws(Rng& g, usize n) { std::vector<f64> v(n); for (auto& x : v) x = g.gauss(); return v; }

// The world: random-walk closes; y = the h 21 label close[t+22]/close[t+1]-1 (0 past maturity)
// and s_t its cross-sectional sd; pool_a = y + 3 s u predicts it (rank IC about .3), pool_b = w
// is noise. Members are every name from session 63. Ra, Rb: their centred ranks.
// Noise: book = Ra; candidate = Ra with random disjoint adjacent-rank swaps (rank-space noise
// independent of the label). Planted: book = .5 Ra + .5 Rb; candidate = 3 y - s u, orthogonal
// to pool_a and predictive.
enum class Mode { Noise, Planted };
struct World {
  std::vector<f64> close, a, w, ra, rb, book, cand;
  explicit World(Mode mode) {
    Rng g{7106U};
    const auto r = draws(g, D * N), u = draws(g, D * N);
    w = draws(g, D * N);
    close.assign(D * N, 0.0);
    for (usize i = 0; i < N; ++i) {
      f64 lp = 0.0;
      for (usize t = 0; t < D; ++t) { lp += 0.01 * r[t * N + i]; close[t * N + i] = 100.0 * std::exp(lp); }
    }
    std::vector<f64> y(D * N, 0.0), s(D, 1.0);
    for (usize t = 0; t + 22 < D; ++t) {
      f64 sum = 0.0;
      for (usize i = 0; i < N; ++i) { y[t * N + i] = close[(t + 22) * N + i] / close[(t + 1) * N + i] - 1.0; sum += y[t * N + i]; }
      const f64 mean = sum / static_cast<f64>(N);
      f64 ss = 0.0;
      for (usize i = 0; i < N; ++i) ss += (y[t * N + i] - mean) * (y[t * N + i] - mean);
      s[t] = std::sqrt(ss / static_cast<f64>(N));
    }
    a.resize(D * N); cand.assign(D * N, missing);
    for (usize t = 0; t < D; ++t) for (usize i = 0; i < N; ++i) a[t * N + i] = y[t * N + i] + 3.0 * s[t] * u[t * N + i];
    ra = ranks(a); rb = ranks(w);
    if (mode == Mode::Noise) {
      book = ra;
      std::vector<usize> order(N); std::vector<f64> values(N);
      for (usize t = 63; t < D; ++t) {
        std::iota(order.begin(), order.end(), usize{0});
        std::sort(order.begin(), order.end(), [&](usize x, usize z) {
          return ra[t * N + x] < ra[t * N + z] || (ra[t * N + x] == ra[t * N + z] && x < z);
        });
        for (usize k = 0; k < N; ++k) values[k] = ra[t * N + order[k]];
        const usize offset = g.uni() < 0.5 ? 0U : 1U;
        for (usize k = offset; k + 1 < N; k += 2) if (g.uni() < 0.5) std::swap(values[k], values[k + 1]);
        for (usize k = 0; k < N; ++k) cand[t * N + order[k]] = values[k];
      }
    } else {
      book.resize(D * N);
      for (usize c = 0; c < D * N; ++c) { book[c] = 0.5 * ra[c] + 0.5 * rb[c]; cand[c] = 3.0 * y[c] - s[c / N] * u[c]; }
    }
  }
  static std::vector<f64> ranks(const std::vector<f64>& values) {
    std::vector<f64> out(D * N); std::vector<u8> member(N); std::vector<std::pair<f64, usize>> sorted;
    for (usize t = 0; t < D; ++t) {
      std::fill(member.begin(), member.end(), static_cast<u8>(t >= 63));
      const auto status = cb::centred_tied_ranks(std::span<const f64>(values.data() + t * N, N), member,
                                                 std::span<f64>(out.data() + t * N, N), sorted);
      EXPECT_TRUE(status.has_value());
    }
    return out;
  }
};

struct Fixture {
  Directory dir;
  st::MarginalIcConfig cfg;
  std::string role_sha, library_sha, weights_sha, pool_sha;
  fs::path cache_entry_dir;
  bool ok{};
  explicit Fixture(Mode mode) {
    if (dir.path.empty()) return;
    const World world(mode);
    ok = write_role(world) && write_library() && write_weights() && write_cache(world) && write_pool(world);
    cfg.candidate_cache_directory = (dir.path / "cache").string();
    cfg.library_path = (dir.path / "library.json").string();
    cfg.pool_path = (dir.path / "pool" / "train_combined.json").string();
    cfg.role_manifest = (dir.path / "role" / "manifest.json").string();
    cfg.output_directory = (dir.path / "out").string();
    cfg.min_names = 20;
  }
  std::vector<i64> sessions() const {
    std::vector<i64> out(D);
    for (usize t = 0; t < D; ++t) out[t] = (17683 + static_cast<i64>(t)) * day;
    return out;
  }
  static std::vector<u64> ids() { std::vector<u64> out(N); for (usize i = 0; i < N; ++i) out[i] = 1000U + i; return out; }
  bool write_role(const World& world) {
    const auto role = dir.path / "role";
    if (!fs::create_directory(role)) return false;
    std::vector<u8> member(D * N, 0), present(D * N, 1);
    for (usize t = 63; t < D; ++t) std::fill_n(member.begin() + static_cast<std::ptrdiff_t>(t * N), N, u8{1});
    const std::vector<f64> volume(D * N, 1e6);
    const auto keys = sessions();
    Json files;
    if (!payload(role, files, "sessions.i64", keys) || !payload(role, files, "ids.u64", ids()) ||
        !payload(role, files, "member.u8", member) || !payload(role, files, "present.u8", present) ||
        !payload(role, files, "close.f64", world.close) || !payload(role, files, "raw_close.f64", world.close) ||
        !payload(role, files, "volume.f64", volume)) return false;
    const Json membership{{"rule", "research-prior63-usd-adv-topn-v1"}, {"top_n", N}, {"lookback_sessions", 63},
        {"lag_sessions", 1}, {"min_raw_price_exclusive", 5}, {"min_adv_exclusive", 5000000},
        {"ties", "securityID-ascending"}, {"missing", "complete-prior-calendar-window-required"},
        {"common_stock_verified", false}};
    return json_file(role / "manifest.json", {{"schema", "atx.recent-research-role/v1"}, {"status", "complete"},
        {"instrument_namespace", "spiderrock.securityID"}, {"dates", D}, {"instruments", N},
        {"score_begin", score_begin}, {"score_end", D}, {"score_start_ns", keys[score_begin]},
        {"score_end_ns", keys.back() + day}, {"source_sha256", std::string(64, 'a')},
        {"membership_recipe", membership.dump()}, {"clock_recipe", "modeled-session+22h-mark+23h-decision-v1"},
        {"close_basis", "f64(raw-f32-close)*f64-cumulReturnFactor"}, {"volume_basis", "raw-share-volume"},
        {"common_stock_verified", false}, {"historical_vintage_verified", false},
        {"declared_output_bytes", D * N * 26 + D * 8 + N * 8}, {"files", files}}, role_sha);
  }
  static Json row(const char* id, const char* family, const char* dsl) {
    return {{"id", id}, {"family", family}, {"theme", family}, {"dsl", dsl}, {"sign_policy", "train-rank-ic21"},
            {"horizons", {5, 21, 63}}, {"prior_sign", 1}};
  }
  static std::string dsl_of(const std::string& id) {
    return id == "pool_a" ? "close" : (id == "pool_b" ? "volume" : "rank(close)");
  }
  bool write_library() {
    return json_file(dir.path / "library.json", {{"schema", "atx.dsl-ic-library/v1"}, {"id", "synthetic-marginal"},
        {"fields", Json::array({{{"name", "close"}}, {{"name", "raw_close"}}, {{"name", "volume"}}})},
        {"families", Json::array({{{"id", "alpha"}}, {{"id", "beta"}}, {{"id", "gamma"}}})},
        {"candidates", Json::array({row("pool_a", "alpha", "close"), row("pool_b", "beta", "volume"),
                                    row("cand", "gamma", "rank(close)")})}}, library_sha);
  }
  bool write_weights() {
    return json_file(dir.path / "weights.json", {{"schema", "atx.dsl-composition-weights/v1"},
        {"library_sha256", library_sha}, {"train_manifest_sha256", role_sha},
        {"weights", {{"pool_a", 0.5}, {"pool_b", 0.5}, {"cand", 0.0}}}, {"signs", {{"pool_a", 1}, {"pool_b", 1}}}},
        weights_sha);
  }
  bool write_entry(const std::string& id, const std::vector<f64>& signal) {
    auto dsl_sha = core::sha256_hex(dsl_of(id)); if (!dsl_sha) return false;
    const auto stem = id + "." + dsl_sha->substr(0, 16);
    Json files;
    if (!payload(cache_entry_dir, files, stem + ".f64", signal)) return false;
    std::string unused;
    return json_file(cache_entry_dir / (stem + ".json"), {{"schema", "atx.dsl-candidate-signal/v2"},
        {"candidate_id", id}, {"dsl_sha256", *dsl_sha}, {"role_manifest_sha256", role_sha}, {"dates", D},
        {"instruments", N}, {"bytes", D * N * sizeof(f64)},
        {"layout", "date-major-little-endian-f64;non-finite-stored-as-quiet-NaN"}, {"payload", stem + ".f64"},
        {"payload_sha256", files[stem + ".f64"]["sha256"]}, {"field_payload_sha256", Json::object()},
        {"vm_identity", "synthetic"}}, unused);
  }
  bool write_cache(const World& world) {
    cache_entry_dir = dir.path / "cache" / role_sha;
    std::error_code ec; fs::create_directories(cache_entry_dir, ec);
    return !ec && write_entry("pool_a", world.a) && write_entry("pool_b", world.w) && write_entry("cand", world.cand);
  }
  bool write_pool(const World& world) {
    const auto pool = dir.path / "pool";
    if (!fs::create_directory(pool)) return false;
    std::vector<u8> member(D * N, 0);
    for (usize c = 63 * N; c < D * N; ++c) member[c] = 1;
    Json files;
    if (!payload(pool, files, "train_combined.f64", world.book) || !payload(pool, files, "train_combined_member.u8", member) ||
        !payload(pool, files, "train_combined_sessions.i64", sessions()) ||
        !payload(pool, files, "train_combined_ids.u64", ids())) return false;
    return json_file(pool / "train_combined.json", {{"schema", "atx.dsl-combined-signal/v1"}, {"status", "complete"},
        {"role", "train"}, {"layout", "date-major-little-endian"}, {"dates", D}, {"instruments", N},
        {"score_begin", score_begin}, {"score_end", D}, {"role_manifest_sha256", role_sha},
        {"library_sha256", library_sha}, {"composition_weights_sha256", weights_sha}, {"files", files}}, pool_sha);
  }
};
const Json& candidate(const Json& out, const std::string& id) {
  for (const auto& r : out.at("candidates")) if (r.at("id") == id) return r;
  throw std::runtime_error("no candidate row " + id);
}
f64 real(const Json& row, const char* key) { return row.at(key).is_number() ? row.at(key).get<f64>() : missing; }

TEST(MarginalIc, PoolPlusNoiseHasZeroMarginalWithin2Se) {
  Fixture f(Mode::Noise); ASSERT_TRUE(f.ok);
  std::ostringstream progress;
  const auto status = st::run_marginal_ic(f.cfg, progress);
  ASSERT_TRUE(status) << status.error().to_string();
  const auto out = read_json(fs::path(f.cfg.output_directory) / "marginal_ic.json");
  EXPECT_EQ(out.at("schema"), "atx.marginal-ic/v1");
  EXPECT_EQ(out.at("window").at("rows"), rows);
  ASSERT_EQ(out.at("candidates").size(), 3U);
  for (const auto& r : out.at("candidates"))
    for (const char* key : {"id", "ic21", "ic21_hac_t", "marginal_ic21", "marginal_hac_t", "max_abs_rho", "max_rho_member"})
      EXPECT_TRUE(r.contains(key)) << key;
  const auto& c = candidate(out, "cand");
  EXPECT_EQ(c.at("dates"), rows); EXPECT_EQ(c.at("marginal_dates"), rows);
  EXPECT_GT(real(c, "ic21_hac_t"), 5.0);                 // it predicts the label ...
  EXPECT_LT(std::abs(real(c, "marginal_hac_t")), 2.0);   // ... with nothing the book lacks
  EXPECT_EQ(c.at("max_rho_member"), "pool_a"); EXPECT_GT(real(c, "max_abs_rho"), 0.99);
  EXPECT_FALSE(c.at("book_member").get<bool>());         // no --themes: no member flags
  // pool_a's rank IS the book: its residual is spanned on every date, its marginal exactly 0.
  const auto& a = candidate(out, "pool_a");
  EXPECT_EQ(a.at("spanned_dates"), rows); EXPECT_EQ(real(a, "marginal_ic21"), 0.0);
  EXPECT_TRUE(a.at("marginal_hac_t").is_null());
  EXPECT_EQ(out.at("inputs").at("pool").at("sha256"), f.pool_sha);
  EXPECT_TRUE(out.at("inputs").at("themes").is_null());
}

TEST(MarginalIc, PlantedOrthogonalRecovered) {
  Fixture f(Mode::Planted); ASSERT_TRUE(f.ok);
  f.cfg.themes_path = (f.dir.path / "weights.json").string();
  f.cfg.pool_sha256 = f.pool_sha; f.cfg.library_sha256 = f.library_sha;
  std::ostringstream progress;
  const auto status = st::run_marginal_ic(f.cfg, progress);
  ASSERT_TRUE(status) << status.error().to_string();
  const auto out = read_json(fs::path(f.cfg.output_directory) / "marginal_ic.json");
  // Book composite plus the alpha and beta theme composites, which sum to it exactly: the
  // rank-deficient design still projects.
  ASSERT_EQ(out.at("method").at("regressors").size(), 3U);
  Json members = Json::object();
  members["alpha"] = Json::array({"pool_a"}); members["beta"] = Json::array({"pool_b"});
  EXPECT_EQ(out.at("inputs").at("themes").at("members"), members);
  const auto& c = candidate(out, "cand");
  EXPECT_GT(real(c, "marginal_ic21"), 0.8);
  EXPECT_GT(real(c, "marginal_hac_t"), 10.0);
  EXPECT_LT(std::abs(real(c, "marginal_ic21") - real(c, "ic21")), 0.1);
  EXPECT_LT(real(c, "max_abs_rho"), 0.25);
  EXPECT_FALSE(c.at("book_member").get<bool>());
  const auto& a = candidate(out, "pool_a");
  EXPECT_TRUE(a.at("book_member").get<bool>());
  EXPECT_EQ(a.at("sign"), 1); EXPECT_EQ(a.at("sign_source"), "pinned-weights");
  EXPECT_EQ(c.at("sign_source"), "library-prior-sign");
}

TEST(MarginalIc, StreamsByDateUnder600MiB) {
  constexpr u64 limit = 600ULL << 20;
  // v7.1 TRAIN role: 1,155 sessions x 5,627 names, 734 mature rows, 48 candidates, 11 regressors.
  const auto v71 = st::marginal_ic_working_bytes(1155, 5627, 734, 48, 11);
  ASSERT_TRUE(v71); EXPECT_LT(*v71, limit);
  // A four-year role with a larger universe and 64 candidates still fits.
  const auto four_year = st::marginal_ic_working_bytes(1510, 6000, 1105, 64, 11);
  ASSERT_TRUE(four_year); EXPECT_LT(*four_year, limit);
  // One more candidate costs rows, not a panel: under 1% of a single dates x names payload.
  const auto more = st::marginal_ic_working_bytes(1155, 5627, 734, 49, 11);
  ASSERT_TRUE(more); EXPECT_LT(*more - *v71, 1155ULL * 5627ULL * 8ULL / 100ULL);
  EXPECT_FALSE(st::marginal_ic_working_bytes(1155, 5627, 734, 48, 12));
  // Admission runs before any payload: a 32 MiB budget refuses and writes nothing.
  Fixture f(Mode::Noise); ASSERT_TRUE(f.ok);
  std::ostringstream progress;
  f.cfg.max_working_bytes = 32ULL << 20;
  const auto refused = st::run_marginal_ic(f.cfg, progress);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), core::ErrorCode::Unavailable);
  EXPECT_FALSE(fs::exists(f.cfg.output_directory));
  f.cfg.max_working_bytes = limit;
  const auto status = st::run_marginal_ic(f.cfg, progress);
  ASSERT_TRUE(status) << status.error().to_string();
  const auto out = read_json(fs::path(f.cfg.output_directory) / "marginal_ic.json");
  const auto expected = st::marginal_ic_working_bytes(D, N, rows, 3, 1);
  ASSERT_TRUE(expected); EXPECT_EQ(out.at("working_bytes"), *expected);
}

TEST(MarginalIc, RefusesInputsNotBoundToThePool) {
  Fixture f(Mode::Noise); ASSERT_TRUE(f.ok);
  std::ostringstream progress;
  auto cfg = f.cfg;
  cfg.pool_sha256 = std::string(64, '0');
  EXPECT_FALSE(st::run_marginal_ic(cfg, progress));
  // A themes file other than the pool's pinned weights.
  cfg = f.cfg; std::string other;
  ASSERT_TRUE(json_file(f.dir.path / "other_weights.json", {{"schema", "atx.dsl-composition-weights/v1"},
      {"library_sha256", f.library_sha}, {"weights", {{"pool_a", 1.0}}}, {"signs", {{"pool_a", 1}}}}, other));
  cfg.themes_path = (f.dir.path / "other_weights.json").string();
  EXPECT_FALSE(st::run_marginal_ic(cfg, progress));
  // A candidate without a cache entry.
  cfg = f.cfg;
  auto dsl_sha = core::sha256_hex(std::string("rank(close)")); ASSERT_TRUE(dsl_sha);
  fs::remove(f.cache_entry_dir / ("cand." + dsl_sha->substr(0, 16) + ".json"));
  const auto missing = st::run_marginal_ic(cfg, progress);
  ASSERT_FALSE(missing); EXPECT_EQ(missing.error().code(), core::ErrorCode::NotFound);
  EXPECT_FALSE(fs::exists(f.cfg.output_directory));
}
} // namespace
