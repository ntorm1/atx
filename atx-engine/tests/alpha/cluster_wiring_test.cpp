// atx::engine::alpha — S11-5 ClusterWiring: the rolling cluster panel wired into
// the DSL group operators as an `IndClass.cluster` field.
//
// This suite proves the headline fact: a Panel column named `IndClass.cluster`
// (broadcast from a ClusterPanel) is recognized by the EXISTING group operators
// with NO typecheck/registry/VM change, and evaluates BIT-FOR-BIT identically on
// the vectorized VM and the tree-walking oracle (the mandatory differential,
// mirroring alpha_cs_test.cpp). It also covers:
//   * NaN (kUnclustered) cells excluded from their group's reduction;
//   * the step-function broadcast (snapshot -> date-range hold; pre-first NaN);
//   * a cluster column baked into a sealed segment, attached, round-tripped;
//   * the online group_map feeding WeightPolicy::to_target_weights;
//   * two-runs-equal + a pinned FNV-1a-64 digest of the broadcast column.
//
// Naming: Subject_Condition_ExpectedResult.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring> // std::memcpy (digest)
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error> // std::error_code
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/cluster_field.hpp"
#include "atx/engine/alpha/cluster_panel.hpp"
#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/segment_panel.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

#include "atx/engine/loop/signal_source.hpp" // SignalView
#include "atx/engine/loop/types.hpp"         // InstrumentId, Universe
#include "atx/engine/loop/weight_policy.hpp" // WeightPolicy, Transform

