// atx::engine::alpha — build_cluster_panel point-in-time / determinism tests
// (S11-3).
//
// build_cluster_panel turns a returns Panel into a rolling time series of
// cluster assignments: per recluster date t it takes the in-universe, non-NaN
// instruments over the strict window [t-window+1, t], optionally CAPM-
// residualizes them against an equal-weight market factor, estimates the
// order-fixed correlation, cleans it (rmt_clean), partitions it (cluster), and
// scatters the canonical labels back into a full-length cluster_id with
// kUnclustered for every instrument outside the valid set.
//
// The load-bearing guarantees this suite proves:
//   1. NO LOOK-AHEAD (the crux): the snapshot computed for rebalance date t is
//      BYTE-identical whether or not the source Panel carries any dates after t.
//      This mirrors validation/bias_audit.hpp's check_no_lookahead truncation-
//      invariance, applied to the whole snapshot (cluster_id + n_labels).
//   2. Cadence: one snapshot per recluster date, at the expected dates, ascending.
//   3. Insufficient-history / out-of-universe instruments -> kUnclustered.
//   4. A planted regime shift moves cluster membership AFTER the shift, not before.
//   5. CAPM residualization strips a planted common market factor (clusters differ
//      from the raw-returns clusters when the market factor dominates).
//   6/7. Determinism: two-runs-equal and a pinned FNV-1a-64 digest of the panel.
//   8. Degenerate configs -> Err(InvalidArgument).
//
// Naming: Subject_Condition_ExpectedResult.

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/cluster_panel.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/validation/bias_audit.hpp"

namespace atxtest_cluster_panel {

using atx::engine::alpha::build_cluster_panel;
using atx::engine::alpha::ClusterPanel;
using atx::engine::alpha::ClusterPanelConfig;
using atx::engine::alpha::Panel;

// ---------------------------------------------------------------------------
//  Fixtures — deterministic, RNG-free return panels built from index arithmetic.
// ---------------------------------------------------------------------------

// Build a single-field returns Panel from a date-major return matrix
// rets[date][inst]. `universe` (date-major, dates*instruments, {0,1}) is optional
// (empty == all-in-universe). The field is named "ret" (the default return_field).
[[nodiscard]] Panel make_returns_panel(const std::vector<std::vector<atx::f64>> &rets,
                                       std::vector<std::uint8_t> universe = {}) {
  const atx::usize dates = rets.size();
  const atx::usize instruments = dates == 0 ? 0 : rets[0].size();
  std::vector<atx::f64> col(dates * instruments, 0.0);
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::usize i = 0; i < instruments; ++i) {
      col[d * instruments + i] = rets[d][i];
    }
  }
  std::vector<std::string> names = {"ret"};
  std::vector<std::vector<atx::f64>> cols = {std::move(col)};
  auto p =
      Panel::create(dates, instruments, std::move(names), std::move(cols), std::move(universe));
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

// Deterministic, full-rank pseudo-noise in [-1,1]: a SplitMix64-style integer hash
// of (date, inst) folded to a double. RNG-free and reproducible, but — unlike a
// shared sinusoid — uncorrelated across instruments and full-rank across a window,
// so the per-window covariance has a genuine noise bulk for rmt_clean to fit and a
// planted factor structure that sits clearly above the Marchenko-Pastur edge.
[[nodiscard]] inline atx::f64 hash_noise(std::uint64_t date, std::uint64_t inst) noexcept {
  std::uint64_t x = date * 0x9E3779B97F4A7C15ULL + inst * 0xC2B2AE3D27D4EB4FULL +
                    0x165667B19E3779F9ULL;
  x ^= x >> 33;
  x *= 0xFF51AFD7ED558CCDULL;
  x ^= x >> 33;
  x *= 0xC4CEB9FE1A85EC53ULL;
  x ^= x >> 33;
  return (static_cast<atx::f64>(x >> 11) / static_cast<atx::f64>(std::uint64_t{1} << 53)) * 2.0 -
         1.0;
}

