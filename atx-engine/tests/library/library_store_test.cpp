// library_store_test.cpp — S4-1: LibraryStore (append-only segmented store).
//
// LibraryStore SCALES combine::AlphaStore into a disk-backed, append-only,
// immutable-segment store: stage() buffers into an in-memory memtable, flush()
// seals the memtable into a one-pass mmap'able segment (record.hpp framing) and
// records it in the sqlite segment catalog; reads dispatch over the UNION of
// sealed segments + the live memtable by global AlphaId.
//
// These are the round-trip / survivorship / determinism PROOFS against the
// dominant Sprint-4 risk (silent data corruption):
//   * FlushedSegmentRoundTripsByteIdenticalToAlphaStore (LOAD-BEARING): a flushed
//     + reopened-from-disk segment reads back BIT-IDENTICAL to the in-memory
//     AlphaStore — including NaN cells and a delisted/final-value column tail.
//   * ReadsAcrossMultipleSegments: a global AlphaId spanning two sealed segments
//     resolves to the right row.
//   * TwoBuildsByteIdentical: two builds of the same fixture produce the same
//     segment integrity crc (determinism).
//   * SealedSegmentImmutableAfterMoreAdmits: a sealed segment file is byte-frozen
//     across later admits + flushes (append-only).

#include <bit>        // std::bit_cast (bit-identical f64 comparison)
#include <cstdint>    // std::uint64_t
#include <filesystem> // per-test temp directory
#include <fstream>    // raw segment-file byte read (immutability proof)
#include <limits>  // quiet_NaN
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp" // f64, u32, u64, usize

#include "atx/engine/combine/metrics.hpp" // combine::AlphaMetrics
#include "atx/engine/combine/store.hpp"   // combine::AlphaStore, AlphaId
#include "atx/engine/library/record.hpp"  // Provenance
#include "atx/engine/library/store.hpp"   // the unit under test

