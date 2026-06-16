// atx::engine::data — universe construction tests (p3 S1-4).
//
// Suite: DataUniverse
//
// Covers the 6 named S1-4 tests + the survivorship-caveat doc assertion:
//   1. MarketCapIsSplitInvariant — a split (price/k, shares*k) leaves market cap
//      (shares × RAW close) unchanged within ε; using raw (not adjusted) close is
//      the load-bearing convention.
//   2. AdvIsCausalNoLookAhead — truncating future dates does not change a past
//      date's ADV (the causal/no-look-ahead proof): ADV over a prefix axis is
//      byte-identical to ADV over the full axis at every retained past date.
//   3. LiquidityFloorDropsBelowThreshold — instruments with ADV below min_adv_usd
//      fall out of the membership mask; those above stay in.
//   4. TopNByAdvDeterministicTieBreak — with tied ADV and a top-N cap, the kept
//      set is the lowest canonical instrument ids (deterministic tie-break).
//   5. SectorGicsThenSicFallbackThenSentinel — GICS wins; else SIC; else the
//      sentinel -1 (never coerced to 0).
//   6. MembershipExcludesNanPriceCells — a NaN raw-close cell yields NaN market
//      cap and is excluded from the universe even when present.
//   + SurvivorshipCaveatDocumentedOrDeferred — the listed-only survivorship bias
//      is referenced in data-ingestion-reference.md IF that doc exists (it lands
//      in S1-1); else the assertion is recorded as deferred-to-S1-1 (never a
//      false pass — the bias is still documented in universe.hpp + the ledger).
//
// All fixtures are tiny self-contained deterministic Panels/Datasets — no on-disk
// parquet. The doc-caveat test resolves the doc path from __FILE__.

#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <filesystem>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/corporate_actions.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"
#include "atx/engine/data/universe.hpp"

namespace atxtest_data_universe_test {

namespace fs = std::filesystem;
using atx::engine::alpha::Panel;
using atx::engine::data::build_universe;
using atx::engine::data::corp_action_schema;
using atx::engine::data::Dataset;
using atx::engine::data::DatasetProvenance;
using atx::engine::data::DateKey;
using atx::engine::data::InstKey;
using atx::engine::data::kNoSector;
using atx::engine::data::kNoSectorCode;
using atx::engine::data::UniverseConfig;
using atx::engine::data::UniverseFields;

namespace {

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// ---------------------------------------------------------------------------
// Fixture builders — tiny deterministic Panel + axis-matched corp-action Dataset.
// ---------------------------------------------------------------------------

// Build a price Panel from raw `close` + `volume` columns (date-major,
// dates*instruments), optionally an empty universe (all in-universe). Extra named
// columns (e.g. a pre-supplied "adv5") may be appended via `extra`.
[[nodiscard]] Panel make_price_panel(atx::usize dates, atx::usize instruments,
                                     std::vector<atx::f64> close, std::vector<atx::f64> volume,
                                     std::vector<std::uint8_t> universe = {},
                                     std::vector<std::pair<std::string, std::vector<atx::f64>>>
                                         extra = {}) {
  std::vector<std::string> names = {"close", "volume"};
  std::vector<std::vector<atx::f64>> cols = {std::move(close), std::move(volume)};
  for (auto &e : extra) {
    names.push_back(std::move(e.first));
    cols.push_back(std::move(e.second));
  }
  auto r = Panel::create(dates, instruments, std::move(names), std::move(cols), std::move(universe));
  EXPECT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().to_string());
  return std::move(r).value();
}

// Build an axis-matched corporate-action Dataset. Only the columns the universe
// unit reads need real values: shares_outstanding (col 2), gics (col 4), sic
// (col 5). The others are filled with the loader's defaults (factor NaN, dividend
// 0, filed-date sentinel). All inputs are date-major dates*instruments; sector
// inputs carry kNoSector (-1) where absent.
[[nodiscard]] Dataset make_corp_dataset(atx::usize dates, atx::usize instruments,
                                        std::vector<atx::f64> shares,
                                        std::vector<atx::f64> gics = {},
                                        std::vector<atx::f64> sic = {}) {
  const atx::usize cells = dates * instruments;
  if (gics.empty()) {
    gics.assign(cells, kNoSector);
  }
  if (sic.empty()) {
    sic.assign(cells, kNoSector);
  }
  std::vector<std::vector<atx::f64>> cols(6);
  cols[0].assign(cells, kNaN);                                    // cum_adj_factor
  cols[1].assign(cells, 0.0);                                     // cash_dividend
  cols[2] = std::move(shares);                                    // shares_outstanding
  cols[3].assign(cells, static_cast<atx::f64>(atx::i64{-1}));     // shares_filed_date sentinel
  cols[4] = std::move(gics);                                      // gics_sector_code
  cols[5] = std::move(sic);                                       // sic_code

  std::vector<DateKey> ds(dates);
  for (atx::usize d = 0; d < dates; ++d) {
    ds[d] = static_cast<DateKey>(d); // strictly ascending epoch-day proxy
  }
  std::vector<InstKey> insts(instruments);
  for (atx::usize i = 0; i < instruments; ++i) {
    insts[i] = static_cast<InstKey>(i);
  }
  DatasetProvenance prov{"test:universe_fixture", "S1-4 unit fixture"};
  auto r = Dataset::create(corp_action_schema(), std::move(ds), std::move(insts), std::move(cols),
                           /*mask=*/{}, std::move(prov));
  EXPECT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().to_string());
  return std::move(r).value();
}

