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
// Task 5 is the correctness-evidence layer: the route on PRODUCTION-SHAPED data,
// i.e. a db with CLUSTERED absence (one session missing a whole cohort of names,
// the shape the real 2025-11-24 has — 12 of 102 — and NOT one name per date):
//
//  15. ClusteredAbsenceDropsAndRenormalizes — a cohort absent on the entry session
//                                          is dropped, the basket RENORMALIZES to
//                                          stay vega-neutral over the survivors,
//                                          the run continues, and the drop is
//                                          reported per row on `signals`.
//  16. ClusteredAbsenceUnderAHeldBookFailsLoudly — the same cohort absent AFTER
//                                          entry is a hard NotFound: held lots
//                                          with no surface abort the run rather
//                                          than truncate NAV in silence.
//  17. ClusteredAbsenceBelowMinNamesIsADiagnosedNoTradeStep — too few survivors is
//                                          `Unavailable` at the book builder and
//                                          the engine's NO-TRADE CONTRACT at the
//                                          run: no lots, a diagnosed row, and a
//                                          recovery on the next full session.
//  18. MissingIndexOnOneDateFailsLoudly   — the index is never droppable: absent
//                                          at entry it is a resolve NotFound
//                                          naming it; absent under a held book it
//                                          is the unpriced-lot abort.
//  19. BitIdenticalAcrossThreadCounts     — the standing engine contract, bit_cast
//                                          exact, over an absence-carrying corpus.
//  20. WindowSubsetMatchesFullRunPrefix   — a sub-window agrees with the full run
//                                          bit-for-bit on the shared prefix.
//
// Task 6 is the performance-evidence layer: the two levers this route deliberately
// leans on are the engine's PRIVATE (Sealed-mmap) snapshot cache and its snapshot
// look-ahead, and neither is visible in the run's output — only in how many times
// an archive is opened. It also adds the config reader's `unpriced` key, because
// the real sp100 window CANNOT run under the engine's fail-closed default and an
// operator's only other options were to shorten the window or edit code (tests 4-7
// cover the key; test 22 is what discovered the need).
//
//  21. PrivateCachePathIsUsed            — one archive OPEN per session at every
//                                          look-ahead depth (no reloads), the
//                                          zeroed `stats.cache` that is the
//                                          private path's signature, and a
//                                          shared-cache CONTROL that shows what
//                                          installing one would cost.
//  22. RealSp100Baseline                 — env-gated (ATX_SP100_SURFACE_DB), the
//                                          production sp100 surface db over the
//                                          longest window that corpus can actually
//                                          replay (it is missing the INDEX on 18 of
//                                          its 140 sessions — see the test's own
//                                          note). READ-ONLY, no hard wall-clock
//                                          assertion (the number goes in the sprint
//                                          report); SKIPPED unless the operator
//                                          points it at a db.
//
// Post-review closure: the config tests above all parse text this file AUTHORS,
// which leaves the file operators actually COPY guarded by nothing:
//
//  23. ShippedExampleConfigParses        — examples/sp100_dispersion_config.tsv,
//                                          read off disk via the configure-time
//                                          ATX_SP100_DISPERSION_CONFIG path: it
//                                          parses, its documented values land,
//                                          and it still names NEITHER `unpriced`
//                                          nor `hedge_kind` (the fail-closed
//                                          absence the quickstart overlays).
//
// Fixtures are synthetic eSSVI surfaces written into a fresh SurfaceDb under
// %TEMP% (make_test_db below), plus config text written to throwaway %TEMP%
// files (write_temp_file). Test 23 reads one committed repo file, READ-ONLY.
// Only test 22 reads the real data lake, only when its env var is set, and it
// opens that tree strictly for READING.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib> // std::getenv / _dupenv_s (the env-gated real-db baseline)
#include <filesystem>
#include <fstream>
#include <iostream> // std::cerr (the baseline's numbers go to the report, not an assert)
#include <memory>
#include <numeric> // std::accumulate (the baseline's unpriced-lot total)
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"              // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"              // Clock, MarketSnapshot, FrictionModel
#include "atx/vol/dispersion.hpp"            // DispersionSide, WeightingScheme, StrikeRule
#include "atx/vol/research/dispersion_backtest.hpp"   // DispersionBacktestConfig
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
// Task 5's per-date absence control: `{date, {symbols}}` means the partition for
// `date` is written WITHOUT those symbols. Absence is exactly "not in that
// partition's archive" — there is no tombstone and no null surface — so a reader
// of that session's snapshot simply cannot resolve the symbol.
//
// CLUSTERED, NOT UNIFORM. A real db loses a whole cohort of names in ONE session
// (2025-11-24 misses 12 of 102) because the loss is a fitter/feed outage, not an
// independent per-name coin flip. A fixture that sprinkles one absence per date
// exercises a code path no production date ever takes, and hid two defects in the
// previous sprint. Every use of this parameter therefore names ONE date and
// SEVERAL symbols.
//
// NB: both the date and the symbol views are non-owning, exactly like
// `SurfaceArchiveItem::symbol` — they must alias storage that outlives the call.
using DateAbsence = std::vector<std::pair<std::string_view, std::vector<std::string_view>>>;

