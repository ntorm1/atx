// atx-vol SurfaceDb-driven dispersion backtest gate tests.
//
// Task 1 of the surface-db dispersion sprint adds `Clock::between(lo, hi)`, the
// date-window subset every later task in this sprint uses to carve a run window
// out of a db-backed clock. A SurfaceDb partition key IS the ISO date, and the
// canonical keys sort lexicographically == chronologically, so the window is a
// plain string-range filter over `Clock::refs()`.
//
//   1. BetweenSelectsInclusiveWindow      — [lo, hi] is inclusive on BOTH ends
//                                           and keeps the refs' archive paths.
//   2. BetweenClampsToAvailableRange      — bounds outside the corpus clamp to
//                                           the available refs, they do not error.
//   3. BetweenEmptyWindowIsInvalidArgument— lo > hi, and a window containing no
//                                           partition, are both InvalidArgument
//                                           whose message names the available range.
//
// Task 2 adds `read_dispersion_backtest_config`, the flat key<TAB>value file
// that authors a `DispersionBacktestConfig` for the surface-db route:
//
//   4. ConfigReaderDefaultsAndOverrides   — a subset file overrides exactly the
//                                           keys it names; every other field is
//                                           default-constructed.
//   5. ConfigReaderParsesEveryDocumentedKey — every key in the header's documented
//                                           list reaches its field, including the
//                                           nested run/strike/friction paths.
//   6. ConfigReaderMapsRemainingEnumTokens— the enum tokens the two tests above
//                                           do not exercise, so the whole token
//                                           table is pinned.
//   7. ConfigReaderRejectsUnknownKeyAndBadValue — unknown key, unparsable number,
//                                           bad enum token, malformed row and a
//                                           missing file are all errors naming the
//                                           offender.
//
// Task 3 adds `universe_from_surface_db`, which reads the db MANIFEST's symbol
// table (not its partitions) into an equal-weight `DispersionUniverse`:
//
//   8. UniverseFromDbEqualWeightsExcludesIndexAndDisabled — the index leaves the
//                                           basket, a disabled symbol never
//                                           enters it, every survivor is 1/n, the
//                                           order is the manifest's sorted one
//                                           and uids stay 0.
//   9. UniverseFromDbMissingIndexIsInvalidArgument — an index that is absent, and
//                                           one that is present but DISABLED, are
//                                           both InvalidArgument naming it and the
//                                           manifest size.
//  10. UniverseFromDbIndexOnlyManifestYieldsEmptyBasket — the 1/n division does
//                                           not run on an empty basket (no inf
//                                           weights); how few names is too few is
//                                           the caller's min_names policy.
//
// Task 4 composes all three into `run_surface_db_dispersion_backtest`, the
// one-call entry point (db root + date window -> timed RunOutcome):
//
//  11. EndToEndOnSyntheticDbWindow       — the equal-weight route runs a real
//                                          dispersion book over EXACTLY the
//                                          window's sessions, prices lots, and
//                                          leaves `run.snapshot_cache` null (the
//                                          Sealed-mmap perf lock).
//  12. EndToEndRejectsEmptyWindowWithAvailableRange — a window outside the corpus
//                                          is InvalidArgument naming the range,
//                                          propagated from `Clock::between`.
//  13. EndToEndUniversePathRoutesThroughReadUniverse — a `universe_path` spec
//                                          takes the point-in-time route and the
//                                          authored weights actually move the
//                                          result off the equal-weight one.
//  14. EndToEndPropagatesStageErrors      — every failing stage (open, between,
//                                          read_universe, universe derivation)
//                                          surfaces its code AND the offender.
//
// Fixtures are synthetic eSSVI surfaces written into a fresh SurfaceDb under
// %TEMP% (make_test_db below), plus config text written to throwaway %TEMP%
// files (write_temp_file); nothing here reads the real data lake.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"              // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"              // Clock, MarketSnapshot, FrictionModel
#include "atx/vol/dispersion.hpp"            // DispersionSide, WeightingScheme, StrikeRule
#include "atx/vol/dispersion_backtest.hpp"   // DispersionBacktestConfig
#include "atx/vol/dispersion_surface_db.hpp" // read_dispersion_backtest_config
#include "atx/vol/priced_surface.hpp"        // PricedSurface, PricingContext
#include "atx/vol/strategy.hpp"              // HedgeSpec
#include "atx/vol/surface_archive.hpp"       // SurfaceArchiveItem
#include "atx/vol/surface_db.hpp"            // SurfaceDb
#include "atx/vol/surface_parity.hpp"        // SliceContext
#include "atx/vol/types.hpp"                 // Result, ErrorCode
#include "atx/vol/vol_curve.hpp"             // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"           // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kDayNs = 86'400'000'000'000LL;

// A synthetic eSSVI PricedSurface (flat forward == spot, genuine American
// premium via q_eff=0.02), 7 slices T in [0.05, 1.0]. Copied from
// surface_db_backtest_test.cpp's make_surface (the sprint's fixture pattern).
[[nodiscard]] PricedSurface make_surface(double S, std::int64_t now_ts, double vol_bump,
                                         std::uint32_t uid) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i) + vol_bump;
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = S;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, S, 0.0, 0.02, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = S;
  pc.r = kR;
  pc.now_ts_ns = now_ts;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  EXPECT_TRUE(ps.has_value()) << (ps.has_value() ? std::string{} : ps.error().to_string());
  return std::move(*ps);
}