#include "atx/tsdb/builder.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// NOLINTBEGIN(misc-include-cleaner)
#include <windows.h>
// NOLINTEND(misc-include-cleaner)
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace atxtest_cluster_wiring_test {

using atx::engine::alpha::analyze;
using atx::engine::alpha::append_cluster_field;
using atx::engine::alpha::broadcast_cluster_field;
using atx::engine::alpha::cluster_group_map;
using atx::engine::alpha::ClusterPanel;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::kClusterFieldName;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;

// A process-lifetime Library so any borrowed OpSig stays valid across tests.
[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// Two cells agree iff both NaN, or exactly value-equal.
[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

// Compile a bare expression all the way to a Program (parse -> analyze -> compile).
[[nodiscard]] Program compile_ok(std::string_view src) {
  auto ast = parse_expr(src, shared_lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << (ana ? "" : ana.error().message());
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

// The core differential assertion: VM == oracle, cell by cell.
void expect_vm_matches_oracle(std::string_view expr, const Panel &panel) {
  const Program prog = compile_ok(expr);
  Engine engine{panel};
  auto vm = engine.evaluate(prog);
  ASSERT_TRUE(vm.has_value()) << "VM: " << (vm ? "" : vm.error().message());
  auto ref = evaluate_reference(prog, panel);
  ASSERT_TRUE(ref.has_value()) << "oracle: " << (ref ? "" : ref.error().message());

  const SignalSet &v = vm.value();
  const SignalSet &r = ref.value();
  ASSERT_EQ(v.alphas.size(), r.alphas.size());
  ASSERT_EQ(v.dates, r.dates);
  ASSERT_EQ(v.instruments, r.instruments);
  for (atx::usize a = 0; a < v.alphas.size(); ++a) {
    ASSERT_EQ(v.alphas[a].values.size(), r.alphas[a].values.size());
    for (atx::usize i = 0; i < v.alphas[a].values.size(); ++i) {
      const atx::f64 vc = v.alphas[a].values[i];
      const atx::f64 rc = r.alphas[a].values[i];
      EXPECT_TRUE(same_cell(vc, rc)) << "expr '" << expr << "' alpha " << a << " cell " << i
                                     << ": VM=" << vc << " oracle=" << rc;
    }
  }
}

// Run the VM and return the single root's date-major values (for known-value asserts).
[[nodiscard]] std::vector<atx::f64> vm_values(std::string_view expr, const Panel &panel) {
  const Program prog = compile_ok(expr);
  Engine engine{panel};
  auto out = engine.evaluate(prog);
  EXPECT_TRUE(out.has_value()) << "VM: " << (out ? "" : out.error().message());
  if (!out.has_value() || out.value().alphas.empty()) {
    return {};
  }
  return out.value().alphas[0].values;
}

// FNV-1a-64 over the column's IEEE bytes (same offset/prime scheme as the S11
// store fingerprint / the S11-1..S11-4 pinned digests). A NaN is normalized to a
// single canonical quiet-NaN bit pattern so the digest is platform-stable.
[[nodiscard]] atx::u64 fnv1a64(const std::vector<atx::f64> &xs) noexcept {
  constexpr atx::u64 kOffset = 1469598103934665603ULL;
  constexpr atx::u64 kPrime = 1099511628211ULL;
  atx::u64 h = kOffset;
  for (const atx::f64 x : xs) {
    atx::u64 bits = 0;
    const atx::f64 v = std::isnan(x) ? std::numeric_limits<atx::f64>::quiet_NaN() : x;
    static_assert(sizeof(bits) == sizeof(v));
    std::memcpy(&bits, &v, sizeof(bits));
    for (int b = 0; b < 8; ++b) {
      h ^= (bits >> (8 * b)) & 0xFFULL;
      h *= kPrime;
    }
  }
  return h;
}

// ---------------------------------------------------------------------------
//  Panel + ClusterPanel fixtures.
// ---------------------------------------------------------------------------

// Build a Panel with a `ret` field (the return column the source carries) plus the
// five OHLCV names a DSL expr may reference. Field order: ret/close/open/high/low/
// volume. An empty universe means all cells in-universe. (No IndClass.cluster yet —
// that is appended via append_cluster_field.)
[[nodiscard]] Panel make_source_panel(atx::usize dates, atx::usize instruments,
                                      std::vector<atx::f64> ret_col,
                                      std::vector<std::uint8_t> universe = {}) {
  std::vector<std::string> names = {"ret", "close", "open", "high", "low", "volume"};
  std::vector<std::vector<atx::f64>> cols(names.size(),
                                          std::vector<atx::f64>(dates * instruments, 0.0));
  cols[0] = std::move(ret_col);
  // Give `close` a distinct deterministic pattern so the group ops have signal.
  for (atx::usize i = 0; i < dates * instruments; ++i) {
    cols[1][i] = 1.0 + static_cast<atx::f64>(i % 97); // close
    cols[2][i] = cols[1][i] - 0.5;                    // open
    cols[3][i] = cols[1][i] + 1.0;                    // high
    cols[4][i] = cols[1][i] - 1.0;                    // low
    cols[5][i] = 1000.0 + static_cast<atx::f64>(i);   // volume
  }
  auto p = Panel::create(dates, instruments, std::move(names), std::move(cols), std::move(universe));
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

// A single-snapshot ClusterPanel holding `labels` from date 0.
[[nodiscard]] ClusterPanel one_snapshot(std::vector<int> labels, atx::i64 n_labels,
                                        atx::usize instruments) {
  ClusterPanel cp;
  cp.instruments = instruments;
  ClusterPanel::Snapshot snap;
  snap.date = 0;
  snap.cluster_id = std::move(labels);
  snap.n_labels = n_labels;
  cp.snapshots.push_back(std::move(snap));
  return cp;
}

// ===========================================================================
//  Differential — group ops over the broadcast IndClass.cluster match the oracle.
// ===========================================================================

// group_neutralize / group_zscore / group_rank / group_mean over an
// IndClass.cluster column appended from a ClusterPanel must equal the oracle
// BIT-FOR-BIT on the VM. This is the mandatory differential (the headline fact:
// no new operators — the existing group_* family keys IndClass.cluster by name).
TEST(ClusterWiring, GroupOpsOverClusterField_MatchOracleBitForBit) {
  const atx::usize dates = 4;
  const atx::usize instruments = 6;
  // ret is irrelevant to the broadcast here; give it a deterministic fill.
  std::vector<atx::f64> ret(dates * instruments);
  for (atx::usize i = 0; i < ret.size(); ++i) {
    ret[i] = 0.001 * static_cast<atx::f64>((i * 7) % 11) - 0.005;
  }
  const Panel src = make_source_panel(dates, instruments, std::move(ret));

  // Two clusters {0,0,0,1,1,1}; held across all dates from snapshot at date 0.
  const ClusterPanel cp = one_snapshot({0, 0, 0, 1, 1, 1}, /*n_labels=*/2, instruments);
  auto augmented = append_cluster_field(src, cp);
  ASSERT_TRUE(augmented.has_value()) << (augmented ? "" : augmented.error().message());
  const Panel &panel = augmented.value();

  // IndClass.cluster resolves as a field on the augmented panel.
  ASSERT_TRUE(panel.field_id(kClusterFieldName).has_value());

  expect_vm_matches_oracle("group_neutralize(close, IndClass.cluster)", panel);
  expect_vm_matches_oracle("group_zscore(close, IndClass.cluster)", panel);
  expect_vm_matches_oracle("group_rank(close, IndClass.cluster)", panel);
  expect_vm_matches_oracle("group_mean(close, IndClass.cluster)", panel);
  // indneutralize is the pinned alias of group_neutralize — exercise it too.
  expect_vm_matches_oracle("indneutralize(ret, IndClass.cluster)", panel);
}

// IndClass.cluster is group-typed BY NAME with no typecheck change: analyze()
// accepts group_neutralize(.., IndClass.cluster) as a group op (it would reject a
// non-Group 2nd argument). Proves the typecheck seam directly.
TEST(ClusterWiring, ClusterFieldIsGroupTyped_NoTypecheckChange) {
  // is_group_field() recognizes the IndClass. prefix (the headline fact).
  EXPECT_TRUE(atx::engine::alpha::detail::is_group_field(kClusterFieldName));

  const atx::usize dates = 2;
  const atx::usize instruments = 4;
  const Panel src = make_source_panel(dates, instruments,
                                      std::vector<atx::f64>(dates * instruments, 0.01));
  const ClusterPanel cp = one_snapshot({0, 0, 1, 1}, 2, instruments);
  auto augmented = append_cluster_field(src, cp);
  ASSERT_TRUE(augmented.has_value());

  // Compiles + typechecks as a group op end-to-end (parse -> analyze -> compile).
  auto ast = parse_expr("group_neutralize(close, IndClass.cluster)", shared_lib());
  ASSERT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << "group op over IndClass.cluster must typecheck: "
                               << (ana ? "" : ana.error().message());
}

// A kUnclustered (-1 -> NaN) cell is excluded from its group's reduction and its
// own output is NaN, exactly like an out-of-group cell in cs_ops.
TEST(ClusterWiring, UnclusteredCell_StaysNaNAndExcluded) {
  const atx::usize dates = 1;
  const atx::usize instruments = 4;
  // close = {10, 20, 100, 140}; clusters {0,0,1,-1}. Instrument 3 is unclustered.
  const Panel src = make_source_panel(dates, instruments,
                                      std::vector<atx::f64>(dates * instruments, 0.0));
  const ClusterPanel cp = one_snapshot({0, 0, 1, ClusterPanel::kUnclustered}, 2, instruments);
  auto augmented = append_cluster_field(src, cp);
  ASSERT_TRUE(augmented.has_value());

  // The broadcast cell for instrument 3 must be NaN.
  const std::vector<atx::f64> field =
      broadcast_cluster_field(cp, dates);
  EXPECT_TRUE(std::isnan(field[3])) << "kUnclustered must encode to NaN";
  EXPECT_DOUBLE_EQ(field[0], 0.0);
  EXPECT_DOUBLE_EQ(field[2], 1.0);

  // group_neutralize: instrument 3 -> NaN; group 0 = {i0,i1} demeans; group 1 = {i2}
  // singleton demeans to 0.
  const std::vector<atx::f64> gn =
      vm_values("group_neutralize(close, IndClass.cluster)", augmented.value());
  ASSERT_EQ(gn.size(), instruments);
  EXPECT_TRUE(std::isnan(gn[3])); // unclustered -> NaN, excluded from any group
  EXPECT_NEAR(gn[0] + gn[1], 0.0, 1e-12); // group 0 demeaned -> sums to 0
  EXPECT_DOUBLE_EQ(gn[2], 0.0);           // singleton group 1 -> x - x == 0
  expect_vm_matches_oracle("group_neutralize(close, IndClass.cluster)", augmented.value());
}

// ===========================================================================
//  Broadcast step-function correctness.
// ===========================================================================

// A 2-snapshot ClusterPanel broadcasts each snapshot's labels to its date range,
// and dates before the first snapshot are NaN (warm-up / kUnclustered).
TEST(ClusterWiring, BroadcastStepFunction_HoldsLabelsAndPreFirstIsNaN) {
  const atx::usize instruments = 3;
  ClusterPanel cp;
  cp.instruments = instruments;
  // Snapshot A holds from date 2; snapshot B holds from date 5. Dates 0,1 are
  // pre-first (warm-up) -> NaN.
  ClusterPanel::Snapshot a;
  a.date = 2;
  a.cluster_id = {0, 1, 0};
  a.n_labels = 2;
  ClusterPanel::Snapshot b;
  b.date = 5;
  b.cluster_id = {1, 1, 0};
  b.n_labels = 2;
  cp.snapshots.push_back(std::move(a));
  cp.snapshots.push_back(std::move(b));

  const atx::usize dates = 7;
  const std::vector<atx::f64> col = broadcast_cluster_field(cp, dates);
  ASSERT_EQ(col.size(), dates * instruments);

  // Dates 0,1: all NaN (before the first snapshot).
  for (atx::usize d = 0; d < 2; ++d) {
    for (atx::usize j = 0; j < instruments; ++j) {
      EXPECT_TRUE(std::isnan(col[d * instruments + j])) << "pre-first date " << d << " inst " << j;
    }
  }
  // Dates 2,3,4: snapshot A {0,1,0}.
  for (atx::usize d = 2; d < 5; ++d) {
    EXPECT_DOUBLE_EQ(col[d * instruments + 0], 0.0);
    EXPECT_DOUBLE_EQ(col[d * instruments + 1], 1.0);
    EXPECT_DOUBLE_EQ(col[d * instruments + 2], 0.0);
  }
  // Dates 5,6: snapshot B {1,1,0} (last snapshot holds to panel end).
  for (atx::usize d = 5; d < 7; ++d) {
    EXPECT_DOUBLE_EQ(col[d * instruments + 0], 1.0);
    EXPECT_DOUBLE_EQ(col[d * instruments + 1], 1.0);
    EXPECT_DOUBLE_EQ(col[d * instruments + 2], 0.0);
  }
}

// ===========================================================================
//  Segment-baked cluster column round-trips.
// ===========================================================================

[[nodiscard]] std::string temp_dir(const std::string &name) {
#if defined(_WIN32)
  // NOLINTBEGIN(misc-include-cleaner)
  // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  wchar_t tmp_dir[MAX_PATH + 1]{};
  GetTempPathW(MAX_PATH + 1, tmp_dir);
  wchar_t tmp_file[MAX_PATH + 1]{};
  GetTempFileNameW(tmp_dir, L"atx", 0, tmp_file);
  // NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  std::wstring wpath(tmp_file);
  const std::wstring wsuffix(name.begin(), name.end());
  // GetTempFileNameW created a file; reuse its unique stem as a directory name.
  std::remove(std::string(wpath.begin(), wpath.end()).c_str());
  wpath += wsuffix + L"_dir";
  const std::string path(wpath.begin(), wpath.end());
  // NOLINTEND(misc-include-cleaner)
#else
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  char buf[L_tmpnam]{};
  // NOLINTNEXTLINE(cert-msc50-cpp,cert-msc30-c)
  std::tmpnam(buf);
  const std::string path = std::string(buf) + name + "_dir";
#endif
  return path;
}

// Bake the cluster column into a sealed segment (under the short `cluster` tag —
// the atx-tsdb field-name cap is 15 bytes, so the full 16-char `IndClass.cluster`
// cannot survive a round-trip verbatim), attach via attach_multi_segment_panel,
// rename to `IndClass.cluster`, and confirm the column matches the broadcast
// source values cell-for-cell.
TEST(ClusterWiring, SegmentBakedClusterColumn_RoundTrips) {
  namespace fs_alias = std::filesystem;
  using atx::engine::alpha::kClusterSegmentField;
  using atx::engine::alpha::rename_field_to_cluster;
  const atx::usize dates = 3;
  const atx::usize instruments = 4;

  // Source cluster panel: one snapshot at date 0, clusters {0,0,1,1}.
  const ClusterPanel cp = one_snapshot({0, 0, 1, 1}, 2, instruments);
  const std::vector<atx::f64> cluster_col = broadcast_cluster_field(cp, dates);

  // Build a sealed segment carrying close + the baked cluster field (short tag).
  const std::vector<std::string> fnames{"close", std::string{kClusterSegmentField}};
  const std::vector<std::string> syms{"AAA", "BBB", "CCC", "DDD"};
  std::vector<atx::i64> axis(dates);
  for (atx::usize d = 0; d < dates; ++d) {
    axis[d] = static_cast<atx::i64>((d + 1) * 100);
  }
  atx::tsdb::SegmentBuilder b(fnames, syms, axis);
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::usize i = 0; i < instruments; ++i) {
      b.set(0, d, static_cast<atx::u32>(i), static_cast<atx::f64>(d * 10 + i)); // close
      const atx::f64 cv = cluster_col[d * instruments + i];
      // Only set present cells; a NaN cluster cell stays absent (present bit clear)
      // — but every cell here is clustered, so all are written.
      if (!std::isnan(cv)) {
        b.set(1, d, static_cast<atx::u32>(i), cv);
      }
    }
  }

  const std::string dir = temp_dir("seg_cluster");
  std::error_code ec;
  fs_alias::create_directories(dir, ec);
  ASSERT_FALSE(ec) << ec.message();
  const std::string seg_path = dir + "/2026-01-01.seg";
  ASSERT_TRUE(b.write(seg_path, 0).has_value());

  // Attach: select close + the baked cluster tag, then rename to IndClass.cluster.
  const std::vector<std::string> sel{"close", std::string{kClusterSegmentField}};
  auto attached = atx::engine::alpha::attach_multi_segment_panel(
      dir, atx::engine::alpha::TimeWindow{}, sel);
  ASSERT_TRUE(attached.has_value()) << (attached ? "" : attached.error().message());
  auto renamed = rename_field_to_cluster(attached.value());
  ASSERT_TRUE(renamed.has_value()) << (renamed ? "" : renamed.error().message());
  const Panel &ap = renamed.value();
  ASSERT_EQ(ap.dates(), dates);
  ASSERT_EQ(ap.instruments(), instruments);

  const auto cid = ap.field_id(kClusterFieldName);
  ASSERT_TRUE(cid.has_value());
  for (atx::usize d = 0; d < dates; ++d) {
    const std::span<const atx::f64> xs = ap.field_cross_section(cid.value(), d);
    for (atx::usize i = 0; i < instruments; ++i) {
      EXPECT_DOUBLE_EQ(xs[i], cluster_col[d * instruments + i])
          << "segment-baked cluster cell (" << d << "," << i << ") must match the source";
    }
  }

  // And the attached panel drives a group op end-to-end (VM == oracle).
  expect_vm_matches_oracle("group_neutralize(close, IndClass.cluster)", ap);

  // Cleanup.
  std::remove(seg_path.c_str());
  fs_alias::remove_all(dir, ec);
}

// ===========================================================================
//  Online group_map -> WeightPolicy::to_target_weights.
// ===========================================================================

// A planted 2-cluster book neutralized by to_target_weights using the per-date
// cluster group_map: each cluster's weights sum to ~0 (the policy's group-
// neutral invariant), and the gross holds at the configured leverage.
TEST(ClusterWiring, OnlineClusterGroupMap_NeutralizesPerCluster) {
  using atx::engine::InstrumentId;
  using atx::engine::SignalView;
  using atx::engine::Universe;
  using atx::engine::WeightPolicy;

  const atx::usize instruments = 6;
  // Clusters {0,0,0,1,1,1} held from date 0.
  const ClusterPanel cp = one_snapshot({0, 0, 0, 1, 1, 1}, 2, instruments);
  const std::vector<atx::u32> gmap = cluster_group_map(cp, /*date=*/0);
  ASSERT_EQ(gmap.size(), instruments);
  // Real cluster ids preserved (0 / 1); no instrument is unclustered here.
  const std::array<atx::u32, 6> expect_groups{0, 0, 0, 1, 1, 1};
  for (atx::usize i = 0; i < instruments; ++i) {
    EXPECT_EQ(gmap[i], expect_groups[i]);
  }

  const std::array<InstrumentId, 6> u{InstrumentId{1}, InstrumentId{2}, InstrumentId{3},
                                      InstrumentId{4}, InstrumentId{5}, InstrumentId{6}};
  const std::array<atx::f64, 6> sig{1.0, 5.0, 2.0, 9.0, 4.0, 7.0};

  WeightPolicy policy{};
  policy.industry_neutral = true; // cluster-neutralize via the group_map
  const auto w =
      policy.to_target_weights(SignalView{std::span<const atx::f64>{sig}}, Universe{u},
                               std::span<const atx::u32>{gmap});
  ASSERT_EQ(w.size(), instruments);

  // Cluster-neutral invariant: each cluster's weights sum to ~0.
  atx::f64 c0 = 0.0;
  atx::f64 c1 = 0.0;
  atx::f64 gross = 0.0;
  for (atx::usize i = 0; i < instruments; ++i) {
    (gmap[i] == 0U ? c0 : c1) += w[i];
    gross += std::fabs(w[i]);
  }
  EXPECT_NEAR(c0, 0.0, 1e-9);
  EXPECT_NEAR(c1, 0.0, 1e-9);
  EXPECT_NEAR(gross, 1.0, 1e-9); // gross-normalized to the default leverage
}

// An unclustered instrument is mapped to a disjoint SINGLETON group (id >= the
// real-id space), so its per-group demean drives it to ~0 without contaminating a
// real cluster's mean.
TEST(ClusterWiring, OnlineGroupMap_UnclusteredIsSingleton) {
  const atx::usize instruments = 4;
  // Instrument 3 is unclustered.
  const ClusterPanel cp = one_snapshot({0, 0, 1, ClusterPanel::kUnclustered}, 2, instruments);
  const std::vector<atx::u32> gmap = cluster_group_map(cp, 0);
  ASSERT_EQ(gmap.size(), instruments);
  EXPECT_EQ(gmap[0], 0U);
  EXPECT_EQ(gmap[1], 0U);
  EXPECT_EQ(gmap[2], 1U);
  // Singleton id is >= instruments (disjoint from real ids 0/1), unique per inst.
  EXPECT_GE(gmap[3], static_cast<atx::u32>(instruments));
  EXPECT_NE(gmap[3], gmap[2]);

  // Before the first snapshot every instrument is its own singleton.
  ClusterPanel late;
  late.instruments = instruments;
  ClusterPanel::Snapshot s;
  s.date = 5;
  s.cluster_id = {0, 0, 1, 1};
  s.n_labels = 2;
  late.snapshots.push_back(std::move(s));
  const std::vector<atx::u32> pre = cluster_group_map(late, /*date=*/0); // before date 5
  for (atx::usize i = 0; i < instruments; ++i) {
    EXPECT_GE(pre[i], static_cast<atx::u32>(instruments)); // all singletons (unclustered)
  }
}

// ===========================================================================
//  Two-runs-equal + pinned digest of the broadcast column.
// ===========================================================================

// Helper: a deterministic 2-snapshot panel used by the digest tests.
[[nodiscard]] ClusterPanel digest_fixture(atx::usize instruments) {
  ClusterPanel cp;
  cp.instruments = instruments;
  ClusterPanel::Snapshot a;
  a.date = 1;
  a.cluster_id = {0, 1, 2, 0, 1, ClusterPanel::kUnclustered};
  a.n_labels = 3;
  ClusterPanel::Snapshot b;
  b.date = 3;
  b.cluster_id = {2, 2, 0, 1, ClusterPanel::kUnclustered, 1};
  b.n_labels = 3;
  cp.snapshots.push_back(std::move(a));
  cp.snapshots.push_back(std::move(b));
  return cp;
}

TEST(ClusterWiring, BroadcastTwoRunsEqual_AndPinnedDigest) {
  const atx::usize instruments = 6;
  const atx::usize dates = 5;
  const ClusterPanel cp = digest_fixture(instruments);

  const std::vector<atx::f64> a = broadcast_cluster_field(cp, dates);
  const std::vector<atx::f64> b = broadcast_cluster_field(cp, dates);
  ASSERT_EQ(a.size(), b.size());
  for (atx::usize i = 0; i < a.size(); ++i) {
    EXPECT_TRUE(same_cell(a[i], b[i])) << "two-runs broadcast must be bit-identical at cell " << i;
  }

  // Pinned FNV-1a-64 digest of the broadcast column (NaN normalized to canonical
  // quiet-NaN). Regenerated below if the broadcast layout ever changes legitimately.
  const atx::u64 digest = fnv1a64(a);
  EXPECT_EQ(digest, 5678576743557081587ULL) << "broadcast column digest drifted; got " << digest;
}


}  // namespace atxtest_cluster_wiring_test
