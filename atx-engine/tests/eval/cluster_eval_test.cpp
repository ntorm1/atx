// atx::engine::eval — S11-6 ClusterEval: cluster validation + the GICS-grouping
// head-to-head benchmark.
//
// This suite proves the HONEST-EVIDENCE unit. It does NOT assume clusters beat
// GICS — it proves the instruments that measure the question:
//   * temporal cluster stability (per-cluster best-match Jaccard, clusterboot
//     analogue): planted-identical snapshots recover Jaccard ~1 (nothing
//     flagged); shuffled-each-period snapshots recover ~0 AND are flagged;
//   * Adjusted Rand Index: 1 when cluster ≡ sector, ~0 when the two labelings
//     are independent;
//   * the head-to-head harness: the SAME alpha neutralized by sector vs cluster
//     over a small real-shaped fixture emits BOTH scorecards + a delta that is
//     their field-by-field difference;
//   * no-look-ahead: truncating future snapshots does not change a past
//     stability score;
//   * two-runs-equal across every metric + the harness;
//   * the rendered report carries the research §9 caveat verbatim.
//
// Naming: Subject_Condition_ExpectedResult. Tiny deterministic fixtures only —
// NO real ORATS data (the real p3-S2 run is a separate gated step; this harness
// is callable for it).

#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/cluster_field.hpp" // append_cluster_field, kClusterFieldName
#include "atx/engine/alpha/cluster_panel.hpp" // ClusterPanel
#include "atx/engine/alpha/panel.hpp"         // Panel

#include "atx/engine/eval/cluster_eval.hpp"

namespace atxtest_cluster_eval {

using atx::engine::alpha::append_cluster_field;
using atx::engine::alpha::ClusterPanel;
using atx::engine::alpha::Panel;
using atx::engine::eval::adjusted_rand_index;
using atx::engine::eval::agreement_vs_gics;
using atx::engine::eval::cluster_stability;
using atx::engine::eval::HeadToHead;
using atx::engine::eval::kGicsCaveat;
using atx::engine::eval::normalized_mutual_info;
using atx::engine::eval::run_head_to_head;
using atx::engine::eval::render_head_to_head;
using atx::engine::eval::Scorecard;
using atx::engine::eval::StabilityCfg;
using atx::engine::eval::StabilityResult;

// --------------------------------------------------------------------------
//  Fixtures.
// --------------------------------------------------------------------------

// A snapshot at `date` holding `labels`.
[[nodiscard]] ClusterPanel::Snapshot snap(atx::usize date, std::vector<int> labels) {
  ClusterPanel::Snapshot s;
  s.date = date;
  s.cluster_id = std::move(labels);
  s.n_labels = 0;
  for (const int v : s.cluster_id) {
    if (v >= 0) {
      s.n_labels = std::max<atx::i64>(s.n_labels, static_cast<atx::i64>(v) + 1);
    }
  }
  return s;
}

// A source Panel with `ret` + the OHLCV names a DSL alpha may reference, plus an
// explicit `IndClass.sector` group column. Field order: ret/close/.../volume/
// IndClass.sector. The close column carries a deterministic cross-sectional
// pattern so group_neutralize has signal. `sector` is one f64 label per
// instrument, broadcast across every date.
[[nodiscard]] Panel make_source_with_sector(atx::usize dates, atx::usize instruments,
                                            const std::vector<int> &sector_labels) {
  std::vector<std::string> names = {"ret",    "close",  "open",
                                    "high",   "low",    "volume",
                                    "IndClass.sector"};
  std::vector<std::vector<atx::f64>> cols(names.size(),
                                          std::vector<atx::f64>(dates * instruments, 0.0));
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::usize j = 0; j < instruments; ++j) {
      const atx::usize i = d * instruments + j;
      // A deterministic per-(date,instrument) close with cross-sectional spread.
      const atx::f64 close = 100.0 + static_cast<atx::f64>(j) * 3.0 +
                             static_cast<atx::f64>((d * 7 + j * 5) % 17);
      cols[1][i] = close;        // close
      cols[2][i] = close - 0.5;  // open
      cols[3][i] = close + 1.0;  // high
      cols[4][i] = close - 1.0;  // low
      cols[5][i] = 1000.0 + static_cast<atx::f64>(i); // volume
      // ret is a small deterministic return so PnL is non-degenerate.
      cols[0][i] = 0.001 * static_cast<atx::f64>(((d + 1) * (j + 2)) % 13) - 0.005;
      cols[6][i] = static_cast<atx::f64>(sector_labels[j]); // IndClass.sector
    }
  }
  auto p = Panel::create(dates, instruments, std::move(names), std::move(cols), {});
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