// Fresh per-test temp dir under the system temp root, self-cleaning at start so
// a prior crashed run does not leak stale manifest/partition files into this
// run. Copied from surface_db_test.cpp:150-153 (via surface_db_backtest_test.cpp).
[[nodiscard]] fs::path test_root(std::string_view name) {
  auto p = fs::temp_directory_path() / ("atx_surface_db_disp_" + std::string(name));
  fs::remove_all(p);
  return p;
}

// Build a SurfaceDb at `root` with one partition per entry of `dates`, each
// holding every entry of `symbols` (uid = 1-based index, distinct spot and vol
// bump per symbol, gentle per-date spot drift so nothing is degenerate). The
// partition's `now_ts_ns` advances one day per date in `dates` ORDER, so the
// caller may hand dates out of chronological order to exercise sorting.
//
// The manifest's SYMBOL TABLE is seeded too: every `symbols` entry enabled and
// every `disabled` entry disabled. That is a separate step because the table and
// the partitions are ORTHOGONAL namespaces (surface_db.hpp, write_partition):
// writing a partition registers NOTHING, so a db built by partitions alone has
// an empty `symbols()` and no notion of which underlyings it is "for". Anything
// that reads the manifest's universe — Task 3's universe_from_surface_db — sees
// only what is seeded here. `disabled` entries deliberately get NO surface in
// any partition: a switched-off name is exactly the one the fitter stopped
// producing, and the table is what still remembers it.
//
// Seeding goes through the BATCH upsert so the whole table costs one atomic
// manifest rewrite rather than one per symbol.
//
// Shared fixture builder for this file — later tasks in this sprint extend it.
[[nodiscard]] Result<SurfaceDb> make_test_db(const fs::path &root,
                                             const std::vector<std::string_view> &dates,
                                             const std::vector<std::string_view> &symbols,
                                             const std::vector<std::string_view> &disabled = {}) {
  auto db = SurfaceDb::create(root.string());
  if (!db.has_value()) {
    return atx::core::Err(db.error());
  }
  // NB: DbSymbolEntry::symbol is a std::string_view, exactly like
  // SurfaceArchiveItem::symbol below — it must alias `symbols`/`disabled`, which
  // outlive this call, never a temporary std::string.
  std::vector<DbSymbolEntry> entries;
  entries.reserve(symbols.size() + disabled.size());
  for (const std::string_view s : symbols) {
    SymbolFitConfig cfg{};
    cfg.enabled = true;
    entries.push_back(DbSymbolEntry{s, cfg, std::nullopt});
  }
  for (const std::string_view s : disabled) {
    SymbolFitConfig cfg{};
    cfg.enabled = false;
    entries.push_back(DbSymbolEntry{s, cfg, std::nullopt});
  }
  if (auto seeded = db->upsert_symbols(entries); !seeded.has_value()) {
    return atx::core::Err(seeded.error());
  }
  constexpr std::int64_t kBaseTs = 1'700'000'000'000'000'000LL;
  for (std::size_t d = 0; d < dates.size(); ++d) {
    const std::int64_t ts = kBaseTs + static_cast<std::int64_t>(d) * kDayNs;
    std::vector<PricedSurface> surfaces;
    surfaces.reserve(symbols.size());
    for (std::size_t s = 0; s < symbols.size(); ++s) {
      const double spot =
          100.0 * static_cast<double>(s + 1) * (1.0 + 0.002 * static_cast<double>(d));
      surfaces.push_back(
          make_surface(spot, ts, 0.01 * static_cast<double>(s), static_cast<std::uint32_t>(s + 1)));
    }
    // NB: SurfaceArchiveItem::symbol is a std::string_view — it must alias
    // `symbols`, which outlives this call, never a temporary std::string.
    std::vector<SurfaceArchiveItem> items;
    items.reserve(symbols.size());
    for (std::size_t s = 0; s < symbols.size(); ++s) {
      items.push_back(SurfaceArchiveItem{symbols[s], &surfaces[s]});
    }
    auto st = db->write_partition(dates[d], items);
    if (!st.has_value()) {
      return atx::core::Err(st.error());
    }
  }
  return db;
}

// Task 2 config-text fixture: write `text` to a throwaway file under the system
// temp root and hand back its path. `gtest_discover_tests` registers every TEST
// as its own ctest entry, so sibling tests can run in SEPARATE PROCESSES in
// parallel — a bare counter would collide across them. The per-process random
// token plus the in-process counter makes the name unique on both axes. Callers
// remove the file at the end of the test.
[[nodiscard]] fs::path write_temp_file(std::string_view text) {
  static const std::string token = [] {
    std::random_device rd;
    return std::to_string(rd());
  }();
  static int counter = 0;
  const auto path = fs::temp_directory_path() /
                    ("atx_disp_cfg_" + token + "_" + std::to_string(++counter) + ".tsv");
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  EXPECT_TRUE(out.good()) << path.string();
  out << text;
  out.close();
  EXPECT_TRUE(out.good()) << path.string();
  return path;
}

// The four-date corpus every test in this file windows over.
const std::vector<std::string_view> kDates = {"2026-01-05", "2026-01-06", "2026-01-07",
                                              "2026-01-08"};
const std::vector<std::string_view> kSymbols = {"SPY", "AAPL"};

// Task 4's run corpus: six BUSINESS days, 2026-01-05 (Mon) .. 2026-01-12 (Mon),
// so the weekend of the 10th/11th is simply absent — a real db's partition set
// has exactly that shape, and a window whose ends straddle a gap is the case a
// naive "count the days between lo and hi" would get wrong.
const std::vector<std::string_view> kRunDates = {"2026-01-05", "2026-01-06", "2026-01-07",
                                                 "2026-01-08", "2026-01-09", "2026-01-12"};