// A deterministic three-block return panel: instruments [0,bs) move with factor
// f0, [bs,2bs) with f1, [2bs,3bs) with f2; the three factors are mutually
// near-orthogonal sinusoids so a correlation clusterer recovers three blocks.
// `phase` lets two callers produce different-but-still-blocky panels.
[[nodiscard]] std::vector<std::vector<atx::f64>>
three_block_rets(atx::usize dates, atx::usize bs, atx::f64 phase = 0.0) {
  const atx::usize n = 3 * bs;
  std::vector<std::vector<atx::f64>> rets(dates, std::vector<atx::f64>(n, 0.0));
  for (atx::usize d = 0; d < dates; ++d) {
    const atx::f64 t = static_cast<atx::f64>(d) + phase;
    const atx::f64 f0 = std::sin(0.30 * t);
    const atx::f64 f1 = std::sin(0.11 * t + 1.7);
    const atx::f64 f2 = std::sin(0.07 * t + 3.1);
    for (atx::usize i = 0; i < n; ++i) {
      // Full-rank per-name noise so each window covariance has a genuine MP bulk
      // and the three block factors sit above the rmt_clean edge (a shared-sinusoid
      // "wiggle" would be rank-deficient and collapse the spectrum). The phase
      // offset shifts the noise stream so two callers get distinct-but-blocky data.
      const atx::f64 noise =
          0.6 * hash_noise(static_cast<std::uint64_t>(d) + static_cast<std::uint64_t>(phase * 16.0),
                           static_cast<std::uint64_t>(i));
      atx::f64 base = 0.0;
      if (i < bs) {
        base = f0;
      } else if (i < 2 * bs) {
        base = f1;
      } else {
        base = f2;
      }
      rets[d][i] = base + noise;
    }
  }
  return rets;
}

// FNV-1a-64 over the flattened snapshots: for each snapshot fold date, n_labels,
// then every cluster_id (all as 64-bit little-endian words). Local to the test, as
// the determinism contract requires (offset 1469598103934665603, prime
// 1099511628211).
[[nodiscard]] std::uint64_t panel_digest(const ClusterPanel &cp) {
  std::uint64_t h = 1469598103934665603ULL;
  auto fold_u64 = [&](std::uint64_t v) {
    for (int b = 0; b < 8; ++b) {
      h ^= (v >> (b * 8)) & 0xFFULL;
      h *= 1099511628211ULL;
    }
  };
  fold_u64(static_cast<std::uint64_t>(cp.instruments));
  fold_u64(static_cast<std::uint64_t>(cp.snapshots.size()));
  for (const auto &s : cp.snapshots) {
    fold_u64(static_cast<std::uint64_t>(s.date));
    fold_u64(static_cast<std::uint64_t>(s.n_labels));
    for (const int cid : s.cluster_id) {
      fold_u64(static_cast<std::uint64_t>(static_cast<std::int64_t>(cid)));
    }
  }
  return h;
}