// Flat date-major index.
[[nodiscard]] atx::usize at(atx::usize t, atx::usize i, atx::usize instruments) {
  return t * instruments + i;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. MarketCapIsSplitInvariant
// ---------------------------------------------------------------------------
TEST(DataUniverse, MarketCapIsSplitInvariant) {
  // One instrument, one date. Pre-split: price 100, shares 1,000,000.
  // Post-split (4:1): RAW price 25, shares 4,000,000. Cap = 100M both ways.
  const Panel pre = make_price_panel(1, 1, {100.0}, {10000.0});
  const Dataset corp_pre = make_corp_dataset(1, 1, {1.0e6});
  const Panel post = make_price_panel(1, 1, {25.0}, {10000.0});
  const Dataset corp_post = make_corp_dataset(1, 1, {4.0e6});

  UniverseConfig cfg; // defaults fine; only market_cap is inspected
  const auto pre_r = build_universe(pre, corp_pre, cfg);
  const auto post_r = build_universe(post, corp_post, cfg);
  ASSERT_TRUE(pre_r.has_value()) << (pre_r.has_value() ? "" : pre_r.error().to_string());
  ASSERT_TRUE(post_r.has_value()) << (post_r.has_value() ? "" : post_r.error().to_string());

  const atx::f64 cap_pre = pre_r.value().market_cap[0];
  const atx::f64 cap_post = post_r.value().market_cap[0];
  EXPECT_NEAR(cap_pre, 1.0e8, 1.0e-3);
  EXPECT_NEAR(cap_post, 1.0e8, 1.0e-3);
  EXPECT_NEAR(cap_pre, cap_post, 1.0e-6); // split-invariant within ε
}

// ---------------------------------------------------------------------------
// 2. AdvIsCausalNoLookAhead
// ---------------------------------------------------------------------------
TEST(DataUniverse, AdvIsCausalNoLookAhead) {
  // One instrument, 6 dates. ADV window = 3. Build the full panel and a prefix
  // panel (first 4 dates, identical data). A retained past date's ADV must be
  // byte-identical — truncating the FUTURE cannot move a PAST trailing mean.
  const atx::usize ni = 1;
  const std::vector<atx::f64> close_full = {10.0, 11.0, 12.0, 13.0, 14.0, 15.0};
  const std::vector<atx::f64> vol_full = {100.0, 100.0, 100.0, 100.0, 100.0, 100.0};

  UniverseConfig cfg;
  cfg.adv_window = 3;

  const Panel full = make_price_panel(6, ni, close_full, vol_full);
  const Dataset corp_full = make_corp_dataset(6, ni, std::vector<atx::f64>(6, 1.0e6));
  const auto full_r = build_universe(full, corp_full, cfg);
  ASSERT_TRUE(full_r.has_value()) << (full_r.has_value() ? "" : full_r.error().to_string());

  const atx::usize prefix_dates = 4;
  const std::vector<atx::f64> close_pre(close_full.begin(), close_full.begin() + prefix_dates);
  const std::vector<atx::f64> vol_pre(vol_full.begin(), vol_full.begin() + prefix_dates);
  const Panel prefix = make_price_panel(prefix_dates, ni, close_pre, vol_pre);
  const Dataset corp_pre = make_corp_dataset(prefix_dates, ni, std::vector<atx::f64>(4, 1.0e6));
  const auto pre_r = build_universe(prefix, corp_pre, cfg);
  ASSERT_TRUE(pre_r.has_value()) << (pre_r.has_value() ? "" : pre_r.error().to_string());

  // Every date present in BOTH builds must carry an identical ADV (bit-for-bit).
  for (atx::usize t = 0; t < prefix_dates; ++t) {
    const atx::f64 a_full = full_r.value().adv_usd[at(t, 0, ni)];
    const atx::f64 a_pre = pre_r.value().adv_usd[at(t, 0, ni)];
    if (std::isnan(a_full)) {
      EXPECT_TRUE(std::isnan(a_pre)) << "date " << t;
    } else {
      EXPECT_EQ(std::bit_cast<atx::u64>(a_full), std::bit_cast<atx::u64>(a_pre)) << "date " << t;
    }
  }
  // Sanity: date 2 (first full window) = mean of dollar-volume[0..2] = mean(1000,
  // 1100, 1200) = 1100.
  EXPECT_TRUE(std::isnan(full_r.value().adv_usd[at(0, 0, ni)])); // window incomplete
  EXPECT_TRUE(std::isnan(full_r.value().adv_usd[at(1, 0, ni)]));
  EXPECT_NEAR(full_r.value().adv_usd[at(2, 0, ni)], 1100.0, 1.0e-9);
}

// ---------------------------------------------------------------------------
// 3. LiquidityFloorDropsBelowThreshold
// ---------------------------------------------------------------------------
TEST(DataUniverse, LiquidityFloorDropsBelowThreshold) {
  // Two instruments, ADV window = 1 (so ADV == dollar_volume on every date).
  // Inst 0 trades $2M/day (above floor), inst 1 $0.5M/day (below). Floor = $1M.
  const atx::usize ni = 2;
  // date-major: [d0i0, d0i1]
  const std::vector<atx::f64> close = {100.0, 100.0};
  const std::vector<atx::f64> vol = {20000.0, 5000.0}; // dvol = 2.0e6, 0.5e6
  const Panel p = make_price_panel(1, ni, close, vol);
  const Dataset corp = make_corp_dataset(1, ni, {1.0e9, 1.0e9}); // big caps; cap floor not binding

  UniverseConfig cfg;
  cfg.adv_window = 1;
  cfg.min_adv_usd = 1.0e6;
  cfg.min_mktcap_usd = 0.0;

  const auto r = build_universe(p, corp, cfg);
  ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().to_string());
  EXPECT_NEAR(r.value().adv_usd[0], 2.0e6, 1.0e-3);
  EXPECT_NEAR(r.value().adv_usd[1], 0.5e6, 1.0e-3);
  EXPECT_EQ(r.value().in_universe[0], 1); // above floor -> in
  EXPECT_EQ(r.value().in_universe[1], 0); // below floor -> out
}

