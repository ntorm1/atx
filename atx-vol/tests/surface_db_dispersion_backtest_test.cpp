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
// Fixtures are synthetic eSSVI surfaces written into a fresh SurfaceDb under
// %TEMP% (make_test_db below), plus config text written to throwaway %TEMP%
// files (write_temp_file); nothing here reads the real data lake.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
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
// Shared fixture builder for this file — later tasks in this sprint extend it.
[[nodiscard]] Result<SurfaceDb> make_test_db(const fs::path &root,
                                             const std::vector<std::string_view> &dates,
                                             const std::vector<std::string_view> &symbols) {
  auto db = SurfaceDb::create(root.string());
  if (!db.has_value()) {
    return atx::core::Err(db.error());
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