// Shared fixture builder for this file — later tasks in this sprint extend it.
[[nodiscard]] Result<SurfaceDb> make_test_db(const fs::path &root,
                                             const std::vector<std::string_view> &dates,
                                             const std::vector<std::string_view> &symbols,
                                             const std::vector<std::string_view> &disabled = {},
                                             const DateAbsence &absent = {}) {
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
    // Which of `symbols` this partition actually carries. The surviving symbols
    // keep the spot / vol-bump / uid their GLOBAL index `s` gives them, so a
    // partition written with an absence is bit-identical, for every surviving
    // name, to the same partition written without one — the absence is the only
    // difference between the two corpora.
    std::vector<std::size_t> present;
    present.reserve(symbols.size());
    for (std::size_t s = 0; s < symbols.size(); ++s) {
      bool gone = false;
      for (const auto &[date, missing] : absent) {
        if (date != dates[d]) {
          continue;
        }
        gone = std::find(missing.begin(), missing.end(), symbols[s]) != missing.end();
        if (gone) {
          break;
        }
      }
      if (!gone) {
        present.push_back(s);
      }
    }
    std::vector<PricedSurface> surfaces;
    surfaces.reserve(present.size()); // reserved exactly: `&surfaces[k]` below must stay valid
    for (const std::size_t s : present) {
      const double spot =
          100.0 * static_cast<double>(s + 1) * (1.0 + 0.002 * static_cast<double>(d));
      surfaces.push_back(
          make_surface(spot, ts, 0.01 * static_cast<double>(s), static_cast<std::uint32_t>(s + 1)));
    }
    // NB: SurfaceArchiveItem::symbol is a std::string_view — it must alias
    // `symbols`, which outlives this call, never a temporary std::string.
    std::vector<SurfaceArchiveItem> items;
    items.reserve(present.size());
    for (std::size_t k = 0; k < present.size(); ++k) {
      items.push_back(SurfaceArchiveItem{symbols[present[k]], &surfaces[k]});
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

// ── Task 5 fixtures: production-shaped (CLUSTERED) absence ──────────────────
//
// SPY plus FIVE basket names over four sessions, so one session can lose TWO
// names at once and still leave three survivors — enough to keep a `min_names=2`
// run tradeable and to make a `min_names=4` run untradeable on exactly that
// session. Two of five in one session is the same SHAPE as the real 2025-11-24
// (12 of 102 in one session): a cohort, not a sprinkle.
const std::vector<std::string_view> kAbsDates = {"2026-01-05", "2026-01-06", "2026-01-07",
                                                 "2026-01-08"};
const std::vector<std::string_view> kAbsSymbols = {"SPY",  "AAPL", "AMZN",
                                                   "GOOG", "MSFT", "NVDA"};
// The cohort that goes missing together. Named once so every absence test drops
// the SAME set and a reader cannot mistake one test's absence for another's.
const std::vector<std::string_view> kAbsentCohort = {"MSFT", "NVDA"};

// The strategy diagnostics channel is `BacktestResult::signals`: an
// insertion-ordered (name -> per-recorded-row series) list, parallel to `date`,
// populated only when `DispersionBacktestConfig::record_diagnostics` is set.
// `DispersionStrategy::signals` publishes `implied_corr`, `n_names_dropped`,
// `corr_vega` and `corr_gamma` on that channel.
[[nodiscard]] const std::vector<double> *signal_series(const BacktestResult &r,
                                                       std::string_view name) {
  for (const auto &[key, series] : r.signals) {
    if (key == name) {
      return &series;
    }
  }
  return nullptr;
}

// The archive path of one date in a db, so a test can load exactly that session's
// snapshot and interrogate the strategy's per-name drop list on it.
[[nodiscard]] std::string archive_path_of(const SurfaceDb &db, std::string_view date) {
  const auto clock = Clock::from_surface_db(db);
  EXPECT_TRUE(clock.has_value());
  if (!clock.has_value()) {
    return {};
  }
  for (const auto &ref : clock->refs()) {
    if (ref.date == date) {
      return ref.archive_path;
    }
  }
  ADD_FAILURE() << "no partition for " << date;
  return {};
}

// The SHIPPED example config's absolute path, or "" when it cannot be found.
// Prefers the configure-time path baked by tests/CMakeLists.txt
// (ATX_SP100_DISPERSION_CONFIG), then a few relative roots so the test also runs
// from a hand-launched exe. Same probe shape as amzn_earnings_test.cpp's
// find_fixture (ATX_AMZN_FIXTURE).
constexpr const char *kShippedConfigRel = "atx-vol/examples/sp100_dispersion_config.tsv";

[[nodiscard]] std::string shipped_example_config() {
  std::vector<std::string> candidates;
#ifdef ATX_SP100_DISPERSION_CONFIG
  candidates.emplace_back(ATX_SP100_DISPERSION_CONFIG);
#endif
  candidates.emplace_back(kShippedConfigRel);
  candidates.emplace_back(std::string("../") + kShippedConfigRel);
  candidates.emplace_back(std::string("../../") + kShippedConfigRel);
  for (const std::string &c : candidates) {
    std::error_code ec;
    if (fs::exists(c, ec) && !ec) {
      return c;
    }
  }
  return {};
}

// ── Task 6 helpers ──────────────────────────────────────────────────────────

// `name`'s value, or "" when unset/empty. Read with `_dupenv_s` under
// MSVC/clang-cl: plain `std::getenv` trips /WX (-Wdeprecated-declarations).
// Same pattern as mag7_dispersion_backtest_test.cpp:200-214.
[[nodiscard]] std::string env_or_empty(const char *name) {
#if defined(_MSC_VER)
  char *raw = nullptr;
  std::size_t n = 0;
  if (::_dupenv_s(&raw, &n, name) != 0 || raw == nullptr) {
    return {};
  }
  std::string out(raw);
  std::free(raw);
  return out;
#else
  const char *raw = std::getenv(name);
  return raw == nullptr ? std::string{} : std::string(raw);
#endif
}

// Bit-exact agreement of the two series every determinism gate in this file
// compares. `std::bit_cast<std::uint64_t>` — no tolerance, because "close enough
// across a perf knob" is precisely the regression these gates exist to catch.
void expect_bit_identical_track(const BacktestResult &a, const BacktestResult &b) {
  ASSERT_EQ(a.size(), b.size());
  ASSERT_EQ(a.nav.size(), b.nav.size());
  ASSERT_EQ(a.pnl_total.size(), b.pnl_total.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a.nav[i]), std::bit_cast<std::uint64_t>(b.nav[i]))
        << "nav row " << i;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a.pnl_total[i]),
              std::bit_cast<std::uint64_t>(b.pnl_total[i]))
        << "pnl_total row " << i;
  }
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
  // The unpriced-lot policy is the one key whose default is a SAFETY property: a
  // config that does not name it must still fail closed, so a corpus that loses a
  // held name mid-run aborts instead of quietly truncating NAV. Pinned against the
  // engine's own default AND against the literal, so neither can drift alone.
  EXPECT_EQ(cfg->run.unpriced, defaults.run.unpriced);
  EXPECT_EQ(cfg->run.unpriced, UnpricedLotPolicy::Error);
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
                                    "prefetch_depth\t3\n"
                                    "unpriced\texclude_and_report\n");
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
  // `exclude_and_report` is the non-default token, so this assertion fails if the
  // key is dropped from the dispatch chain (which would leave the fail-closed
  // default in place and silently run a DIFFERENT policy than the file names).
  EXPECT_EQ(cfg->run.unpriced, UnpricedLotPolicy::ExcludeAndReport);
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
                                    "hedge_cadence\tdaily\n"
                                    "unpriced\terror\n");
  const auto cfg = read_dispersion_backtest_config(path);
  ASSERT_TRUE(cfg.has_value()) << (cfg.has_value() ? std::string{} : cfg.error().to_string());
  EXPECT_EQ(cfg->weighting, WeightingScheme::EqualVega);
  EXPECT_EQ(cfg->strike.rule, StrikeRule::FixedMoneyness);
  EXPECT_EQ(cfg->hedge_kind, HedgeSpec::Kind::DeltaToZero);
  EXPECT_EQ(cfg->hedge_cadence, HedgeSpec::Cadence::Daily);
  // `error` is also the default, so this row proves the token is ACCEPTED (an
  // unrecognized one is InvalidArgument, pinned below) rather than that it landed.
  EXPECT_EQ(cfg->run.unpriced, UnpricedLotPolicy::Error);

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

  // The unpriced-lot policy gets its OWN rejection case rather than riding on
  // `side`'s: it is the one key whose default is a safety property, so a token
  // this reader does not recognize must NEVER fall through to a default — the
  // operator would get a fail-closed run while their file said the opposite (or,
  // worse, the reverse). Both the key and the offending token are named.
  const auto policy_path = write_temp_file("unpriced\texclude\n");
  const auto bad_policy = read_dispersion_backtest_config(policy_path);
  ASSERT_FALSE(bad_policy.has_value());
  EXPECT_EQ(bad_policy.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(bad_policy.error().message().find("unpriced"), std::string::npos)
      << bad_policy.error().message();
  EXPECT_NE(bad_policy.error().message().find("exclude"), std::string::npos)
      << bad_policy.error().message();

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

  for (const auto &p : {key_path, val_path, enum_path, policy_path, tail_path, neg_path, shape_path,
                        empty_path})
    fs::remove(p);
}

