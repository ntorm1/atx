// library_manifest_test.cpp — S4-5: LibraryManifest (content-addressed versioned snapshot).
//
// LibraryManifest is the content-addressed, deterministic snapshot of the whole
// library: an alpha_id-ordered list of {alpha_id, canon_hash, lifecycle_at_snapshot,
// segment_crc} entries + the master seeds, content-addressed by a crc32 over a
// FIXED-byte-layout serialization of (entries ++ seeds) => version_id. Two builds
// of the same library content + seeds produce the SAME version_id and the SAME
// per-segment integrity crcs (the L6/L7 byte-identity invariant); one more alpha
// changes the version_id.
//
// These are the snapshot-byte-identity PROOFS against the dominant Sprint-4 risk
// (a snapshot that is NOT byte-identical => manifest integrity fails silently):
//   * RebuildIsByteIdentical (LOAD-BEARING): two libraries built from the same
//     fixed inputs + seeds snapshot to the same version_id AND the same segment
//     integrity crcs (determinism, L7; rebuild_equals holds).
//   * VersionIdChangesWithContent: one more alpha => a different version_id (the
//     content-address actually depends on the content, not vacuously constant).
//   * WriteReadRoundTrip: a manifest written to a sidecar file reads back with the
//     same version_id + entry count (durable, no field loss).

#include <filesystem> // per-test temp directory
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp" // f64, u32, u64, usize

#include "atx/engine/combine/gate.hpp"     // AlphaGate, GateConfig
#include "atx/engine/combine/metrics.hpp"  // combine::AlphaMetrics, compute_metrics
#include "atx/engine/combine/store.hpp"    // combine::AlphaId
#include "atx/engine/library/library.hpp"  // Library facade (snapshot)
#include "atx/engine/library/manifest.hpp" // the unit under test
#include "atx/engine/library/record.hpp"   // Provenance, SegmentReaderLite