namespace atxtest_library_store_test {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::engine::combine::AlphaId;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::AlphaStore;

namespace lib = atx::engine::library;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// A per-test unique temp directory under the OS temp dir. Unique by test name +
// a static counter so two tmpdir() calls in one test (the round-trip reopen)
// must pass the SAME name to address the SAME library.
[[nodiscard]] std::string tmpdir(const std::string &tag = "") {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->test_suite_name() : "S4") + "_" +
                     std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_s4_lib" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec); // fresh per construction
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

[[nodiscard]] AlphaMetrics sample_metrics(f64 base) {
  return AlphaMetrics{/*sharpe*/ base + 0.1,  /*turnover*/ base + 0.2, /*returns*/ base + 0.3,
                      /*drawdown*/ base + 0.4, /*margin*/ base + 0.5,   /*fitness*/ base + 0.6,
                      /*holding_days*/ base + 0.7};
}

[[nodiscard]] lib::Provenance sample_prov(usize a) {
  return lib::Provenance{"rank(close) - ts_mean(volume, 5)",
                         std::vector<u64>{0xA0ull + a, 0xB0ull + a},
                         /*op*/ static_cast<atx::u16>(a % 3),
                         /*seed*/ 5000ull + a};
}

// A fixture exercising survivorship + NaN: T=6, N=3, plus a deliberate NaN cell
// in the PnL stream and a "delisted/final-value" instrument column whose tail
// goes NaN (a column that stops updating part-way through the window).
struct NanFixture {
  u32 n_alphas{2};
  u32 n_instruments{3};
  u64 n_periods{6};
  std::vector<std::vector<f64>> pnl;     // per-alpha PnL
  std::vector<std::vector<f64>> pos;     // per-alpha flat [T*N]
  std::vector<AlphaMetrics> metrics;
  std::vector<lib::Provenance> provenance;
};

[[nodiscard]] NanFixture fixture_with_nan_and_delisted_cols() {
  NanFixture fx;
  const usize np = static_cast<usize>(fx.n_periods);
  const usize ni = fx.n_instruments;
  for (usize a = 0; a < fx.n_alphas; ++a) {
    std::vector<f64> pnl(np);
    for (usize t = 0; t < np; ++t) {
      pnl[t] = static_cast<f64>(a) * 10.0 + static_cast<f64>(t);
    }
    pnl[0] = kNaN; // pre-first-valid period is NaN (stored verbatim)
    std::vector<f64> pos(np * ni);
    for (usize t = 0; t < np; ++t) {
      for (usize j = 0; j < ni; ++j) {
        f64 v = static_cast<f64>(a) + 0.1 * static_cast<f64>(t) + 0.01 * static_cast<f64>(j);
        // Instrument column 2 is "delisted" after period 2: its tail is NaN
        // (a survivorship-bias-relevant final-value/NaN-tail column).
        if (j == 2 && t >= 3) {
          v = kNaN;
        }
        pos[t * ni + j] = v;
      }
    }
    fx.pnl.push_back(std::move(pnl));
    fx.pos.push_back(std::move(pos));
    fx.metrics.push_back(sample_metrics(static_cast<f64>(a)));
    fx.provenance.push_back(sample_prov(a));
  }
  return fx;
}

void fill_store(AlphaStore &mem, const NanFixture &fx) {
  for (usize a = 0; a < fx.n_alphas; ++a) {
    ASSERT_TRUE(mem.insert(nullptr, fx.pnl[a], fx.pos[a], fx.metrics[a]).has_value());
  }
}

void stage_all(lib::LibraryStore &store, const AlphaStore &mem, const NanFixture &fx) {
  for (usize a = 0; a < mem.n_alphas(); ++a) {
    std::vector<f64> pos_flat(fx.pos[a]);
    std::vector<f64> pnl(fx.pnl[a]);
    ASSERT_TRUE(store.stage(nullptr, pnl, pos_flat, fx.metrics[a], fx.provenance[a]).has_value());
  }
}

// Stage `n` trivial alphas (T=4, N=2) into a store (no flush).
void stage_n(lib::LibraryStore &store, usize n) {
  const std::vector<f64> pnl{0.0, 1.0, 2.0, 3.0};
  const std::vector<f64> pos(8, 0.5); // 4 periods x 2 inst
  for (usize a = 0; a < n; ++a) {
    ASSERT_TRUE(store.stage(nullptr, pnl, pos, sample_metrics(static_cast<f64>(a)),
                            sample_prov(a))
                    .has_value());
  }
}

[[nodiscard]] std::vector<char> read_file_bytes(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
}

// Build a library in `dir` from `n` staged alphas, flush, and return the
// integrity crc of the single sealed segment (the determinism witness).
[[nodiscard]] u32 build_and_seal(const std::string &dir, usize n) {
  lib::LibraryStore store(dir);
  stage_n(store, n);
  EXPECT_TRUE(store.flush().has_value());
  auto reader = lib::SegmentReaderLite::attach(store.segment_path(0));
  EXPECT_TRUE(reader.has_value());
  return reader->integrity_crc();
}

TEST(LibraryStore, FlushedSegmentRoundTripsByteIdenticalToAlphaStore) { // LOAD-BEARING
  const std::string dir = tmpdir("rt");
  AlphaStore mem;
  const NanFixture fx = fixture_with_nan_and_delisted_cols();
  fill_store(mem, fx);

  {
    lib::LibraryStore lib_store(dir);
    stage_all(lib_store, mem, fx);
    ASSERT_TRUE(lib_store.flush().has_value());
  } // close: drop the writer so the reopen maps fresh from disk

  lib::LibraryStore reopened(dir); // attach from disk (mmap-ro)
  ASSERT_EQ(reopened.n_alphas(), mem.n_alphas());
  ASSERT_EQ(reopened.n_periods(), mem.n_periods());
  ASSERT_EQ(reopened.n_instruments(), mem.n_instruments());

  for (AlphaId id{0}; id.value < mem.n_alphas(); ++id.value) {
    const std::span<const f64> a = mem.pnl(id);
    const std::span<const f64> b = reopened.pnl(id);
    ASSERT_EQ(a.size(), b.size());
    for (usize i = 0; i < a.size(); ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(a[i]), std::bit_cast<std::uint64_t>(b[i]))
          << "pnl bit mismatch at alpha " << id.value << " period " << i;
    }
    // Positions bit-identical too (incl. the delisted NaN-tail column).
    for (usize t = 0; t < mem.n_periods(); ++t) {
      const std::span<const f64> pa = mem.positions(id, t);
      const std::span<const f64> pb = reopened.positions(id, t);
      ASSERT_EQ(pa.size(), pb.size());
      for (usize j = 0; j < pa.size(); ++j) {
        EXPECT_EQ(std::bit_cast<std::uint64_t>(pa[j]), std::bit_cast<std::uint64_t>(pb[j]))
            << "pos bit mismatch at alpha " << id.value << " period " << t << " inst " << j;
      }
    }
    EXPECT_EQ(reopened.get(id).metrics.fitness, mem.get(id).metrics.fitness);
  }
}