// A single-snapshot ClusterPanel holding `labels` from date 0 (covers all dates).
[[nodiscard]] ClusterPanel cluster_from(std::vector<int> labels, atx::usize instruments) {
  ClusterPanel cp;
  cp.instruments = instruments;
  cp.snapshots.push_back(snap(0, std::move(labels)));
  return cp;
}

// =============================================================================
//  1. Stability — planted-identical snapshots recover Jaccard ~1, nothing flagged.
// =============================================================================
TEST(ClusterEval, StablePlantedClusters_JaccardOne_NothingFlagged) {
  // Three snapshots, IDENTICAL labels {0,0,0,1,1,1,2,2,2} each period.
  const std::vector<int> labels{0, 0, 0, 1, 1, 1, 2, 2, 2};
  std::vector<ClusterPanel::Snapshot> snaps{snap(0, labels), snap(5, labels), snap(10, labels)};

  const StabilityResult r = cluster_stability(snaps, StabilityCfg{});
  ASSERT_EQ(r.steps.size(), 2u); // two transitions

  for (const auto &st : r.steps) {
    EXPECT_NEAR(st.churn, 0.0, 1e-12) << "identical labels -> zero churn";
    for (const atx::f64 j : st.best_jaccard) {
      if (!std::isnan(j)) {
        EXPECT_NEAR(j, 1.0, 1e-12) << "each cluster perfectly recovered -> Jaccard 1";
      }
    }
  }
  EXPECT_NEAR(r.mean_churn, 0.0, 1e-12);
  EXPECT_TRUE(r.flagged.empty()) << "stable clusters must not be flagged";
}

// =============================================================================
//  2. Stability — shuffled-each-period clusters recover Jaccard ~0 AND are flagged.
// =============================================================================
TEST(ClusterEval, ShuffledClusters_JaccardLow_Flagged) {
  // Snapshot A: three tight blocks. Snapshot B: every instrument reassigned so no
  // A-cluster survives as a block (a maximal shuffle: members of each A-cluster are
  // scattered one-per B-cluster, so best-match Jaccard is small).
  const std::vector<int> a{0, 0, 0, 1, 1, 1, 2, 2, 2};
  const std::vector<int> b{0, 1, 2, 0, 1, 2, 0, 1, 2};
  std::vector<ClusterPanel::Snapshot> snaps{snap(0, a), snap(5, b)};

  StabilityCfg cfg;
  cfg.jaccard_floor = 0.6;
  const StabilityResult r = cluster_stability(snaps, cfg);
  ASSERT_EQ(r.steps.size(), 1u);

  // Each A-cluster (size 3) best-matches a B-cluster sharing exactly 1 member:
  // Jaccard = 1 / (3 + 3 - 1) = 0.2 < floor.
  for (const atx::f64 j : r.steps[0].best_jaccard) {
    if (!std::isnan(j)) {
      EXPECT_LT(j, 0.6) << "a shuffled cluster recovers below the floor";
    }
  }
  EXPECT_GT(r.steps[0].churn, 0.5) << "a maximal shuffle churns most members";
  // All three A-clusters flagged (label 0,1,2 at snapshot index 0).
  EXPECT_EQ(r.flagged.size(), 3u);
  for (const auto &[snap_idx, label] : r.flagged) {
    EXPECT_EQ(snap_idx, 0u);
    EXPECT_GE(label, 0);
    EXPECT_LE(label, 2);
  }
}

