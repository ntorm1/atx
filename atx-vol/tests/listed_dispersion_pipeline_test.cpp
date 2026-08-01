#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/backtest.hpp"                   // MarketSnapshot
#include "atx/vol/contract_projection.hpp"        // ProjectedMaturitySpec, ProjectedOption
#include "atx/vol/corpus.hpp"                     // CorpusManifest, CorpusEntry (engine-driven clock)
#include "atx/vol/dispersion.hpp"                 // DispersionMember, DispersionBook, build_dispersion_book
#include "atx/vol/historical_projection.hpp"      // HistoricalProjectionScenario/Frame/Config, ProjectedHistoricalVar
#include "atx/vol/listed_dispersion.hpp"          // ListedOptionQuote
#include "atx/vol/listed_dispersion_pipeline.hpp" // module under test
#include "atx/vol/listed_dispersion_reconciliation.hpp" // ListedReconciliationSnapshot, reconcile_listed_dispersion
#include "atx/vol/listed_dispersion_schedule.hpp" // ListedRiskLookup, ListedOptionRisk
#include "atx/vol/listed_dispersion_strategy.hpp" // ListedDispersionStrategy, ScheduleMarkPolicy, MarkDivergence
#include "atx/vol/portfolio_pricer.hpp"           // SurfaceSet, kNsPerYear
#include "atx/vol/priced_surface.hpp"             // PricedSurface
#include "atx/vol/query_pricing.hpp"              // QueryExecution
#include "atx/vol/strategy.hpp"                   // IStrategy (the foreign-strategy stub)
#include "atx/vol/surface_archive.hpp"            // write_surface_archive_v2_file
#include "atx/vol/types.hpp"                      // ErrorCode, Side, Status
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

// ── Synthetic-surface scaffolding (mirrors listed_dispersion_reconciliation_test.cpp:35-76) ──
constexpr double kRate = 0.04;
constexpr std::int64_t kDayNs = 86'400'000'000'000LL;
constexpr std::int64_t kNow0 = 1'700'000'000'000'000'000LL;
constexpr std::int64_t kNow1 = kNow0 + kDayNs;
constexpr std::int64_t kNow2 = kNow1 + kDayNs;
constexpr std::int64_t kExpiry0 = kNow0 + static_cast<std::int64_t>(0.10 * kNsPerYear);
constexpr std::int64_t kExpiry1 = kNow1 + static_cast<std::int64_t>(0.10 * kNsPerYear);

PricedSurface make_surface(std::uint32_t uid, double spot, std::int64_t now) {
  CurveSurface curves;
  std::vector<SliceContext> context;
  std::uint16_t expiry_id = 0;
  for (const double term : {0.05, 0.10, 0.20, 0.50}) {
    const double forward = spot * std::exp((kRate - 0.02) * term);
    EssviParams parameters{};
    parameters.theta = 0.04 + 0.01 * term;
    parameters.phi = 1.4;
    parameters.rho = -0.35;
    parameters.psi = 0.5;
    parameters.p = 0.5;
    parameters.lambda = 0.5;
    parameters.T = term;
    parameters.F = forward;
    parameters.expiry_id = expiry_id++;
    curves.push(std::make_unique<EssviCurve>(parameters, std::exp(-kRate * term)));
    context.push_back(SliceContext{term, forward, 0.0, 0.02, 100, 7});
  }
  PricingContext pricing;
  pricing.S = spot;
  pricing.r = kRate;
  pricing.now_ts_ns = now;
  pricing.method = AmericanMethod::AndersenLake;
  pricing.al_opts = al_fast_opts();
  pricing.uid = uid;
  auto result = PricedSurface::create(std::move(curves), std::move(context), pricing);
  EXPECT_TRUE(result) << (result ? std::string{} : result.error().to_string());
  return std::move(*result);
}

std::vector<PricedSurface> surfaces(std::int64_t now, double shift) {
  std::vector<PricedSurface> result;
  result.push_back(make_surface(1u, 500.0 + shift, now));
  result.push_back(make_surface(2u, 100.0 + 0.3 * shift, now));
  result.push_back(make_surface(3u, 200.0 - 0.2 * shift, now));
  return result;
}

std::vector<const PricedSurface *> pointers(const std::vector<PricedSurface> &surfaces) {
  std::vector<const PricedSurface *> result;
  for (const PricedSurface &surface : surfaces) {
    result.push_back(&surface);
  }
  return result;
}

// ── Schedule / reconciliation-snapshot scaffolding (mirrors
//    listed_dispersion_reconciliation_test.cpp:82-171) ─────────────────────────
ListedOptionQuote option(std::string symbol, std::uint32_t id, double strike, Side side,
                         std::string date, std::int64_t now, std::int64_t expiry) {
  ListedOptionQuote quote;
  quote.trade_date = std::move(date);
  quote.symbol = std::move(symbol);
  quote.instrument_id = id;
  quote.raw_symbol = quote.symbol + std::to_string(id);
  quote.expiry_ts_ns = expiry;
  quote.strike = strike;
  quote.side = side;
  quote.bid = 1.0;
  quote.ask = 1.2;
  quote.quote_ts_ns = now;
  quote.multiplier = 100.0;
  quote.standard_monthly = true;
  quote.standard_deliverable = true;
  quote.source_fingerprint = 1000u + id;
  return quote;
}

ListedStraddle straddle(std::string symbol, std::uint32_t uid, std::uint32_t id, double strike,
                        double weight, const std::string &date, std::int64_t now,
                        std::int64_t expiry) {
  ListedStraddle result;
  result.symbol = std::move(symbol);
  result.uid = uid;
  result.expiry_ts_ns = expiry;
  result.strike = strike;
  result.call = option(result.symbol, id, strike, Side::Call, date, now, expiry);
  result.put = option(result.symbol, id + 1u, strike, Side::Put, date, now, expiry);
  result.raw_weight = weight;
  result.normalized_weight = weight;
  return result;
}

ListedDispersionSelection selection(const std::string &date, std::int64_t now, std::int64_t expiry,
                                    std::uint32_t id_base) {
  ListedDispersionSelection result;
  result.trade_date = date;
  result.valuation_ts_ns = now;
  result.expiry_ts_ns = expiry;
  result.dte_days = static_cast<double>(expiry - now) / kListedNsPerDay;
  result.index = straddle("SPY", 1u, id_base, 500.0, 0.0, date, now, expiry);
  result.names.push_back(straddle("N0", 2u, id_base + 2u, 100.0, 0.4, date, now, expiry));
  result.names.push_back(straddle("N1", 3u, id_base + 4u, 200.0, 0.6, date, now, expiry));
  return result;
}

ListedScheduleRoll roll(const ListedDispersionSelection &selected,
                        const std::vector<PricedSurface> &source, std::uint32_t cohort) {
  auto set = SurfaceSet::create(pointers(source));
  EXPECT_TRUE(set) << (set ? std::string{} : set.error().to_string());
  ListedScheduleBuildConfig config;
  config.gross_index_vega_target_per_vol_point = 1000.0;
  config.cohort = cohort;
  config.surface_fingerprint = 9000u + cohort;
  auto result = build_listed_dispersion_roll(selected, *set, config);
  EXPECT_TRUE(result) << (result ? std::string{} : result.error().to_string());
  return std::move(*result);
}

std::vector<ListedOptionQuote> quotes_for(const ListedScheduleRoll &roll,
                                          const SurfaceSet &surfaces, const std::string &date,
                                          std::int64_t now, std::uint32_t id_offset) {
  std::vector<ListedOptionQuote> quotes;
  for (const ListedScheduleLeg &leg : roll.legs) {
    // WS-ZC1: SurfaceSet::find resolves to a `SurfaceRef` handle, not a
    // `const PricedSurface *`. Declared type only; `->` and nullptr compare unchanged.
    const SurfaceRef surface = surfaces.find(leg.uid);
    EXPECT_NE(surface, nullptr);
    const double term = static_cast<double>(leg.expiry_ts_ns - now) / kNsPerYear;
    auto mark = surface->fair_value(leg.strike, term, leg.side);
    EXPECT_TRUE(mark) << (mark ? std::string{} : mark.error().to_string());
    ListedOptionQuote quote;
    quote.trade_date = date;
    quote.symbol = leg.symbol;
    quote.instrument_id = leg.instrument_id + id_offset;
    quote.raw_symbol = leg.raw_symbol;
    quote.expiry_ts_ns = leg.expiry_ts_ns;
    quote.strike = leg.strike;
    quote.side = leg.side;
    quote.bid = std::max(0.0, *mark - 0.1);
    quote.ask = *mark + 0.1;
    quote.quote_ts_ns = now;
    quote.multiplier = leg.multiplier;
    quote.standard_monthly = true;
    quote.standard_deliverable = true;
    quote.source_fingerprint = 7000u + quote.instrument_id;
    quotes.push_back(std::move(quote));
  }
  return quotes;
}

// A synthetic MarketSnapshot: the forward/risk seams take a MarketSnapshot (they
// read snapshot.ts_ns() and snapshot.find(uid)), and a MarketSnapshot is only
// constructible via load-from-disk, so write a v2 surface archive and load it.
fs::path fresh_dir() {
  const fs::path path = fs::temp_directory_path() / "atx-listed-pipeline";
  std::error_code error;
  fs::remove_all(path, error);
  fs::create_directories(path, error);
  return path;
}

std::string write_archive(const fs::path &dir, const std::string &date,
                          const std::vector<PricedSurface> &surfaces) {
  const std::vector<SurfaceArchiveItem> items = {
      {"SPY", &surfaces[0]}, {"N0", &surfaces[1]}, {"N1", &surfaces[2]}};
  const std::string path = (dir / (date + ".atxvsa")).string();
  const Status status = write_surface_archive_v2_file(path, items);
  EXPECT_TRUE(status) << (status ? std::string{} : status.error().to_string());
  return path;
}