TEST(LibraryStore, ReadsAcrossMultipleSegments) {
  const std::string dir = tmpdir("multi");
  lib::LibraryStore store(dir);
  stage_n(store, 5);
  ASSERT_TRUE(store.flush().has_value()); // segment 0: ids 0..4
  stage_n(store, 7);
  ASSERT_TRUE(store.flush().has_value()); // segment 1: ids 5..11

  EXPECT_EQ(store.n_alphas(), 12u);
  EXPECT_EQ(store.pnl(AlphaId{11}).size(), store.n_periods()); // global id spans segments
  // id 11 lives in segment 1, local row 6: pnl row is {0,1,2,3}.
  const std::span<const f64> r11 = store.pnl(AlphaId{11});
  ASSERT_EQ(r11.size(), 4u);
  EXPECT_DOUBLE_EQ(r11[3], 3.0);
  // id 4 lives in segment 0, local row 4.
  const std::span<const f64> r4 = store.pnl(AlphaId{4});
  ASSERT_EQ(r4.size(), 4u);
  EXPECT_DOUBLE_EQ(r4[2], 2.0);
}

TEST(LibraryStore, ReadsLiveMemtableBeforeFlush) {
  const std::string dir = tmpdir("live");
  lib::LibraryStore store(dir);
  stage_n(store, 3); // staged, NOT flushed
  EXPECT_EQ(store.n_alphas(), 3u);
  const std::span<const f64> r2 = store.pnl(AlphaId{2});
  ASSERT_EQ(r2.size(), 4u);
  EXPECT_DOUBLE_EQ(r2[1], 1.0);
}

TEST(LibraryStore, TwoBuildsByteIdentical) { // determinism
  const u32 crc1 = build_and_seal(tmpdir("a"), 4);
  const u32 crc2 = build_and_seal(tmpdir("b"), 4);
  EXPECT_EQ(crc1, crc2);
}

TEST(LibraryStore, SealedSegmentImmutableAfterMoreAdmits) { // append-only
  const std::string dir = tmpdir("immut");
  lib::LibraryStore store(dir);
  stage_n(store, 3);
  ASSERT_TRUE(store.flush().has_value());
  const std::vector<char> before = read_file_bytes(store.segment_path(0));
  ASSERT_FALSE(before.empty());

  stage_n(store, 3);
  ASSERT_TRUE(store.flush().has_value());
  const std::vector<char> after = read_file_bytes(store.segment_path(0));
  EXPECT_EQ(before, after); // segment 0 is byte-frozen after seal
}

TEST(LibraryStore, FlushEmptyMemtableIsNoOp) {
  const std::string dir = tmpdir("empty");
  lib::LibraryStore store(dir);
  ASSERT_TRUE(store.flush().has_value()); // nothing staged: Ok, no segment
  EXPECT_EQ(store.n_alphas(), 0u);
  EXPECT_EQ(store.n_segments(), 0u);
}


}  // namespace atxtest_library_store_test