TEST(SurfaceDbDispersionBacktest, ShippedExampleConfigParses) {
  // The four tests above parse configs this file AUTHORS. This one parses the
  // file we SHIP — examples/sp100_dispersion_config.tsv, which the operator guide
  // tells operators to `Copy-Item` and which §4's table reproduces verbatim. It is
  // not a build input, so before this test a renamed key, a fat-fingered enum
  // token or a space-instead-of-tab in it would ship broken and only surface in a
  // hand run.
  const std::string path = shipped_example_config();
  ASSERT_FALSE(path.empty()) << "sp100_dispersion_config.tsv not found; expected the "
                                "ATX_SP100_DISPERSION_CONFIG path baked by tests/CMakeLists.txt";
  const auto cfg = read_dispersion_backtest_config(path);
  ASSERT_TRUE(cfg.has_value()) << (cfg.has_value() ? std::string{} : cfg.error().to_string());

  // The production shape, pinned against the doc's §4 table. `min_names` and
  // `entry_every_n` are additionally what RealSp100Baseline hard-codes to mirror
  // this file, so a drift here and a drift there cannot cancel out silently.
  EXPECT_DOUBLE_EQ(cfg->target_dte_days, 30.0);
  EXPECT_DOUBLE_EQ(cfg->roll_dte_days, 7.0);
  EXPECT_DOUBLE_EQ(cfg->gross_index_vega, 10000.0);
  EXPECT_EQ(cfg->min_names, 60u);
  EXPECT_EQ(cfg->entry_every_n, 21u);
  EXPECT_EQ(cfg->side, DispersionSide::ShortIndexLongNames);
  EXPECT_EQ(cfg->weighting, WeightingScheme::VegaNeutral);
  EXPECT_EQ(cfg->strike.rule, StrikeRule::AtmForwardStraddle);
  EXPECT_TRUE(cfg->record_diagnostics);
  EXPECT_EQ(cfg->run.price.n_threads, 0u); // 0 => hardware concurrency
  EXPECT_EQ(cfg->run.prefetch_depth, 2u);

  // The load-bearing ABSENCES. The shipped file names neither `unpriced` nor
  // `hedge_kind`, so both must come back as the engine's fail-closed defaults —
  // which is exactly WHY the quickstart overlays those two keys onto a COPY
  // instead of editing this file. Adding either key here would silently turn the
  // documented "deliberately left fail-closed" production shape into something
  // else, and make the quickstart's two Add-Content lines a no-op.
  EXPECT_EQ(cfg->run.unpriced, UnpricedLotPolicy::Error);
  EXPECT_EQ(cfg->hedge_kind, HedgeSpec::Kind::DeltaToZero);
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

// ── Task 5: correctness on production-shaped (CLUSTERED-ABSENCE) data ───────
//
// Every test below runs the route over a db in which ONE session is missing a
// COHORT of names — the shape a real db actually has (2025-11-24 misses 12 of
// 102 in one session) — because absence is a fitter/feed outage and not an
// independent per-name coin flip. WHERE in the window that session falls decides
// which engine contract is under test, and the three answers are genuinely
// different behaviours:
//
//   absence on the ENTRY session   -> the names are dropped before they can be
//                                     bought, the basket renormalizes over the
//                                     survivors, the run continues (15/17);
//   absence UNDER A HELD BOOK      -> the held straddles have no mark, and
//                                     `UnpricedLotPolicy::Error` (the RunConfig
//                                     default) ABORTS the run rather than let
//                                     NAV silently truncate (16);
//   absence taking survivors below
//   `min_names` on the entry step  -> the engine's documented NO-TRADE CONTRACT:
//                                     no lots are opened, the run continues, and
//                                     the step is DIAGNOSED (17).
//
// A uniform "one name missing per date" fixture would exercise none of the three
// distinctly, which is exactly how two defects survived the previous sprint.

TEST(SurfaceDbDispersionBacktest, ClusteredAbsenceDropsAndRenormalizes) {
  // The absent cohort goes missing on the window's ENTRY session, so the names
  // never enter the book and the drop/renormalize path is what runs.
  const auto root = test_root("t5_clustered_absence");
  auto db =
      make_test_db(root, kAbsDates, kAbsSymbols, {}, DateAbsence{{"2026-01-05", kAbsentCohort}});
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());

  const auto u = universe_from_surface_db(*db, "SPY");
  ASSERT_TRUE(u.has_value()) << (u.has_value() ? std::string{} : u.error().to_string());
  ASSERT_EQ(u->names.size(), 5u); // the MANIFEST still carries all five: absence is per-PARTITION
  DispersionBacktestConfig probe_cfg;
  probe_cfg.min_names = 2;
  probe_cfg.record_diagnostics = true;
  const DispersionStrategy probe = make_dispersion_backtest_strategy(*u, probe_cfg);
  const auto snap = MarketSnapshot::load(archive_path_of(*db, "2026-01-05"));
  ASSERT_TRUE(snap.has_value());

  // PER-NAME DROP LIST. `DispersionStrategy::dropped_on` is the only accessor that
  // carries the REASON (the run-level channel carries a count); it is pinned here
  // so a future refactor cannot reclassify a whole-symbol absence as something
  // else. The reason is `NotInSnapshot`, NOT `SurfaceNotFound`: the symbol is
  // absent from the partition's DIRECTORY, so `MarketSnapshot::uid_of` never
  // returns a uid and the resolve stage drops it. `SurfaceNotFound` is the
  // strictly later failure — a uid that resolved but has no surface in the
  // `SurfaceSet` — which a per-partition absence cannot produce.
  const std::vector<DroppedName> names = probe.dropped_on(*snap);
  ASSERT_EQ(names.size(), 2u);
  EXPECT_EQ(names[0].symbol, "MSFT");
  EXPECT_EQ(names[1].symbol, "NVDA");
  EXPECT_EQ(names[0].reason, DropReason::NotInSnapshot);
  EXPECT_EQ(names[1].reason, DropReason::NotInSnapshot);
  EXPECT_NE(names[0].detail.find("MSFT"), std::string::npos) << names[0].detail;

  // RENORMALIZATION, not merely dropping. The book the strategy builds on that
  // session must still be VEGA-NEUTRAL: the sizing allocates each survivor
  // w_i / Σ_survivors w of the index leg's vega, so Σ|q·v| over the three
  // survivors equals the index leg's |q·v| EXACTLY as it would with all five. An
  // implementation that dropped the two names but kept the original 1/5 weights
  // would land at 3/5 of the index vega here — a 40% under-hedged book that every
  // NAV-level assertion would still accept.
  const auto book = probe.build_book(*snap);
  ASSERT_TRUE(book.has_value()) << (book.has_value() ? std::string{} : book.error().to_string());
  ASSERT_EQ(book->used_names.size(), 3u); // AAPL, AMZN, GOOG
  ASSERT_EQ(book->name_legs.size(), 3u);
  double basket_vega = 0.0;
  for (const DispersionLeg &leg : book->name_legs) {
    basket_vega += std::fabs(leg.straddle_qty * leg.straddle_vega);
  }
  const double index_vega = std::fabs(book->index_leg.straddle_qty * book->index_leg.straddle_vega);
  ASSERT_GT(index_vega, 0.0);
  EXPECT_NEAR(basket_vega / index_vega, 1.0, 1e-12) << basket_vega << " vs " << index_vega;

  // THE RUN. The absent session still produces a row, and the drop is reported on
  // the run's diagnostics channel — `BacktestResult::signals`, the (name -> series
  // parallel to `date`) list `record_diagnostics` populates.
  SurfaceDbDispersionSpec spec = run_spec(root, "2026-01-05", "2026-01-08");
  spec.config.min_names = 2;
  spec.config.record_diagnostics = true;
  const auto out = run_surface_db_dispersion_backtest(spec);
  ASSERT_TRUE(out.has_value()) << (out.has_value() ? std::string{} : out.error().to_string());
  EXPECT_EQ(out->result.size(), 4u);

  const std::vector<double> *dropped = signal_series(out->result, "n_names_dropped");
  ASSERT_NE(dropped, nullptr) << "record_diagnostics did not reach the signals channel";
  ASSERT_EQ(dropped->size(), 4u);
  // CLUSTERED: the drops land on ONE row, not spread across the window.
  EXPECT_DOUBLE_EQ((*dropped)[0], 2.0);
  EXPECT_DOUBLE_EQ((*dropped)[1], 0.0);
  EXPECT_DOUBLE_EQ((*dropped)[2], 0.0);
  EXPECT_DOUBLE_EQ((*dropped)[3], 0.0);
  // The surviving basket still produced a tradeable signal on the absent session.
  const std::vector<double> *corr = signal_series(out->result, "implied_corr");
  ASSERT_NE(corr, nullptr);
  ASSERT_EQ(corr->size(), 4u);
  EXPECT_TRUE(std::isfinite((*corr)[0])) << (*corr)[0];
  EXPECT_GT(peak_open_lots(out->result), 0.0);
  for (const double nav : out->result.nav) {
    EXPECT_TRUE(std::isfinite(nav)) << nav;
  }

  // TEETH. The SAME corpus without the absence, over the SAME window: the run
  // opens the two dropped names' straddles (two lots each) and prints a different
  // NAV. Without this an implementation that silently ignored the absence — or one
  // whose fixture never actually omitted anything — would satisfy everything above.
  const auto full_root = test_root("t5_clustered_absence_full");
  auto full_db = make_test_db(full_root, kAbsDates, kAbsSymbols);
  ASSERT_TRUE(full_db.has_value())
      << (full_db.has_value() ? std::string{} : full_db.error().to_string());
  SurfaceDbDispersionSpec full_spec = run_spec(full_root, "2026-01-05", "2026-01-08");
  full_spec.config.min_names = 2;
  full_spec.config.record_diagnostics = true;
  const auto full = run_surface_db_dispersion_backtest(full_spec);
  ASSERT_TRUE(full.has_value()) << (full.has_value() ? std::string{} : full.error().to_string());
  ASSERT_EQ(full->result.size(), 4u);
  const std::vector<double> *full_dropped = signal_series(full->result, "n_names_dropped");
  ASSERT_NE(full_dropped, nullptr);
  for (const double d : *full_dropped) {
    EXPECT_DOUBLE_EQ(d, 0.0); // nothing to drop when every partition is complete
  }
  ASSERT_FALSE(out->result.n_open_lots.empty());
  ASSERT_FALSE(full->result.n_open_lots.empty());
  // Exactly two straddles (call + put each) fewer, and no other difference in book
  // shape — the drop removed the two names' legs and nothing else.
  EXPECT_DOUBLE_EQ(out->result.n_open_lots.front(), full->result.n_open_lots.front() - 4.0);
  EXPECT_NE(out->result.nav.back(), full->result.nav.back());
  fs::remove_all(full_root);
  fs::remove_all(root);
}

