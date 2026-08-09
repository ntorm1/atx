// track_key.hpp -- content-addressed track identity + economics-rev tripwire
// (Task D1, backtest-production-lakehouse sprint).
//
// Four TDD gates the brief asks for verbatim:
//   (a) SameConfigTwiceIsIdentical
//   (b) EconomicsFieldChangesProduceDifferentKeys (+ the mirror-image
//       ExecutionFieldChangesDoNotAffectKey, proving the OTHER half of the
//       split: excluded fields must NOT perturb the key)
//   (c) EconomicsRevChangeProducesDifferentKey
//   (d) GoldenHexPinIsStableAcrossRestarts
//
// Plus the Step 5 economics tripwire (TrackKeyGoldenReplay), corpus-gated and
// GTEST_SKIP-clean in every worktree today -- see golden_pin.hpp.

#include "atx/vol/research/track_key.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/backtest.hpp"
#include "atx/vol/backtest_template.hpp"
#include "atx/vol/research/golden_pin.hpp"

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] BacktestStrategyTemplate make_template() {
  auto made = make_40_delta_3_calendar_month_strangle_template();
  EXPECT_TRUE(made.has_value()) << (made ? "" : made.error().to_string());
  return made ? std::move(*made) : BacktestStrategyTemplate{};
}

[[nodiscard]] std::array<std::uint8_t, 32> fixed_snapshot_id(std::uint8_t seed = 0xAB) {
  std::array<std::uint8_t, 32> id{};
  id.fill(seed);
  return id;
}

} // namespace

// ── (a) same config twice => identical key ──────────────────────────────────

TEST(TrackKeyTest, SameConfigTwiceIsIdentical) {
  const BacktestStrategyTemplate tmpl = make_template();
  RunConfig cfg{};
  cfg.frictions.spread_kind = FrictionModel::SpreadKind::PriceBps;
  cfg.frictions.half_spread_bps = 5.0;
  cfg.financing.borrow_rate = 0.01;

  const std::string engine_id = make_engine_id();
  const std::array<std::uint8_t, 32> snapshot_id = fixed_snapshot_id();

  const TrackKey first = make_track_key(canonical_config_bytes(tmpl, cfg), engine_id, snapshot_id);
  const TrackKey second = make_track_key(canonical_config_bytes(tmpl, cfg), engine_id, snapshot_id);

  EXPECT_EQ(first, second);
  EXPECT_EQ(first.hex(), second.hex());
  EXPECT_EQ(make_engine_id(), engine_id) << "make_engine_id() must be deterministic within a build";
}

// ── (b) any single economics field change => different key ─────────────────