// =============================================================================
//  3. ARI — 1 when cluster ≡ sector; ≈ 0 when the two labelings are independent.
// =============================================================================
TEST(ClusterEval, AdjustedRandIndex_IdenticalIsOne_IndependentNearZero) {
  // cluster ≡ sector (even up to relabeling) -> ARI exactly 1.
  const std::vector<atx::f64> sector{0, 0, 0, 1, 1, 1, 2, 2, 2};
  const std::vector<atx::f64> cluster_same{2, 2, 2, 0, 0, 0, 1, 1, 1}; // relabeled but same partition
  EXPECT_NEAR(adjusted_rand_index(std::span<const atx::f64>{cluster_same},
                                  std::span<const atx::f64>{sector}),
              1.0, 1e-12);
  // NMI is also 1 for an identical partition.
  EXPECT_NEAR(normalized_mutual_info(std::span<const atx::f64>{cluster_same},
                                     std::span<const atx::f64>{sector}),
              1.0, 1e-12);

  // An independent labeling: a sector taxonomy crossed with clusters that carry
  // no information about it. ARI is exactly 0 only IN EXPECTATION over random
  // labelings (never guaranteed for one finite table), so the contract is that an
  // independent labeling scores NEAR 0 and is clearly separated from the
  // identical case (ARI 1) — it must not look like agreement. The balanced cross
  // below lands close to 0 (small-magnitude, well under any agreement threshold).
  const std::vector<atx::f64> sec2{0, 0, 0, 0, 1, 1, 1, 1};
  const std::vector<atx::f64> clu2{0, 1, 0, 1, 0, 1, 0, 1};
  const atx::f64 ari_indep = adjusted_rand_index(std::span<const atx::f64>{clu2},
                                                 std::span<const atx::f64>{sec2});
  EXPECT_LT(std::abs(ari_indep), 0.25) << "independent labelings -> ARI near 0, ari=" << ari_indep;
  EXPECT_LT(ari_indep, 0.5) << "independent labelings must not look like agreement";

  // The defining contrast: a chance-level cross-classification scores DRAMATICALLY
  // lower than the identical partition (which is exactly 1). A balanced 3-sector /
  // 2-cluster cross (each cluster takes half of every sector) is near chance — its
  // ARI is small in magnitude and nowhere near agreement. (ARI is 0 only in
  // EXPECTATION over random labelings; one finite table lands close to but not at
  // 0, which is the honest behavior of the statistic.)
  const std::vector<atx::f64> sec3{0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2};
  const std::vector<atx::f64> clu3{0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1};
  const atx::f64 ari_prop = adjusted_rand_index(std::span<const atx::f64>{clu3},
                                                std::span<const atx::f64>{sec3});
  EXPECT_LT(std::abs(ari_prop), 0.25) << "chance-level table -> ARI near 0, ari=" << ari_prop;
  EXPECT_GT(1.0 - ari_prop, 0.5) << "chance-level ARI must be far below the identical case (1.0)";
}

// =============================================================================
//  4. Head-to-head — runs on a real-shaped fixture, emits BOTH scorecards + delta.
// =============================================================================
TEST(ClusterEval, HeadToHead_EmitsBothScorecardsAndDelta) {
  const atx::usize dates = 24;
  const atx::usize instruments = 8;
  // GICS sectors: 4 in sector 0, 4 in sector 1.
  const std::vector<int> sector{0, 0, 0, 0, 1, 1, 1, 1};
  const Panel src = make_source_with_sector(dates, instruments, sector);

  // Data-driven clusters that DEPART from GICS: a 3/3/2 partition.
  const ClusterPanel cp = cluster_from({0, 0, 0, 1, 1, 1, 2, 2}, instruments);
  auto augmented = append_cluster_field(src, cp);
  ASSERT_TRUE(augmented.has_value()) << (augmented ? "" : augmented.error().message());

  auto h2h = run_head_to_head(augmented.value(), "rank(close)");
  ASSERT_TRUE(h2h.has_value()) << (h2h ? "" : h2h.error().message());
  const HeadToHead &h = h2h.value();

  // Both scorecards populated (periods == panel dates for each arm).
  EXPECT_EQ(h.sector.periods, dates);
  EXPECT_EQ(h.cluster.periods, dates);
  // Sharpe is a finite number for both arms.
  EXPECT_TRUE(std::isfinite(h.sector.sharpe));
  EXPECT_TRUE(std::isfinite(h.cluster.sharpe));
  // Turnover strictly positive (the book trades) for both arms.
  EXPECT_GT(h.sector.mean_turnover, 0.0);
  EXPECT_GT(h.cluster.mean_turnover, 0.0);

  // The delta is EXACTLY the field-by-field difference cluster - sector.
  EXPECT_DOUBLE_EQ(h.delta.sharpe, h.cluster.sharpe - h.sector.sharpe);
  EXPECT_DOUBLE_EQ(h.delta.dsr, h.cluster.dsr - h.sector.dsr);
  EXPECT_DOUBLE_EQ(h.delta.max_dd, h.cluster.max_dd - h.sector.max_dd);
  EXPECT_DOUBLE_EQ(h.delta.mean_turnover, h.cluster.mean_turnover - h.sector.mean_turnover);
}

