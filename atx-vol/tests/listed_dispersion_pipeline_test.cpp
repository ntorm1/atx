#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "atx/vol/backtest.hpp"                   // MarketSnapshot
#include "atx/vol/dispersion.hpp"                 // DispersionMember
#include "atx/vol/listed_dispersion.hpp"          // ListedOptionQuote
#include "atx/vol/listed_dispersion_pipeline.hpp" // module under test
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
constexpr std::int64_t kNow0 = 1'700'000'000'000'000'000LL;
constexpr std::int64_t kExpiry0 = kNow0 + static_cast<std::int64_t>(0.10 * kNsPerYear);

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