// ── Mark-divergence observation scaffolding (L10) ─────────────────────────────
// Its own tagged temp dir: `fresh_dir()` above is shared and unsuffixed, and it
// remove_all's on entry, so reusing it here would let one fixture delete another's
// archive out from under a live MarketSnapshot.
fs::path fresh_divergence_dir(const char *tag) {
  const fs::path path = fs::temp_directory_path() / (std::string{"atx-listed-pipeline-div-"} + tag);
  std::error_code error;
  fs::remove_all(path, error);
  fs::create_directories(path, error);
  return path;
}

// One roll authored at kNow0 / kExpiry0 over the `surfaces(kNow0, 0.0)` board, so
// every leg's `model_mark` equals the archived surface's live mark bit-for-bit and
// perturbing exactly one leg makes exactly that leg diverge.
//
// `roll_date` is deliberately "2026-07-10" while the observed clock ref carries
// "2026-07-11": that makes a row's `date` field unambiguous about which of the two
// it was read from (production has them equal, which would hide a mix-up).
ListedDispersionSchedule divergence_schedule(const std::vector<PricedSurface> &source) {
  ListedDispersionSchedule schedule;
  schedule.rolls.push_back(roll(selection("2026-07-10", kNow0, kExpiry0, 1u), source, 4u));
  return schedule;
}

// A minimal non-listed IStrategy. The observer's downcast must reject it outright:
// silently recording nothing is the "dropped observation the caller believes it
// made" failure class.
class ForeignStrategy final : public IStrategy {
public:
  Status on_step(const MarketSnapshot &, std::size_t, PortfolioState &, std::uint64_t &) override {
    return atx::core::Ok();
  }
};

} // namespace

// ── (a) REMOVED (C-2 follow-up) ──────────────────────────────────────────────
// A `static_assert(kVegaVolPointToUnitVol == 100.0)` and a
// `TEST(..., VegaVolPointConstantIs100) { EXPECT_EQ(kVegaVolPointToUnitVol, 100.0); }`
// stood here. Both asserted a constant against its own literal — `x == x` — so
// neither could ever fail for any reason a reader would care about, and their
// presence made a DEAD constant look load-bearing. The constant had no call site
// at this tip (E1 abolished the boundary it served) and was deleted; see the note
// at the top of listed_dispersion_pipeline.hpp. The live unit contract is pinned
// by `contract_vega_per_vol_point`'s callers, not by restating 100.0.

// ── (b) ListedDispersionMethodology::policy_fingerprint (L9) ──────────────────
TEST(ListedDispersionPipeline, PolicyFingerprintStableAndSensitive) {
  const ListedDispersionMethodology method{};
  const std::uint64_t fp = method.policy_fingerprint();
  EXPECT_NE(fp, 0u);
  // Stable across calls on an unchanged policy.
  EXPECT_EQ(fp, method.policy_fingerprint());

  // Default thresholds are pinned to the current production values. The policy
  // carries exactly the floors a consumer actually reads — the four fields that
  // were folded into the fingerprint but never read by anything (admission rule,
  // core_min_names_per_roll, query_route, occ_ess_authority) were removed in the
  // Wave B final-review pass, so this list IS the struct.
  EXPECT_EQ(method.min_names_entry, 51u);
  EXPECT_EQ(method.core_min_dates, 60u);
  EXPECT_EQ(method.core_min_rolls, 3u);

  // Any single-threshold difference perturbs the fingerprint.
  ListedDispersionMethodology bumped_entry = method;
  bumped_entry.min_names_entry = 52u;
  EXPECT_NE(bumped_entry.policy_fingerprint(), fp);

  ListedDispersionMethodology bumped_dates = method;
  bumped_dates.core_min_dates = 61u;
  EXPECT_NE(bumped_dates.policy_fingerprint(), fp);

  ListedDispersionMethodology bumped_rolls = method;
  bumped_rolls.core_min_rolls = 4u;
  EXPECT_NE(bumped_rolls.policy_fingerprint(), fp);

  // Every remaining field is covered above: fingerprint sensitivity is total, so
  // a field added without a matching case here leaves a silent hole.
  EXPECT_NE(bumped_entry.policy_fingerprint(), bumped_dates.policy_fingerprint());
  EXPECT_NE(bumped_dates.policy_fingerprint(), bumped_rolls.policy_fingerprint());
}

// ── (c) per-date adapter seams over a synthetic snapshot (L8) ─────────────────
TEST(ListedDispersionPipeline, ForwardAndRiskLookupsOverSyntheticSnapshot) {
  const fs::path dir = fresh_dir();
  const std::vector<PricedSurface> day0 = surfaces(kNow0, 0.0);
  const std::string path = write_archive(dir, "2026-07-10", day0);

  auto loaded = MarketSnapshot::load(path);
  ASSERT_TRUE(loaded) << (loaded ? std::string{} : loaded.error().to_string());
  const MarketSnapshot &snapshot = *loaded;
  ASSERT_EQ(snapshot.ts_ns(), kNow0);

  // Forward lookup: SPY (uid 1) at the ~0.10y front expiry is finite and positive.
  const ListedForwardLookup forward = make_listed_forward_lookup(snapshot);
  DispersionMember index_member;
  index_member.symbol = "SPY";
  index_member.uid = 1u;
  auto fwd = forward(index_member, kExpiry0);
  ASSERT_TRUE(fwd) << (fwd ? std::string{} : fwd.error().to_string());
  EXPECT_TRUE(std::isfinite(*fwd));
  EXPECT_GT(*fwd, 0.0);

  // A uid absent from the snapshot fails closed (surface missing).
  DispersionMember missing_member;
  missing_member.symbol = "ZZZ";
  missing_member.uid = 999u;
  EXPECT_FALSE(forward(missing_member, kExpiry0));

  // Risk lookup: cold certified per-share greeks at (uid, K, residual T, side).
  const double residual_T = static_cast<double>(kExpiry0 - kNow0) / kNsPerYear;
  const ListedRiskLookup risk =
      make_listed_risk_lookup(snapshot, residual_T, /*analytic=*/true, QueryExecution::ColdReference);
  ListedOptionQuote quote;
  quote.strike = *fwd;
  quote.side = Side::Call;
  auto option_risk = risk(1u, quote);
  ASSERT_TRUE(option_risk) << (option_risk ? std::string{} : option_risk.error().to_string());
  EXPECT_TRUE(std::isfinite(option_risk->model_mark));
  EXPECT_GT(option_risk->model_mark, 0.0);
  EXPECT_TRUE(std::isfinite(option_risk->delta_per_share));
  EXPECT_TRUE(std::isfinite(option_risk->vega_per_unit_vol));
  EXPECT_GT(option_risk->vega_per_unit_vol, 0.0);

  // A uid absent from the snapshot fails closed here too.
  EXPECT_FALSE(risk(999u, quote));

  std::error_code error;
  fs::remove_all(dir, error);
}

// ── Task 2 — M1 reconciliation clock-coupling fix ─────────────────────────────
//
// Both tests build the offending timeline shape: a valid one-roll schedule whose
// first roll date is day1 (2026-07-11), fed a FULL timeline that opens with a
// warm-up / low-coverage session on day0 (2026-07-10), strictly before the first
// roll — exactly what run-backtest hands the reconciler from clock.refs().

