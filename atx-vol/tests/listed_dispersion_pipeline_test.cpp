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
#include "atx/vol/dispersion.hpp"                 // DispersionMember, DispersionBook, build_dispersion_book
#include "atx/vol/historical_projection.hpp"      // HistoricalProjectionScenario/Frame/Config, ProjectedHistoricalVar
#include "atx/vol/listed_dispersion.hpp"          // ListedOptionQuote
#include "atx/vol/listed_dispersion_pipeline.hpp" // module under test
#include "atx/vol/listed_dispersion_reconciliation.hpp" // ListedReconciliationSnapshot, reconcile_listed_dispersion
#include "atx/vol/listed_dispersion_schedule.hpp" // ListedRiskLookup, ListedOptionRisk
#include "atx/vol/portfolio_pricer.hpp"           // SurfaceSet, kNsPerYear
#include "atx/vol/priced_surface.hpp"             // PricedSurface
#include "atx/vol/query_pricing.hpp"              // QueryExecution
#include "atx/vol/surface_archive.hpp"            // write_surface_archive_v2_file
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
    const PricedSurface *surface = surfaces.find(leg.uid);
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

} // namespace

// ── (a) kVegaVolPointToUnitVol (M9 / I4) ──────────────────────────────────────
static_assert(kVegaVolPointToUnitVol == 100.0,
              "vega per-vol-point to per-unit-vol factor must be exactly 100");

TEST(ListedDispersionPipeline, VegaVolPointConstantIs100) {
  EXPECT_EQ(kVegaVolPointToUnitVol, 100.0);
}

// ── (b) ListedDispersionMethodology::policy_fingerprint (L9) ──────────────────
TEST(ListedDispersionPipeline, PolicyFingerprintStableAndSensitive) {
  const ListedDispersionMethodology method{};
  const std::uint64_t fp = method.policy_fingerprint();
  EXPECT_NE(fp, 0u);
  // Stable across calls on an unchanged policy.
  EXPECT_EQ(fp, method.policy_fingerprint());

  // Default thresholds are pinned to the current production values.
  EXPECT_EQ(method.min_names_entry, 51u);
  EXPECT_EQ(method.core_min_dates, 60u);
  EXPECT_EQ(method.core_min_rolls, 3u);
  EXPECT_EQ(method.core_min_names_per_roll, 40u);

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

  ListedDispersionMethodology bumped_names = method;
  bumped_names.core_min_names_per_roll = 41u;
  EXPECT_NE(bumped_names.policy_fingerprint(), fp);

  ListedDispersionMethodology flipped_authority = method;
  flipped_authority.occ_ess_authority = !method.occ_ess_authority;
  EXPECT_NE(flipped_authority.policy_fingerprint(), fp);
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

// RED anchor (documents the defect; stays green forever as the defensive
// invariant): feeding the FULL warm-up-led timeline straight into the low-level
// reconciler aborts. The front-date hard-require at
// listed_dispersion_reconciliation.cpp:240 fires before any pricing, so no
// surfaces are dereferenced on this path.
TEST(ListedDispersionPipeline, ReconcileClockCoupling_AbortsOnWarmupLeadIn) {
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

  const auto aborted = reconcile_listed_dispersion(schedule, full);
  ASSERT_FALSE(aborted);
  EXPECT_EQ(aborted.error().code(), ErrorCode::InvalidArgument);
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

  // The production pattern (full clock timeline) still aborts at the low level ...
  EXPECT_FALSE(reconcile_listed_dispersion(schedule, full));

  // ... but the seam trims the lead-in and succeeds.
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
  // carries only two names (< core_min_names_per_roll = 40). This pins that the
  // extraction did NOT silently activate a names-per-roll floor.
  const ListedDispersionSchedule three = schedule_with_rolls(3u);
  ASSERT_EQ(three.rolls.size(), 3u);
  for (const ListedScheduleRoll &r : three.rolls) {
    EXPECT_LT(r.n_names, method.core_min_names_per_roll);
  }
  EXPECT_TRUE(accept_listed_schedule(three, strict, method));
  EXPECT_TRUE(accept_listed_schedule(three, loose, method));
}

// The full builder symbol exists with the planned signature. Its economic path is
// parquet-backed (pinned at T10); here we only pin the declaration/link + contract.
TEST(ListedDispersionPipeline, BuildScheduleSymbolIsDeclared) {
  using BuilderFn = Result<ListedDispersionSchedule> (*)(
      const Clock &, const ListedScheduleSpec &, const ListedDispersionMethodology &,
      std::span<const UniverseRow>, const ListedDefinitionTable &, const RunSpec &);
  const BuilderFn fn = &build_listed_dispersion_schedule;
  EXPECT_NE(reinterpret_cast<const void *>(fn), nullptr);
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

// ── Task 5 — dispersion_book_var + kVegaVolPointToUnitVol routing (M8) ─────────
//
// dispersion_book_var lifts run_projected_var_command's book -> OptionProjectionSpec
// synthesis + PreparedHistoricalProjection::evaluate_into + projected_historical_var
// per confidence (spy_dispersion_backtest.cpp:1119-1194). The book is pre-built by
// the caller: the per-vol-point vega * kVegaVolPointToUnitVol scaling lives in the
// DispersionConfig builder (a CLI boundary wired at T9), so the lift itself carries
// no vega x100. The library re-projects the book positions across historical
// scenarios and splits the loss quantile per requested confidence; the CLI keeps the
// three bespoke TSV emissions (out-of-archive per the partition rule).
//
// The lift reads dispersion.projected_maturity (:1124), defined at :1114 OUTSIDE the
// :1119-1194 range, so the relative-template maturity is a required input here (the
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
  // Mirror the CLI boundary (spy_dispersion_backtest.cpp:1110): per-vol-point gross
  // vega scaled to per-unit-vol via the T1 constant that replaces the hand-applied
  // * 100.0 (M9). The scaling is upstream of dispersion_book_var (which takes the
  // built book), so the constant is exercised here, at the book-build boundary.
  dispersion.target_vega = 100.0 * kVegaVolPointToUnitVol;
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
}