// SPY plus four basket names, so `min_names = 2` is genuinely satisfied by a
// basket the run had to derive rather than by the only name available.
const std::vector<std::string_view> kRunSymbols = {"SPY", "AAPL", "MSFT", "NVDA", "TSLA"};

// A spec pointed at `root` over the window the E2E tests share. Threads pinned
// to 1: this is a correctness gate, and the engine's bit-identity-across-threads
// contract is pinned elsewhere — here it only buys noise.
[[nodiscard]] SurfaceDbDispersionSpec run_spec(const fs::path &root, std::string_view lo,
                                               std::string_view hi) {
  SurfaceDbDispersionSpec spec;
  spec.db_root = root.string();
  spec.date_lo = std::string(lo);
  spec.date_hi = std::string(hi);
  spec.config.min_names = 2;
  spec.config.entry_every_n = 1;
  spec.config.run.price.n_threads = 1;
  return spec;
}

[[nodiscard]] double peak_open_lots(const BacktestResult &r) {
  return r.n_open_lots.empty()
             ? 0.0
             : *std::max_element(r.n_open_lots.begin(), r.n_open_lots.end());
}

} // namespace

TEST(SurfaceDbDispersionBacktest, BetweenSelectsInclusiveWindow) {
  const auto root = test_root("between_window");
  auto db = make_test_db(root, kDates, kSymbols);
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());
  const auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value());
  ASSERT_EQ(clock->size(), 4u);

  const auto sub = clock->between("2026-01-06", "2026-01-07");
  ASSERT_TRUE(sub.has_value()) << (sub.has_value() ? std::string{} : sub.error().to_string());
  ASSERT_EQ(sub->refs().size(), 2u);
  EXPECT_EQ(sub->refs().front().date, "2026-01-06");
  EXPECT_EQ(sub->refs().back().date, "2026-01-07");
  // Both endpoints are INCLUSIVE: a single-date window keeps exactly that date.
  const auto one = clock->between("2026-01-05", "2026-01-05");
  ASSERT_TRUE(one.has_value());
  ASSERT_EQ(one->size(), 1u);
  EXPECT_EQ(one->refs().front().date, "2026-01-05");
  // The subset carries the source refs whole (path included) and the refs still
  // load, so a windowed clock is directly runnable.
  for (const auto &ref : sub->refs()) {
    auto snap = MarketSnapshot::load(ref.archive_path);
    ASSERT_TRUE(snap.has_value()) << ref.archive_path;
    EXPECT_TRUE(snap->uid_of("SPY").has_value());
  }
  // Subsetting is non-mutating: the source clock is untouched.
  EXPECT_EQ(clock->size(), 4u);
  fs::remove_all(root);
}

TEST(SurfaceDbDispersionBacktest, BetweenClampsToAvailableRange) {
  const auto root = test_root("between_clamp");
  auto db = make_test_db(root, kDates, kSymbols);
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());
  const auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value());

  // Bounds far outside the corpus clamp to what exists — not an error.
  const auto sub = clock->between("2020-01-01", "2030-01-01");
  ASSERT_TRUE(sub.has_value()) << (sub.has_value() ? std::string{} : sub.error().to_string());
  EXPECT_EQ(sub->refs().size(), 4u);
  EXPECT_EQ(sub->refs().front().date, "2026-01-05");
  EXPECT_EQ(sub->refs().back().date, "2026-01-08");
  // One-sided overhang clamps on that side alone.
  const auto lo_open = clock->between("2020-01-01", "2026-01-06");
  ASSERT_TRUE(lo_open.has_value());
  EXPECT_EQ(lo_open->size(), 2u);
  const auto hi_open = clock->between("2026-01-07", "2030-01-01");
  ASSERT_TRUE(hi_open.has_value());
  EXPECT_EQ(hi_open->size(), 2u);
  fs::remove_all(root);
}

TEST(SurfaceDbDispersionBacktest, BetweenEmptyWindowIsInvalidArgument) {
  const auto root = test_root("between_empty");
  auto db = make_test_db(root, kDates, kSymbols);
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());
  const auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value());

  const auto sub = clock->between("2026-01-06T", "2026-01-06A"); // lo > hi lexicographically
  ASSERT_FALSE(sub.has_value());
  EXPECT_EQ(sub.error().code(), ErrorCode::InvalidArgument);

  const auto gap = clock->between("2026-02-01", "2026-02-28"); // no partitions in window
  ASSERT_FALSE(gap.has_value());
  EXPECT_EQ(gap.error().code(), ErrorCode::InvalidArgument);
  // The message must name the available range so the operator can self-serve.
  EXPECT_NE(gap.error().message().find("2026-01-05"), std::string::npos) << gap.error().message();
  EXPECT_NE(gap.error().message().find("2026-01-08"), std::string::npos) << gap.error().message();
  EXPECT_NE(sub.error().message().find("2026-01-05"), std::string::npos) << sub.error().message();
  EXPECT_NE(sub.error().message().find("2026-01-08"), std::string::npos) << sub.error().message();
  fs::remove_all(root);
}

// ── Task 2: read_dispersion_backtest_config ─────────────────────────────────