TEST(SurfaceDbDispersionBacktest, ClusteredAbsenceUnderAHeldBookFailsLoudly) {
  // The SAME cohort, now missing MID-window — after the book was opened on
  // 2026-01-05 holding all five names. Those held straddles have no surface to
  // mark against on 2026-01-07, and `RunConfig::unpriced` defaults to
  // `UnpricedLotPolicy::Error`, so the run ABORTS. That default is the reason a
  // clustered outage cannot silently truncate NAV: under `ExcludeAndReport` the
  // step's P&L would be dropped from the total, never recovered when the surfaces
  // reappear, and reported only as a count. Pinned here because it is the single
  // most consequential thing the surface-db route inherits from `RunConfig` — and
  // because an operator hitting it must be able to tell it apart from a bad
  // window or a bad symbol from the message alone.
  const auto root = test_root("t5_absence_held_book");
  auto db =
      make_test_db(root, kAbsDates, kAbsSymbols, {}, DateAbsence{{"2026-01-07", kAbsentCohort}});
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());

  SurfaceDbDispersionSpec spec = run_spec(root, "2026-01-05", "2026-01-08");
  spec.config.min_names = 2;
  spec.config.record_diagnostics = true;
  const auto out = run_surface_db_dispersion_backtest(spec);
  ASSERT_FALSE(out.has_value()) << "a held lot with no surface must never be valued silently";
  EXPECT_EQ(out.error().code(), ErrorCode::NotFound) << out.error().to_string();
  EXPECT_NE(out.error().message().find("held lot"), std::string::npos) << out.error().message();
  EXPECT_NE(out.error().message().find("no surface this step"), std::string::npos)
      << out.error().message();

  // TEETH: the abort is caused by the ABSENCE and not by anything else about this
  // corpus — the identical db with a complete 2026-01-07 runs the same window clean.
  const auto full_root = test_root("t5_absence_held_book_full");
  auto full_db = make_test_db(full_root, kAbsDates, kAbsSymbols);
  ASSERT_TRUE(full_db.has_value())
      << (full_db.has_value() ? std::string{} : full_db.error().to_string());
  SurfaceDbDispersionSpec full_spec = run_spec(full_root, "2026-01-05", "2026-01-08");
  full_spec.config.min_names = 2;
  const auto full = run_surface_db_dispersion_backtest(full_spec);
  ASSERT_TRUE(full.has_value()) << (full.has_value() ? std::string{} : full.error().to_string());
  EXPECT_EQ(full->result.size(), 4u);
  fs::remove_all(full_root);
  fs::remove_all(root);
}