// ---------------------------------------------------------------------------
// 4. TopNByAdvDeterministicTieBreak
// ---------------------------------------------------------------------------
TEST(DataUniverse, TopNByAdvDeterministicTieBreak) {
  // Four instruments, all with IDENTICAL ADV (perfect tie), all above the floor.
  // top_n_by_adv = 2 -> keep exactly 2. The deterministic tie-break is ascending
  // canonical id, so instruments 0 and 1 are kept; 2 and 3 dropped.
  const atx::usize ni = 4;
  const std::vector<atx::f64> close(ni, 100.0);
  const std::vector<atx::f64> vol(ni, 20000.0); // dvol = 2.0e6 for all -> tie
  const Panel p = make_price_panel(1, ni, close, vol);
  const Dataset corp = make_corp_dataset(1, ni, std::vector<atx::f64>(ni, 1.0e9));

  UniverseConfig cfg;
  cfg.adv_window = 1;
  cfg.min_adv_usd = 1.0e6;
  cfg.top_n_by_adv = 2;

  const auto r = build_universe(p, corp, cfg);
  ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().to_string());
  EXPECT_EQ(r.value().in_universe[0], 1);
  EXPECT_EQ(r.value().in_universe[1], 1);
  EXPECT_EQ(r.value().in_universe[2], 0);
  EXPECT_EQ(r.value().in_universe[3], 0);

  // And the cap binds exactly: two kept.
  atx::usize kept = 0;
  for (atx::usize i = 0; i < ni; ++i) {
    kept += r.value().in_universe[i];
  }
  EXPECT_EQ(kept, 2u);
}