TEST(SurfaceDbDispersionBacktest, ConfigReaderDefaultsAndOverrides) {
  // Comments and blank lines are skipped; the named keys override and NOTHING
  // else moves off the default-constructed value.
  const auto path = write_temp_file("# worked subset\n"
                                    "\n"
                                    "target_dte_days\t45\n"
                                    "gross_index_vega\t25000\n"
                                    "min_names\t60\n"
                                    "side\tshort_index_long_names\n"
                                    "weighting\tvega_neutral\n"
                                    "strike_rule\tatm_forward_straddle\n");
  const auto cfg = read_dispersion_backtest_config(path);
  ASSERT_TRUE(cfg.has_value()) << (cfg.has_value() ? std::string{} : cfg.error().to_string());
  EXPECT_DOUBLE_EQ(cfg->target_dte_days, 45.0);
  EXPECT_DOUBLE_EQ(cfg->gross_index_vega, 25000.0);
  EXPECT_EQ(cfg->min_names, 60u);
  EXPECT_EQ(cfg->side, DispersionSide::ShortIndexLongNames);
  EXPECT_EQ(cfg->weighting, WeightingScheme::VegaNeutral);
  EXPECT_EQ(cfg->strike.rule, StrikeRule::AtmForwardStraddle);

  const DispersionBacktestConfig defaults{};
  EXPECT_DOUBLE_EQ(cfg->roll_dte_days, defaults.roll_dte_days);
  EXPECT_EQ(cfg->entry_every_n, defaults.entry_every_n);
  EXPECT_DOUBLE_EQ(cfg->delta_band, defaults.delta_band);
  EXPECT_EQ(cfg->record_diagnostics, defaults.record_diagnostics);
  EXPECT_DOUBLE_EQ(cfg->multiplier, defaults.multiplier);
  EXPECT_EQ(cfg->hedge_kind, defaults.hedge_kind);
  EXPECT_EQ(cfg->hedge_cadence, defaults.hedge_cadence);
  EXPECT_DOUBLE_EQ(cfg->strike.log_moneyness, defaults.strike.log_moneyness);
  EXPECT_DOUBLE_EQ(cfg->strike.target_abs_delta, defaults.strike.target_abs_delta);
  EXPECT_EQ(cfg->run.frictions.spread_kind, defaults.run.frictions.spread_kind);
  EXPECT_DOUBLE_EQ(cfg->run.frictions.half_spread_bps, defaults.run.frictions.half_spread_bps);
  EXPECT_DOUBLE_EQ(cfg->run.frictions.per_contract_cost, defaults.run.frictions.per_contract_cost);
  EXPECT_EQ(cfg->run.price.n_threads, defaults.run.price.n_threads);
  EXPECT_EQ(cfg->run.prefetch_depth, defaults.run.prefetch_depth);
  // Fields the reader deliberately does NOT expose stay untouched too.
  EXPECT_EQ(cfg->project_to_calendar_expiry, defaults.project_to_calendar_expiry);
  EXPECT_EQ(cfg->entry, defaults.entry);
  EXPECT_EQ(cfg->holding, defaults.holding);

  // CRLF is not cosmetic here: `core.autocrlf` rewrites the SHIPPED
  // examples/sp100_dispersion_config.tsv to CRLF on a Windows checkout, so a
  // reader that kept the '\r' would fail to parse its own worked example.
  const auto crlf = write_temp_file("# worked subset\r\n\r\ntarget_dte_days\t45\r\n");
  const auto cfg_crlf = read_dispersion_backtest_config(crlf);
  ASSERT_TRUE(cfg_crlf.has_value())
      << (cfg_crlf.has_value() ? std::string{} : cfg_crlf.error().to_string());
  EXPECT_DOUBLE_EQ(cfg_crlf->target_dte_days, 45.0);
  fs::remove(path);
  fs::remove(crlf);
}

TEST(SurfaceDbDispersionBacktest, ConfigReaderParsesEveryDocumentedKey) {
  // Every key the header documents, each set to a NON-default value so a key
  // silently dropped from the dispatch chain fails here.
  const auto path = write_temp_file("target_dte_days\t60\n"
                                    "roll_dte_days\t14\n"
                                    "gross_index_vega\t50000\n"
                                    "delta_band\t0.25\n"
                                    "min_names\t75\n"
                                    "entry_every_n\t5\n"
                                    "record_diagnostics\t1\n"
                                    "multiplier\t50\n"
                                    "side\tlong_index_short_names\n"
                                    "weighting\tgamma_neutral\n"
                                    "strike_rule\tdelta_strangle\n"
                                    "log_moneyness\t-0.05\n"
                                    "target_abs_delta\t0.3\n"
                                    "hedge_kind\tnone\n"
                                    "hedge_cadence\tat_entry\n"
                                    "half_spread_bps\t12.5\n"
                                    "per_contract_cost\t0.65\n"
                                    "n_threads\t4\n"
                                    "prefetch_depth\t3\n");
  const auto cfg = read_dispersion_backtest_config(path);
  ASSERT_TRUE(cfg.has_value()) << (cfg.has_value() ? std::string{} : cfg.error().to_string());
  EXPECT_DOUBLE_EQ(cfg->target_dte_days, 60.0);
  EXPECT_DOUBLE_EQ(cfg->roll_dte_days, 14.0);
  EXPECT_DOUBLE_EQ(cfg->gross_index_vega, 50000.0);
  EXPECT_DOUBLE_EQ(cfg->delta_band, 0.25);
  EXPECT_EQ(cfg->min_names, 75u);
  EXPECT_EQ(cfg->entry_every_n, 5u);
  EXPECT_TRUE(cfg->record_diagnostics);
  EXPECT_DOUBLE_EQ(cfg->multiplier, 50.0);
  EXPECT_EQ(cfg->side, DispersionSide::LongIndexShortNames);
  EXPECT_EQ(cfg->weighting, WeightingScheme::GammaNeutral);
  EXPECT_EQ(cfg->strike.rule, StrikeRule::DeltaStrangle);
  EXPECT_DOUBLE_EQ(cfg->strike.log_moneyness, -0.05);
  EXPECT_DOUBLE_EQ(cfg->strike.target_abs_delta, 0.3);
  EXPECT_EQ(cfg->hedge_kind, HedgeSpec::Kind::None);
  EXPECT_EQ(cfg->hedge_cadence, HedgeSpec::Cadence::AtEntry);
  EXPECT_DOUBLE_EQ(cfg->run.frictions.half_spread_bps, 12.5);
  EXPECT_DOUBLE_EQ(cfg->run.frictions.per_contract_cost, 0.65);
  EXPECT_EQ(cfg->run.price.n_threads, 4u);
  EXPECT_EQ(cfg->run.prefetch_depth, 3u);
  // A nonzero half-spread is only CHARGED under the PriceBps lane, so authoring
  // one must arm that lane or the knob is a silent no-op (see the header note).
  EXPECT_EQ(cfg->run.frictions.spread_kind, FrictionModel::SpreadKind::PriceBps);
  // record_diagnostics is a 0/1 flag, and 0 must round-trip to false.
  const auto off = write_temp_file("record_diagnostics\t0\n");
  const auto cfg_off = read_dispersion_backtest_config(off);
  ASSERT_TRUE(cfg_off.has_value());
  EXPECT_FALSE(cfg_off->record_diagnostics);
  // per_contract_cost alone leaves the spread lane untouched: it is charged
  // independently of `spread_kind`.
  const auto fee = write_temp_file("per_contract_cost\t1.25\n");
  const auto cfg_fee = read_dispersion_backtest_config(fee);
  ASSERT_TRUE(cfg_fee.has_value());
  EXPECT_DOUBLE_EQ(cfg_fee->run.frictions.per_contract_cost, 1.25);
  EXPECT_EQ(cfg_fee->run.frictions.spread_kind, FrictionModel::SpreadKind::None);
  fs::remove(path);
  fs::remove(off);
  fs::remove(fee);
}