// =============================================================================
//  5. No-look-ahead — truncating future snapshots does not change a past score.
// =============================================================================
TEST(ClusterEval, RollingStability_NoLookAhead) {
  // Four snapshots with varying overlap. The stability score for transition i
  // must depend ONLY on snapshots i and i+1, so truncating snapshots beyond i+1
  // leaves steps[0..i] byte-identical.
  const std::vector<int> s0{0, 0, 1, 1, 2, 2};
  const std::vector<int> s1{0, 0, 1, 2, 2, 2}; // inst 3 moves cluster 1 -> 2
  const std::vector<int> s2{0, 1, 1, 2, 2, 0}; // a later, different partition
  const std::vector<int> s3{2, 2, 2, 0, 0, 1};

  std::vector<ClusterPanel::Snapshot> full{snap(0, s0), snap(3, s1), snap(6, s2), snap(9, s3)};
  std::vector<ClusterPanel::Snapshot> truncated{snap(0, s0), snap(3, s1)}; // only the first transition

  const StabilityResult rf = cluster_stability(full, StabilityCfg{});
  const StabilityResult rt = cluster_stability(truncated, StabilityCfg{});
  ASSERT_GE(rf.steps.size(), 1u);
  ASSERT_EQ(rt.steps.size(), 1u);

  // The FIRST transition's churn + per-cluster Jaccard must match exactly between
  // the full and truncated streams (no future snapshot influenced the past score).
  EXPECT_DOUBLE_EQ(rf.steps[0].churn, rt.steps[0].churn);
  ASSERT_EQ(rf.steps[0].best_jaccard.size(), rt.steps[0].best_jaccard.size());
  for (atx::usize c = 0; c < rf.steps[0].best_jaccard.size(); ++c) {
    const atx::f64 a = rf.steps[0].best_jaccard[c];
    const atx::f64 b = rt.steps[0].best_jaccard[c];
    EXPECT_TRUE((std::isnan(a) && std::isnan(b)) || a == b)
        << "transition 0 cluster " << c << " changed when future snapshots were added";
  }
}

// =============================================================================
//  6. Two-runs-equal — every metric + the harness replay byte-identical.
// =============================================================================
TEST(ClusterEval, AllMetricsAndHarness_TwoRunsEqual) {
  // Stability.
  std::vector<ClusterPanel::Snapshot> snaps{snap(0, {0, 0, 1, 1, 2, 2}),
                                            snap(4, {0, 1, 1, 2, 2, 0}),
                                            snap(8, {2, 2, 0, 0, 1, 1})};
  const StabilityResult r1 = cluster_stability(snaps, StabilityCfg{});
  const StabilityResult r2 = cluster_stability(snaps, StabilityCfg{});
  ASSERT_EQ(r1.steps.size(), r2.steps.size());
  EXPECT_DOUBLE_EQ(r1.mean_churn, r2.mean_churn);
  for (atx::usize s = 0; s < r1.steps.size(); ++s) {
    EXPECT_DOUBLE_EQ(r1.steps[s].churn, r2.steps[s].churn);
    ASSERT_EQ(r1.steps[s].best_jaccard.size(), r2.steps[s].best_jaccard.size());
    for (atx::usize c = 0; c < r1.steps[s].best_jaccard.size(); ++c) {
      const atx::f64 a = r1.steps[s].best_jaccard[c];
      const atx::f64 b = r2.steps[s].best_jaccard[c];
      EXPECT_TRUE((std::isnan(a) && std::isnan(b)) || a == b);
    }
  }

  // ARI / NMI.
  const std::vector<atx::f64> x{0, 0, 1, 1, 2, 2};
  const std::vector<atx::f64> y{0, 1, 1, 2, 2, 0};
  EXPECT_DOUBLE_EQ(adjusted_rand_index(std::span<const atx::f64>{x}, std::span<const atx::f64>{y}),
                   adjusted_rand_index(std::span<const atx::f64>{x}, std::span<const atx::f64>{y}));
  EXPECT_DOUBLE_EQ(
      normalized_mutual_info(std::span<const atx::f64>{x}, std::span<const atx::f64>{y}),
      normalized_mutual_info(std::span<const atx::f64>{x}, std::span<const atx::f64>{y}));

  // The harness.
  const atx::usize dates = 16;
  const atx::usize instruments = 6;
  const Panel src = make_source_with_sector(dates, instruments, {0, 0, 0, 1, 1, 1});
  const ClusterPanel cp = cluster_from({0, 0, 1, 1, 2, 2}, instruments);
  auto aug = append_cluster_field(src, cp);
  ASSERT_TRUE(aug.has_value());
  auto a = run_head_to_head(aug.value(), "rank(close)");
  auto b = run_head_to_head(aug.value(), "rank(close)");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_DOUBLE_EQ(a->sector.sharpe, b->sector.sharpe);
  EXPECT_DOUBLE_EQ(a->cluster.sharpe, b->cluster.sharpe);
  EXPECT_DOUBLE_EQ(a->delta.sharpe, b->delta.sharpe);
  EXPECT_DOUBLE_EQ(a->cluster.mean_turnover, b->cluster.mean_turnover);
}