// ---------------------------------------------------------------------------
// 5. SectorGicsThenSicFallbackThenSentinel
// ---------------------------------------------------------------------------
TEST(DataUniverse, SectorGicsThenSicFallbackThenSentinel) {
  // Three instruments, one date:
  //   inst 0: GICS=45, SIC=3571  -> GICS wins (45)
  //   inst 1: GICS absent, SIC=2834 -> SIC fallback (2834)
  //   inst 2: GICS absent, SIC absent -> sentinel (-1), NEVER 0
  const atx::usize ni = 3;
  const std::vector<atx::f64> close(ni, 100.0);
  const std::vector<atx::f64> vol(ni, 1000.0);
  const Panel p = make_price_panel(1, ni, close, vol);

  const std::vector<atx::f64> gics = {45.0, kNoSector, kNoSector};
  const std::vector<atx::f64> sic = {3571.0, 2834.0, kNoSector};
  const Dataset corp = make_corp_dataset(1, ni, std::vector<atx::f64>(ni, 1.0e9), gics, sic);

  UniverseConfig cfg;
  const auto r = build_universe(p, corp, cfg);
  ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().to_string());
  EXPECT_EQ(r.value().sector_code[0], 45);   // GICS preferred
  EXPECT_EQ(r.value().sector_code[1], 2834); // SIC fallback
  EXPECT_EQ(r.value().sector_code[2], kNoSectorCode);
  EXPECT_EQ(kNoSectorCode, -1);              // sentinel is -1, never 0
}

// ---------------------------------------------------------------------------
// 6. MembershipExcludesNanPriceCells
// ---------------------------------------------------------------------------
TEST(DataUniverse, MembershipExcludesNanPriceCells) {
  // Two instruments, one date, both PRESENT (empty universe == all in). Inst 0 has
  // a valid close; inst 1 has a NaN close. NaN close -> NaN market_cap -> the cap
  // floor (>= 0) is FALSE for NaN, so inst 1 is excluded even though present.
  const atx::usize ni = 2;
  const std::vector<atx::f64> close = {100.0, kNaN};
  const std::vector<atx::f64> vol = {1000.0, 1000.0};
  const Panel p = make_price_panel(1, ni, close, vol);
  const Dataset corp = make_corp_dataset(1, ni, {1.0e6, 1.0e6});

  UniverseConfig cfg;
  cfg.adv_window = 1;
  cfg.min_adv_usd = 0.0;    // no liquidity floor -> isolate the NaN-price effect
  cfg.min_mktcap_usd = 0.0; // floor 0; NaN cap still fails (NaN >= 0 is false)

  const auto r = build_universe(p, corp, cfg);
  ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().to_string());
  EXPECT_TRUE(std::isnan(r.value().market_cap[1])); // NaN price -> NaN cap
  EXPECT_EQ(r.value().in_universe[0], 1);           // valid cell -> in
  EXPECT_EQ(r.value().in_universe[1], 0);           // NaN price cell -> excluded
}

// ---------------------------------------------------------------------------
// + SurvivorshipCaveatDocumentedOrDeferred
// ---------------------------------------------------------------------------
TEST(DataUniverse, SurvivorshipCaveatDocumentedOrDeferred) {
  // The listed-only survivorship bias must be DOCUMENTED. Its canonical home is
  // p3-impl/data-ingestion-reference.md §universe (authored in S1-1). If that doc
  // exists, assert it references survivorship; if it does not yet exist, the
  // assertion is deferred to S1-1 (the bias is still recorded in universe.hpp's
  // header doc-comment + the S1-4 ledger row, so this is never a silent gap).
  const fs::path doc =
      fs::path(__FILE__).parent_path().parent_path() / "plans" / "p3-impl" /
      "data-ingestion-reference.md";
  std::error_code ec;
  if (!fs::exists(doc, ec)) {
    GTEST_SKIP() << "data-ingestion-reference.md not present yet (lands in S1-1); "
                    "survivorship caveat documented in universe.hpp + the S1-4 ledger row.";
  }
  std::ifstream in(doc);
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string text = buf.str();
  EXPECT_NE(text.find("survivorship"), std::string::npos)
      << "data-ingestion-reference.md exists but does not reference the survivorship caveat";
}

} // namespace atxtest_data_universe_test