TEST(SurfaceDbDispersionBacktest, ConfigReaderMapsRemainingEnumTokens) {
  // The tokens the two tests above do not reach, so every documented token in
  // every documented enum is pinned to a value exactly once across the file.
  const auto path = write_temp_file("weighting\tequal_vega\n"
                                    "strike_rule\tfixed_moneyness\n"
                                    "hedge_kind\tdelta_to_zero\n"
                                    "hedge_cadence\tdaily\n");
  const auto cfg = read_dispersion_backtest_config(path);
  ASSERT_TRUE(cfg.has_value()) << (cfg.has_value() ? std::string{} : cfg.error().to_string());
  EXPECT_EQ(cfg->weighting, WeightingScheme::EqualVega);
  EXPECT_EQ(cfg->strike.rule, StrikeRule::FixedMoneyness);
  EXPECT_EQ(cfg->hedge_kind, HedgeSpec::Kind::DeltaToZero);
  EXPECT_EQ(cfg->hedge_cadence, HedgeSpec::Cadence::Daily);

  const auto theta = write_temp_file("weighting\ttheta_neutral\n");
  const auto cfg_theta = read_dispersion_backtest_config(theta);
  ASSERT_TRUE(cfg_theta.has_value());
  EXPECT_EQ(cfg_theta->weighting, WeightingScheme::ThetaNeutral);
  fs::remove(path);
  fs::remove(theta);
}