namespace {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::engine::combine::AlphaGate;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::GateConfig;

namespace lib = atx::engine::library;

[[nodiscard]] std::string tmpdir(const std::string &tag = "") {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->test_suite_name() : "S4") + "_" +
                     std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_s4_manifest" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

[[nodiscard]] GateConfig default_gate_cfg() { return GateConfig{}; }

// A deterministic synthetic candidate whose compute_metrics CLEARS the default
// gate floors (sharpe >= 1, fitness >= 1, turnover <= 0.7). `k` seeds a tiny
// distinct drift so each candidate has a unique pnl/hash and the streams are not
// pathologically correlated. T=64, N=1.
struct CandidateData {
  u64 canon_hash;
  std::vector<f64> pnl;
  std::vector<f64> pos_flat; // T*N, N=1
  AlphaMetrics metrics;
  lib::Provenance prov;
  usize as_of;
};

constexpr usize kT = 64;
constexpr usize kN = 1;

// Build a deterministic candidate clearing the default gate. The pnl is a small
// positive mean with low variance (high Sharpe), trades lightly (low turnover),
// so fitness and Sharpe both clear 1.0. A per-candidate phase `k` decorrelates.
[[nodiscard]] CandidateData make_candidate(u64 h, usize k) {
  CandidateData c;
  c.canon_hash = h;
  c.pnl.resize(kT);
  c.pos_flat.assign(kT * kN, 0.0);
  c.pnl[0] = 0.0; // structural zero (period 0)
  for (usize t = 1; t < kT; ++t) {
    // Strong steady positive return + tiny deterministic decorrelating wobble.
    const f64 wobble =
        0.0006 * static_cast<f64>(((t * 2654435761u + k * 40503u) % 7u)) - 0.0018;
    c.pnl[t] = 0.010 + wobble;
    // Light, near-static position so turnover is small (clears max_turnover).
    c.pos_flat[t * kN] = 0.10 + 0.001 * static_cast<f64>((t + k) % 3u);
  }
  c.pos_flat[0] = 0.10;
  c.metrics = atx::engine::combine::compute_metrics(c.pnl, c.pos_flat, kN, /*book*/ 1.0);
  c.prov = lib::Provenance{"synthetic", std::vector<u64>{}, /*op*/ 0, /*seed*/ 1000 + k};
  c.as_of = 1;
  return c;
}

[[nodiscard]] lib::AlphaCandidate to_candidate(const CandidateData &c) {
  return lib::AlphaCandidate{c.canon_hash, c.pnl, c.pos_flat, c.metrics, c.prov, c.as_of, nullptr};
}

// Fixed, deterministic inputs: `n` distinct candidates derived from a base seed.
[[nodiscard]] std::vector<CandidateData> fixed_inputs(usize n) {
  std::vector<CandidateData> out;
  out.reserve(n);
  for (usize k = 0; k < n; ++k) {
    // A distinct synthetic canon hash per candidate (deterministic, collision-free
    // across the small fixture). Not parsed from DSL here — the manifest test only
    // needs distinct stable u64 keys; the integration test exercises the parse path.
    const u64 h = 0x9E3779B97F4A7C15ull * static_cast<u64>(k + 1);
    out.push_back(make_candidate(h, k));
  }
  return out;
}

// One more distinct alpha appended to a copy of `base`.
[[nodiscard]] std::vector<CandidateData> one_more_alpha(std::vector<CandidateData> base) {
  const usize k = base.size();
  const u64 h = 0x9E3779B97F4A7C15ull * static_cast<u64>(k + 100);
  base.push_back(make_candidate(h, k));
  return base;
}

// Build a library in `dir` from `inputs`, admit all (default gate), flush, and
// return the snapshot manifest. The master seed is fixed so the corr index +
// manifest seeds are reproducible (L7).
constexpr u64 kMasterSeed = 4242;

[[nodiscard]] lib::LibraryManifest
build_library(const std::string &dir, const std::vector<CandidateData> &inputs) {
  lib::Library facade = lib::Library::open(dir, default_gate_cfg(), {kMasterSeed});
  const AlphaGate gate{default_gate_cfg()};
  for (const auto &c : inputs) {
    const auto v = facade.admit(to_candidate(c), gate);
    EXPECT_EQ(v.kind, lib::AdmitKind::Accept)
        << "fixture candidate did not clear the default gate (kind="
        << static_cast<int>(v.kind) << ")";
  }
  EXPECT_TRUE(facade.flush_all().has_value());
  return facade.snapshot();
}

// The per-segment integrity crcs of a library dir, in segment-id order.
[[nodiscard]] std::vector<u32> segment_crcs(const std::string &dir) {
  lib::Library re = lib::Library::open(dir, default_gate_cfg(), {kMasterSeed});
  std::vector<u32> crcs;
  for (usize i = 0; i < re.n_segments(); ++i) {
    auto reader = lib::SegmentReaderLite::attach(re.segment_path(i));
    EXPECT_TRUE(reader.has_value());
    if (reader) {
      crcs.push_back(reader->integrity_crc());
    }
  }
  return crcs;
}

// ---- tests ------------------------------------------------------------------

TEST(LibraryManifest, RebuildIsByteIdentical) { // L6/L7
  const auto inputs = fixed_inputs(5);
  const auto dirA = tmpdir("a"); // capture: tmpdir() wipes the dir on re-call
  const auto dirB = tmpdir("b");
  const auto v1 = build_library(dirA, inputs);
  const auto v2 = build_library(dirB, inputs);
  EXPECT_EQ(v1.version_id, v2.version_id);
  EXPECT_FALSE(segment_crcs(dirA).empty()); // sanity: a non-empty library
  EXPECT_NE(v1.version_id, 0u);
  EXPECT_EQ(v1.entries.size(), v2.entries.size());
}

TEST(LibraryManifest, SegmentCrcsByteIdentical) { // the segment-level determinism
  const auto inputs = fixed_inputs(5);
  const auto dirA = tmpdir("a");
  const auto dirB = tmpdir("b");
  (void)build_library(dirA, inputs);
  (void)build_library(dirB, inputs);
  EXPECT_EQ(segment_crcs(dirA), segment_crcs(dirB));
  EXPECT_FALSE(segment_crcs(dirA).empty());
}

TEST(LibraryManifest, VersionIdChangesWithContent) {
  const auto base = fixed_inputs(5);
  const auto v1 = build_library(tmpdir("a"), base);
  const auto v2 = build_library(tmpdir("b"), one_more_alpha(base));
  EXPECT_NE(v1.version_id, v2.version_id);
  EXPECT_EQ(v1.entries.size() + 1u, v2.entries.size());
}

TEST(LibraryManifest, WriteReadRoundTrip) {
  const auto m = build_library(tmpdir("a"), fixed_inputs(5));
  const std::string path = tmpdir("w") + "/manifest.atxman";
  ASSERT_TRUE(lib::write_manifest(m, path).has_value());
  auto r = lib::read_manifest(path);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message());
  EXPECT_EQ(r->version_id, m.version_id);
  EXPECT_EQ(r->entries.size(), m.entries.size());
  EXPECT_EQ(r->master_seeds, m.master_seeds);
  // Spot-check that an entry round-trips field-for-field.
  ASSERT_FALSE(m.entries.empty());
  EXPECT_EQ(r->entries.front().alpha_id, m.entries.front().alpha_id);
  EXPECT_EQ(r->entries.front().canon_hash, m.entries.front().canon_hash);
  EXPECT_EQ(r->entries.front().segment_crc, m.entries.front().segment_crc);
  EXPECT_EQ(r->entries.front().lifecycle_at_snapshot, m.entries.front().lifecycle_at_snapshot);
}

TEST(LibraryManifest, RebuildEqualsHolds) { // rebuild_equals byte-check
  const auto inputs = fixed_inputs(6);
  const auto dirA = tmpdir("a");
  const auto m = build_library(dirA, inputs);
  // Re-derive the segment crcs + version id from the on-disk library and assert
  // they match the snapshot (no drift between snapshot and a fresh reopen).
  EXPECT_TRUE(lib::rebuild_equals(m, dirA, default_gate_cfg(), {kMasterSeed}));
}

} // namespace