TEST(SurfaceDbDispersionBacktest, ClusteredAbsenceBelowMinNamesIsADiagnosedNoTradeStep) {
  // Three survivors against `min_names = 4`. `build_dispersion_book` refuses the
  // date with `Unavailable`, and `DispersionStrategy::on_step_impl` converts that
  // ONE code — under DropRenormalize only — into the engine's documented NO-TRADE
  // CONTRACT: open nothing, leave the held book exactly as found, continue. The
  // step is therefore NOT an error at the run level, and this test pins that it is
  // also NOT SILENT: the row exists, carries zero open lots, a NaN `implied_corr`
  // and the drop count, and the very next complete session opens the book. Every
  // one of those four is asserted, so removing the diagnostic — the thing that
  // would make the no-trade step invisible — fails here.
  const auto root = test_root("t5_below_min_names");
  auto db =
      make_test_db(root, kAbsDates, kAbsSymbols, {}, DateAbsence{{"2026-01-05", kAbsentCohort}});
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());

  // The refusal, pinned at the layer that produces it: code AND the counts the
  // operator needs (how many survived, of how many, against what minimum).
  const auto u = universe_from_surface_db(*db, "SPY");
  ASSERT_TRUE(u.has_value()) << (u.has_value() ? std::string{} : u.error().to_string());
  DispersionBacktestConfig probe_cfg;
  probe_cfg.min_names = 4;
  const DispersionStrategy probe = make_dispersion_backtest_strategy(*u, probe_cfg);
  const auto snap = MarketSnapshot::load(archive_path_of(*db, "2026-01-05"));
  ASSERT_TRUE(snap.has_value());
  // The counts are POST-RESOLVE: `build_book` hands `build_dispersion_book` the
  // three names that resolved, so the denominator is 3 and not the manifest's 5 —
  // the two whole-symbol absences were already spent at the resolve stage and are
  // reported there (`dropped_on` / the run's `n_names_dropped`), not here. Pinned
  // verbatim so the wording cannot drift into implying only three were ever asked
  // for.
  const auto refused = probe.build_book(*snap);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code(), ErrorCode::Unavailable) << refused.error().to_string();
  EXPECT_NE(refused.error().message().find("only 3 of 3 names survived (min 4)"), std::string::npos)
      << refused.error().message();

  SurfaceDbDispersionSpec spec = run_spec(root, "2026-01-05", "2026-01-08");
  spec.config.min_names = 4;
  spec.config.record_diagnostics = true;
  const auto out = run_surface_db_dispersion_backtest(spec);
  ASSERT_TRUE(out.has_value()) << (out.has_value() ? std::string{} : out.error().to_string());
  ASSERT_EQ(out->result.size(), 4u);

  // Row 0 traded NOTHING.
  ASSERT_EQ(out->result.n_open_lots.size(), 4u);
  EXPECT_DOUBLE_EQ(out->result.n_open_lots[0], 0.0);
  // ...and said so, on both diagnostic axes.
  const std::vector<double> *corr = signal_series(out->result, "implied_corr");
  ASSERT_NE(corr, nullptr);
  ASSERT_EQ(corr->size(), 4u);
  EXPECT_TRUE(std::isnan((*corr)[0])) << (*corr)[0];
  const std::vector<double> *dropped = signal_series(out->result, "n_names_dropped");
  ASSERT_NE(dropped, nullptr);
  ASSERT_EQ(dropped->size(), 4u);
  EXPECT_DOUBLE_EQ((*dropped)[0], 2.0);
  // The run RECOVERS on the next complete session rather than staying flat: the
  // no-trade step is one date's verdict, not the run's.
  EXPECT_GT(out->result.n_open_lots[1], 0.0);
  for (std::size_t i = 1; i < 4; ++i) {
    EXPECT_DOUBLE_EQ((*dropped)[i], 0.0);
    EXPECT_TRUE(std::isfinite((*corr)[i])) << i << ": " << (*corr)[i];
  }
  fs::remove_all(root);
}

TEST(SurfaceDbDispersionBacktest, MissingIndexOnOneDateFailsLoudly) {
  // An index-less dispersion step is meaningless — there is nothing to disperse
  // AGAINST — so unlike a basket name the index is never droppable under any
  // missing-name policy. Both places a partition can lose it are pinned, because
  // they fail through DIFFERENT machinery and a refactor could plausibly silence
  // either one alone.
  const std::vector<std::string_view> index_only = {"SPY"};

  // (a) The index is missing on the window's ENTRY session: `resolve_universe_uids`
  //     refuses to bind the universe at all. NotFound naming SPY — never a drop.
  const auto entry_root = test_root("t5_missing_index_entry");
  auto entry_db =
      make_test_db(entry_root, kAbsDates, kAbsSymbols, {}, DateAbsence{{"2026-01-05", index_only}});
  ASSERT_TRUE(entry_db.has_value())
      << (entry_db.has_value() ? std::string{} : entry_db.error().to_string());
  SurfaceDbDispersionSpec entry_spec = run_spec(entry_root, "2026-01-05", "2026-01-08");
  entry_spec.config.min_names = 2;
  const auto no_entry_index = run_surface_db_dispersion_backtest(entry_spec);
  ASSERT_FALSE(no_entry_index.has_value());
  EXPECT_EQ(no_entry_index.error().code(), ErrorCode::NotFound)
      << no_entry_index.error().to_string();
  EXPECT_NE(no_entry_index.error().message().find("SPY"), std::string::npos)
      << no_entry_index.error().message();
  EXPECT_NE(no_entry_index.error().message().find("not present in snapshot directory"),
            std::string::npos)
      << no_entry_index.error().message();
  fs::remove_all(entry_root);

  // (b) The index vanishes MID-window, with the index straddle already held: the
  //     engine's unpriced-lot guard aborts instead of marking the book without it.
  const auto held_root = test_root("t5_missing_index_held");
  auto held_db =
      make_test_db(held_root, kAbsDates, kAbsSymbols, {}, DateAbsence{{"2026-01-07", index_only}});
  ASSERT_TRUE(held_db.has_value())
      << (held_db.has_value() ? std::string{} : held_db.error().to_string());
  SurfaceDbDispersionSpec held_spec = run_spec(held_root, "2026-01-05", "2026-01-08");
  held_spec.config.min_names = 2;
  const auto no_held_index = run_surface_db_dispersion_backtest(held_spec);
  ASSERT_FALSE(no_held_index.has_value());
  EXPECT_EQ(no_held_index.error().code(), ErrorCode::NotFound) << no_held_index.error().to_string();
  EXPECT_NE(no_held_index.error().message().find("held lot"), std::string::npos)
      << no_held_index.error().message();
  fs::remove_all(held_root);
}