TEST(SurfaceDbDispersionBacktest, ConfigReaderRejectsUnknownKeyAndBadValue) {
  const auto key_path = write_temp_file("target_dte_dayz\t45\n");
  const auto bad_key = read_dispersion_backtest_config(key_path);
  ASSERT_FALSE(bad_key.has_value()); // typo safety: unknown key is an error naming the key
  EXPECT_EQ(bad_key.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(bad_key.error().message().find("target_dte_dayz"), std::string::npos)
      << bad_key.error().message();

  const auto val_path = write_temp_file("min_names\tmany\n");
  const auto bad_val = read_dispersion_backtest_config(val_path);
  ASSERT_FALSE(bad_val.has_value());
  EXPECT_EQ(bad_val.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(bad_val.error().message().find("min_names"), std::string::npos)
      << bad_val.error().message();
  EXPECT_NE(bad_val.error().message().find("many"), std::string::npos) << bad_val.error().message();

  const auto enum_path = write_temp_file("side\tsideways\n");
  const auto bad_enum = read_dispersion_backtest_config(enum_path);
  ASSERT_FALSE(bad_enum.has_value());
  EXPECT_EQ(bad_enum.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(bad_enum.error().message().find("sideways"), std::string::npos)
      << bad_enum.error().message();

  // A trailing-garbage number is NOT a partial parse, and a negative count does
  // not wrap into a huge unsigned.
  const auto tail_path = write_temp_file("target_dte_days\t45x\n");
  EXPECT_FALSE(read_dispersion_backtest_config(tail_path).has_value());
  const auto neg_path = write_temp_file("min_names\t-3\n");
  EXPECT_FALSE(read_dispersion_backtest_config(neg_path).has_value());

  // Shape errors: no tab at all, and a key with an empty value.
  const auto shape_path = write_temp_file("target_dte_days 45\n");
  const auto bad_shape = read_dispersion_backtest_config(shape_path);
  ASSERT_FALSE(bad_shape.has_value());
  EXPECT_EQ(bad_shape.error().code(), ErrorCode::InvalidArgument);
  const auto empty_path = write_temp_file("target_dte_days\t\n");
  EXPECT_FALSE(read_dispersion_backtest_config(empty_path).has_value());

  // A missing file is an error, not a silently default-constructed config.
  const auto absent = fs::temp_directory_path() / "atx_disp_cfg_does_not_exist.tsv";
  fs::remove(absent);
  EXPECT_FALSE(read_dispersion_backtest_config(absent).has_value());

  for (const auto &p : {key_path, val_path, enum_path, tail_path, neg_path, shape_path, empty_path})
    fs::remove(p);
}

// ── Task 3: universe_from_surface_db ────────────────────────────────────────

TEST(SurfaceDbDispersionBacktest, UniverseFromDbEqualWeightsExcludesIndexAndDisabled) {
  const auto root = test_root("universe_equal_weight");
  // Deliberately NOT in sorted order, so "the basket comes out sorted" is a
  // claim about the manifest's canonical order and not about the caller's.
  const std::vector<std::string_view> enabled = {"SPY", "NVDA", "AAPL", "MSFT"};
  const std::vector<std::string_view> disabled = {"TSLA"};
  auto db = make_test_db(root, kDates, enabled, disabled);
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());
  // Fixture precondition, pinned through the PUBLIC config accessor: the
  // manifest carries all five and TSLA's stored config is the disabled one. This
  // is what ties the derivation below to `SymbolFitConfig::enabled` rather than
  // to whatever bit the derivation happens to read.
  EXPECT_EQ(db->symbols().size(), 5u);
  const auto tsla = db->symbol_config("TSLA");
  ASSERT_TRUE(tsla.has_value()) << (tsla.has_value() ? std::string{} : tsla.error().to_string());
  EXPECT_FALSE(tsla->enabled);
  const auto spy = db->symbol_config("SPY");
  ASSERT_TRUE(spy.has_value());
  EXPECT_TRUE(spy->enabled);

  const auto u = universe_from_surface_db(*db, "SPY");
  ASSERT_TRUE(u.has_value()) << (u.has_value() ? std::string{} : u.error().to_string());
  EXPECT_EQ(u->index.symbol, "SPY");
  EXPECT_DOUBLE_EQ(u->index.weight, 1.0);
  ASSERT_EQ(u->names.size(), 3u); // AAPL, MSFT, NVDA — no SPY, no TSLA
  for (const auto &m : u->names) {
    EXPECT_DOUBLE_EQ(m.weight, 1.0 / 3.0);
    EXPECT_NE(m.symbol, "SPY");  // the index never doubles as a basket name
    EXPECT_NE(m.symbol, "TSLA"); // a disabled symbol never enters the basket
  }
  // Deterministic order: `SurfaceDb::symbols()` is the manifest's canonical
  // ascending order (DbManifest::open rejects records that are not strictly
  // ascending), so the derivation preserves sortedness without sorting.
  EXPECT_TRUE(std::is_sorted(u->names.begin(), u->names.end(),
                             [](const DispersionMember &a, const DispersionMember &b) {
                               return a.symbol < b.symbol;
                             }));
  EXPECT_EQ(u->names[0].symbol, "AAPL");
  EXPECT_EQ(u->names[1].symbol, "MSFT");
  EXPECT_EQ(u->names[2].symbol, "NVDA");

  // uid fields are 0 here: uids are snapshot-local and rebound per step by
  // resolve_universe_uids via MarketSnapshot::uid_of — assert that contract.
  EXPECT_EQ(u->index.uid, 0u);
  for (const auto &m : u->names) {
    EXPECT_EQ(m.uid, 0u);
  }

  // The index is matched case-insensitively (the manifest stores canonical
  // upper-case), and the members carry the CANONICAL spelling either way, so an
  // operator's lower-case config key cannot produce a universe whose symbols
  // fail to match the snapshot directory later.
  const auto lower = universe_from_surface_db(*db, "spy");
  ASSERT_TRUE(lower.has_value()) << (lower.has_value() ? std::string{} : lower.error().to_string());
  EXPECT_EQ(lower->index.symbol, "SPY");
  EXPECT_EQ(lower->names.size(), 3u);
  fs::remove_all(root);
}

TEST(SurfaceDbDispersionBacktest, UniverseFromDbMissingIndexIsInvalidArgument) {
  const auto root = test_root("universe_missing_index");
  const std::vector<std::string_view> enabled = {"SPY", "AAPL", "MSFT"};
  const std::vector<std::string_view> disabled = {"TSLA"};
  auto db = make_test_db(root, kDates, enabled, disabled);
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());

  const auto u = universe_from_surface_db(*db, "QQQ"); // not in the manifest
  ASSERT_FALSE(u.has_value());
  EXPECT_EQ(u.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(u.error().message().find("QQQ"), std::string::npos) << u.error().message();
  // The manifest's symbol count too: it separates "typo in the index" from
  // "pointed at the wrong db" without the operator having to dump the manifest.
  EXPECT_NE(u.error().message().find("4 symbols"), std::string::npos) << u.error().message();

  // A symbol that IS in the manifest but is switched off is not a usable index
  // leg either — the enabled filter runs before the index match, so a disabled
  // index reads as absent and is rejected the same way.
  const auto off = universe_from_surface_db(*db, "TSLA");
  ASSERT_FALSE(off.has_value());
  EXPECT_EQ(off.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(off.error().message().find("TSLA"), std::string::npos) << off.error().message();
  fs::remove_all(root);
}

TEST(SurfaceDbDispersionBacktest, UniverseFromDbIndexOnlyManifestYieldsEmptyBasket) {
  // A manifest whose only enabled symbol IS the index: the 1/n division must not
  // run on an empty basket. An inf/NaN weight here would flow straight into
  // sizing, so the guard is asserted rather than assumed. Deciding that a basket
  // is too thin belongs to the caller (DispersionBacktestConfig::min_names), so
  // this is an empty universe and not an error.
  const auto root = test_root("universe_index_only");
  const std::vector<std::string_view> enabled = {"SPY"};
  const std::vector<std::string_view> disabled = {"AAPL"};
  auto db = make_test_db(root, kDates, enabled, disabled);
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());

  const auto u = universe_from_surface_db(*db, "SPY");
  ASSERT_TRUE(u.has_value()) << (u.has_value() ? std::string{} : u.error().to_string());
  EXPECT_EQ(u->index.symbol, "SPY");
  EXPECT_TRUE(u->names.empty());
  fs::remove_all(root);
}

// ── Task 4: run_surface_db_dispersion_backtest ──────────────────────────────

TEST(SurfaceDbDispersionBacktest, EndToEndOnSyntheticDbWindow) {
  const auto root = test_root("e2e_window");
  auto db = make_test_db(root, kRunDates, kRunSymbols);
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());

  SurfaceDbDispersionSpec spec = run_spec(root, "2026-01-06", "2026-01-09");
  const auto out = run_surface_db_dispersion_backtest(spec);
  ASSERT_TRUE(out.has_value()) << (out.has_value() ? std::string{} : out.error().to_string());

  // EXACTLY the window's sessions: a fresh run records the inception row plus one
  // per subsequent ref, so the row count is the windowed clock's ref count. The
  // corpus has six partitions; seeing six here would mean the window was dropped.
  EXPECT_EQ(out->result.size(), 4u);
  ASSERT_EQ(out->result.date.size(), 4u);
  EXPECT_EQ(out->result.date.front(), "2026-01-06");
  EXPECT_EQ(out->result.date.back(), "2026-01-09");
  for (const std::string &d : out->result.date) {
    EXPECT_NE(d, "2026-01-05"); // before the window
    EXPECT_NE(d, "2026-01-12"); // after it
  }
  for (const double nav : out->result.nav) {
    EXPECT_TRUE(std::isfinite(nav)) << nav;
  }
  EXPECT_GT(out->stats.n_steps, 0u);
  EXPECT_EQ(out->stats.n_steps, static_cast<std::uint64_t>(out->result.size()));
  EXPECT_GT(out->stats.wall_clock_ms, 0.0);
  EXPECT_TRUE(std::isfinite(out->sheet.total_return));

  // TEETH. A run that built no book at all would satisfy every assertion above
  // (four rows of zeros), so the gate that this is a real dispersion backtest is
  // that lots were opened and the book carried vega.
  EXPECT_GT(peak_open_lots(out->result), 0.0);
  // `gross_vega_abs` is populated by `run_backtest` and EMPTY on any result that
  // did not come from it (backtest.hpp), so the emptiness check is the assertion
  // that this really is an engine result — and it keeps max_element off end().
  ASSERT_FALSE(out->result.gross_vega_abs.empty());
  EXPECT_GT(*std::max_element(out->result.gross_vega_abs.begin(), out->result.gross_vega_abs.end()),
            0.0);

  // PERF LOCK (the plan's locked decision). The route must NOT install a shared
  // SnapshotCache: a null `run.snapshot_cache` is what makes the engine build its
  // PRIVATE cache, and only the private one may mmap `ArchiveBacking::Sealed`
  // (backtest.cpp). `run_timed` reports `cfg.run.snapshot_cache->stats()` when a
  // shared cache was supplied and a ZEROED SnapshotCacheStats otherwise, so an
  // installed cache would show nonzero loads over a four-date run.
  EXPECT_EQ(spec.config.run.snapshot_cache, nullptr);
  EXPECT_EQ(out->stats.cache.loads, 0u);
  EXPECT_EQ(out->stats.cache.hits, 0u);

  // A single-session window is legal and yields exactly the inception row.
  const auto one = run_surface_db_dispersion_backtest(run_spec(root, "2026-01-07", "2026-01-07"));
  ASSERT_TRUE(one.has_value()) << (one.has_value() ? std::string{} : one.error().to_string());
  ASSERT_EQ(one->result.size(), 1u);
  EXPECT_EQ(one->result.date.front(), "2026-01-07");
  fs::remove_all(root);
}

TEST(SurfaceDbDispersionBacktest, EndToEndRejectsEmptyWindowWithAvailableRange) {
  const auto root = test_root("e2e_empty_window");
  auto db = make_test_db(root, kRunDates, kRunSymbols);
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());

  // A window the corpus does not cover is an ERROR, not an empty run: a zero-row
  // tearsheet would read as "the strategy did nothing" rather than "you asked for
  // dates this db has none of".
  const auto out = run_surface_db_dispersion_backtest(run_spec(root, "2027-01-01", "2027-02-01"));
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().code(), ErrorCode::InvalidArgument);
  // `Clock::between`'s available-range text must survive the composition, so the
  // operator can correct the window from the message alone.
  EXPECT_NE(out.error().message().find("2026-01-05"), std::string::npos) << out.error().message();
  EXPECT_NE(out.error().message().find("2026-01-12"), std::string::npos) << out.error().message();
  fs::remove_all(root);
}