// Truncate a returns Panel to its first `keep` dates (rebuild via create()), so a
// truncation-invariance test can re-run the builder on a strictly-shorter Panel.
[[nodiscard]] Panel truncate_panel(const Panel &src, atx::usize keep) {
  const atx::usize instruments = src.instruments();
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> cols;
  for (atx::usize f = 0; f < src.num_fields(); ++f) {
    names.emplace_back(src.field_name(f));
    const auto whole = src.field_all(static_cast<atx::u32>(f));
    cols.emplace_back(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(keep * instruments));
  }
  // Rebuild the universe mask for the kept prefix (all-in-universe if every kept
  // cell is in-universe, else materialize the prefix mask).
  std::vector<std::uint8_t> mask(keep * instruments, 1);
  bool any_out = false;
  for (atx::usize d = 0; d < keep; ++d) {
    for (atx::usize i = 0; i < instruments; ++i) {
      if (!src.in_universe(d, i)) {
        mask[d * instruments + i] = 0;
        any_out = true;
      }
    }
  }
  if (!any_out) {
    mask.clear(); // empty == all-in-universe
  }
  auto p = Panel::create(keep, instruments, std::move(names), std::move(cols), std::move(mask));
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

[[nodiscard]] bool snapshots_equal(const ClusterPanel::Snapshot &a,
                                   const ClusterPanel::Snapshot &b) {
  return a.date == b.date && a.n_labels == b.n_labels && a.cluster_id == b.cluster_id;
}

// ===========================================================================
//  1. NO LOOK-AHEAD — the crux test. The snapshot at t is byte-identical whether
//     or not the Panel carries dates after t.
// ===========================================================================
TEST(ClusterPanel, BuildClusterPanel_TruncatedPanelAtT_SnapshotIdentical) {
  const atx::usize bs = 3, dates = 60;
  auto full_panel = make_returns_panel(three_block_rets(dates, bs));
  ClusterPanelConfig cfg{};
  cfg.window = 20;
  cfg.recluster_every = 5;
  cfg.k = 3;

  auto full = build_cluster_panel(full_panel, cfg);
  ASSERT_TRUE(full.has_value()) << (full ? "" : full.error().message());

  // For every snapshot at date t, rebuild the panel truncated to exactly [0, t]
  // and assert the snapshot the builder produces for t is unchanged.
  for (const auto &snap : full->snapshots) {
    const atx::usize t = snap.date;
    Panel trunc_panel = truncate_panel(full_panel, t + 1);
    auto trunc = build_cluster_panel(trunc_panel, cfg);
    ASSERT_TRUE(trunc.has_value()) << (trunc ? "" : trunc.error().message());
    ASSERT_FALSE(trunc->snapshots.empty());
    const auto &last = trunc->snapshots.back();
    EXPECT_EQ(last.date, t);
    EXPECT_TRUE(snapshots_equal(last, snap))
        << "look-ahead leak at t=" << t << ": snapshot changed when future bars removed";
  }
}

// The same property, expressed through the shared bias_audit truncation-invariance
// harness over the flattened snapshot stream.
TEST(ClusterPanel, BuildClusterPanel_BiasAuditHarness_TruncationInvariant) {
  const atx::usize bs = 3, dates = 60;
  auto full_panel = make_returns_panel(three_block_rets(dates, bs));
  ClusterPanelConfig cfg{};
  cfg.window = 20;
  cfg.recluster_every = 5;
  cfg.k = 3;

  // recompute(n_visible): flatten the snapshots of the panel truncated to the first
  // n_visible dates into one f64 stream (date, n_labels, cluster_id...). A causal
  // builder reproduces the earlier prefix bit-for-bit.
  auto recompute = [&](atx::usize n_visible) {
    Panel sub = truncate_panel(full_panel, n_visible);
    auto cp = build_cluster_panel(sub, cfg);
    std::vector<atx::f64> flat;
    if (cp.has_value()) {
      for (const auto &s : cp->snapshots) {
        flat.push_back(static_cast<atx::f64>(s.date));
        flat.push_back(static_cast<atx::f64>(s.n_labels));
        for (const int cid : s.cluster_id) {
          flat.push_back(static_cast<atx::f64>(cid));
        }
      }
    }
    return flat;
  };
  // Compare the full run against a run that sees only the first 45 dates; the
  // common prefix of the flattened stream must match exactly.
  const auto full = recompute(dates);
  const atx::usize cut = 45;
  const auto trunc = recompute(cut);
  ASSERT_FALSE(trunc.empty());
  EXPECT_TRUE(atx::engine::validation::check_no_lookahead(
      full.size(), trunc.size(), [&](atx::usize n) {
        return n == full.size() ? full : trunc;
      }));
}

// ===========================================================================
//  2. CADENCE — one snapshot per recluster date, at the expected dates, ascending.
// ===========================================================================
TEST(ClusterPanel, BuildClusterPanel_Cadence_SnapshotDatesMatchStep) {
  const atx::usize bs = 3, dates = 50;
  auto panel = make_returns_panel(three_block_rets(dates, bs));

  ClusterPanelConfig c1{};
  c1.window = 20;
  c1.recluster_every = 1;
  c1.k = 3;
  auto every1 = build_cluster_panel(panel, c1);
  ASSERT_TRUE(every1.has_value()) << (every1 ? "" : every1.error().message());
  // First valid t is window-1 == 19; every date through 49 -> 31 snapshots.
  ASSERT_EQ(every1->snapshots.size(), dates - 19u);
  EXPECT_EQ(every1->snapshots.front().date, 19u);
  EXPECT_EQ(every1->snapshots.back().date, 49u);
  for (atx::usize s = 0; s < every1->snapshots.size(); ++s) {
    EXPECT_EQ(every1->snapshots[s].date, 19u + s); // ascending, step 1
  }

  ClusterPanelConfig c20 = c1;
  c20.recluster_every = 20;
  auto every20 = build_cluster_panel(panel, c20);
  ASSERT_TRUE(every20.has_value()) << (every20 ? "" : every20.error().message());
  // t = 19, 39 (59 would exceed dates) -> 2 snapshots.
  ASSERT_EQ(every20->snapshots.size(), 2u);
  EXPECT_EQ(every20->snapshots[0].date, 19u);
  EXPECT_EQ(every20->snapshots[1].date, 39u);
}

// ===========================================================================
//  3. INSUFFICIENT HISTORY / OUT-OF-UNIVERSE -> kUnclustered.
// ===========================================================================
TEST(ClusterPanel, BuildClusterPanel_OutOfUniverseInstrument_Unclustered) {
  const atx::usize bs = 3, dates = 40;
  const atx::usize n = 3 * bs; // 9 instruments
  auto rets = three_block_rets(dates, bs);

  // Instrument 4 is out-of-universe on ONE date inside the first window -> excluded
  // from that window's valid set (the whole-window-in-universe rule).
  std::vector<std::uint8_t> mask(dates * n, 1);
  const atx::usize out_inst = 4;
  mask[10 * n + out_inst] = 0; // date 10 is inside window [0,19]
  auto panel = make_returns_panel(rets, mask);

  ClusterPanelConfig cfg{};
  cfg.window = 20;
  cfg.recluster_every = 5;
  cfg.k = 3;
  auto cp = build_cluster_panel(panel, cfg);
  ASSERT_TRUE(cp.has_value()) << (cp ? "" : cp.error().message());
  ASSERT_FALSE(cp->snapshots.empty());

  const auto &first = cp->snapshots.front(); // date 19, window [0,19] includes date 10
  EXPECT_EQ(first.date, 19u);
  EXPECT_EQ(first.cluster_id.size(), n);
  EXPECT_EQ(first.cluster_id[out_inst], ClusterPanel::kUnclustered);
  // Every other instrument is clusterable.
  for (atx::usize i = 0; i < n; ++i) {
    if (i != out_inst) {
      EXPECT_NE(first.cluster_id[i], ClusterPanel::kUnclustered) << "inst " << i;
    }
  }

  // A NaN return inside a window also drops the instrument from that window.
  std::vector<std::vector<atx::f64>> rets_nan = three_block_rets(dates, bs);
  rets_nan[12][7] = std::numeric_limits<atx::f64>::quiet_NaN(); // date 12 in window [0,19]
  auto panel_nan = make_returns_panel(rets_nan);
  auto cp_nan = build_cluster_panel(panel_nan, cfg);
  ASSERT_TRUE(cp_nan.has_value()) << (cp_nan ? "" : cp_nan.error().message());
  EXPECT_EQ(cp_nan->snapshots.front().cluster_id[7], ClusterPanel::kUnclustered);
}

// ===========================================================================
//  4. REGIME SHIFT — cluster membership CHANGES after the shift date, not before.
// ===========================================================================
TEST(ClusterPanel, BuildClusterPanel_RegimeShift_MembershipChangesAfterShift) {
  // 10 instruments in two factor blocks of five. Before the shift, block A =
  // {0,1,2,3,4} co-move on factor A and block B = {5,6,7,8,9} on factor B. After
  // the shift, the SWITCHER (instrument 4) moves onto factor B, so a window
  // confined to pre-shift dates groups 4 with {0,1,2,3}, while a window fully past
  // the shift groups it with {5,6,7,8,9}.
  //
  // Fixture strength is load-bearing here. The clustering pipeline runs the raw
  // window correlation through rmt_clean BEFORE partitioning: an MP-edge fit clips
  // every eigenvalue it judges to be sampling noise. With thin blocks / a short
  // window / loud noise, q = N/T pushes the MP edge above the two factor
  // eigenvalues, rmt_clean clips the WHOLE spectrum to ~identity, and Ward then
  // ties everywhere and peels off an arbitrary pair — the partition no longer
  // reflects the planted regime. Five-member blocks, a 40-day window (q = 10/40 =
  // 0.25), and a modest 0.15 noise amplitude keep both factor eigenvalues clearly
  // above the MP edge, so the cleaned correlation stays a crisp 2-block matrix
  // (verified: within-block ~0.99, cross-block ~0.0) and the k=2 cut recovers the
  // factor-A / factor-B blocks. (See atx-core hierarchical_test's block-recovery
  // fixtures for the within>>between separation this mirrors.)
  const atx::usize half = 5;            // members per factor block
  const atx::usize n = 2 * half;        // 10 instruments
  const atx::usize dates = 120, window = 40, shift = 60, sw = half - 1; // switcher == 4
  std::vector<std::vector<atx::f64>> rets(dates, std::vector<atx::f64>(n, 0.0));
  for (atx::usize d = 0; d < dates; ++d) {
    const atx::f64 t = static_cast<atx::f64>(d);
    // Two near-orthogonal factors over any 40-day window, plus full-rank per-name
    // noise so the per-window covariance has a Marchenko-Pastur bulk while the two
    // factor eigenvalues sit clearly above the rmt_clean edge.
    const atx::f64 fa = std::sin(0.55 * t);
    const atx::f64 fb = std::cos(0.20 * t + 1.0);
    const bool post = d >= shift;
    for (atx::usize i = 0; i < n; ++i) {
      const atx::f64 noise = 0.15 * hash_noise(d, i);
      atx::f64 base;
      if (i == sw) {
        base = post ? fb : fa; // the switcher
      } else if (i < half) {
        base = fa;
      } else {
        base = fb;
      }
      rets[d][i] = base + noise;
    }
  }
  auto panel = make_returns_panel(rets);

  ClusterPanelConfig cfg{};
  cfg.window = static_cast<int>(window);
  cfg.recluster_every = 1;
  cfg.k = 2;
  auto cp = build_cluster_panel(panel, cfg);
  ASSERT_TRUE(cp.has_value()) << (cp ? "" : cp.error().message());

  auto snap_at = [&](atx::usize t) -> const ClusterPanel::Snapshot & {
    for (const auto &s : cp->snapshots) {
      if (s.date == t) {
        return s;
      }
    }
    ADD_FAILURE() << "no snapshot at t=" << t;
    return cp->snapshots.front();
  };

  {
    std::string p = "pre n=" + std::to_string(snap_at(window - 1).n_labels) + ":";
    for (const int c : snap_at(window - 1).cluster_id) p += " " + std::to_string(c);
    std::string q = "post n=" + std::to_string(snap_at(shift + window - 1).n_labels) + ":";
    for (const int c : snap_at(shift + window - 1).cluster_id) q += " " + std::to_string(c);
    GTEST_LOG_(INFO) << p << " | " << q;
  }
  // Pre-shift window [0, window-1]: the switcher clusters WITH block A
  // (representative instrument 0) and AWAY from block B (representative 5).
  const auto &pre = snap_at(window - 1); // t=39
  EXPECT_EQ(pre.cluster_id[sw], pre.cluster_id[0]);
  EXPECT_NE(pre.cluster_id[sw], pre.cluster_id[half]);

  // Fully-post-shift window [shift, shift+window-1]: the switcher now clusters
  // WITH block B (representative instrument 5) and AWAY from block A (instrument 0).
  const auto &post = snap_at(shift + window - 1); // t=99
  EXPECT_EQ(post.cluster_id[sw], post.cluster_id[half]);
  EXPECT_NE(post.cluster_id[sw], post.cluster_id[0]);
}

// ===========================================================================
//  4b. CLEAN 3-BLOCK RECOVERY — a snapshot over a clean three-factor panel
//      recovers exactly the three planted blocks at k=3.
// ===========================================================================
TEST(ClusterPanel, BuildClusterPanel_ThreeBlockFixture_RecoversThreeBlocks) {
  // Five instruments per factor block (15 total) over a 40-day window keep all
  // three factor eigenvalues above the rmt_clean MP edge (q = 15/40 = 0.375), so
  // the cleaned correlation is a crisp 3-block matrix and the k=3 cut recovers
  // the planted blocks {0..4}, {5..9}, {10..14} exactly. Three near-orthogonal
  // factors plus a modest 0.15 noise amplitude (the same strength the regime
  // fixture validated against the MP edge) give within-block >> between-block
  // correlation through the clean step.
  const atx::usize bs = 5, dates = 60, window = 40;
  const atx::usize n = 3 * bs;
  std::vector<std::vector<atx::f64>> rets(dates, std::vector<atx::f64>(n, 0.0));
  for (atx::usize d = 0; d < dates; ++d) {
    // Three INDEPENDENT full-rank factor streams (distinct hash_noise instrument
    // ids 1000/2000/3000) rather than sinusoids: finite-window sinusoids of
    // different frequencies still share a large common component, so rmt_clean
    // collapses them to a single factor and no block survives. Independent
    // pseudo-random factors are mutually near-orthogonal over the window, so the
    // three block eigenvalues stay distinct and above the MP edge and the k=3 cut
    // recovers the planted blocks {0..4},{5..9},{10..14}.
    const atx::f64 f0 = hash_noise(d, 1000);
    const atx::f64 f1 = hash_noise(d, 2000);
    const atx::f64 f2 = hash_noise(d, 3000);
    for (atx::usize i = 0; i < n; ++i) {
      const atx::f64 noise = 0.15 * hash_noise(d, i);
      const atx::f64 base = (i < bs) ? f0 : (i < 2 * bs) ? f1 : f2;
      rets[d][i] = base + noise;
    }
  }
  auto panel = make_returns_panel(rets);

  ClusterPanelConfig cfg{};
  cfg.window = static_cast<int>(window);
  cfg.recluster_every = static_cast<int>(window); // a single snapshot at t=window-1
  cfg.k = 3;
  auto cp = build_cluster_panel(panel, cfg);
  ASSERT_TRUE(cp.has_value()) << (cp ? "" : cp.error().message());
  ASSERT_FALSE(cp->snapshots.empty());

  const auto &snap = cp->snapshots.front();
  EXPECT_EQ(snap.date, window - 1);
  EXPECT_EQ(snap.n_labels, 3);
  // Within each planted block every instrument shares the block representative's
  // label; across blocks the labels differ. Canonical labels (ascending smallest-
  // member index) make block {0..4}=0, {5..9}=1, {10..14}=2, but we assert the
  // partition by co-membership rather than the literal ids to stay robust.
  for (atx::usize b = 0; b < 3; ++b) {
    const atx::usize rep = b * bs;
    for (atx::usize i = b * bs; i < (b + 1) * bs; ++i) {
      EXPECT_EQ(snap.cluster_id[i], snap.cluster_id[rep]) << "block " << b << " inst " << i;
    }
  }
  EXPECT_NE(snap.cluster_id[0], snap.cluster_id[bs]);
  EXPECT_NE(snap.cluster_id[bs], snap.cluster_id[2 * bs]);
  EXPECT_NE(snap.cluster_id[0], snap.cluster_id[2 * bs]);
  // Every instrument is clusterable (no kUnclustered cell on a full-universe panel).
  for (atx::usize i = 0; i < n; ++i) {
    EXPECT_NE(snap.cluster_id[i], ClusterPanel::kUnclustered) << "inst " << i;
  }
}

// ===========================================================================
//  4c. ALGO SELECTION — the cfg.algo field is honored end-to-end. SpongeSym
//      (signed-graph) is exposed but NOT the default; selecting it routes a real
//      window through atx::core::cluster's signed-graph partitioner, which stays
//      deterministic and yields a valid full-length labeling. We assert the
//      plumbing + determinism, not block recovery: SpongeSym optimizes a signed-
//      cut objective that keys on NEGATIVE correlations as repulsion, so on a
//      positive-cross-correlation fixture it need not reproduce Ward's distance-
//      based partition (a different, equally valid objective).
// ===========================================================================
TEST(ClusterPanel, BuildClusterPanel_SpongeSymAlgo_HonoredAndDeterministic) {
  // Clean three-block fixture (independent factor streams).
  const atx::usize bs = 5, dates = 60, window = 40;
  const atx::usize n = 3 * bs;
  std::vector<std::vector<atx::f64>> rets(dates, std::vector<atx::f64>(n, 0.0));
  for (atx::usize d = 0; d < dates; ++d) {
    const atx::f64 f0 = hash_noise(d, 1000);
    const atx::f64 f1 = hash_noise(d, 2000);
    const atx::f64 f2 = hash_noise(d, 3000);
    for (atx::usize i = 0; i < n; ++i) {
      const atx::f64 base = (i < bs) ? f0 : (i < 2 * bs) ? f1 : f2;
      rets[d][i] = base + 0.15 * hash_noise(d, i);
    }
  }
  auto panel = make_returns_panel(rets);

  ClusterPanelConfig hier_cfg{};
  hier_cfg.window = static_cast<int>(window);
  hier_cfg.recluster_every = static_cast<int>(window);
  hier_cfg.k = 3;
  // The default is Hierarchical (asserted in cluster_scaffold_test); confirm here too.
  EXPECT_EQ(hier_cfg.algo, atx::core::cluster::Algo::Hierarchical);
  auto hier = build_cluster_panel(panel, hier_cfg);
  ASSERT_TRUE(hier.has_value()) << (hier ? "" : hier.error().message());

  ClusterPanelConfig sponge_cfg = hier_cfg;
  sponge_cfg.algo = atx::core::cluster::Algo::SpongeSym;

  // SpongeSym path is exercised and is itself deterministic: two builds agree.
  auto a = build_cluster_panel(panel, sponge_cfg);
  auto b = build_cluster_panel(panel, sponge_cfg);
  ASSERT_TRUE(a.has_value()) << (a ? "" : a.error().message());
  ASSERT_TRUE(b.has_value());
  ASSERT_FALSE(a->snapshots.empty());
  EXPECT_TRUE(snapshots_equal(a->snapshots.front(), b->snapshots.front()));

  // It produces a valid full-length partition: every instrument is clustered (the
  // panel is full-universe) into a canonical label in [0, n_labels).
  const auto &snap = a->snapshots.front();
  EXPECT_EQ(snap.cluster_id.size(), n);
  EXPECT_GT(snap.n_labels, 0);
  EXPECT_LE(snap.n_labels, 3);
  for (atx::usize i = 0; i < n; ++i) {
    EXPECT_NE(snap.cluster_id[i], ClusterPanel::kUnclustered) << "inst " << i;
    EXPECT_GE(snap.cluster_id[i], 0);
    EXPECT_LT(snap.cluster_id[i], static_cast<int>(snap.n_labels)) << "inst " << i;
  }
}

// ===========================================================================
//  5. CAPM RESIDUAL — stripping a planted common market factor changes the
//     clusters relative to raw returns when the market factor dominates.
// ===========================================================================
TEST(ClusterPanel, BuildClusterPanel_CapmResidual_StripsCommonMarketFactor) {
  // 6 instruments, all loaded heavily on a single common market factor `mkt` so
  // RAW returns are near-perfectly correlated across the board (a single blob). A
  // weak idiosyncratic block structure ({0,1,2} share gA, {3,4,5} share gB) is
  // masked by the market in raw space but DOMINATES the CAPM residual.
  const atx::usize n = 6, dates = 60, window = 30;
  std::vector<std::vector<atx::f64>> rets(dates, std::vector<atx::f64>(n, 0.0));
  for (atx::usize d = 0; d < dates; ++d) {
    const atx::f64 t = static_cast<atx::f64>(d);
    const atx::f64 mkt = std::sin(0.20 * t);             // dominant common factor
    const atx::f64 ga = 0.35 * std::cos(0.50 * t + 0.5); // block-A residual factor
    const atx::f64 gb = 0.35 * std::cos(0.50 * t + 2.9); // block-B residual factor
    for (atx::usize i = 0; i < n; ++i) {
      const atx::f64 noise = 0.10 * hash_noise(d, i);
      const atx::f64 grp = (i < 3) ? ga : gb;
      rets[d][i] = mkt + grp + noise; // unit market beta + block residual + noise
    }
  }
  auto panel = make_returns_panel(rets);

  ClusterPanelConfig raw_cfg{};
  raw_cfg.window = static_cast<int>(window);
  raw_cfg.recluster_every = static_cast<int>(window); // single snapshot at t=window-1
  raw_cfg.k = 2;
  raw_cfg.residualize = ClusterPanelConfig::Residualize::None;
  auto raw = build_cluster_panel(panel, raw_cfg);
  ASSERT_TRUE(raw.has_value()) << (raw ? "" : raw.error().message());
  ASSERT_FALSE(raw->snapshots.empty());

  ClusterPanelConfig capm_cfg = raw_cfg;
  capm_cfg.residualize = ClusterPanelConfig::Residualize::CAPM;
  auto capm = build_cluster_panel(panel, capm_cfg);
  ASSERT_TRUE(capm.has_value()) << (capm ? "" : capm.error().message());
  ASSERT_FALSE(capm->snapshots.empty());

  const auto &cs = capm->snapshots.front().cluster_id;
  // After residualizing out the market, the weak block structure is recovered:
  // {0,1,2} share a label and {3,4,5} share a label, the two differing.
  EXPECT_EQ(cs[0], cs[1]);
  EXPECT_EQ(cs[1], cs[2]);
  EXPECT_EQ(cs[3], cs[4]);
  EXPECT_EQ(cs[4], cs[5]);
  EXPECT_NE(cs[0], cs[3]);
  // The CAPM partition differs from the raw partition (the market masked the
  // blocks in raw space).
  EXPECT_NE(capm->snapshots.front().cluster_id, raw->snapshots.front().cluster_id);
}

// ===========================================================================
//  6. DETERMINISM — recluster_every=1 and =20 both stable across two builds.
// ===========================================================================
TEST(ClusterPanel, BuildClusterPanel_BothCadences_Deterministic) {
  const atx::usize bs = 3, dates = 50;
  auto panel = make_returns_panel(three_block_rets(dates, bs));

  for (const int step : {1, 20}) {
    ClusterPanelConfig cfg{};
    cfg.window = 20;
    cfg.recluster_every = step;
    cfg.k = 3;
    auto a = build_cluster_panel(panel, cfg);
    auto b = build_cluster_panel(panel, cfg);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_EQ(a->snapshots.size(), b->snapshots.size());
    for (atx::usize s = 0; s < a->snapshots.size(); ++s) {
      EXPECT_TRUE(snapshots_equal(a->snapshots[s], b->snapshots[s])) << "step=" << step;
    }
  }
}

// ===========================================================================
//  7. TWO-RUNS-EQUAL — identical ClusterPanel across two builds (CAPM path too).
// ===========================================================================
TEST(ClusterPanel, BuildClusterPanel_TwoRuns_DigestEqual) {
  const atx::usize bs = 4, dates = 55;
  auto panel = make_returns_panel(three_block_rets(dates, bs));
  ClusterPanelConfig cfg{};
  cfg.window = 25;
  cfg.recluster_every = 7;
  cfg.k = 3;
  cfg.residualize = ClusterPanelConfig::Residualize::CAPM;
  auto a = build_cluster_panel(panel, cfg);
  auto b = build_cluster_panel(panel, cfg);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(panel_digest(*a), panel_digest(*b));
}

// ===========================================================================
//  8. PINNED DIGEST — FNV-1a-64 over the flattened snapshots equals the golden.
// ===========================================================================
TEST(ClusterPanel, BuildClusterPanel_PinnedDigest_MatchesGolden) {
  const atx::usize bs = 3, dates = 50;
  auto panel = make_returns_panel(three_block_rets(dates, bs));
  ClusterPanelConfig cfg{};
  cfg.window = 20;
  cfg.recluster_every = 10;
  cfg.k = 3;
  auto cp = build_cluster_panel(panel, cfg);
  ASSERT_TRUE(cp.has_value()) << (cp ? "" : cp.error().message());
  // Golden pinned from the as-built deterministic result; any drift in the
  // windowing / correlation / clean / cluster pipeline trips this.
  constexpr std::uint64_t kGolden = 4293541372186093814ULL;
  EXPECT_EQ(panel_digest(*cp), kGolden) << "actual digest = " << panel_digest(*cp);
}

// ===========================================================================
//  9. DEGENERATE CONFIGS — invalid args -> Err(InvalidArgument); degenerate but
//     valid configs guard to all-kUnclustered / empty snapshots.
// ===========================================================================
TEST(ClusterPanel, BuildClusterPanel_DegenerateConfigs_GuardedOrErr) {
  const atx::usize bs = 3, dates = 30;
  const atx::usize n = 3 * bs;
  auto panel = make_returns_panel(three_block_rets(dates, bs));

  auto is_invalid_arg = [](const auto &r) {
    return !r.has_value() && r.error().code() == atx::core::ErrorCode::InvalidArgument;
  };

  // Non-positive window / recluster_every / k -> InvalidArgument.
  {
    ClusterPanelConfig c{};
    c.window = 0;
    c.recluster_every = 5;
    c.k = 3;
    EXPECT_TRUE(is_invalid_arg(build_cluster_panel(panel, c)));
  }
  {
    ClusterPanelConfig c{};
    c.window = 20;
    c.recluster_every = 0;
    c.k = 3;
    EXPECT_TRUE(is_invalid_arg(build_cluster_panel(panel, c)));
  }
  {
    ClusterPanelConfig c{};
    c.window = 20;
    c.recluster_every = 5;
    c.k = 0;
    EXPECT_TRUE(is_invalid_arg(build_cluster_panel(panel, c)));
  }
  // k beyond the instrument count -> InvalidArgument.
  {
    ClusterPanelConfig c{};
    c.window = 20;
    c.recluster_every = 5;
    c.k = static_cast<int>(n) + 1;
    EXPECT_TRUE(is_invalid_arg(build_cluster_panel(panel, c)));
  }
  // window > dates -> no recluster date is reachable -> empty snapshots (valid).
  {
    ClusterPanelConfig c{};
    c.window = static_cast<int>(dates) + 5;
    c.recluster_every = 5;
    c.k = 3;
    auto r = build_cluster_panel(panel, c);
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message());
    EXPECT_TRUE(r->snapshots.empty());
    EXPECT_EQ(r->instruments, n);
  }
  // Unknown return field -> Err (NotFound propagated from Panel::field_id).
  {
    ClusterPanelConfig c{};
    c.window = 20;
    c.recluster_every = 5;
    c.k = 3;
    c.return_field = "no_such_field";
    auto r = build_cluster_panel(panel, c);
    EXPECT_FALSE(r.has_value());
  }
}