// =============================================================================
//  7. Report — the rendered head-to-head carries the §9 caveat verbatim.
// =============================================================================
TEST(ClusterEval, RenderHeadToHead_PrintsResearchCaveat) {
  const atx::usize dates = 12;
  const atx::usize instruments = 6;
  const Panel src = make_source_with_sector(dates, instruments, {0, 0, 0, 1, 1, 1});
  const ClusterPanel cp = cluster_from({0, 0, 1, 1, 2, 2}, instruments);
  auto aug = append_cluster_field(src, cp);
  ASSERT_TRUE(aug.has_value());
  auto h = run_head_to_head(aug.value(), "rank(close)");
  ASSERT_TRUE(h.has_value());

  const std::vector<std::string> lines = render_head_to_head(h.value());
  ASSERT_FALSE(lines.empty());

  // The §9 caveat must appear verbatim somewhere in the rendered report.
  bool found_caveat = false;
  bool found_sharpe = false;
  bool found_capacity = false;
  for (const std::string &ln : lines) {
    if (ln == std::string(kGicsCaveat)) {
      found_caveat = true;
    }
    if (ln.find("OOS Sharpe") != std::string::npos) {
      found_sharpe = true;
    }
    if (ln.find("%ADV capacity") != std::string::npos) {
      found_capacity = true;
    }
  }
  EXPECT_TRUE(found_caveat) << "the research §9 caveat must be printed inline";
  EXPECT_TRUE(found_sharpe) << "the OOS Sharpe head-to-head line must be present";
  EXPECT_TRUE(found_capacity) << "the %ADV capacity line must be present";
  EXPECT_NE(kGicsCaveat, nullptr);
  EXPECT_NE(std::string(kGicsCaveat).find("UNPROVEN"), std::string::npos);
}

// =============================================================================
//  8. Agreement-over-time — per-date ARI/NMI across the augmented panel.
// =============================================================================
TEST(ClusterEval, AgreementVsGics_PerDateAriNmi) {
  const atx::usize dates = 6;
  const atx::usize instruments = 6;
  // sector == {0,0,0,1,1,1}; cluster departs as {0,0,1,1,2,2}.
  const Panel src = make_source_with_sector(dates, instruments, {0, 0, 0, 1, 1, 1});
  const ClusterPanel cp = cluster_from({0, 0, 1, 1, 2, 2}, instruments);
  auto aug = append_cluster_field(src, cp);
  ASSERT_TRUE(aug.has_value());

  auto agree = agreement_vs_gics(aug.value());
  ASSERT_TRUE(agree.has_value()) << (agree ? "" : agree.error().message());
  ASSERT_EQ(agree->per_date.size(), dates);

  // The cluster labeling departs from GICS but is correlated with it, so the mean
  // ARI sits strictly between independence (0) and identity (1).
  EXPECT_GT(agree->mean_ari, 0.0) << "cluster is correlated with GICS";
  EXPECT_LT(agree->mean_ari, 1.0) << "cluster departs from GICS (not identical)";
  EXPECT_GE(agree->mean_nmi, 0.0);
  EXPECT_LE(agree->mean_nmi, 1.0);
}

} // namespace atxtest_cluster_eval