TEST(SurfaceDbDispersionBacktest, EndToEndUniversePathRoutesThroughReadUniverse) {
  const auto root = test_root("e2e_universe_path");
  auto db = make_test_db(root, kRunDates, kRunSymbols);
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());

  // Two of the four names, deliberately UNEQUALLY weighted and deliberately NOT
  // the equal-weight basket the manifest would derive.
  const auto universe = write_temp_file("effective_date\tsymbol\traw_weight\tsource\tas_of\n"
                                        "2026-01-01\tAAPL\t0.75\ttest\t2026-01-01\n"
                                        "2026-01-01\tMSFT\t0.25\ttest\t2026-01-01\n");
  SurfaceDbDispersionSpec spec = run_spec(root, "2026-01-06", "2026-01-09");
  spec.universe_path = universe;
  const auto out = run_surface_db_dispersion_backtest(spec);
  ASSERT_TRUE(out.has_value()) << (out.has_value() ? std::string{} : out.error().to_string());
  EXPECT_EQ(out->result.size(), 4u);
  EXPECT_EQ(out->result.date.front(), "2026-01-06");
  EXPECT_GT(peak_open_lots(out->result), 0.0);

  // TEETH: the file is READ, not merely accepted. A two-name, 75/25 basket cannot
  // produce the same NAV track as the manifest's four-name equal-weight one, so an
  // implementation that ignored `universe_path` and fell through to the
  // equal-weight route would match here.
  const auto equal = run_surface_db_dispersion_backtest(run_spec(root, "2026-01-06", "2026-01-09"));
  ASSERT_TRUE(equal.has_value()) << (equal.has_value() ? std::string{} : equal.error().to_string());
  ASSERT_EQ(equal->result.nav.size(), out->result.nav.size());
  EXPECT_NE(equal->result.nav.back(), out->result.nav.back());

  fs::remove(universe);
  fs::remove_all(root);
}