TEST(SurfaceDbDispersionBacktest, BitIdenticalAcrossThreadCounts) {
  // The standing engine contract, asserted on a PRODUCTION-SHAPED run: the corpus
  // carries a clustered absence on its entry session, so the thread-count
  // invariance covers the drop/renormalize path and not just the clean one.
  // Comparison is `std::bit_cast<std::uint64_t>` — exact, no tolerance: "close
  // enough across thread counts" is precisely the regression this must catch.
  const auto root = test_root("t5_bit_identity");
  auto db =
      make_test_db(root, kRunDates, kRunSymbols, {}, DateAbsence{{"2026-01-05", kAbsentCohort}});
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());

  auto one = run_spec(root, "2026-01-05", "2026-01-12");
  one.config.run.price.n_threads = 1;
  auto many = run_spec(root, "2026-01-05", "2026-01-12");
  many.config.run.price.n_threads = 0; // 0 => hardware concurrency (PriceOptions)
  const auto a = run_surface_db_dispersion_backtest(one);
  const auto b = run_surface_db_dispersion_backtest(many);
  ASSERT_TRUE(a.has_value()) << (a.has_value() ? std::string{} : a.error().to_string());
  ASSERT_TRUE(b.has_value()) << (b.has_value() ? std::string{} : b.error().to_string());
  ASSERT_EQ(a->result.size(), b->result.size());
  ASSERT_EQ(a->result.size(), 6u);
  for (std::size_t i = 0; i < a->result.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a->result.pnl_total[i]),
              std::bit_cast<std::uint64_t>(b->result.pnl_total[i]))
        << "row " << i;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a->result.nav[i]),
              std::bit_cast<std::uint64_t>(b->result.nav[i]))
        << "row " << i;
  }
  // TEETH: two all-zero tracks are trivially bit-identical, so the run must have
  // actually traded and moved.
  EXPECT_GT(peak_open_lots(a->result), 0.0);
  EXPECT_NE(a->result.nav.back(), 0.0);
  fs::remove_all(root);
}

TEST(SurfaceDbDispersionBacktest, WindowSubsetMatchesFullRunPrefix) {
  // Determinism of WINDOWING itself: [d1..d6] and [d1..d4] must agree bit-for-bit
  // on the shared prefix. This is what catches window-dependent state leaking
  // backwards into early steps — a look-ahead that sized off a later date, a
  // prefetch that mutated the base snapshot, an accumulator seeded from the ref
  // count. Again over a corpus with a clustered absence on the entry session.
  const auto root = test_root("t5_window_prefix");
  auto db =
      make_test_db(root, kRunDates, kRunSymbols, {}, DateAbsence{{"2026-01-05", kAbsentCohort}});
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());

  const auto a = run_surface_db_dispersion_backtest(run_spec(root, "2026-01-05", "2026-01-12"));
  const auto b = run_surface_db_dispersion_backtest(run_spec(root, "2026-01-05", "2026-01-08"));
  ASSERT_TRUE(a.has_value()) << (a.has_value() ? std::string{} : a.error().to_string());
  ASSERT_TRUE(b.has_value()) << (b.has_value() ? std::string{} : b.error().to_string());
  ASSERT_EQ(a->result.size(), 6u);
  ASSERT_EQ(b->result.size(), 4u);
  for (std::size_t i = 0; i < b->result.size(); ++i) {
    EXPECT_EQ(a->result.date[i], b->result.date[i]) << "row " << i;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a->result.nav[i]),
              std::bit_cast<std::uint64_t>(b->result.nav[i]))
        << "row " << i;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a->result.pnl_total[i]),
              std::bit_cast<std::uint64_t>(b->result.pnl_total[i]))
        << "row " << i;
  }
  // TEETH: the prefix must be non-trivial — a NAV track that never moves would
  // make the comparison above vacuous.
  EXPECT_GT(peak_open_lots(b->result), 0.0);
  EXPECT_NE(b->result.nav.back(), 0.0);
  fs::remove_all(root);
}

// ── Task 6: the perf levers ─────────────────────────────────────────────────