// PREMISE SUPERSEDED at the main -> feat/pipeline-m merge; rewritten to assert the
// contract that actually holds.
//
// This was main's RED anchor: it asserted that feeding the FULL warm-up-led timeline
// straight into the low-level reconciler ABORTS on the front-date hard-require
// ("first snapshot must be first entry date"). The trunk fixed the SAME underlying
// defect independently and at a lower layer — `276239d fix(vol): point-in-time
// dispersion universe + removals + reconcile deferral (C1-C4)`, change C2:
//
//   "reconcile_listed_dispersion no longer requires the first snapshot to be the
//    first entry date. The schedule builder legitimately defers the first roll
//    (coverage gate); pre-entry dates now emit flat position-free rows, so
//    row-count alignment with the canonical backtest is preserved. A genuinely
//    missing first-roll date is still a hard error."
//
// So main's M1 (trim the lead-in in a wrapping seam) and the trunk's C2 (tolerate
// the lead-in in the reconciler itself) are CONVERGENT fixes for one defect. Both
// survive the merge and both work; only this negative control became false, because
// the guard it pinned no longer exists.
//
// Rewritten rather than deleted, and deliberately NOT a duplicate of the trunk's
// `ListedDispersionReconciliation.ToleratesDeferredFirstRollWithLeadingFlatDates`
// (which already covers the flat-leading-row shape in detail). What that test does
// NOT cover, and what this one now pins, is the guard's SURVIVING teeth: a timeline
// with no snapshot on/after the first scheduled roll date is still a hard error.
// Without this, C2's relaxation could silently widen into "any timeline is fine".
TEST(ListedDispersionPipeline, ReconcileClockCoupling_ToleratesWarmupLeadInButNotAMissingEntry) {
  const std::vector<PricedSurface> day0 = surfaces(kNow0, 0.0);
  const std::vector<PricedSurface> day1 = surfaces(kNow1, 2.0);
  const std::vector<PricedSurface> day2 = surfaces(kNow2, -1.0);

  ListedDispersionSchedule schedule;
  schedule.rolls.push_back(roll(selection("2026-07-11", kNow1, kExpiry1, 1u), day1, 4u));
  ASSERT_EQ(schedule.rolls.front().roll_date, "2026-07-11");

  auto set0 = SurfaceSet::create(pointers(day0));
  auto set1 = SurfaceSet::create(pointers(day1));
  auto set2 = SurfaceSet::create(pointers(day2));
  ASSERT_TRUE(set0 && set1 && set2);
  const std::vector<ListedOptionQuote> quotes1 =
      quotes_for(schedule.rolls[0], *set1, "2026-07-11", kNow1, 0u);
  const std::vector<ListedOptionQuote> quotes2 =
      quotes_for(schedule.rolls[0], *set2, "2026-07-12", kNow2, 1000u);

  const std::vector<ListedReconciliationSnapshot> full = {
      {"2026-07-10", kNow0, &*set0, {}},      // warm-up lead-in (before first roll)
      {"2026-07-11", kNow1, &*set1, quotes1}, // first roll / entry
      {"2026-07-12", kNow2, &*set2, quotes2}, // held
  };

  // C2: the warm-up lead-in is now TOLERATED at the low level. One row per snapshot
  // is emitted throughout (so the timeline stays aligned with the canonical
  // backtest), and the pre-entry date is flat and position-free.
  const auto tolerated = reconcile_listed_dispersion(schedule, full);
  ASSERT_TRUE(tolerated) << tolerated.error().to_string();
  ASSERT_EQ(tolerated->rows.size(), full.size());
  EXPECT_EQ(tolerated->rows[0].date, "2026-07-10");
  EXPECT_EQ(tolerated->rows[0].n_held_lots, 0u);

  // The surviving hard error: drop every snapshot on/after the first roll date and
  // the reconciler must still refuse. Nail the SPECIFIC guard — InvalidArgument has
  // several sources in this function, so matching the code alone would keep passing
  // if the abort moved to a different one.
  const std::vector<ListedReconciliationSnapshot> lead_in_only = {full[0]};
  const auto missing_entry = reconcile_listed_dispersion(schedule, lead_in_only);
  ASSERT_FALSE(missing_entry);
  EXPECT_EQ(missing_entry.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(
      missing_entry.error().to_string().find("no snapshot on/after the first scheduled roll date"),
      std::string::npos)
      << missing_entry.error().to_string();
}

// GREEN target: the new seam trims the warm-up lead-in so reconciliation starts at
// the first roll date and succeeds — bit-identical to a manually-trimmed reconcile
// beginning at that roll. This is the M1 fix wired at assemble_reconciliation_snapshots.
TEST(ListedDispersionPipeline, ReconcileListedSchedule_TrimsWarmupLeadIn) {
  const std::vector<PricedSurface> day0 = surfaces(kNow0, 0.0);
  const std::vector<PricedSurface> day1 = surfaces(kNow1, 2.0);
  const std::vector<PricedSurface> day2 = surfaces(kNow2, -1.0);

  ListedDispersionSchedule schedule;
  schedule.rolls.push_back(roll(selection("2026-07-11", kNow1, kExpiry1, 1u), day1, 4u));
  ASSERT_EQ(schedule.rolls.front().roll_date, "2026-07-11");

  auto set0 = SurfaceSet::create(pointers(day0));
  auto set1 = SurfaceSet::create(pointers(day1));
  auto set2 = SurfaceSet::create(pointers(day2));
  ASSERT_TRUE(set0 && set1 && set2);
  const std::vector<ListedOptionQuote> quotes1 =
      quotes_for(schedule.rolls[0], *set1, "2026-07-11", kNow1, 0u);
  const std::vector<ListedOptionQuote> quotes2 =
      quotes_for(schedule.rolls[0], *set2, "2026-07-12", kNow2, 1000u);

  const std::vector<ListedReconciliationSnapshot> full = {
      {"2026-07-10", kNow0, &*set0, {}},      // warm-up lead-in (before first roll)
      {"2026-07-11", kNow1, &*set1, quotes1}, // first roll / entry
      {"2026-07-12", kNow2, &*set2, quotes2}, // held
  };
  const std::size_t first_roll_index = 1u; // day1 is the first roll date in `full`

  // The production pattern (full clock timeline) is now ACCEPTED at the low level
  // too, since the trunk's C2 (276239d) relaxed the front-date hard-require — see
  // the note on ReconcileClockCoupling_ToleratesWarmupLeadInButNotAMissingEntry.
  // The two routes differ in SHAPE, not in success: C2 keeps the pre-entry session
  // as a flat position-free row (one row per snapshot, aligned with the canonical
  // backtest), whereas the M1 seam TRIMS it, so the seam emits one row fewer. That
  // difference is asserted directly below.
  const auto untrimmed = reconcile_listed_dispersion(schedule, full);
  ASSERT_TRUE(untrimmed) << untrimmed.error().to_string();
  EXPECT_EQ(untrimmed->rows.size(), full.size());

  // ... and the seam trims the lead-in and succeeds.
  auto trimmed = reconcile_listed_schedule(schedule, full);
  ASSERT_TRUE(trimmed) << (trimmed ? std::string{} : trimmed.error().to_string());

  // Equal to a manually-trimmed reconcile that starts at the first roll date.
  auto manual = reconcile_listed_dispersion(
      schedule, std::span<const ListedReconciliationSnapshot>(full).subspan(first_roll_index));
  ASSERT_TRUE(manual) << (manual ? std::string{} : manual.error().to_string());
  EXPECT_EQ(*trimmed, *manual);

  // The assembler alone returns the same trimmed timeline (front date == first roll).
  auto assembled = assemble_reconciliation_snapshots(full, schedule);
  ASSERT_TRUE(assembled) << (assembled ? std::string{} : assembled.error().to_string());
  ASSERT_EQ(assembled->size(), full.size() - first_roll_index);
  EXPECT_EQ(assembled->front().date, schedule.rolls.front().roll_date);

  // A timeline that never contains the first roll date is an explicit error, not a
  // silent empty reconcile.
  const std::vector<ListedReconciliationSnapshot> no_roll = {full.front()}; // day0 only
  EXPECT_FALSE(assemble_reconciliation_snapshots(no_roll, schedule));
  EXPECT_FALSE(reconcile_listed_schedule(schedule, no_roll));
}

// M1 END-TO-END (Wave B final review, Important #1). The two tests above stop at
// the seam — they never call the validator that run-backtest invokes on the very
// next line, which is why the defect shipped. Trimming makes the reconciliation
// SHORTER than the backtest, and validate_listed_reconciliation_backtest used to
// hard-require equal row counts, so a warm-up lead-in still aborted the run one
// call downstream under a misleading message ("invalid tolerance or row count").
// This test drives the PRODUCTION pair — reconcile_listed_schedule over the full
// clock timeline, then the validator against a full-clock backtest — with a
// nonzero lead-in.
TEST(ListedDispersionPipeline, ValidateReconciliationAcceptsWarmupLeadIn) {
  const std::vector<PricedSurface> day0 = surfaces(kNow0, 0.0);
  const std::vector<PricedSurface> day1 = surfaces(kNow1, 2.0);
  const std::vector<PricedSurface> day2 = surfaces(kNow2, -1.0);

  ListedDispersionSchedule schedule;
  schedule.rolls.push_back(roll(selection("2026-07-11", kNow1, kExpiry1, 1u), day1, 4u));

  auto set0 = SurfaceSet::create(pointers(day0));
  auto set1 = SurfaceSet::create(pointers(day1));
  auto set2 = SurfaceSet::create(pointers(day2));
  ASSERT_TRUE(set0 && set1 && set2);
  const std::vector<ListedOptionQuote> quotes1 =
      quotes_for(schedule.rolls[0], *set1, "2026-07-11", kNow1, 0u);
  const std::vector<ListedOptionQuote> quotes2 =
      quotes_for(schedule.rolls[0], *set2, "2026-07-12", kNow2, 1000u);

  // The full clock.refs() timeline run-backtest hands the reconciler: one
  // warm-up session ahead of the first roll, then the roll and a held session.
  const std::vector<ListedReconciliationSnapshot> full = {
      {"2026-07-10", kNow0, &*set0, {}},
      {"2026-07-11", kNow1, &*set1, quotes1},
      {"2026-07-12", kNow2, &*set2, quotes2},
  };

  auto reconciliation = reconcile_listed_schedule(schedule, full);
  ASSERT_TRUE(reconciliation)
      << (reconciliation ? std::string{} : reconciliation.error().to_string());
  // The lead-in really is trimmed: fewer reconciliation rows than clock sessions.
  ASSERT_EQ(reconciliation->rows.size(), full.size() - 1u);

  // The backtest the engine produces alongside it: one row per clock session,
  // INCLUDING the warm-up. Its option P&L is reconstructed from the
  // reconciliation for the sessions they share (row 0 is the warm-up, all zero),
  // so the only thing under test is the row-count / date-alignment contract, not
  // the P&L arithmetic.
  BacktestResult backtest;
  backtest.date = {"2026-07-10", "2026-07-11", "2026-07-12"};
  backtest.ts_ns = {kNow0, kNow1, kNow2};
  backtest.pnl_total.assign(backtest.date.size(), 0.0);
  backtest.pnl_settlement.assign(backtest.date.size(), 0.0);
  backtest.pnl_shares.assign(backtest.date.size(), 0.0);
  backtest.financing.assign(backtest.date.size(), 0.0);
  backtest.cost.assign(backtest.date.size(), 0.0);
  for (std::size_t i = 0; i < reconciliation->rows.size(); ++i) {
    backtest.pnl_total[i + 1] = reconciliation->rows[i].model_option_pnl;
  }
  ASSERT_EQ(backtest.date[1], reconciliation->rows.front().date);

  // THE GATE. Before the fix this returned InvalidArgument purely because
  // 2 rows != 3 rows, defeating M1 in production.
  const auto ok = validate_listed_reconciliation_backtest(*reconciliation, backtest);
  EXPECT_TRUE(ok) << (ok ? std::string{} : ok.error().to_string());

  // Suffix-ness is still enforced, not merely relaxed: a reconciliation that
  // stops before the backtest's last session is rejected.
  BacktestResult longer = backtest;
  longer.date.push_back("2026-07-13");
  longer.ts_ns.push_back(kNow2 + 1);
  longer.pnl_total.push_back(0.0);
  longer.pnl_settlement.push_back(0.0);
  longer.pnl_shares.push_back(0.0);
  longer.financing.push_back(0.0);
  longer.cost.push_back(0.0);
  EXPECT_FALSE(validate_listed_reconciliation_backtest(*reconciliation, longer));

  // ... and a reconciliation whose first date is absent from the backtest is an
  // error rather than a silent pass.
  BacktestResult disjoint = backtest;
  disjoint.date = {"2026-08-10", "2026-08-11", "2026-08-12"};
  EXPECT_FALSE(validate_listed_reconciliation_backtest(*reconciliation, disjoint));

  // With no lead-in the offset is zero and the comparison is the historical
  // row-for-row one, unchanged.
  BacktestResult exact;
  exact.date = {"2026-07-11", "2026-07-12"};
  exact.ts_ns = {kNow1, kNow2};
  exact.pnl_total = {reconciliation->rows[0].model_option_pnl,
                     reconciliation->rows[1].model_option_pnl};
  exact.pnl_settlement.assign(2, 0.0);
  exact.pnl_shares.assign(2, 0.0);
  exact.financing.assign(2, 0.0);
  exact.cost.assign(2, 0.0);
  const auto zero_lead_in = validate_listed_reconciliation_backtest(*reconciliation, exact);
  EXPECT_TRUE(zero_lead_in)
      << (zero_lead_in ? std::string{} : zero_lead_in.error().to_string());
}

// ── Task 3 — build_listed_dispersion_schedule + acceptance gate (M7) ──────────
//
// The full builder needs live OPRA parquet + surfaces, so its economic output is
// pinned byte-identical at T10 (trade_schedule golden b640b3ab...). Here we drive
// the *pure* acceptance logic that does not touch parquet, through the extracted
// `accept_listed_schedule` seam.

namespace {

// Build a schedule with `count` real rolls off the synthetic surfaces. Each roll
// carries only two names (N0, N1) — far below core_min_names_per_roll (40) — which
// is exactly the shape that proves the acceptance gate does NOT enforce a
// names-per-roll floor (that literal is inert in the example's build path).
ListedDispersionSchedule schedule_with_rolls(std::size_t count) {
  const std::vector<PricedSurface> day = surfaces(kNow0, 0.0);
  ListedDispersionSchedule schedule;
  for (std::size_t index = 0; index < count; ++index) {
    const std::string date = "2026-07-" + std::to_string(10 + index);
    const std::uint32_t cohort = static_cast<std::uint32_t>(index + 1u);
    schedule.rolls.push_back(
        roll(selection(date, kNow0, kExpiry0, 1u + 10u * cohort), day, cohort));
  }
  return schedule;
}

} // namespace

// The acceptance gate is the verbatim entry/three-roll gate from
// build_schedule_command (spy_dispersion_backtest.cpp:532-534): reject an empty
// roll set, and in core mode reject fewer than core_min_rolls (3) rolls. It must
// NOT introduce any new gate — in particular no <40 names-per-roll floor (that
// methodology field is inert in the build path).
TEST(ListedDispersionPipeline, BuildSchedule_RejectsEmptyAndSubThreshold) {
  const ListedDispersionMethodology method{};
  ASSERT_EQ(method.core_min_rolls, 3u);

  ListedScheduleSpec loose{};   // core_mode == false (entry gate only)
  ListedScheduleSpec strict{};  // core mode (three-roll gate)
  strict.core_mode = true;

  const ListedDispersionSchedule empty{};

  // Empty roll set fails the entry gate under BOTH modes with Unavailable and the
  // pinned message.
  const auto empty_loose = accept_listed_schedule(empty, loose, method);
  ASSERT_FALSE(empty_loose);
  EXPECT_EQ(empty_loose.error().code(), ErrorCode::Unavailable);
  EXPECT_NE(empty_loose.error().to_string().find("entry/three-roll acceptance gate"),
            std::string::npos);
  const auto empty_strict = accept_listed_schedule(empty, strict, method);
  ASSERT_FALSE(empty_strict);
  EXPECT_EQ(empty_strict.error().code(), ErrorCode::Unavailable);

  // One roll: accepted in loose (non-core) mode, rejected in core mode
  // (< core_min_rolls). Two rolls: still rejected in core mode.
  const ListedDispersionSchedule one = schedule_with_rolls(1u);
  EXPECT_TRUE(accept_listed_schedule(one, loose, method));
  const auto one_core = accept_listed_schedule(one, strict, method);
  ASSERT_FALSE(one_core);
  EXPECT_EQ(one_core.error().code(), ErrorCode::Unavailable);

  const ListedDispersionSchedule two = schedule_with_rolls(2u);
  EXPECT_FALSE(accept_listed_schedule(two, strict, method));

  // Three rolls satisfy the core-mode three-roll gate — even though every roll
  // carries only two names, far below the 40-name floor `RunVerifyOptions` applies
  // at archive-verify time. This pins that the extraction did NOT silently
  // activate a names-per-roll floor in the build path (the acceptance gate is
  // rolls-only). The floor is deliberately not a field of the methodology policy:
  // no build-path consumer reads one.
  constexpr std::uint32_t kVerifyNamesPerRollFloor = 40u; // RunVerifyOptions' value
  const ListedDispersionSchedule three = schedule_with_rolls(3u);
  ASSERT_EQ(three.rolls.size(), 3u);
  for (const ListedScheduleRoll &r : three.rolls) {
    EXPECT_LT(r.n_names, kVerifyNamesPerRollFloor);
  }
  EXPECT_TRUE(accept_listed_schedule(three, strict, method));
  EXPECT_TRUE(accept_listed_schedule(three, loose, method));
}

// The full builder symbol exists with the planned signature. Its economic path is
// parquet-backed (pinned at T10); here we only pin the declaration/link + contract.
TEST(ListedDispersionPipeline, BuildScheduleSymbolIsDeclared) {
  // T9/O4: the builder gained a trailing optional `PhaseTimer *` (default nullptr) so
  // the CLI can charge the `selection` / `quote_join` diagnostics phases the extraction
  // would otherwise have collapsed. A null timer is economically identical.
  using BuilderFn = Result<ListedDispersionSchedule> (*)(
      const Clock &, const ListedScheduleSpec &, const ListedDispersionMethodology &,
      std::span<const UniverseRow>, const ListedDefinitionTable &, const RunSpec &, PhaseTimer *);
  const BuilderFn fn = &build_listed_dispersion_schedule;
  EXPECT_NE(reinterpret_cast<const void *>(fn), nullptr);

  // F-6: the shipped CLI calls this audited form, whose mandatory sink receives
  // every attempted selection (including no-basket dates) for artifact publication.
  using AuditedBuilderFn = Result<ListedDispersionSchedule> (*)(
      const Clock &, const ListedScheduleSpec &, const ListedDispersionMethodology &,
      std::span<const UniverseRow>, const ListedDefinitionTable &, const RunSpec &, PhaseTimer *,
      const ListedQuoteRejectSink &);
  const AuditedBuilderFn audited = &build_listed_dispersion_schedule_audited;
  EXPECT_NE(reinterpret_cast<const void *>(audited), nullptr);
}

// ── REV-FIXTAIL I-A — the three F6 quote-quality keys reach the SHIPPED route ──
//
// `quote_min_bid`, `quote_max_age_ns` and `quote_reject_locked` bind by name in
// the strict typed reader (dispersion_run.cpp), passed `reject_unknown()`, and
// were then written into `run_config.tsv` — an artifact dispersion_run.hpp
// documents as "the EFFECTIVE value of every execution knob the run actually
// used". Their only consumer was `dispersion_build_schedule`, a LIBRARY-ONLY
// entry point with no shipped caller. The shipped `build-schedule` now constructs
// the same `ListedScheduleSpec`, routes its quality policy through selection, and
// uses the audited builder's rejection sink to publish `quote_rejects.tsv`.
//
// The wiring is gated HERE rather than by a comment because the selection config
// the builder runs under is now built by one named function that both the loop
// and this test call — the same reason F5's `make_listed_replay_run_config`
// exists. A "verbatim" comment cannot fail; this can.
TEST(ListedDispersionPipeline, ScheduleSpecQualityPolicyReachesSelection) {
  ListedScheduleSpec spec{};
  spec.target_dte_days = 45.0;
  spec.min_dte_days = 20.0;
  spec.max_dte_days = 70.0;
  spec.min_names = 12u;
  spec.quality.min_bid = 0.05;
  spec.quality.max_quote_age_ns = 300'000'000'000LL;
  spec.quality.reject_locked = true;

  const ListedDispersionSelectionConfig selection = listed_selection_config_from(spec);

  // The four the loop already carried, so the lift stays verbatim.
  EXPECT_DOUBLE_EQ(selection.target_dte_days, 45.0);
  EXPECT_DOUBLE_EQ(selection.min_dte_days, 20.0);
  EXPECT_DOUBLE_EQ(selection.max_dte_days, 70.0);
  EXPECT_EQ(selection.min_names, 12u);
  // The three that did not.
  EXPECT_DOUBLE_EQ(selection.quality.min_bid, 0.05)
      << "spec key `quote_min_bid` is published as effective and never reached selection";
  EXPECT_EQ(selection.quality.max_quote_age_ns, 300'000'000'000LL)
      << "spec key `quote_max_age_ns` is published as effective and never reached selection";
  EXPECT_TRUE(selection.quality.reject_locked)
      << "spec key `quote_reject_locked` is published as effective and never reached selection";
}

// The other half: wiring the three cannot move a golden. Every quality default on
// `ListedScheduleSpec` is exactly the value `ListedDispersionSelectionConfig`
// already default-constructed inside the selection loop, so a spec naming none of
// the three produces a byte-identical selection config before and after. (Measured
// alongside: no run_spec.tsv under the published corpus root names any of the
// three keys, so no pinned run changes.) If a default here ever diverges, this
// pins the day it happens.
TEST(ListedDispersionPipeline, DefaultScheduleSpecKeepsTheShippedSelectionDefaults) {
  const ListedDispersionSelectionConfig selection =
      listed_selection_config_from(ListedScheduleSpec{});
  const ListedDispersionSelectionConfig pinned{}; // what the loop built before

  EXPECT_DOUBLE_EQ(selection.target_dte_days, pinned.target_dte_days);
  EXPECT_DOUBLE_EQ(selection.min_dte_days, pinned.min_dte_days);
  EXPECT_DOUBLE_EQ(selection.max_dte_days, pinned.max_dte_days);
  EXPECT_EQ(selection.min_names, pinned.min_names);
  EXPECT_DOUBLE_EQ(selection.required_multiplier, pinned.required_multiplier);
  EXPECT_DOUBLE_EQ(selection.quality.min_bid, pinned.quality.min_bid);
  EXPECT_EQ(selection.quality.max_quote_age_ns, pinned.quality.max_quote_age_ns);
  EXPECT_EQ(selection.quality.reject_locked, pinned.quality.reject_locked);
}

// ── Task 4 — project_listed_schedule + I1 two-route cold parity (M6, I1) ──────
//
// project_listed_schedule authors the projected_schedule marks; run-projected-backtest
// --execution cold recomputes replay marks. I1 requires the two routes to share ONE
// asserted parity constant (analytic=true + QueryExecution::ColdReference) so the two
// never drift — the I1 root cause was two hand-maintained copies. ProjectionConfig{}
// is that single constant.

// The single asserted parity constant is canonically cold: certified analytic greeks
// on the ColdReference route, no fast tier. Both cold routes read THIS default.
TEST(ListedDispersionPipeline, ProjectionConfigColdIsCanonical) {
  // Pinned at COMPILE time, not merely at run time: the parity constant is a
  // default-member-initializer, so a drift in it is a compile error here rather
  // than a test that must be run to notice.
  static_assert(ProjectionConfig{}.analytic, "the cold parity constant must be analytic");
  static_assert(ProjectionConfig{}.execution == QueryExecution::ColdReference,
                "the cold parity constant must be the ColdReference route");
  const ProjectionConfig cfg{};
  EXPECT_TRUE(cfg.analytic);
  EXPECT_EQ(cfg.execution, QueryExecution::ColdReference);
}

// I1 (the headline gate): bit-exact leg-mark parity. project_listed_schedule reprices
// each frozen listed roll onto surface ATM-forward strikes with COLD certified greeks
// (make_listed_risk_lookup cold seed). The projected schedule's persisted leg marks
// must equal the live cold seed marks the projected-backtest replay recomputes through
// the SAME make_listed_risk_lookup / full_greek_seed(..., analytic=true, ColdReference)
// — keyed on the SAME ProjectionConfig constant. Asserted on the raw doubles (EXPECT_EQ).
TEST(ListedDispersionPipeline, TwoRouteColdParity_LegMarksEqual) {
  const fs::path dir = fresh_dir();
  const std::vector<PricedSurface> day0 = surfaces(kNow0, 0.0);
  const std::string path = write_archive(dir, "2026-07-10", day0);

  // One-roll listed schedule off the synthetic surfaces at kNow0 (built via the
  // SurfaceSet/fair_value listed path — how the real listed build authors a roll).
  ListedDispersionSchedule listed;
  listed.rolls.push_back(roll(selection("2026-07-10", kNow0, kExpiry0, 1u), day0, 1u));
  ASSERT_EQ(listed.rolls.front().valuation_ts_ns, kNow0);
  ASSERT_EQ(listed.rolls.front().roll_date, "2026-07-10");

  // Load the snapshot for the roll date; the per-roll archive lookup borrows it.
  auto loaded = MarketSnapshot::load(path);
  ASSERT_TRUE(loaded) << (loaded ? std::string{} : loaded.error().to_string());
  const MarketSnapshot &snapshot = *loaded;
  ASSERT_EQ(snapshot.ts_ns(), kNow0);

  const ListedArchiveLookup archives =
      [&](std::string_view roll_date) -> Result<const MarketSnapshot *> {
    if (roll_date == "2026-07-10") {
      return atx::core::Ok(&snapshot);
    }
    return atx::core::Err(ErrorCode::NotFound, "no archive for roll date");
  };

  // Route 1 — the cold projection authors the projected_schedule marks.
  const ProjectionConfig cfg{}; // {analytic:true, execution:ColdReference} — I1 constant
  auto projected = project_listed_schedule(listed, archives, cfg);
  ASSERT_TRUE(projected) << (projected ? std::string{} : projected.error().to_string());
  ASSERT_EQ(projected->rolls.size(), 1u);
  const ListedScheduleRoll &proll = projected->rolls.front();
  ASSERT_EQ(proll.legs.size(), 2u * (1u + proll.n_names));
  ASSERT_FALSE(proll.legs.empty());
  // Projection preserves roll identity; only per-member strike + cold greeks change.
  EXPECT_EQ(proll.roll_date, listed.rolls.front().roll_date);
  EXPECT_EQ(proll.valuation_ts_ns, listed.rolls.front().valuation_ts_ns);
  EXPECT_EQ(proll.expiry_ts_ns, listed.rolls.front().expiry_ts_ns);
  EXPECT_EQ(proll.cohort, listed.rolls.front().cohort);
  EXPECT_EQ(proll.n_names, listed.rolls.front().n_names);

  // Route 2 — independently recompute each leg's cold seed mark through the SAME
  // make_listed_risk_lookup the projected-backtest replay uses, at the SAME residual T
  // and the SAME ProjectionConfig knobs. residual_T derives from the projected roll,
  // which preserves the listed valuation/expiry, so it equals the projection's residual.
  const double residual_T =
      static_cast<double>(proll.expiry_ts_ns - proll.valuation_ts_ns) / kNsPerYear;
  const ListedRiskLookup replay_lookup =
      make_listed_risk_lookup(snapshot, residual_T, cfg.analytic, cfg.execution);

  for (const ListedScheduleLeg &leg : proll.legs) {
    ListedOptionQuote quote;
    quote.strike = leg.strike; // the projected surface ATM-forward strike
    quote.side = leg.side;
    auto replay = replay_lookup(leg.uid, quote);
    ASSERT_TRUE(replay) << (replay ? std::string{} : replay.error().to_string());
    // Bit-exact: the persisted projected mark equals the live cold seed mark.
    EXPECT_EQ(leg.model_mark, replay->model_mark);
    EXPECT_GT(leg.model_mark, 0.0); // marks are meaningful (nonzero), not vacuous parity
  }

  // A roll date with no archive propagates the lookup error (structural guard intact).
  ListedDispersionSchedule orphan;
  orphan.rolls.push_back(roll(selection("2099-01-01", kNow0, kExpiry0, 1u), day0, 1u));
  EXPECT_FALSE(project_listed_schedule(orphan, archives, cfg));

  std::error_code error;
  fs::remove_all(dir, error);
}

// ── Task 5 — dispersion_book_var (M8) ─────────────────────────────────────────
//
// dispersion_book_var lifts run_projected_var_command's book -> OptionProjectionSpec
// synthesis + PreparedHistoricalProjection::evaluate_into + projected_historical_var
// per confidence. The book is pre-built by the caller, so the lift itself applies no
// vega scaling of any kind. The library re-projects the book positions across
// historical scenarios and splits the loss quantile per requested confidence; the
// CLI keeps the three bespoke TSV emissions (out-of-archive per the partition rule).
//
// REV-TAIL M-5 / I-2, two corrections to what this block used to say. (1) The
// "per-vol-point vega * 100 scaling in the DispersionConfig
// builder (a CLI boundary wired at T9)" no longer exists: E1 redefined
// `DispersionConfig::target_vega` as dollars per VOL POINT, and there is now no
// scaling anywhere on this route. (2) The `spy_dispersion_backtest.cpp:1119-1194`
// line references described a CLI body that no longer exists -- `run-projected-var`
// is a dispatch into `dispersion_run_projected_var`, which since REV-TAIL I-2 calls
// THIS function, so this test now gates the shipped route rather than a duplicate
// of it.
//
// The lift reads `dispersion.projected_maturity`, which was defined OUTSIDE the
// lifted range, so the relative-template maturity is a required input here (the
// plan's book-only signature omits it; days(N) is a relative template that MUST NOT
// be reconstructed to an absolute expiry, or per-scenario aging would change).
TEST(ListedDispersionPipeline, DispersionBookVar_SplitsConfidences) {
  // A small dispersion book off the synthetic surfaces: SPY (uid 1) index + two
  // names (uid 2, 3) => six positions (call/put per straddle across index + 2 names).
  DispersionUniverse universe;
  universe.index = DispersionMember{"SPY", 1u, 0.0};
  universe.names.push_back(DispersionMember{"N0", 2u, 0.4});
  universe.names.push_back(DispersionMember{"N1", 3u, 0.6});

  const std::vector<PricedSurface> book_surfaces = surfaces(kNow0, 0.0);
  auto book_set = SurfaceSet::create(pointers(book_surfaces));
  ASSERT_TRUE(book_set) << (book_set ? std::string{} : book_set.error().to_string());

  constexpr double kTargetDteDays = 30.0;
  const ProjectedMaturitySpec maturity =
      ProjectedMaturitySpec::days(static_cast<std::int32_t>(std::llround(kTargetDteDays)));

  DispersionConfig dispersion;
  dispersion.target_T = kTargetDteDays / 365.25;
  // UNITS (E1 / AN-P1-1). `DispersionConfig::target_vega` is DOLLARS OF VEGA PER
  // VOL POINT (dispersion.hpp:249-262), so a $100/vol-point index leg is written
  // as `100.0` and scaled by nothing.
  //
  // REV-TAIL M-5: this line read `100.0 * kVegaVolPointToUnitVol`, under a comment
  // claiming to "mirror the CLI boundary (spy_dispersion_backtest.cpp:1110)". E1
  // abolished that boundary -- the constant had no call site left at all and was
  // deleted in the C-2 follow-up -- so the line denoted $10,000/vol-point where $100 was
  // intended, a 100x book. Every assertion in this test is structural (counts,
  // n_failed, ES >= VaR, VaR(99) >= VaR(95), reference == frames.back().value) and
  // therefore SCALE-INVARIANT: it passed either way and could never self-correct,
  // which made it the most misleading text in the tree about the exact unit hazard
  // that produced the 100x bug this sprint fixed. Do not reintroduce a scaling
  // here without changing what `target_vega` means.
  dispersion.target_vega = 100.0;
  dispersion.side = DispersionSide::ShortIndexLongNames;
  dispersion.multiplier = 100.0;
  dispersion.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, 2u};
  dispersion.projected_maturity = maturity;
  auto book = build_dispersion_book(universe, *book_set, dispersion);
  ASSERT_TRUE(book) << (book ? std::string{} : book.error().to_string());
  ASSERT_EQ(book->positions.size(), 6u);

  // Three historical scenarios (surfaces at distinct valuation timestamps). Keep the
  // surface vectors + SurfaceSets alive for the whole dispersion_book_var call.
  const std::vector<PricedSurface> s0 = surfaces(kNow0, 0.0);
  const std::vector<PricedSurface> s1 = surfaces(kNow1, 5.0);
  const std::vector<PricedSurface> s2 = surfaces(kNow2, -5.0);
  auto set0 = SurfaceSet::create(pointers(s0));
  auto set1 = SurfaceSet::create(pointers(s1));
  auto set2 = SurfaceSet::create(pointers(s2));
  ASSERT_TRUE(set0 && set1 && set2);
  const std::vector<HistoricalProjectionScenario> scenarios = {
      {kNow0, &*set0}, {kNow1, &*set1}, {kNow2, &*set2}};

  const std::vector<double> confidences = {0.95, 0.99};
  HistoricalProjectionConfig hp_cfg;
  hp_cfg.n_threads = 1;

  auto var = dispersion_book_var(*book, maturity, scenarios, confidences, hp_cfg);
  ASSERT_TRUE(var) << (var ? std::string{} : var.error().to_string());

  // n_positions matches the book; frames one-per-scenario; legs scenario-major.
  EXPECT_EQ(var->n_positions, book->positions.size());
  ASSERT_EQ(var->frames.size(), scenarios.size());
  EXPECT_EQ(var->legs.size(), scenarios.size() * book->positions.size());

  // Every scenario projected cleanly (no failed legs; all positions ok).
  for (const HistoricalProjectionFrame &frame : var->frames) {
    EXPECT_EQ(frame.n_failed, 0u);
    EXPECT_EQ(frame.n_ok, book->positions.size());
  }

  // The confidences split into two risks carrying exactly 0.95 / 0.99, each over all
  // three successful scenarios.
  ASSERT_EQ(var->risks.size(), 2u);
  EXPECT_EQ(var->risks[0].confidence, 0.95);
  EXPECT_EQ(var->risks[1].confidence, 0.99);
  for (const ProjectedHistoricalVar &risk : var->risks) {
    EXPECT_EQ(risk.n_scenarios, scenarios.size());
  }

  // Free risk invariants — these cost nothing and are what actually catches a
  // synthesis that wires the confidences, the tail or the reference the wrong way
  // round. The structural assertions above would all still pass under such a bug.
  for (const ProjectedHistoricalVar &risk : var->risks) {
    // ES is the mean of the tail beyond VaR, so it can never be the smaller loss.
    EXPECT_GE(risk.expected_shortfall, risk.value_at_risk) << risk.confidence;
    // The reference is the book's current value: the LAST (most recent) scenario
    // frame, not the first — swapping them silently reverses every P&L sign.
    EXPECT_EQ(risk.reference_value, var->frames.back().value);
  }
  // A deeper confidence is a worse loss: VaR(99%) >= VaR(95%).
  EXPECT_GE(var->risks[1].value_at_risk, var->risks[0].value_at_risk);
  EXPECT_GE(var->risks[1].expected_shortfall, var->risks[0].expected_shortfall);
}

// ── (h) Mark divergence collector (L10) ───────────────────────────────────────
// PROVENANCE OF THE GATES BELOW — read this before adding or relaxing one.
//   * `BpsMetricMatchesTheFrozenFormula` and the five hand-built-`StepEvent` tests
//     prove ARITHMETIC AND PLUMBING ONLY. They construct the event themselves, so
//     they say nothing about where — or whether — the engine fires the hook.
//   * `MarkDivergenceObserverRidesTheEngineStepHook` is the ENGINE-INTEGRATION gate:
//     it drives the real `run_backtest` strategy overload and asserts a nonzero row
//     count came back through `RunConfig::step_observer`. That is the only test here
//     that can fail if the hook moves to a position where the strategy's per-step
//     divergence record has already been cleared or overwritten.
// A comparison that would pass on zero rows is not a gate: the production cold route
// legitimately emits `mark_divergence rows=0`, so every gate below either asserts a
// row count > 0 or pairs its zero-row assertion with a nonzero control.

TEST(ListedDispersionPipeline, BpsMetricMatchesTheFrozenFormula) {
  // Dyadic inputs on purpose, so each expectation is an exactly representable literal
  // a reader derives by hand from |live - schedule| / |schedule| * 1e4 — not a value
  // read back out of the function under test. (Decimal cases like 2.0 -> 2.02 are NOT
  // bit-exactly 100.0 in binary64; see the report's plan-error note.)
  EXPECT_EQ(listed_mark_divergence_bps(2.0, 2.5), 2500.0);
  // Absolute value: a live mark BELOW the frozen mark reports the same magnitude.
  EXPECT_EQ(listed_mark_divergence_bps(2.0, 1.5), 2500.0);
  // The denominator is |schedule_mark|, so a negative frozen mark still yields a
  // positive bps rather than a sign-flipped one.
  EXPECT_EQ(listed_mark_divergence_bps(-1.0, -1.25), 2500.0);
  // No divergence, no bps — with a nonzero denominator, so this is not the L2 branch.
  EXPECT_EQ(listed_mark_divergence_bps(4.0, 4.0), 0.0);
  // Finding L2, PRESERVED deliberately: a zero frozen mark collapses the metric to
  // exactly 0.0 instead of reporting an infinite relative error, so a deep-OTM leg
  // with a frozen mark of 0 is UNDERSTATED. This value feeds a pinned artifact
  // column, so changing it is a deliberate economic decision, not a refactor.
  EXPECT_EQ(listed_mark_divergence_bps(0.0, 0.5), 0.0);
  EXPECT_EQ(listed_mark_divergence_bps(-0.0, 0.5), 0.0);
}

TEST(ListedDispersionPipeline, MarkDivergenceObserverCapturesThePerturbedLeg) {
  const std::vector<PricedSurface> source = surfaces(kNow0, 0.0);
  ListedDispersionSchedule schedule = divergence_schedule(source);
  ASSERT_GE(schedule.rolls.front().legs.size(), 2u);
  // Perturb the LAST leg (a constituent put, symbol "N1"), never the first (the index
  // call, "SPY"): a collector that reported `roll.legs.front()` instead of the leg it
  // actually matched would otherwise pass by coincidence.
  ListedScheduleLeg &target = schedule.rolls.front().legs.back();
  const double live_mark = target.model_mark;
  target.model_mark += 0.01;
  const ListedScheduleLeg perturbed = target;

  const fs::path dir = fresh_divergence_dir("capture");
  auto snapshot = MarketSnapshot::load(write_archive(dir, "2026-07-10", source));
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
  auto strategy = ListedDispersionStrategy::create(schedule, 0.0, ScheduleMarkPolicy::Record);
  ASSERT_TRUE(strategy.has_value()) << strategy.error().to_string();

  PortfolioState book;
  std::uint64_t next_id = 100u;
  const Status stepped = strategy->on_step(*snapshot, 0u, book, next_id);
  ASSERT_TRUE(stepped.has_value()) << stepped.error().to_string();
  // Anti-vacuity: the collector's INPUT exists and the roll really fired.
  ASSERT_EQ(strategy->last_mark_divergences().size(), 1u);
  ASSERT_EQ(strategy->next_roll_index(), 1u);

  std::vector<ListedMarkDivergenceRow> rows;
  const StepObserver observer = make_mark_divergence_observer(schedule, rows);
  const SnapshotRef ref{"2026-07-11", "observer-does-not-read-this.atxvsa"};
  const Status status = observer(StepEvent{0u, ref, *snapshot, *strategy});
  ASSERT_TRUE(status.has_value()) << status.error().to_string();
  ASSERT_EQ(rows.size(), 1u);
  const ListedMarkDivergenceRow &row = rows.front();

  // `date` is the OBSERVED step's clock date, not the roll's authored roll_date; the
  // fixture makes the two differ so the source is unambiguous.
  EXPECT_EQ(row.date, "2026-07-11");
  EXPECT_NE(row.date, schedule.rolls.front().roll_date);
  // symbol / raw_symbol come from the MATCHED leg, and they are distinct strings, so
  // neither writing the same field twice nor taking legs.front() survives.
  EXPECT_EQ(row.symbol, perturbed.symbol);
  EXPECT_EQ(row.raw_symbol, perturbed.raw_symbol);
  EXPECT_NE(row.symbol, row.raw_symbol);
  EXPECT_NE(row.symbol, schedule.rolls.front().legs.front().symbol);
  EXPECT_EQ(row.strike, perturbed.strike);
  EXPECT_EQ(row.expiry_ts_ns, perturbed.expiry_ts_ns);
  EXPECT_EQ(row.side, perturbed.side);
  // The marks: `schedule_mark` is the FROZEN (perturbed) mark, `live_mark` the mark
  // the surface actually produced. Swapping them flips the sign of `diff`.
  EXPECT_EQ(row.schedule_mark, perturbed.model_mark);
  EXPECT_EQ(row.live_mark, live_mark);
  EXPECT_NE(row.live_mark, row.schedule_mark);
  EXPECT_EQ(row.diff, live_mark - perturbed.model_mark);
  EXPECT_LT(row.diff, 0.0); // the frozen mark was perturbed UP, so live - frozen < 0
  EXPECT_EQ(row.abs_diff_bps_of_mark,
            std::abs(live_mark - perturbed.model_mark) / std::abs(perturbed.model_mark) * 1.0e4);
  EXPECT_GT(row.abs_diff_bps_of_mark, 0.0);
  EXPECT_TRUE(std::isfinite(row.abs_diff_bps_of_mark));
  std::error_code error;
  fs::remove_all(dir, error);
}

// MULTIPLICITY + ORDER. Every other fixture perturbs exactly one leg, which leaves the
// per-divergence loop and the `out.push_back` APPEND semantics untested — and "one row
// per divergence, in divergence order, accumulating" is precisely what T4 bit-compares
// against the shadow's arena. Three otherwise-invisible defects are gated here:
// clear-then-push, a `break` after the first push, and hoisting the push out of the loop.
// (Accumulation ACROSS steps is gated by …RidesTheEngineStepHook's two-roll clock.)
TEST(ListedDispersionPipeline, MarkDivergenceObserverAppendsOneRowPerDivergedLegInOrder) {
  const std::vector<PricedSurface> source = surfaces(kNow0, 0.0);
  ListedDispersionSchedule schedule = divergence_schedule(source);
  std::vector<ListedScheduleLeg> &legs = schedule.rolls.front().legs;
  // The roll is 2 x (1 index + 2 names) = 6 legs, ordered index call/put then each name's
  // call/put. Perturb legs[2] (the N0 CALL) and legs[5] (the N1 PUT): different symbol,
  // different side AND different strike, so the two rows cannot be confused for each
  // other and neither can stand in for the other under a matching bug.
  ASSERT_EQ(legs.size(), 6u);
  const double first_live = legs[2].model_mark;
  const double second_live = legs[5].model_mark;
  legs[2].model_mark += 0.01;
  legs[5].model_mark += 0.02; // a DIFFERENT perturbation, so the rows' diffs differ too
  const ListedScheduleLeg first = legs[2];
  const ListedScheduleLeg second = legs[5];
  ASSERT_NE(first.symbol, second.symbol);
  ASSERT_NE(first.side, second.side);
  ASSERT_NE(first.strike, second.strike);

  const fs::path dir = fresh_divergence_dir("multiplicity");
  auto snapshot = MarketSnapshot::load(write_archive(dir, "2026-07-10", source));
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
  auto strategy = ListedDispersionStrategy::create(schedule, 0.0, ScheduleMarkPolicy::Record);
  ASSERT_TRUE(strategy.has_value()) << strategy.error().to_string();
  PortfolioState book;
  std::uint64_t next_id = 100u;
  ASSERT_TRUE(strategy->on_step(*snapshot, 0u, book, next_id).has_value());
  // TWO divergences on one step: the input multiplicity the collector must reproduce.
  const std::vector<MarkDivergence> &divergences = strategy->last_mark_divergences();
  ASSERT_EQ(divergences.size(), 2u);

  std::vector<ListedMarkDivergenceRow> rows;
  const StepObserver observer = make_mark_divergence_observer(schedule, rows);
  const SnapshotRef ref{"2026-07-11", "observer-does-not-read-this.atxvsa"};
  const Status status = observer(StepEvent{0u, ref, *snapshot, *strategy});
  ASSERT_TRUE(status.has_value()) << status.error().to_string();
  // ONE row per divergence — not one row, not one per leg.
  ASSERT_EQ(rows.size(), divergences.size());
  ASSERT_EQ(rows.size(), 2u);

  // DIVERGENCE ORDER, asserted against the strategy's own sequence (a different object,
  // so this is not a restatement of the collector's own loop). Reversing or reordering
  // the loop moves these.
  for (std::size_t i = 0; i < rows.size(); ++i) {
    EXPECT_EQ(rows[i].strike, divergences[i].strike) << i;
    EXPECT_EQ(rows[i].expiry_ts_ns, divergences[i].expiry_ts_ns) << i;
    EXPECT_EQ(rows[i].side, divergences[i].side) << i;
    EXPECT_EQ(rows[i].schedule_mark, divergences[i].schedule_mark) << i;
    EXPECT_EQ(rows[i].live_mark, divergences[i].live_mark) << i;
  }
  // And against the fixture's independently known legs, so the relational loop above
  // cannot be satisfied by two copies of the same row.
  EXPECT_EQ(rows[0].symbol, first.symbol);
  EXPECT_EQ(rows[0].raw_symbol, first.raw_symbol);
  EXPECT_EQ(rows[0].side, first.side);
  EXPECT_EQ(rows[0].strike, first.strike);
  EXPECT_EQ(rows[0].schedule_mark, first.model_mark);
  EXPECT_EQ(rows[0].live_mark, first_live);
  EXPECT_EQ(rows[1].symbol, second.symbol);
  EXPECT_EQ(rows[1].raw_symbol, second.raw_symbol);
  EXPECT_EQ(rows[1].side, second.side);
  EXPECT_EQ(rows[1].strike, second.strike);
  EXPECT_EQ(rows[1].schedule_mark, second.model_mark);
  EXPECT_EQ(rows[1].live_mark, second_live);
  // The two rows are genuinely distinct, including their metrics — the perturbations
  // differ by 2x, so a duplicated row cannot satisfy both.
  EXPECT_NE(rows[0].symbol, rows[1].symbol);
  EXPECT_NE(rows[0].diff, rows[1].diff);
  EXPECT_NE(rows[0].abs_diff_bps_of_mark, rows[1].abs_diff_bps_of_mark);
  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(ListedDispersionPipeline, MarkDivergenceObserverIsSilentWhenNothingDiverged) {
  const std::vector<PricedSurface> source = surfaces(kNow0, 0.0);
  const ListedDispersionSchedule clean = divergence_schedule(source);
  ListedDispersionSchedule perturbed = clean;
  perturbed.rolls.front().legs.back().model_mark += 0.01;

  const fs::path dir = fresh_divergence_dir("silent");
  auto snapshot = MarketSnapshot::load(write_archive(dir, "2026-07-10", source));
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
  auto quiet_strategy = ListedDispersionStrategy::create(clean, 0.0, ScheduleMarkPolicy::Record);
  ASSERT_TRUE(quiet_strategy.has_value()) << quiet_strategy.error().to_string();
  auto loud_strategy = ListedDispersionStrategy::create(perturbed, 0.0, ScheduleMarkPolicy::Record);
  ASSERT_TRUE(loud_strategy.has_value()) << loud_strategy.error().to_string();

  PortfolioState quiet_book;
  std::uint64_t quiet_id = 100u;
  ASSERT_TRUE(quiet_strategy->on_step(*snapshot, 0u, quiet_book, quiet_id).has_value());
  PortfolioState loud_book;
  std::uint64_t loud_id = 100u;
  ASSERT_TRUE(loud_strategy->on_step(*snapshot, 0u, loud_book, loud_id).has_value());

  // The silence must mean "the roll fired and nothing diverged", never "no roll
  // fired": the cursor advanced and the book opened the whole roll.
  ASSERT_TRUE(quiet_strategy->all_rolls_consumed());
  ASSERT_EQ(quiet_strategy->next_roll_index(), 1u);
  ASSERT_EQ(quiet_book.lots.size(), clean.rolls.front().legs.size());
  ASSERT_TRUE(quiet_strategy->last_mark_divergences().empty());
  ASSERT_EQ(loud_strategy->last_mark_divergences().size(), 1u);

  // ONE observer, TWO events. The clean event must append nothing and the diverging
  // event must append a row, so the zero-row assertion cannot be satisfied by an
  // observer that never appends anything at all. Both schedules share every leg KEY
  // (only one leg's model_mark differs), so a single observer serves both.
  std::vector<ListedMarkDivergenceRow> rows;
  const StepObserver observer = make_mark_divergence_observer(clean, rows);
  const SnapshotRef ref{"2026-07-11", "observer-does-not-read-this.atxvsa"};

  const Status quiet = observer(StepEvent{0u, ref, *snapshot, *quiet_strategy});
  ASSERT_TRUE(quiet.has_value()) << quiet.error().to_string();
  EXPECT_TRUE(rows.empty());

  const Status loud = observer(StepEvent{0u, ref, *snapshot, *loud_strategy});
  ASSERT_TRUE(loud.has_value()) << loud.error().to_string();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows.front().symbol, clean.rolls.front().legs.back().symbol);
  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(ListedDispersionPipeline, MarkDivergenceObserverRejectsAForeignStrategy) {
  const std::vector<PricedSurface> source = surfaces(kNow0, 0.0);
  const ListedDispersionSchedule schedule = divergence_schedule(source);
  const fs::path dir = fresh_divergence_dir("foreign");
  auto snapshot = MarketSnapshot::load(write_archive(dir, "2026-07-10", source));
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  std::vector<ListedMarkDivergenceRow> rows;
  const StepObserver observer = make_mark_divergence_observer(schedule, rows);
  ForeignStrategy foreign;
  const SnapshotRef ref{"2026-07-11", "observer-does-not-read-this.atxvsa"};
  const Status status = observer(StepEvent{0u, ref, *snapshot, foreign});
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(status.error().message().find("not a ListedDispersionStrategy"), std::string::npos);
  // Fail-CLOSED: the run aborts. A fail-silent observer would return Ok here and the
  // caller would archive an empty mark_divergence section it believes was measured.
  EXPECT_TRUE(rows.empty());
  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(ListedDispersionPipeline, MarkDivergenceObserverRejectsAnUnmatchableLeg) {
  const std::vector<PricedSurface> source = surfaces(kNow0, 0.0);
  ListedDispersionSchedule schedule = divergence_schedule(source);
  schedule.rolls.front().legs.back().model_mark += 0.01;

  const fs::path dir = fresh_divergence_dir("unmatchable");
  auto snapshot = MarketSnapshot::load(write_archive(dir, "2026-07-10", source));
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
  auto strategy = ListedDispersionStrategy::create(schedule, 0.0, ScheduleMarkPolicy::Record);
  ASSERT_TRUE(strategy.has_value()) << strategy.error().to_string();
  PortfolioState book;
  std::uint64_t next_id = 100u;
  ASSERT_TRUE(strategy->on_step(*snapshot, 0u, book, next_id).has_value());
  ASSERT_EQ(strategy->last_mark_divergences().size(), 1u);

  // The observer sees a COPY whose leg keys have all moved, so the recorded
  // divergence can no longer be attributed to a leg. That must abort rather than
  // emit a row with an empty symbol.
  ListedDispersionSchedule shifted = schedule;
  for (ListedScheduleLeg &leg : shifted.rolls.front().legs) {
    leg.strike += 1.0;
  }
  std::vector<ListedMarkDivergenceRow> rows;
  const StepObserver observer = make_mark_divergence_observer(shifted, rows);
  const SnapshotRef ref{"2026-07-11", "observer-does-not-read-this.atxvsa"};
  const Status status = observer(StepEvent{0u, ref, *snapshot, *strategy});
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::NotFound);
  // Byte-identical to the shadow replay's message, so the CLI's error text does not
  // move when the shadow is deleted. Asserted as a whole string, not just the code.
  EXPECT_EQ(status.error().message(), "mark divergence leg not found in roll");
  EXPECT_TRUE(rows.empty());
  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(ListedDispersionPipeline, MarkDivergenceObserverGuardsTheRollCursorAndValuationDate) {
  const std::vector<PricedSurface> source = surfaces(kNow0, 0.0);
  ListedDispersionSchedule schedule = divergence_schedule(source);
  schedule.rolls.front().legs.back().model_mark += 0.01;

  const fs::path dir = fresh_divergence_dir("guards");
  auto snapshot = MarketSnapshot::load(write_archive(dir, "2026-07-10", source));
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
  auto strategy = ListedDispersionStrategy::create(schedule, 0.0, ScheduleMarkPolicy::Record);
  ASSERT_TRUE(strategy.has_value()) << strategy.error().to_string();
  PortfolioState book;
  std::uint64_t next_id = 100u;
  ASSERT_TRUE(strategy->on_step(*snapshot, 0u, book, next_id).has_value());
  ASSERT_EQ(strategy->last_mark_divergences().size(), 1u);
  const SnapshotRef ref{"2026-07-11", "observer-does-not-read-this.atxvsa"};

  // (a) `schedule` is an independent input, so the roll cursor is bounds-checked
  // rather than assumed: indexing rolls[cursor - 1] of an unrelated schedule would be
  // undefined behavior. The shadow replay could not hit this — it shared one schedule
  // object with its strategy.
  ListedDispersionSchedule no_rolls;
  std::vector<ListedMarkDivergenceRow> cursor_rows;
  const StepObserver cursor_observer = make_mark_divergence_observer(no_rolls, cursor_rows);
  const Status cursor = cursor_observer(StepEvent{0u, ref, *snapshot, *strategy});
  ASSERT_FALSE(cursor.has_value());
  EXPECT_EQ(cursor.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(cursor.error().message().find("roll cursor outside"), std::string::npos);
  EXPECT_TRUE(cursor_rows.empty());

  // (b) StepEvent::snapshot is load-bearing, not decorative: a roll valued one
  // nanosecond away from the observed snapshot is not this run's roll.
  ListedDispersionSchedule off_by_one_ns = schedule;
  off_by_one_ns.rolls.front().valuation_ts_ns += 1;
  std::vector<ListedMarkDivergenceRow> ts_rows;
  const StepObserver ts_observer = make_mark_divergence_observer(off_by_one_ns, ts_rows);
  const Status ts = ts_observer(StepEvent{0u, ref, *snapshot, *strategy});
  ASSERT_FALSE(ts.has_value());
  EXPECT_EQ(ts.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(ts.error().message().find("roll valuation date"), std::string::npos);
  EXPECT_TRUE(ts_rows.empty());

  // Control: the UNMODIFIED schedule over the very same event DOES produce a row, so
  // neither rejection above is an unrelated failure wearing a guard's name.
  std::vector<ListedMarkDivergenceRow> ok_rows;
  const StepObserver ok_observer = make_mark_divergence_observer(schedule, ok_rows);
  const Status ok = ok_observer(StepEvent{0u, ref, *snapshot, *strategy});
  ASSERT_TRUE(ok.has_value()) << ok.error().to_string();
  EXPECT_EQ(ok_rows.size(), 1u);
  std::error_code error;
  fs::remove_all(dir, error);
}

// THE ENGINE-INTEGRATION GATE. Everything above hand-builds its StepEvent and so cannot
// observe where — or whether — the engine fires the hook. This one drives the real
// `run_backtest` strategy overload with nothing but `RunConfig::step_observer` set, over
// a TWO-DATE / TWO-ROLL clock, and asserts three rows accumulated across two steps:
//
//   * a nonzero row count at all — if the hook fired anywhere that has already cleared
//     or overwritten last_mark_divergences_, this reports 0 and fails while every unit
//     test above still passes;
//   * rows from BOTH steps, which is the only collector-level coverage of T1's per-step
//     firing site (a one-date clock exercises the inception site alone);
//   * ACCUMULATION — `out` is appended to, never cleared per call. A per-call clear
//     leaves only the last step's rows, which no single-step test can see.
TEST(ListedDispersionPipeline, MarkDivergenceObserverRidesTheEngineStepHook) {
  const std::vector<PricedSurface> day0 = surfaces(kNow0, 0.0);
  const std::vector<PricedSurface> day1 = surfaces(kNow1, 2.0);
  ListedDispersionSchedule schedule;
  schedule.rolls.push_back(roll(selection("2026-07-10", kNow0, kExpiry0, 1u), day0, 4u));
  schedule.rolls.push_back(roll(selection("2026-07-11", kNow1, kExpiry1, 101u), day1, 5u));
  ASSERT_EQ(schedule.rolls[0].legs.size(), 6u);
  ASSERT_EQ(schedule.rolls[1].legs.size(), 6u);

  // ONE diverged leg on the first roll, TWO on the second. So the expected row sequence
  // is [roll0 N1-put, roll1 N0-call, roll1 N1-put]: a total that no per-call clear (2),
  // no `break` (2) and no hoisted push (2) can produce.
  const double live_a = schedule.rolls[0].legs[5].model_mark;
  schedule.rolls[0].legs[5].model_mark += 0.01;
  const double live_b = schedule.rolls[1].legs[2].model_mark;
  schedule.rolls[1].legs[2].model_mark += 0.01;
  const double live_c = schedule.rolls[1].legs[5].model_mark;
  schedule.rolls[1].legs[5].model_mark += 0.02;
  const ListedScheduleLeg leg_a = schedule.rolls[0].legs[5];
  const ListedScheduleLeg leg_b = schedule.rolls[1].legs[2];
  const ListedScheduleLeg leg_c = schedule.rolls[1].legs[5];

  const fs::path dir = fresh_divergence_dir("engine");
  CorpusManifest manifest;
  manifest.dates = {"2026-07-10", "2026-07-11"};
  CorpusEntry entry0;
  entry0.date = "2026-07-10";
  entry0.symbol = "SPY";
  entry0.status = CorpusFitStatus::Ok;
  entry0.archive_path = write_archive(dir, "2026-07-10", day0);
  CorpusEntry entry1;
  entry1.date = "2026-07-11";
  entry1.symbol = "SPY";
  entry1.status = CorpusFitStatus::Ok;
  entry1.archive_path = write_archive(dir, "2026-07-11", day1);
  manifest.entries.push_back(std::move(entry0));
  manifest.entries.push_back(std::move(entry1));
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  ASSERT_EQ(clock->size(), 2u);
  auto strategy = ListedDispersionStrategy::create(schedule, 0.0, ScheduleMarkPolicy::Record);
  ASSERT_TRUE(strategy.has_value()) << strategy.error().to_string();

  std::vector<ListedMarkDivergenceRow> rows;
  RunConfig config;
  config.prefetch_snapshots = false;
  config.step_observer = make_mark_divergence_observer(schedule, rows);
  const auto result = run_backtest(*clock, *strategy, config);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  // The run genuinely stepped twice and BOTH rolls genuinely fired.
  ASSERT_EQ(result->size(), 2u);
  ASSERT_TRUE(strategy->all_rolls_consumed());
  ASSERT_EQ(strategy->next_roll_index(), 2u);

  // THE GATE: three rows, accumulated across two steps, in divergence-then-step order.
  ASSERT_EQ(rows.size(), 3u);
  // Rows from BOTH steps: step 0's inception fire and step 1's per-step fire.
  EXPECT_EQ(rows[0].date, "2026-07-10");
  EXPECT_EQ(rows[1].date, "2026-07-11");
  EXPECT_EQ(rows[2].date, "2026-07-11");
  // Step 0's single row survived step 1 — i.e. `out` was appended to, not rebuilt.
  EXPECT_EQ(rows[0].symbol, leg_a.symbol);
  EXPECT_EQ(rows[0].raw_symbol, leg_a.raw_symbol);
  EXPECT_EQ(rows[0].side, leg_a.side);
  EXPECT_EQ(rows[0].strike, leg_a.strike);
  EXPECT_EQ(rows[0].expiry_ts_ns, leg_a.expiry_ts_ns);
  EXPECT_EQ(rows[0].schedule_mark, leg_a.model_mark);
  EXPECT_NEAR(rows[0].live_mark, live_a, 1.0e-12);
  // Step 1's two rows, in the roll's leg order (N0 call before N1 put).
  EXPECT_EQ(rows[1].symbol, leg_b.symbol);
  EXPECT_EQ(rows[1].raw_symbol, leg_b.raw_symbol);
  EXPECT_EQ(rows[1].side, leg_b.side);
  EXPECT_EQ(rows[1].strike, leg_b.strike);
  EXPECT_EQ(rows[1].expiry_ts_ns, leg_b.expiry_ts_ns);
  EXPECT_EQ(rows[1].schedule_mark, leg_b.model_mark);
  EXPECT_NEAR(rows[1].live_mark, live_b, 1.0e-12);
  EXPECT_EQ(rows[2].symbol, leg_c.symbol);
  EXPECT_EQ(rows[2].raw_symbol, leg_c.raw_symbol);
  EXPECT_EQ(rows[2].side, leg_c.side);
  EXPECT_EQ(rows[2].strike, leg_c.strike);
  EXPECT_EQ(rows[2].expiry_ts_ns, leg_c.expiry_ts_ns);
  EXPECT_EQ(rows[2].schedule_mark, leg_c.model_mark);
  EXPECT_NEAR(rows[2].live_mark, live_c, 1.0e-12);
  // Roll 1's expiry differs from roll 0's, so a collector that reused the previous
  // roll's legs is caught independently of the symbols.
  EXPECT_NE(rows[0].expiry_ts_ns, rows[1].expiry_ts_ns);
  for (const ListedMarkDivergenceRow &row : rows) {
    EXPECT_NE(row.live_mark, row.schedule_mark);
    EXPECT_LT(row.diff, 0.0); // every frozen mark was perturbed UP
    EXPECT_GT(row.abs_diff_bps_of_mark, 0.0);
    EXPECT_TRUE(std::isfinite(row.abs_diff_bps_of_mark));
  }
  std::error_code error;
  fs::remove_all(dir, error);
}