TEST(TrackKeyTest, EconomicsFieldChangesProduceDifferentKeys) {
  const BacktestStrategyTemplate tmpl = make_template();
  const RunConfig baseline{};
  const std::string engine_id = make_engine_id();
  const std::array<std::uint8_t, 32> snapshot_id = fixed_snapshot_id();
  const TrackKey baseline_key =
      make_track_key(canonical_config_bytes(tmpl, baseline), engine_id, snapshot_id);

  const auto expect_different = [&](std::string_view label, const RunConfig &mutated) {
    const TrackKey mutated_key =
        make_track_key(canonical_config_bytes(tmpl, mutated), engine_id, snapshot_id);
    EXPECT_NE(mutated_key.hex(), baseline_key.hex())
        << "economics field did not change the key: " << label;
  };

  // frictions (brief's named example "friction f")
  {
    RunConfig cfg = baseline;
    cfg.frictions.half_spread_bps = 12.5;
    expect_different("frictions.half_spread_bps", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.frictions.spread_kind = FrictionModel::SpreadKind::VolTicks;
    expect_different("frictions.spread_kind", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.frictions.vol_tick = 0.02;
    expect_different("frictions.vol_tick", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.frictions.impact_fraction = 0.001;
    expect_different("frictions.impact_fraction", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.frictions.per_contract_cost = 0.65;
    expect_different("frictions.per_contract_cost", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.frictions.hedge_slippage_bps = 3.0;
    expect_different("frictions.hedge_slippage_bps", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.frictions.crossing_fraction_single = 0.9;
    expect_different("frictions.crossing_fraction_single", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.frictions.crossing_fraction_complex = 0.6;
    expect_different("frictions.crossing_fraction_complex", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.frictions.quote_lookup = [](const OptionContract &) { return std::nullopt; };
    expect_different("frictions.quote_lookup (set vs unset)", cfg);
  }

  // financing (brief's named example "financing rate")
  {
    RunConfig cfg = baseline;
    cfg.financing.borrow_rate = 0.02;
    expect_different("financing.borrow_rate", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.financing.finance_premium = true;
    expect_different("financing.finance_premium", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.financing.shares_carry = true;
    expect_different("financing.shares_carry", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.financing.initial_cash = 1'000.0;
    expect_different("financing.initial_cash", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.financing.share_dividends.push_back(ShareDividend{7, 123, 0.5});
    expect_different("financing.share_dividends", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.financing.reference_uid = 42;
    expect_different("financing.reference_uid", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.financing.flat_r = 0.03;
    expect_different("financing.flat_r", cfg);
  }

  // policy enums (brief's named example "policy enum")
  {
    // RunConfig{}'s default is ALREADY UnpricedLotPolicy::Error (WS-F F1(c)
    // fail-closed default, backtest.hpp) -- mutate to the OTHER enumerator.
    RunConfig cfg = baseline;
    cfg.unpriced = UnpricedLotPolicy::ExcludeAndReport;
    expect_different("unpriced", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.surface_provenance_policy = SurfaceProvenancePolicy::RequireAdmittedRisk;
    expect_different("surface_provenance_policy", cfg);
  }
  // FIX-ROUND 1 (post-review correction): reconcile_nav / reconcile_nav_tol
  // were originally excluded as "execution" on the theory that they only gate
  // a post-hoc assertion. That misses reconcile_row's fail-closed ABORT
  // (backtest.cpp:3415-3429, via ATX_TRY_VOID at :3804/:4168) when
  // reconcile_nav is true and drift exceeds the tolerance -- the run produces
  // no BacktestResult at all. A track cached under reconcile_nav=false could
  // otherwise be served to a caller who asked for the reconcile_nav=true
  // validation guarantee, silently bypassing it. See track_key.hpp.
  {
    RunConfig cfg = baseline;
    cfg.reconcile_nav = !baseline.reconcile_nav;
    expect_different("reconcile_nav", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.reconcile_nav_tol = 1.0;
    expect_different("reconcile_nav_tol", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.book_entry_fill_slippage = !baseline.book_entry_fill_slippage;
    expect_different("book_entry_fill_slippage", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.swap_fixing_cadence = SwapFixingCadence::AcceptClockAsSchedule;
    expect_different("swap_fixing_cadence", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.clock_gaps = ClockGapPolicy::Error;
    expect_different("clock_gaps", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.margin_breach = MarginBreachPolicy::Halt;
    expect_different("margin_breach", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.exercise_policy = ExercisePolicy::Simulate;
    expect_different("exercise_policy", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.query_pricing_tier = QueryPricingTier::RepresentativeFast;
    expect_different("query_pricing_tier", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.query_cache_build_policy = QueryCacheBuildPolicy::ReuseOnly;
    expect_different("query_cache_build_policy", cfg);
  }

  // The template's own economics fold in too (brief: canonical_config covers
  // BacktestStrategyTemplate + RunConfig economics).
  {
    BacktestStrategyTemplate mutated_template = tmpl;
    mutated_template.legs.front().quantity *= 2.0;
    const TrackKey mutated_key =
        make_track_key(canonical_config_bytes(mutated_template, baseline), engine_id, snapshot_id);
    EXPECT_NE(mutated_key.hex(), baseline_key.hex()) << "template leg quantity did not change the key";
  }
}

// Discipline half of the same split: EXECUTION fields must NOT perturb the
// key, or identical economics on different topologies (thread count,
// prefetch depth, snapshot pool wiring) would miss the cache -- exactly the
// failure mode the brief calls out by name.
TEST(TrackKeyTest, ExecutionFieldChangesDoNotAffectKey) {
  const BacktestStrategyTemplate tmpl = make_template();
  const RunConfig baseline{};
  const std::string engine_id = make_engine_id();
  const std::array<std::uint8_t, 32> snapshot_id = fixed_snapshot_id();
  const TrackKey baseline_key =
      make_track_key(canonical_config_bytes(tmpl, baseline), engine_id, snapshot_id);

  const auto expect_same = [&](std::string_view label, const RunConfig &mutated) {
    const TrackKey mutated_key =
        make_track_key(canonical_config_bytes(tmpl, mutated), engine_id, snapshot_id);
    EXPECT_EQ(mutated_key.hex(), baseline_key.hex()) << "execution field changed the key: " << label;
  };

  {
    RunConfig cfg = baseline;
    cfg.price.n_threads = 8;
    expect_same("price.n_threads", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.price.analytic_greeks = !baseline.price.analytic_greeks;
    expect_same("price.analytic_greeks", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.record_every_n = 5;
    expect_same("record_every_n", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.prefetch_snapshots = !baseline.prefetch_snapshots;
    expect_same("prefetch_snapshots", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.settlement_mark_memo = !baseline.settlement_mark_memo;
    expect_same("settlement_mark_memo", cfg);
  }
  {
    RunConfig cfg = baseline;
    cfg.prefetch_depth = 8;
    expect_same("prefetch_depth", cfg);
  }
  // reconcile_nav / reconcile_nav_tol moved OUT of this test in FIX-ROUND 1
  // (post-review): reconcile_nav's fail-closed abort (reconcile_row,
  // backtest.cpp:3415-3429/:3804/:4168) makes it an economics field, not an
  // execution one -- see EconomicsFieldChangesProduceDifferentKeys below and
  // track_key.hpp's INCLUDED entry for the full rationale.
}

// ── (c) same config, different kBacktestEconomicsRev => different key ──────

TEST(TrackKeyTest, EconomicsRevChangeProducesDifferentKey) {
  const BacktestStrategyTemplate tmpl = make_template();
  const RunConfig cfg{};
  const std::vector<std::uint8_t> config_bytes = canonical_config_bytes(tmpl, cfg);
  const std::array<std::uint8_t, 32> snapshot_id = fixed_snapshot_id();

  // make_track_key takes engine_id as caller-supplied data and never reads
  // kBacktestEconomicsRev itself -- exactly what lets this test exercise "the
  // rev changed" without recompiling against a different constant. A real
  // rev bump changes make_engine_id()'s output the same way.
  const TrackKey rev1 = make_track_key(config_bytes, "1.0.0|1|deadbeefcafebabe", snapshot_id);
  const TrackKey rev2 = make_track_key(config_bytes, "1.0.0|2|deadbeefcafebabe", snapshot_id);
  EXPECT_NE(rev1.hex(), rev2.hex());
}

// ── (d) golden hex pin -- stable across process restarts ───────────────────

TEST(TrackKeyTest, GoldenHexPinIsStableAcrossRestarts) {
  // A FIXED, version-independent engine_id: this test pins the SHA-256 +
  // canonical-encoding + hex() implementation, not ATX_VOL_VERSION_STRING or
  // ra_schema_hash(), which may legitimately move for reasons unrelated to
  // economics (a routine version bump must not break this test).
  const BacktestStrategyTemplate tmpl = make_template();
  RunConfig cfg{};
  cfg.frictions.spread_kind = FrictionModel::SpreadKind::PriceBps;
  cfg.frictions.half_spread_bps = 7.5;
  cfg.financing.borrow_rate = 0.015;
  cfg.unpriced = UnpricedLotPolicy::Error;

  std::array<std::uint8_t, 32> snapshot_id{};
  for (std::size_t i = 0; i < snapshot_id.size(); ++i) {
    snapshot_id[i] = static_cast<std::uint8_t>(i);
  }

  const TrackKey key =
      make_track_key(canonical_config_bytes(tmpl, cfg), "golden-pin-engine-id/1", snapshot_id);

  // FIX-ROUND 1 (post-review): re-pinned. reconcile_nav / reconcile_nav_tol
  // moved from excluded to included in canonical_config_bytes (see
  // track_key.hpp's INCLUDED entry and task-D1-report.md) -- the encoding
  // widened by two fields, so the SAME (tmpl, cfg, engine_id, snapshot_id)
  // now hashes to a different, but equally deterministic, 64-char digest.
  // `cfg` above does not set reconcile_nav/reconcile_nav_tol explicitly, so
  // this also pins their RunConfig{} defaults (true / 1.0e-6) being encoded.
  constexpr std::string_view kExpectedHex =
      "851d54ea078bb8289b33ad99256ac14c0129fb0698c4dc80acf61e368e8f86f7";
  EXPECT_EQ(key.hex().size(), 64u);
  EXPECT_EQ(key.hex(), kExpectedHex) << "actual hex was: " << key.hex();
}

// ── track_key_from_hex -- the inverse of hex() (Task D5) ────────────────────
//
// Needed because TrackStore::compact() reads only the hex string back out of
// a staged file's metadata (the original TrackKey struct is long gone by
// then) -- a caller that wants to call Catalog::mark_compacted (which takes a
// TrackKey, not a string) after compact() has to parse it back.

TEST(TrackKeyTest, FromHexRoundTripsHex) {
  const BacktestStrategyTemplate tmpl = make_template();
  RunConfig cfg{};
  cfg.frictions.half_spread_bps = 3.0;
  const TrackKey key =
      make_track_key(canonical_config_bytes(tmpl, cfg), make_engine_id(), fixed_snapshot_id());
  const std::string hex = key.hex();

  auto parsed = track_key_from_hex(hex);
  ASSERT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().to_string());
  EXPECT_EQ(parsed->hex(), hex);
  EXPECT_EQ(parsed->sha256, key.sha256);
}

TEST(TrackKeyTest, FromHexRejectsWrongLength) {
  auto too_short = track_key_from_hex("ab12");
  ASSERT_FALSE(too_short.has_value());
  EXPECT_EQ(too_short.error().code(), atx::core::ErrorCode::InvalidArgument);

  const std::string too_long(65, 'a');
  auto rejected_long = track_key_from_hex(too_long);
  ASSERT_FALSE(rejected_long.has_value());
  EXPECT_EQ(rejected_long.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(TrackKeyTest, FromHexRejectsUppercaseOrNonHexCharacters) {
  // hex() only ever emits lowercase (kHexDigits, track_key.cpp) -- a string
  // that did not round-trip through it must not silently canonicalize.
  const std::string upper(64, 'A');
  auto rejected_upper = track_key_from_hex(upper);
  ASSERT_FALSE(rejected_upper.has_value());
  EXPECT_EQ(rejected_upper.error().code(), atx::core::ErrorCode::InvalidArgument);

  std::string non_hex(64, '0');
  non_hex[10] = 'z';
  auto rejected_nonhex = track_key_from_hex(non_hex);
  ASSERT_FALSE(rejected_nonhex.has_value());
  EXPECT_EQ(rejected_nonhex.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ── Step 5 economics tripwire (mechanical piece; CI wiring is Task D6's) ───

namespace {

[[nodiscard]] std::optional<fs::path> find_golden_82_session_corpus_root() {
#if defined(_MSC_VER)
  char *raw = nullptr;
  std::size_t size = 0;
  if (::_dupenv_s(&raw, &size, "ATX_VOL_GOLDEN_82_SESSION_CORPUS") == 0 && raw != nullptr) {
    const std::string override_path(raw);
    std::free(raw);
    if (!override_path.empty() && fs::exists(override_path)) {
      return fs::path(override_path);
    }
  }
#else
  if (const char *raw = std::getenv("ATX_VOL_GOLDEN_82_SESSION_CORPUS")) {
    if (fs::exists(raw)) {
      return fs::path(raw);
    }
  }
#endif
  const char *candidates[] = {
      "data/golden/82-session-spy",
      "../data/golden/82-session-spy",
      "../../data/golden/82-session-spy",
      "C:/atx/data/golden/82-session-spy",
  };
  for (const char *candidate : candidates) {
    if (fs::exists(candidate)) {
      return fs::path(candidate);
    }
  }
  return std::nullopt;
}

} // namespace

// Runs (once wired -- see below) the 82-session golden replay and compares
// final_nav against the pinned literal + kBacktestEconomicsRev pairing in
// golden_pin.hpp. Skips cleanly with a named reason when the corpus is
// absent, which is every worktree today (real corpora are not checked in;
// prior tasks in this sprint hit the same absence). Structured so a future
// CI job (Task D6) can supply a real corpus and complete the replay call
// without touching this test's skip contract.
TEST(TrackKeyGoldenReplay, Pinned82SessionNavUnlessEconomicsRevBumped) {
  const std::optional<fs::path> corpus_root = find_golden_82_session_corpus_root();
  if (!corpus_root.has_value()) {
    GTEST_SKIP() << "golden 82-session SPY corpus not available (checked "
                    "$ATX_VOL_GOLDEN_82_SESSION_CORPUS and data/golden/82-session-spy "
                    "relative to a few candidate working directories); the economics "
                    "tripwire is pinned at final_nav="
                 << std::setprecision(17) << kGolden82SessionFinalNav
                 << " for kBacktestEconomicsRev=" << kGolden82SessionEconomicsRev
                 << " and cannot run without it. See dispersion_run.cpp:2937 for the "
                    "replay entrypoint ('surface-only projected backtest complete') and "
                    "Task D6 for the CI job that supplies a real corpus + run_spec.";
  }

  // NOT YET WIRED. Running the actual multi-name "surface-only projected"
  // dispersion replay (run_dispersion_surface_backtest, dispersion_run.cpp)
  // needs a run_spec.tsv this worktree does not have checked in either --
  // golden re-pinning is "a single coordinated event owned by the sprint
  // controller" (CHANGELOG.md) and Task D6 explicitly owns the CI gate that
  // supplies it. D1's job is the corpus-detection + skip-cleanly contract
  // above, which every run of this test exercises; this branch documents the
  // extension point rather than guessing at an unverifiable pipeline call.
  GTEST_SKIP() << "golden 82-session corpus root found at " << corpus_root->string()
               << " but the replay call is not wired up yet (Task D6)";
}
