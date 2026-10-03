#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
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
#include "atx/engine/build_flavor.hpp"
#include "atx/engine/combine/marginal_rank_ic.hpp"
#include "atx/engine/factory/ic_screen.hpp"
#include "strategy_ic_runner.hpp"
#include "strategy_marginal_ic.hpp"
#include "strategy_marginal_pair_cache.hpp"

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

// ew-theme-std-v1 weights (review B-2): the library adds pool_c (Rc, the ranks of fresh noise)
// and probe = .25 Ra + .25 Rc; a v2 weights file with a theme_standardise block groups
// {pool_a, pool_c} as t_one and {pool_b} as t_two, although the library rows name the themes
// alpha, delta and beta. With rerank the book's t_one term is the re-rank of .25 Ra + .25 Rc,
// which is exactly probe's rank; plain (rerank false) is the rule's identity switch.
enum class Block { none, rerank, plain };
struct Entry { std::string id, family, dsl; std::vector<f64> signal; };

// P9 S1 options of the fixture (defaults: the v8 fixture, byte for byte):
//   holes  the pool's member mask drops about one name in seven per scored date (the role's
//          members stay every name), so compacted rows are shorter than the role's rows;
//   extra  noise members m0.. in their own families f0.. (one theme each), weighted 0.01 with
//          sign +1 in the Block::none weights file: with pool_a and pool_b, 2 + extra themes.
struct Fixture {
  Directory dir;
  st::MarginalIcConfig cfg;
  std::string role_sha, library_sha, weights_sha, pool_sha;
  fs::path cache_entry_dir;
  std::string entry_identity; // the sidecars' vm_identity: this build's (see write_cache)
  Block block{Block::none};
  bool holes{false};
  usize extra{0};
  std::vector<Entry> entries; // library order
  bool ok{};
  explicit Fixture(Mode mode, Block theme_block = Block::none, bool with_holes = false,
                   usize extra_members = 0)
      : block(theme_block), holes(with_holes), extra(extra_members) {
    if (dir.path.empty()) return;
    const World world(mode);
    entries = {{"pool_a", "alpha", "close", world.a}, {"pool_b", "beta", "volume", world.w},
               {"cand", "gamma", "rank(close)", world.cand}};
    if (block != Block::none) {
      Rng g{8117U};
      auto c = draws(g, D * N);
      const auto rc = World::ranks(c);
      std::vector<f64> probe(D * N);
      for (usize x = 0; x < D * N; ++x) probe[x] = 0.25 * world.ra[x] + 0.25 * rc[x];
      entries.push_back({"pool_c", "delta", "rank(volume)", std::move(c)});
      entries.push_back({"probe", "epsilon", "close + volume", std::move(probe)});
    }
    Rng noise{9127U};
    for (usize k = 0; k < extra; ++k)
      entries.push_back({"m" + std::to_string(k), "f" + std::to_string(k),
                         "ts_rank(close, " + std::to_string(k + 2U) + ")", draws(noise, D * N)});
    ok = write_role(world) && write_library() && write_weights() && write_cache() && write_pool(world);
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
  static Json row(const Entry& e) {
    return {{"id", e.id}, {"family", e.family}, {"theme", e.family}, {"dsl", e.dsl}, {"sign_policy", "train-rank-ic21"},
            {"horizons", {5, 21, 63}}, {"prior_sign", 1}};
  }
  bool write_library() {
    Json families = Json::array(), candidates = Json::array();
    for (const auto& e : entries) { families.push_back(Json{{"id", e.family}}); candidates.push_back(row(e)); }
    return json_file(dir.path / "library.json", {{"schema", "atx.dsl-ic-library/v1"}, {"id", "synthetic-marginal"},
        {"fields", Json::array({{{"name", "close"}}, {{"name", "raw_close"}}, {{"name", "volume"}}})},
        {"families", std::move(families)}, {"candidates", std::move(candidates)}}, library_sha);
  }
  bool write_weights() {
    if (block == Block::none) {
      Json weights{{"pool_a", 0.5}, {"pool_b", 0.5}, {"cand", 0.0}};
      Json signs{{"pool_a", 1}, {"pool_b", 1}};
      for (usize k = 0; k < extra; ++k) {
        weights["m" + std::to_string(k)] = 0.01;
        signs["m" + std::to_string(k)] = 1;
      }
      return json_file(dir.path / "weights.json", {{"schema", "atx.dsl-composition-weights/v1"},
          {"library_sha256", library_sha}, {"train_manifest_sha256", role_sha},
          {"weights", std::move(weights)}, {"signs", std::move(signs)}}, weights_sha);
    }
    const Json themes{{"pool_a", "t_one"}, {"pool_c", "t_one"}, {"pool_b", "t_two"}};
    return json_file(dir.path / "weights.json", {{"schema", "atx.dsl-composition-weights/v2"},
        {"library_sha256", library_sha}, {"train_manifest_sha256", role_sha},
        {"weights", {{"pool_a", 0.25}, {"pool_b", 0.5}, {"cand", 0.0}, {"pool_c", 0.25}, {"probe", 0.0}}},
        {"signs", {{"pool_a", 1}, {"pool_b", 1}, {"pool_c", 1}}},
        {"theme_standardise", {{"rule", "ew-theme-std-v1"}, {"rerank", block == Block::rerank}, {"themes", themes}}}},
        weights_sha);
  }
  bool write_entry(const Entry& e) {
    auto dsl_sha = core::sha256_hex(e.dsl); if (!dsl_sha) return false;
    const auto stem = e.id + "." + dsl_sha->substr(0, 16);
    Json files;
    if (!payload(cache_entry_dir, files, stem + ".f64", e.signal)) return false;
    std::string unused;
    return json_file(cache_entry_dir / (stem + ".json"), {{"schema", "atx.dsl-candidate-signal/v2"},
        {"candidate_id", e.id}, {"dsl_sha256", *dsl_sha}, {"role_manifest_sha256", role_sha}, {"dates", D},
        {"instruments", N}, {"bytes", D * N * sizeof(f64)},
        {"layout", "date-major-little-endian-f64;non-finite-stored-as-quiet-NaN"}, {"payload", stem + ".f64"},
        {"payload_sha256", files[stem + ".f64"]["sha256"]}, {"field_payload_sha256", Json::object()},
        {"vm_identity", entry_identity}}, unused);
  }
  // Entries go where this build's u pass writes them, recording its VM identity (P9 S1 fix
  // round 1: the verb reads that root alone and matches every sidecar's vm_identity): DIR
  // itself under the legacy identity (the v8 fixture's directory), else DIR/<identity>.
  bool write_cache() {
    st::MarginalIcConfig probe;
    probe.candidate_cache_directory = (dir.path / "cache").string();
    const auto layout = st::marginal_cache_roots(probe);
    cache_entry_dir = layout.own / role_sha;
    entry_identity = layout.identity;
    std::error_code ec; fs::create_directories(cache_entry_dir, ec);
    if (ec) return false;
    for (const auto& e : entries) if (!write_entry(e)) return false;
    return true;
  }
  bool write_pool(const World& world) {
    const auto pool = dir.path / "pool";
    if (!fs::create_directory(pool)) return false;
    std::vector<u8> member(D * N, 0);
    for (usize c = 63 * N; c < D * N; ++c) member[c] = (holes && c % 7U == 3U) ? u8{0} : u8{1};
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
  // P9 S1: the book composite plus 33 themes fits; one regressor more is refused.
  const auto themed =
      st::marginal_ic_working_bytes(1155, 5627, 734, 48, cb::kMaxMarginalRegressors);
  ASSERT_TRUE(themed); EXPECT_LT(*themed, limit);
  EXPECT_FALSE(st::marginal_ic_working_bytes(1155, 5627, 734, 48, cb::kMaxMarginalRegressors + 1U));
  // The P9 S1 extras: defaults are the five-argument peak; band workers, --exclude-self and
  // cached pairs only add; workers outside 1..16 and more pairs than exist are refused.
  const auto plain =
      st::marginal_ic_working_bytes(1155, 5627, 734, 48, 11, st::MarginalWorkingExtras{});
  ASSERT_TRUE(plain); EXPECT_EQ(*plain, *v71);
  st::MarginalWorkingExtras banded{4U, true, 48U * 47U / 2U, 1000U};
  const auto heavier = st::marginal_ic_working_bytes(1155, 5627, 734, 48, 11, banded);
  ASSERT_TRUE(heavier); EXPECT_GT(*heavier, *v71); EXPECT_LT(*heavier, limit);
  banded.workers = 17U;
  EXPECT_FALSE(st::marginal_ic_working_bytes(1155, 5627, 734, 48, 11, banded));
  banded.workers = 4U; banded.computed_pairs = 48U * 47U / 2U + 1U;
  EXPECT_FALSE(st::marginal_ic_working_bytes(1155, 5627, 734, 48, 11, banded));
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

// Review B-3 (Ruling E-10): the pool's role carries the scored signals, so a role built with
// --delisting-returns is refused, naming the manifest and the flag, before any output.
TEST(MarginalIc, DelistingReturnsRoleIsRefused) {
  Fixture f(Mode::Noise); ASSERT_TRUE(f.ok);
  auto manifest = read_json(f.cfg.role_manifest);
  manifest["universe"] = {{"id", "linked-operating-v1"}, {"delisting", {{"returns_applied", true}}}};
  std::string unused;
  ASSERT_TRUE(json_file(f.cfg.role_manifest, manifest, unused));
  std::ostringstream progress;
  const auto status = st::run_marginal_ic(f.cfg, progress);
  ASSERT_FALSE(status);
  const auto message = status.error().to_string();
  EXPECT_NE(message.find(f.cfg.role_manifest), std::string::npos) << message;
  EXPECT_NE(message.find("universe.delisting.returns_applied true"), std::string::npos) << message;
  EXPECT_FALSE(fs::exists(f.cfg.output_directory));
}

// Review B-2: an ew-theme-std-v1 weights file groups the book by its theme_standardise block
// (t_one, t_two), not by the library rows (alpha, delta, beta). With rerank each theme regressor
// is the blend's re-ranked theme term, so the marginal statistic is taken inside the theme:
// probe, the raw t_one composite, is spanned on every date, as pool_b (alone in t_two) is.
// Without the re-rank (the rule's identity switch) t_one is linear in the member ranks, and
// probe's rank of that sum is not.
TEST(MarginalIc, StandardisedThemesComeFromTheWeightsBlock) {
  for (const Block block : {Block::rerank, Block::plain}) {
    Fixture f(Mode::Planted, block); ASSERT_TRUE(f.ok);
    f.cfg.themes_path = (f.dir.path / "weights.json").string();
    std::ostringstream progress;
    const auto status = st::run_marginal_ic(f.cfg, progress);
    ASSERT_TRUE(status) << status.error().to_string();
    const auto out = read_json(fs::path(f.cfg.output_directory) / "marginal_ic.json");
    EXPECT_EQ(out.at("method").at("regressors"), Json::array({"book_composite", "theme:t_one", "theme:t_two"}));
    Json members = Json::object();
    members["t_one"] = Json::array({"pool_a", "pool_c"}); members["t_two"] = Json::array({"pool_b"});
    const auto& themes = out.at("inputs").at("themes");
    EXPECT_EQ(themes.at("members"), members);
    EXPECT_EQ(themes.at("grouping"), "theme_standardise");
    EXPECT_EQ(themes.at("rerank").get<bool>(), block == Block::rerank);
    const auto& probe = candidate(out, "probe");
    const auto composite = out.at("method").at("theme_composite").get<std::string>();
    if (block == Block::rerank) {
      EXPECT_EQ(probe.at("spanned_dates"), rows);
      EXPECT_EQ(real(probe, "marginal_ic21"), 0.0);
      EXPECT_EQ(candidate(out, "pool_b").at("spanned_dates"), rows);
      EXPECT_TRUE(composite.starts_with("ew-theme-std-v1 theme term")) << composite;
    } else {
      EXPECT_EQ(probe.at("spanned_dates"), 0U);
      EXPECT_TRUE(composite.starts_with("sum over weighted members")) << composite;
    }
  }
}

// ---- P9 S1: marginal verb speed ---------------------------------------------------------------
// Runs the verb into `name` under the fixture directory and returns its marginal_ic.json; an
// error throws, which fails the calling test with the error's text.
Json run_into(const Fixture& f, st::MarginalIcConfig cfg, const std::string& name) {
  cfg.output_directory = (f.dir.path / name).string();
  std::ostringstream progress;
  const auto status = st::run_marginal_ic(cfg, progress);
  if (!status) throw std::runtime_error(name + ": " + status.error().to_string());
  return read_json(fs::path(cfg.output_directory) / "marginal_ic.json");
}
// The output without its timings (never reproducible) and the named top-level and inputs keys.
Json without(Json out, std::initializer_list<const char*> keys,
             std::initializer_list<const char*> inputs = {}) {
  out.erase(std::string("stage_seconds"));
  for (const char* key : keys) out.erase(std::string(key));
  for (const char* key : inputs) out.at("inputs").erase(std::string(key));
  return out;
}
bool text_file(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary); out << text; out.close();
  return static_cast<bool>(out);
}
// A run that must refuse before any output.
bool refuses(const st::MarginalIcConfig& cfg, const fs::path& output, std::string& message) {
  auto refused_cfg = cfg;
  refused_cfg.output_directory = output.string();
  std::ostringstream progress;
  const auto status = st::run_marginal_ic(refused_cfg, progress);
  if (status) return false;
  message = status.error().to_string();
  return !fs::exists(output);
}

// Ruling P4: --candidates residualises only the listed candidates; every candidate stays in the
// regressors and is a max_abs_rho partner, so each listed row is the full run's row.
TEST(MarginalIc, CandidatesSubsetEqualsFullRows) {
  Fixture f(Mode::Planted, Block::rerank); ASSERT_TRUE(f.ok);
  f.cfg.themes_path = (f.dir.path / "weights.json").string();
  const auto full = run_into(f, f.cfg, "full");
  auto cfg = f.cfg;
  cfg.candidates_path = (f.dir.path / "listed.txt").string();
  // A BOM, CRLF line ends, a blank line and an order other than the library's.
  ASSERT_TRUE(text_file(cfg.candidates_path, "\xEF\xBB\xBF" "probe\r\ncand\r\n\r\n"));
  const auto subset = run_into(f, cfg, "subset");
  ASSERT_EQ(subset.at("candidates").size(), 2U);
  EXPECT_EQ(subset.at("candidates").at(0).dump(), candidate(full, "cand").dump());
  EXPECT_EQ(subset.at("candidates").at(1).dump(), candidate(full, "probe").dump());
  const auto& listed = subset.at("inputs").at("candidates");
  EXPECT_EQ(listed.at("ids"), Json::array({"cand", "probe"}));
  EXPECT_EQ(listed.at("path"), cfg.candidates_path);
  EXPECT_FALSE(full.at("inputs").contains("candidates"));
  // Every other byte but the timings is the full run's.
  EXPECT_EQ(without(subset, {"candidates"}, {"candidates"}).dump(),
            without(full, {"candidates"}).dump());
  // An id outside the library, a repeated id and an empty list refuse before any output.
  for (const char* text : {"cand\nnot_in_library\n", "cand\ncand\n", "\r\n\n"}) {
    ASSERT_TRUE(text_file(cfg.candidates_path, text));
    std::string message;
    EXPECT_TRUE(refuses(cfg, f.dir.path / "refused", message)) << text << message;
  }
}

// Rows compacted to the date's member names (the default) and the v8 full-width rows give the
// same bytes, under a member mask with holes on every scored date, with library themes and with
// the ew-theme-std-v1 grouping re-ranked and plain.
TEST(MarginalIc, CompactedRowsEqual) {
  for (const Block block : {Block::none, Block::rerank, Block::plain}) {
    Fixture f(Mode::Planted, block, true); ASSERT_TRUE(f.ok);
    f.cfg.themes_path = (f.dir.path / "weights.json").string();
    const auto compact = run_into(f, f.cfg, "compact");
    auto cfg = f.cfg;
    cfg.compact_rows = false;
    const auto wide = run_into(f, cfg, "wide");
    EXPECT_EQ(without(compact, {}).dump(), without(wide, {}).dump());
    // The rows are defined on every date with about one name in seven dropped.
    EXPECT_EQ(candidate(compact, "cand").at("dates"), rows);
  }
}

// --pair-cache: a warm run serves every pair from the cache with the computed bits, a subset run
// is served too, another min_names is another key, and a damaged shard refuses.
TEST(MarginalIc, PairCacheHitEqualsCompute) {
  Fixture f(Mode::Noise); ASSERT_TRUE(f.ok);
  const auto plain = run_into(f, f.cfg, "plain");
  auto cfg = f.cfg;
  cfg.pair_cache_directory = (f.dir.path / "pairs").string();
  const auto cold = run_into(f, cfg, "cold");
  const auto warm = run_into(f, cfg, "warm");
  const auto& cold_cache = cold.at("inputs").at("pair_cache");
  EXPECT_EQ(cold_cache.at("hits"), 0U); EXPECT_EQ(cold_cache.at("computed"), 3U);
  EXPECT_EQ(cold_cache.at("shards_read"), 0U);
  ASSERT_TRUE(cold_cache.at("shard").is_string());
  const auto& warm_cache = warm.at("inputs").at("pair_cache");
  EXPECT_EQ(warm_cache.at("hits"), 3U); EXPECT_EQ(warm_cache.at("computed"), 0U);
  EXPECT_EQ(warm_cache.at("shards_read"), 1U); EXPECT_EQ(warm_cache.at("cached_pairs"), 3U);
  EXPECT_TRUE(warm_cache.at("shard").is_null());
  EXPECT_EQ(warm_cache.at("key_sha256"), cold_cache.at("key_sha256"));
  // Every byte but the timings, the cache record and the admitted bytes (which count the
  // loaded pairs) is the run without a cache.
  for (const Json* out : {&cold, &warm})
    EXPECT_EQ(without(*out, {"working_bytes"}, {"pair_cache"}).dump(),
              without(plain, {"working_bytes"}).dump());
  // A --candidates run on the warm cache: both of its pairs are hits, its row the full run's.
  auto subset_cfg = cfg;
  subset_cfg.candidates_path = (f.dir.path / "listed.txt").string();
  ASSERT_TRUE(text_file(subset_cfg.candidates_path, "cand\n"));
  const auto subset = run_into(f, subset_cfg, "subset");
  EXPECT_EQ(subset.at("inputs").at("pair_cache").at("hits"), 2U);
  EXPECT_EQ(subset.at("inputs").at("pair_cache").at("computed"), 0U);
  ASSERT_EQ(subset.at("candidates").size(), 1U);
  EXPECT_EQ(subset.at("candidates").at(0).dump(), candidate(plain, "cand").dump());
  // Another min_names is another key: a clean miss, never a stale hit.
  auto other_cfg = cfg;
  other_cfg.min_names = 21;
  const auto other = run_into(f, other_cfg, "other_key");
  EXPECT_EQ(other.at("inputs").at("pair_cache").at("hits"), 0U);
  EXPECT_NE(other.at("inputs").at("pair_cache").at("key_sha256"), cold_cache.at("key_sha256"));
  // A damaged shard refuses instead of serving.
  const auto key = cold_cache.at("key_sha256").get<std::string>();
  const auto shard = fs::path(cfg.pair_cache_directory) /
      ("pairs" + std::to_string(cb::kPairwiseRowCorrelationVersion) + "_" + key.substr(0, 16)) /
      cold_cache.at("shard").get<std::string>();
  ASSERT_TRUE(fs::exists(shard)) << shard.string();
  auto damaged = read_json(shard);
  auto& counted = damaged.at("record").at("pairs").at(0).at(3);
  counted = counted.get<u64>() + 1U;
  std::string unused;
  ASSERT_TRUE(json_file(shard, damaged, unused));
  std::string message;
  EXPECT_TRUE(refuses(cfg, f.dir.path / "damaged", message)) << message;
  EXPECT_NE(message.find("shard integrity"), std::string::npos) << message;
}

// CM-6: the book composite plus 33 theme composites (34 regressors) run; 34 themes refuse.
TEST(MarginalIc, ThirtyThreeThemes) {
  static_assert(cb::kMaxMarginalRegressors == 34U);
  {
    Fixture f(Mode::Planted, Block::none, false, 31U); ASSERT_TRUE(f.ok);
    f.cfg.themes_path = (f.dir.path / "weights.json").string();
    const auto out = run_into(f, f.cfg, "thirty_three");
    ASSERT_EQ(out.at("method").at("regressors").size(), cb::kMaxMarginalRegressors);
    EXPECT_EQ(out.at("inputs").at("themes").at("members").size(), 33U);
    ASSERT_EQ(out.at("candidates").size(), 34U);
    // A member alone in its theme is a multiple of its theme composite: spanned on every row.
    for (const char* id : {"pool_b", "m0", "m30"})
      EXPECT_EQ(candidate(out, id).at("spanned_dates"), rows) << id;
    const auto& c = candidate(out, "cand");
    EXPECT_EQ(c.at("marginal_dates"), rows);
    EXPECT_GT(real(c, "marginal_ic21"), 0.2);
  }
  Fixture f(Mode::Planted, Block::none, false, 32U); ASSERT_TRUE(f.ok);
  f.cfg.themes_path = (f.dir.path / "weights.json").string();
  std::string message;
  EXPECT_TRUE(refuses(f.cfg, f.dir.path / "thirty_four", message)) << message;
  EXPECT_NE(message.find("1..33 weighted themes"), std::string::npos) << message;
}

// --workers: DetPool date bands write only their own rows and the pair values are added in date
// order after each join, so 3, 4 and 16 workers give the serial bytes, with --exclude-self too.
TEST(MarginalIc, BandsByteIdenticalAt1And4) {
  Fixture f(Mode::Planted, Block::rerank, true); ASSERT_TRUE(f.ok);
  f.cfg.themes_path = (f.dir.path / "weights.json").string();
  for (const bool exclude_self : {false, true}) {
    auto cfg = f.cfg;
    cfg.exclude_self = exclude_self;
    const std::string tag = exclude_self ? "self" : "book";
    const auto serial = run_into(f, cfg, tag + "_w1");
    EXPECT_FALSE(serial.contains("workers"));
    for (const usize workers : {usize{3}, usize{4}, usize{16}}) {
      cfg.workers = workers;
      const auto banded = run_into(f, cfg, tag + "_w" + std::to_string(workers));
      EXPECT_EQ(banded.at("workers"), workers);
      EXPECT_EQ(banded.at("candidates").dump(), serial.at("candidates").dump()) << workers;
      EXPECT_EQ(without(banded, {"working_bytes", "workers"}).dump(),
                without(serial, {"working_bytes"}).dump()) << workers;
      EXPECT_GT(banded.at("working_bytes").get<u64>(), serial.at("working_bytes").get<u64>());
    }
  }
  auto cfg = f.cfg;
  cfg.workers = 17;
  std::string message;
  EXPECT_TRUE(refuses(cfg, f.dir.path / "w17", message)) << message;
}

// Review CM s.4: --exclude-self residualises a book member's row on regressors without its own
// term; other rows keep their bytes, and the option needs --themes.
TEST(MarginalIc, ExcludeSelfDropsTheMembersOwnTerm) {
  Fixture f(Mode::Planted); ASSERT_TRUE(f.ok);
  auto cfg = f.cfg;
  cfg.exclude_self = true;
  std::string message;
  EXPECT_TRUE(refuses(cfg, f.dir.path / "no_themes", message)) << message;
  f.cfg.themes_path = (f.dir.path / "weights.json").string();
  cfg.themes_path = f.cfg.themes_path;
  const auto book = run_into(f, f.cfg, "book");
  const auto self = run_into(f, cfg, "self");
  EXPECT_FALSE(book.at("method").contains("exclude_self"));
  EXPECT_TRUE(self.at("method").contains("exclude_self"));
  EXPECT_EQ(candidate(self, "cand").dump(), candidate(book, "cand").dump());
  // pool_a is its own theme composite (alpha): spanned on every row while it is a regressor,
  // never without it (the rest of the book is pool_b, independent of it).
  EXPECT_EQ(candidate(book, "pool_a").at("spanned_dates"), rows);
  EXPECT_EQ(candidate(self, "pool_a").at("spanned_dates"), 0U);
  EXPECT_TRUE(std::isfinite(real(candidate(self, "pool_a"), "marginal_ic21")));
}

// --verified-digests: payloads the caller already verified are not re-hashed; the rows are
// unchanged, and a line that is not a SHA256 refuses.
TEST(MarginalIc, VerifiedDigestsSkipTheRehash) {
  Fixture f(Mode::Noise); ASSERT_TRUE(f.ok);
  const auto plain = run_into(f, f.cfg, "plain");
  std::string listing;
  for (const auto& row : plain.at("candidates"))
    listing += row.at("payload_sha256").get<std::string>() + "\n";
  auto cfg = f.cfg;
  cfg.verified_digests_path = (f.dir.path / "verified.txt").string();
  ASSERT_TRUE(text_file(cfg.verified_digests_path, listing));
  const auto vouched = run_into(f, cfg, "vouched");
  EXPECT_EQ(vouched.at("inputs").at("verified_digests").at("listed"), 3U);
  EXPECT_EQ(vouched.at("inputs").at("verified_digests").at("accepted"), 3U);
  EXPECT_EQ(without(vouched, {}, {"verified_digests"}).dump(), without(plain, {}).dump());
  ASSERT_TRUE(text_file(cfg.verified_digests_path, listing + "not-a-digest\n"));
  std::string message;
  EXPECT_TRUE(refuses(cfg, f.dir.path / "refused", message)) << message;
}

// The pair cache is keyed by kPairwiseRowCorrelationVersion, so an edit to the sources that
// compute a pair's bits must be a conscious bump-or-repin decision (the IC caches' digest recipe;
// no include closure: the residualisation and HAC sources never touch a pair).
TEST(MarginalIc, PairCacheSourcesPinned) {
  const auto id = st::marginal_pair_cache_identity();
  EXPECT_EQ(id.version, cb::kPairwiseRowCorrelationVersion);
  ASSERT_EQ(id.sources.size(), 2U);
  const auto repo = fs::path{ATX_IMPL_TESTS_DIR}.parent_path().parent_path();
  std::string material;
  for (const auto& rel : id.sources) {
    std::ifstream in(repo / rel, std::ios::binary);
    const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ASSERT_FALSE(raw.empty()) << rel;
    std::string text;
    text.reserve(raw.size());
    for (usize k = 0; k < raw.size(); ++k)
      if (!(raw[k] == '\r' && k + 1 < raw.size() && raw[k + 1] == '\n')) text.push_back(raw[k]);
    material += rel + '\n' + std::to_string(text.size()) + '\n' + text;
  }
  const auto digest = core::sha256_hex(material); ASSERT_TRUE(digest);
  EXPECT_EQ(*digest, id.sources_sha256)
      << "pair sources changed: bump kPairwiseRowCorrelationVersion and update the pinned hash";
}

// DS-1: the build flavour token keeps a Debug and a Release tree off each other's candidate-signal
// and IC-result entries, while the legacy Debug flavour keeps its pre-P9 identity bytes.
TEST(IcIdentity, BuildTokenSeparatesCaches) {
  namespace ae = atx::engine;
  ae::BuildFlavor legacy{};
  legacy.crt = "mdd";
  legacy.xsimd_major = 13;
  EXPECT_TRUE(ae::legacy_build_flavor(legacy));
  EXPECT_EQ(ae::build_flavor_suffix(legacy), "");
  legacy.optimized = true; // the equity-dev tree's /O2 IC translation units
  EXPECT_EQ(ae::build_flavor_suffix(legacy), "");
  ae::BuildFlavor release{};
  release.optimized = true;
  release.ndebug = true;
  release.crt = "md";
  release.xsimd_major = 13;
  EXPECT_FALSE(ae::legacy_build_flavor(release));
  EXPECT_EQ(ae::build_flavor_token(release), "opt_md_ndebug_xs13.0.0");
  EXPECT_EQ(ae::build_flavor_suffix(release), "_opt_md_ndebug_xs13.0.0");
  // Every component moves the token, and leaving the legacy CRT, assertions or xsimd version
  // leaves the legacy (empty) suffix.
  const auto token = ae::build_flavor_token(release);
  const auto moves = [&](auto edit) {
    ae::BuildFlavor edited = release;
    edit(edited);
    return ae::build_flavor_token(edited) != token;
  };
  EXPECT_TRUE(moves([](ae::BuildFlavor& b) { b.optimized = false; }));
  EXPECT_TRUE(moves([](ae::BuildFlavor& b) { b.optimized_for_size = true; }));
  EXPECT_TRUE(moves([](ae::BuildFlavor& b) { b.ndebug = false; }));
  EXPECT_TRUE(moves([](ae::BuildFlavor& b) { b.crt = "mt"; }));
  EXPECT_TRUE(moves([](ae::BuildFlavor& b) { b.xsimd_major = 14; }));
  EXPECT_TRUE(moves([](ae::BuildFlavor& b) { b.xsimd_minor = 1; }));
  EXPECT_TRUE(moves([](ae::BuildFlavor& b) { b.xsimd_patch = 2; }));
  for (const auto& edit : {+[](ae::BuildFlavor& b) { b.crt = "md"; },
                           +[](ae::BuildFlavor& b) { b.ndebug = true; },
                           +[](ae::BuildFlavor& b) { b.xsimd_patch = 1; }}) {
    ae::BuildFlavor edited = legacy;
    edit(edited);
    EXPECT_FALSE(ae::build_flavor_suffix(edited).empty()) << ae::build_flavor_token(edited);
  }
  ae::BuildFlavor fp{};
  fp.fma = true;
  fp.avx2 = true;
  EXPECT_EQ(ae::fp_flavor_suffix(fp), "_fma_avx2");
  fp.fast_math = true;
  EXPECT_EQ(ae::fp_flavor_suffix(fp), "_fma_avx2_fastmath");
  // This build's identities: the IC identity takes its FP words, SIMD width and suffix from the
  // scoring TU; the VM identity ends with its own TU's suffix (the same build's flags).
  const auto flavor = ae::factory::ic_screen_build_flavor();
  const auto ic = st::ic_result_cache_identity().identity;
  EXPECT_TRUE(ic.ends_with(ae::fp_flavor_suffix(flavor) + "_simd" +
                           std::to_string(ae::factory::ic_screen_simd_width()) +
                           ae::build_flavor_suffix(flavor))) << ic;
  const auto vm = st::ic_cache_vm_identity().identity;
  EXPECT_TRUE(vm.ends_with(ae::build_flavor_suffix(ATX_ENGINE_BUILD_FLAVOR))) << vm;
  EXPECT_EQ(ae::legacy_build_flavor(flavor), ae::build_flavor_suffix(flavor).empty());
#if defined(NDEBUG)
  // A Release tree: its own cache roots.
  EXPECT_FALSE(ae::build_flavor_suffix(flavor).empty()) << ic;
#endif
}

// Fix round 1 (review S1 major, DS-1): a Debug and a Release tree never read each other's
// candidate-cache entries. The verb reads only the root its build's u pass writes -- DIR for the
// legacy (equity-dev Debug) identity, keeping the v8 roots DIR/<identity>, DIR; DIR/<identity>
// alone for any other -- and every sidecar it reads must record its identity. So a Release
// build never falls back to the legacy DIR, and no build reads another identity's entry
// wherever it sits. `probe` stands for a build of another flavour; it is no real identity, so
// the test holds in the Debug and the Release tree alike.
TEST(MarginalIc, CacheRootsNeverShareAcrossBuilds) {
  const std::string legacy = "dslvm1_clang18.1"; // strategy_ic_signal_cache.cpp legacy_vm_identity
  const std::string probe = "dslvm1_s1probe";
  // The rule, as strings (gtest does not print std::filesystem::path safely).
  const auto under = [](const std::string& leaf) {
    return (fs::path("cache_dir") / leaf).string();
  };
  st::MarginalIcConfig rule;
  rule.candidate_cache_directory = "cache_dir";
  rule.cache_identity = probe;
  const auto other = st::marginal_cache_roots(rule);
  EXPECT_EQ(other.identity, probe);
  EXPECT_EQ(other.own.string(), under(probe));
  ASSERT_EQ(other.roots.size(), 1U);
  EXPECT_EQ(other.roots.front().string(), under(probe));
  rule.cache_identity.clear();
  const auto own = st::marginal_cache_roots(rule);
  EXPECT_EQ(own.identity, st::ic_cache_vm_identity().identity);
  const bool legacy_build = own.identity == legacy;
  if (legacy_build) {
    EXPECT_EQ(own.own.string(), fs::path("cache_dir").string());
    ASSERT_EQ(own.roots.size(), 2U);
    EXPECT_EQ(own.roots.front().string(), under(legacy));
    EXPECT_EQ(own.roots.back().string(), fs::path("cache_dir").string());
  } else {
    EXPECT_EQ(own.own.string(), under(own.identity));
    ASSERT_EQ(own.roots.size(), 1U);
    EXPECT_EQ(own.roots.front().string(), under(own.identity));
  }
#if defined(NDEBUG)
  EXPECT_FALSE(legacy_build) << own.identity; // a Release tree never reads the legacy DIR
#endif
  // End to end on copies of the fixture's entries (which sit in this build's own root).
  Fixture f(Mode::Noise); ASSERT_TRUE(f.ok);
  const auto reference = run_into(f, f.cfg, "own_root");
  EXPECT_EQ(reference.at("inputs").at("candidate_cache").at("build_vm_identity"), own.identity);
  // The fixture's entries copied into `entry_dir`, each sidecar recording `recorded`.
  const auto place = [&f](const fs::path& entry_dir, const std::string& recorded) {
    std::error_code ec;
    fs::create_directories(entry_dir, ec);
    if (ec) return false;
    for (const auto& item : fs::directory_iterator(f.cache_entry_dir)) {
      const auto target = entry_dir / item.path().filename();
      if (item.path().extension() != ".json") {
        if (!fs::copy_file(item.path(), target, fs::copy_options::overwrite_existing)) return false;
        continue;
      }
      auto sidecar = read_json(item.path());
      sidecar["vm_identity"] = recorded;
      std::string unused;
      if (!json_file(target, sidecar, unused)) return false;
    }
    return true;
  };
  // The fixture's config over cache directory `root`, read as `identity` (empty: this build).
  const auto over = [&f](const fs::path& root, const std::string& identity) {
    auto run = f.cfg;
    run.candidate_cache_directory = root.string();
    run.cache_identity = identity;
    return run;
  };
  // A run that must refuse with `code` and a message containing `says`, before any output.
  const auto refused = [&f](st::MarginalIcConfig run, const std::string& name,
                            core::ErrorCode code, const std::string& says) {
    run.output_directory = (f.dir.path / name).string();
    std::ostringstream progress;
    const auto status = st::run_marginal_ic(run, progress);
    ASSERT_FALSE(status) << name;
    const auto message = status.error().to_string();
    EXPECT_EQ(status.error().code(), code) << message;
    EXPECT_NE(message.find(says), std::string::npos) << message;
    EXPECT_FALSE(fs::exists(run.output_directory)) << name;
  };
  const std::string not_found = "no candidate cache entry", foreign = "records vm_identity";
  // 1. The review's case: a Debug u pass filled only the legacy DIR. A build of any other
  //    identity (this Release tree, or the probe) does not fall back to it.
  const auto legacy_dir = f.dir.path / "legacy";
  ASSERT_TRUE(place(legacy_dir / f.role_sha, legacy));
  refused(over(legacy_dir, probe), "legacy_probe", core::ErrorCode::NotFound, not_found);
  if (legacy_build) {
    const auto debug = run_into(f, over(legacy_dir, ""), "legacy_own");
    EXPECT_EQ(debug.at("candidates").dump(), reference.at("candidates").dump());
  } else {
    refused(over(legacy_dir, ""), "legacy_own", core::ErrorCode::NotFound, not_found);
  }
  // 2. Vice versa: entries only in another build's own root DIR/<probe>/, recording it, are
  //    read by that build alone; this build (the legacy roots included) never reaches them.
  const auto probe_dir = f.dir.path / "probe";
  ASSERT_TRUE(place(probe_dir / probe / f.role_sha, probe));
  refused(over(probe_dir, ""), "probe_own", core::ErrorCode::NotFound, not_found);
  const auto matched = run_into(f, over(probe_dir, probe), "probe_probe");
  EXPECT_EQ(matched.at("inputs").at("candidate_cache").at("build_vm_identity"), probe);
  EXPECT_EQ(matched.at("candidates").dump(), reference.at("candidates").dump());
  // 3. An entry in a build's own root that records another identity refuses: for the probe,
  //    and for this build (under the legacy identity, a foreign sidecar in DIR itself).
  ASSERT_TRUE(place(probe_dir / probe / f.role_sha, legacy));
  refused(over(probe_dir, probe), "probe_foreign", core::ErrorCode::InvalidArgument, foreign);
  const auto mixed_dir = f.dir.path / "mixed";
  st::MarginalIcConfig mixed;
  mixed.candidate_cache_directory = mixed_dir.string();
  ASSERT_TRUE(place(st::marginal_cache_roots(mixed).own / f.role_sha, probe));
  refused(over(mixed_dir, ""), "own_foreign", core::ErrorCode::InvalidArgument, foreign);
}
} // namespace