TEST(SurfaceDbDispersionBacktest, EndToEndPropagatesStageErrors) {
  const auto root = test_root("e2e_stage_errors");
  auto db = make_test_db(root, kRunDates, kRunSymbols);
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());

  // Stage 1 — SurfaceDb::open. A db root that does not exist must name itself, so
  // a typo'd --db is self-diagnosing rather than "NotFound".
  const auto absent_root = (fs::temp_directory_path() / "atx_surface_db_disp_no_such_db").string();
  fs::remove_all(absent_root);
  SurfaceDbDispersionSpec missing_db = run_spec(root, "2026-01-06", "2026-01-09");
  missing_db.db_root = absent_root;
  const auto no_db = run_surface_db_dispersion_backtest(missing_db);
  ASSERT_FALSE(no_db.has_value());
  EXPECT_NE(no_db.error().message().find("SurfaceDb::open"), std::string::npos)
      << no_db.error().message();
  EXPECT_NE(no_db.error().message().find("atx_surface_db_disp_no_such_db"), std::string::npos)
      << no_db.error().message();

  // Stage 2 — Clock::from_surface_db. A db whose manifest exists but holds NO
  // partition has no timeline at all. Without the stage label its InvalidArgument
  // would read like a bad window, sending the operator to fix `--from/--to` on a
  // db that has no dates whatsoever.
  const auto bare_root = test_root("e2e_no_partitions");
  {
    auto bare = SurfaceDb::create(bare_root.string());
    ASSERT_TRUE(bare.has_value()) << (bare.has_value() ? std::string{} : bare.error().to_string());
  }
  const auto no_clock =
      run_surface_db_dispersion_backtest(run_spec(bare_root, "2026-01-06", "2026-01-09"));
  ASSERT_FALSE(no_clock.has_value());
  EXPECT_NE(no_clock.error().message().find("Clock::from_surface_db"), std::string::npos)
      << no_clock.error().message();
  fs::remove_all(bare_root);

  // Stage 4a — read_universe. A universe path that does not exist is an error, not
  // a silent fallback to the equal-weight route (which would run a DIFFERENT book
  // than the operator authored and never say so).
  SurfaceDbDispersionSpec missing_universe = run_spec(root, "2026-01-06", "2026-01-09");
  missing_universe.universe_path = fs::temp_directory_path() / "atx_disp_universe_absent.tsv";
  fs::remove(*missing_universe.universe_path);
  const auto no_universe = run_surface_db_dispersion_backtest(missing_universe);
  ASSERT_FALSE(no_universe.has_value());
  EXPECT_NE(no_universe.error().message().find("read_universe"), std::string::npos)
      << no_universe.error().message();
  EXPECT_NE(no_universe.error().message().find("atx_disp_universe_absent"), std::string::npos)
      << no_universe.error().message();

  // Stage 4b — universe_from_surface_db. An index the manifest does not carry is
  // InvalidArgument naming the caller's spelling.
  SurfaceDbDispersionSpec bad_index = run_spec(root, "2026-01-06", "2026-01-09");
  bad_index.index_symbol = "QQQ";
  const auto no_index = run_surface_db_dispersion_backtest(bad_index);
  ASSERT_FALSE(no_index.has_value());
  EXPECT_EQ(no_index.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(no_index.error().message().find("universe_from_surface_db"), std::string::npos)
      << no_index.error().message();
  EXPECT_NE(no_index.error().message().find("QQQ"), std::string::npos) << no_index.error().message();

  // Every stage failure names the ENTRY POINT as well as the stage: these errors
  // reach an operator through a CLI that may have run several things, and "which
  // call produced this" must be answerable from the message alone.
  for (const std::string &m : {no_db.error().message(), no_clock.error().message(),
                               no_universe.error().message(), no_index.error().message()}) {
    EXPECT_NE(m.find("run_surface_db_dispersion_backtest"), std::string::npos) << m;
  }

  fs::remove_all(root);
}