TEST(SurfaceDbDispersionBacktest, PrivateCachePathIsUsed) {
  const auto root = test_root("t6_private_cache");
  auto db = make_test_db(root, kRunDates, kRunSymbols);
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());
  constexpr std::uint64_t kSessions = 6u; // == kRunDates.size()

  // WHY NOT `stats.cache`. `run_timed` captures `cfg.run.snapshot_cache->stats()`
  // when a cache was SUPPLIED and a zeroed `SnapshotCacheStats{}` otherwise
  // (backtest_driver.cpp:22-29,48) — and this route leaves that handle null ON
  // PURPOSE, because a null handle is exactly what makes the engine build its
  // PRIVATE cache (backtest.cpp:2315-2322). The private cache is engine-internal:
  // no handle to it ever reaches the caller, so every counter on `out->stats.cache`
  // reads zero on this route and `stats.cache.loads == n_steps` is NOT an
  // available assertion here. It IS available on the shared-cache control below,
  // which is where this test pins it.
  //
  // The observable that DOES see the private cache is `MarketSnapshot::open_count()`
  // — the process-wide archive-open counter (`g_open_count`, backtest.cpp:44)
  // incremented once per archive open inside `MarketSnapshot::load` (backtest.cpp:1877),
  // and the same counter the engine's own prefetch-depth gate asserts against,
  // `Backtest.PrefetchDepthIsBitIdenticalToSingleStepLookAhead`. ONE OPEN PER SESSION
  // is the entire perf claim: the private cache is bounded at depth+2 entries
  // (`private_snapshot_cache_capacity`, backtest.cpp:72-74) with
  // INSERTION-ORDER eviction, so a completed look-ahead is never dropped before
  // the step that consumes it. A reload does not change the economics by one bit,
  // so nothing but this counter can catch it.
  SurfaceDbDispersionSpec base = run_spec(root, "2026-01-05", "2026-01-12");
  ASSERT_EQ(base.config.run.snapshot_cache, nullptr);
  ASSERT_TRUE(base.config.run.prefetch_snapshots);
  // The RunConfig default this route inherits — 2 since v1 closeout sprint Task 4.8
  // (plan item 6.7) measured the depth ladder on the 135-session replay (see
  // RunConfig::prefetch_depth). The sweep below still drives 1 explicitly, so the
  // depth-1 shape stays covered.
  ASSERT_EQ(base.config.run.prefetch_depth, 2u);

  const auto run_at_depth = [&](std::size_t depth) -> Result<RunOutcome> {
    SurfaceDbDispersionSpec spec = base;
    spec.config.run.prefetch_depth = depth;
    // Reset immediately before the run: `SurfaceDb::open` / `Clock::from_surface_db`
    // / `universe_from_surface_db` read the MANIFEST only and open no archive, so
    // everything counted between here and the read below is the engine's.
    MarketSnapshot::reset_open_count();
    Result<RunOutcome> out = run_surface_db_dispersion_backtest(spec);
    EXPECT_EQ(MarketSnapshot::open_count(), kSessions)
        << "prefetch_depth=" << depth << ": every partition must open EXACTLY once";
    return out;
  };

  const Result<RunOutcome> d1 = run_at_depth(1u);
  ASSERT_TRUE(d1.has_value()) << (d1.has_value() ? std::string{} : d1.error().to_string());
  EXPECT_EQ(d1->stats.n_steps, kSessions);
  EXPECT_EQ(d1->stats.n_steps, static_cast<std::uint64_t>(d1->result.size()));
  // The private path's SIGNATURE, and the assertion that no shared cache slipped
  // in: all-zero cache telemetry. (examples/surface_db_dispersion_backtest.cpp
  // prints these three and labels the zeros as the by-design private-Sealed case.)
  EXPECT_EQ(d1->stats.cache.loads, 0u);
  EXPECT_EQ(d1->stats.cache.hits, 0u);
  EXPECT_EQ(d1->stats.cache.prefetches, 0u);
  // TEETH: a run that opened six archives and traded nothing would satisfy the
  // open-count gate just as well.
  EXPECT_GT(peak_open_lots(d1->result), 0.0);

  // LOOK-AHEAD DEPTH IS A PERF LEVER AND NOTHING ELSE. Deeper look-ahead must
  // neither reload (the count inside `run_at_depth`) nor move one bit of the
  // result. 8 exceeds the clock, so `prefetch_window`'s past-the-end clamp
  // (backtest.cpp:92) is covered too, and 4 is the depth at which the historical
  // LRU reload defect was first measured.
  const std::size_t depths[] = {2u, 4u, 8u};
  for (const std::size_t depth : depths) {
    SCOPED_TRACE("prefetch_depth=" + std::to_string(depth));
    const Result<RunOutcome> deep = run_at_depth(depth);
    ASSERT_TRUE(deep.has_value()) << (deep.has_value() ? std::string{} : deep.error().to_string());
    expect_bit_identical_track(d1->result, deep->result);
  }

  // THE CONTROL — and the teeth for the three zeros above. A caller-supplied
  // cache is the ONE thing that displaces the private one, and doing so is loudly
  // observable, which is what makes "all zeros" evidence rather than a dead
  // counter. It also lands the brief's `loads == n_steps` in the only place the
  // counter is readable: exactly one COLD load per session (never a reload), the
  // inception load plus one look-ahead per later step, and every later step served
  // warm off that look-ahead.
  SurfaceDbDispersionSpec shared = base;
  shared.config.run.snapshot_cache = std::make_shared<SnapshotCache>();
  // ...and what installing one COSTS. A caller-supplied cache may outlive the run
  // and be shared across books, so it is Mutable — the Sealed mmap is gone
  // (backtest.hpp:332-349, dispersion_surface_db.hpp:151-158). That is the whole
  // reason this route leaves the field null.
  EXPECT_EQ(shared.config.run.snapshot_cache->archive_backing(), ArchiveBacking::Mutable);
  MarketSnapshot::reset_open_count();
  const auto with_cache = run_surface_db_dispersion_backtest(shared);
  ASSERT_TRUE(with_cache.has_value())
      << (with_cache.has_value() ? std::string{} : with_cache.error().to_string());
  EXPECT_EQ(MarketSnapshot::open_count(), kSessions);
  EXPECT_EQ(with_cache->stats.cache.loads, with_cache->stats.n_steps);
  EXPECT_EQ(with_cache->stats.cache.loads, kSessions);
  EXPECT_EQ(with_cache->stats.cache.hits, kSessions - 1u);
  EXPECT_EQ(with_cache->stats.cache.prefetches, kSessions - 1u);
  // The cache choice is a PERF choice: the economics are bit-identical either way,
  // so the route's null-cache default costs the operator nothing but the counters.
  expect_bit_identical_track(d1->result, with_cache->result);

  fs::remove_all(root);
}