// Empty universe across an entire window -> all instruments kUnclustered, k clamped.
TEST(ClusterPanel, BuildClusterPanel_EmptyUniverseWindow_AllUnclustered) {
  const atx::usize bs = 3, dates = 30;
  const atx::usize n = 3 * bs;
  auto rets = three_block_rets(dates, bs);
  // Mark the FIRST window's instruments all out-of-universe so the window at t=19
  // has an empty valid set.
  std::vector<std::uint8_t> mask(dates * n, 1);
  for (atx::usize d = 0; d <= 19; ++d) {
    for (atx::usize i = 0; i < n; ++i) {
      mask[d * n + i] = 0;
    }
  }
  auto panel = make_returns_panel(rets, mask);

  ClusterPanelConfig cfg{};
  cfg.window = 20;
  cfg.recluster_every = 20; // only t=19 reachable in a way the empty window hits
  cfg.k = 3;
  auto cp = build_cluster_panel(panel, cfg);
  ASSERT_TRUE(cp.has_value()) << (cp ? "" : cp.error().message());
  ASSERT_FALSE(cp->snapshots.empty());
  const auto &first = cp->snapshots.front();
  EXPECT_EQ(first.date, 19u);
  EXPECT_EQ(first.n_labels, 0);
  for (const int cid : first.cluster_id) {
    EXPECT_EQ(cid, ClusterPanel::kUnclustered);
  }
}

} // namespace atxtest_cluster_panel