TEST(SurfaceDbDispersionBacktest, RealSp100Baseline) {
  // ENV-GATED AND READ-ONLY. Unset => skipped, so the suite stays hermetic on a
  // machine with no data lake. When set, the route only OPENS the manifest and
  // mmaps partitions, and this test writes no artifact anywhere: the production
  // db is never mutated, created in, or deleted from.
  const std::string root = env_or_empty("ATX_SP100_SURFACE_DB");
  if (root.empty()) {
    GTEST_SKIP() << "set ATX_SP100_SURFACE_DB (e.g. C:/atx-data/surface-db/sp100-2026) to run";
  }

  // ── WHY THIS IS NOT THE 140-SESSION WINDOW ────────────────────────────────
  //
  // The sprint asked for 2026-01-02..2026-07-24 (140 partitions). That window is
  // NOT REPLAYABLE by this route, and the reason is the CORPUS, not the code. Two
  // independent defects, both measured (Task 6 report has the full date lists):
  //
  //   1. THE INDEX IS ABSENT ON 18 OF 140 SESSIONS. `MarketSnapshot::uid_of("SPY")`
  //      fails on 2026-01-22, -01-26, -01-30, -02-24, -03-02, -03-05, -03-06,
  //      -03-10, -04-07, -04-16, -05-04, -05-05, -05-20, -05-22, -05-27, -06-02,
  //      -07-15, -07-22. An index-less dispersion step is meaningless, so the
  //      route refuses it (pinned by `MissingIndexOnOneDateFailsLoudly`), and NO
  //      policy knob can make it survivable. That alone caps any single run at the
  //      longest index-complete stretch, which is 28 sessions (2026-06-03..07-14).
  //   2. BASKET NAMES ARE ABSENT ON 118 OF 140 SESSIONS (1-6 names each). Under a
  //      held book that costs a mark, and the engine correctly refuses to invent
  //      one. Three separate guards fire, and only the first has a policy knob:
  //      the held-lot valuation guard (`UnpricedLotPolicy`), the delta-hedge share
  //      fill (`spot_of` => NotFound, no knob), and the roll-close execution mark
  //      (no knob by design — backtest.hpp:552-553, "close execution always
  //      requires an economically valid mark").
  //
  // So this baseline runs the LONGEST WINDOW THE CORPUS CAN ACTUALLY REPLAY —
  // 2026-06-03..2026-06-26, 17 sessions, found by sweep — under the production
  // strategy shape with exactly two documented knobs moved, each named below. The
  // measurement it produces (ms per session over a ~100-name, 200-lot book) is the
  // number the sprint needs; the window length is not.
  //
  // IF THE CORPUS IS REBUILT, re-derive the window: it is data, not policy.
  const auto baseline_spec = [&](std::size_t prefetch_depth) {
    SurfaceDbDispersionSpec spec;
    spec.db_root = root;
    spec.date_lo = "2026-06-03";
    spec.date_hi = "2026-06-26";
    // The production shape from examples/sp100_dispersion_config.tsv: a 60-name
    // floor and a monthly (21-session) entry cadence.
    spec.config.min_names = 60;
    spec.config.entry_every_n = 21;
    spec.config.run.price.n_threads = 0; // 0 => hardware concurrency (PriceOptions)
    spec.config.run.prefetch_depth = prefetch_depth;
    // KNOB 1. Under the engine's fail-closed default (`UnpricedLotPolicy::Error`,
    // backtest.hpp:561) even this window aborts on its second session, because a
    // held straddle loses its surface. That abort is CORRECT and is pinned by
    // `ClusteredAbsenceUnderAHeldBookFailsLoudly`; it is not what this test
    // measures. The opt-in is made HERE, in the spec, never by weakening the
    // engine default — and the excluded-lot count is printed below so the cost is
    // on the record instead of buried in the NAV.
    spec.config.run.unpriced = UnpricedLotPolicy::ExcludeAndReport;
    // KNOB 2. A delta band no book breaches, so the daily hedge is EVALUATED but
    // never trades. This is not cosmetic: with the default band of 0.0 the hedge
    // unwinds a residual share position on every uid every session, and a name
    // that vanished has no spot to fill against — `run_backtest: no surface for
    // delta hedge on uid=...`, which has no policy knob. Raising the band is the
    // narrowest change that keeps the thing being MEASURED intact: `hedge_fires`
    // stays true, so the full-book `FullGreeks` pricing pass still runs on every
    // step (backtest.cpp:2140-2158) — which is the dominant per-step cost. Setting
    // `hedge_kind = None` would instead SKIP that pass on non-entry steps and
    // report a materially cheaper run than the production shape.
    spec.config.delta_band = 1.0e18;
    return spec;
  };

  MarketSnapshot::reset_open_count();
  const auto d2 = run_surface_db_dispersion_backtest(baseline_spec(2u));
  ASSERT_TRUE(d2.has_value()) << (d2.has_value() ? std::string{} : d2.error().to_string());
  const std::uint64_t opens_d2 = MarketSnapshot::open_count();

  // 17 sessions is what the window holds; >= 15 leaves room for a corpus rebuild
  // that shifts a session without silently accepting a two-row run.
  EXPECT_GE(d2->result.size(), 15u);
  ASSERT_EQ(d2->result.size(), d2->result.nav.size());
  for (const double nav : d2->result.nav) {
    EXPECT_TRUE(std::isfinite(nav)) << nav;
  }
  EXPECT_EQ(d2->stats.n_steps, static_cast<std::uint64_t>(d2->result.size()));
  // The no-reload gate, now on the REAL corpus: one archive open per session.
  EXPECT_EQ(opens_d2, d2->stats.n_steps);
  // The private-Sealed signature holds on the production route too.
  EXPECT_EQ(d2->stats.cache.loads, 0u);
  // TEETH: a window that never opened a lot would satisfy everything above with a
  // flat NAV of zeros. A full SP100 straddle book is ~2 legs x ~100 names.
  EXPECT_GT(peak_open_lots(d2->result), 100.0);

  // The look-ahead comparison the sprint report records. Depth is a perf lever,
  // so the two tracks must be bit-identical; only the wall clock may differ.
  MarketSnapshot::reset_open_count();
  const auto d1 = run_surface_db_dispersion_backtest(baseline_spec(1u));
  ASSERT_TRUE(d1.has_value()) << (d1.has_value() ? std::string{} : d1.error().to_string());
  EXPECT_EQ(MarketSnapshot::open_count(), d1->stats.n_steps);
  expect_bit_identical_track(d2->result, d1->result);

  // NO WALL-CLOCK ASSERTION: it is machine- and page-cache-dependent, and a
  // threshold here would be a flaky gate rather than a measurement. The NUMBERS
  // are the deliverable and they go to stderr for the sprint report.
  std::cerr << "[baseline] db=" << root << " window=" << d2->result.date.front() << ".."
            << d2->result.date.back() << " steps=" << d2->stats.n_steps
            << " archive_opens=" << opens_d2 << "\n"
            << "[baseline] n_threads=0 prefetch_depth=2 wall_ms=" << d2->stats.wall_clock_ms
            << "\n"
            << "[baseline] n_threads=0 prefetch_depth=1 wall_ms=" << d1->stats.wall_clock_ms
            << "\n"
            << "[baseline] peak_open_lots=" << peak_open_lots(d2->result)
            << " total_unpriced_lots="
            << std::accumulate(d2->result.n_unpriced_lots.begin(),
                               d2->result.n_unpriced_lots.end(), 0.0)
            << " nav_back=" << (d2->result.nav.empty() ? 0.0 : d2->result.nav.back()) << "\n";
  // WHICH sessions cost the run a held mark, and how many lots each. Under the
  // engine's fail-closed default the FIRST of these is where the run aborts, so
  // this list is what an operator needs to decide between shortening the window
  // and opting into the lenient arithmetic. Printed, never asserted: it is a
  // property of the corpus, and pinning it here would turn a db rebuild red.
  ASSERT_EQ(d2->result.n_unpriced_lots.size(), d2->result.date.size());
  for (std::size_t i = 0; i < d2->result.date.size(); ++i) {
    if (d2->result.n_unpriced_lots[i] > 0.0) {
      std::cerr << "[baseline] unpriced session " << d2->result.date[i] << ": "
                << d2->result.n_unpriced_lots[i] << " held lot(s) with no surface\n";
    }
  }
}

