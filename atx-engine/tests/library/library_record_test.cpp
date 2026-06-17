// library_record_test.cpp — S4-1: on-disk segment record schema (record.hpp).
//
// The dominant Sprint-4 risk is SILENT DATA CORRUPTION — a store that
// round-trips WRONG. record.hpp is the load-bearing wire contract: the POD
// SegmentHeader / AlphaDirEntry / SegmentFooter framing, the variable-length
// Provenance (de)serialization, and the validate-magic/version/seal/crc-BEFORE-
// exposing-any-byte attach discipline cloned from atx::tsdb's segment format.
//
// Coverage (plan §4.1):
//   * SegmentHeader round-trips byte-for-byte through write_header/read_header,
//     little-endian, with the tag8 magic + a supported format_version.
//   * Provenance (expr_source string + parent_hashes u64 vector + scalars)
//     round-trips through serialize/deserialize_provenance.
//   * A sealed in-memory segment with a corrupted magic byte is REJECTED by
//     attach_bytes; a sealed segment with a corrupted data byte is REJECTED on
//     the integrity-crc mismatch (validate-before-expose).

#include <array>
#include <cstring> // std::memcpy (fixture byte assembly)
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp" // u32, u64, f64, byte

#include "atx/engine/combine/metrics.hpp"     // combine::AlphaMetrics
#include "atx/engine/library/record.hpp"      // the unit under test

namespace atxtest_library_record_test {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::engine::combine::AlphaMetrics;

namespace lib = atx::engine::library;

// A small, fully-specified fixture describing one sealed segment: 3 alphas,
// N=4 instruments, T=8 periods, base AlphaId 100. Enough to exercise the dir +
// pnl + pos + provenance blob sections without being unwieldy.
struct SmallFixture {
  u32 n_alphas{3};
  u32 n_instruments{4};
  u64 n_periods{8};
  u64 base_alpha_id{100};
  std::vector<f64> pnl;                       // [n_alphas * n_periods], alpha-major
  std::vector<f64> pos;                       // [n_alphas * n_periods * n_instruments]
  std::vector<AlphaMetrics> metrics;          // one per alpha
  std::vector<u64> canon_hashes;              // one per alpha
  std::vector<lib::Provenance> provenance;    // one per alpha
};

[[nodiscard]] SmallFixture small_fixture() {
  SmallFixture fx;
  const usize na = fx.n_alphas;
  const usize np = static_cast<usize>(fx.n_periods);
  const usize ni = fx.n_instruments;
  fx.pnl.resize(na * np);
  fx.pos.resize(na * np * ni);
  for (usize a = 0; a < na; ++a) {
    for (usize t = 0; t < np; ++t) {
      fx.pnl[a * np + t] = static_cast<f64>(a) * 100.0 + static_cast<f64>(t);
      for (usize j = 0; j < ni; ++j) {
        fx.pos[(a * np + t) * ni + j] =
            static_cast<f64>(a) + 0.1 * static_cast<f64>(t) + 0.01 * static_cast<f64>(j);
      }
    }
    fx.metrics.push_back(AlphaMetrics{/*sharpe*/ 1.0 + static_cast<f64>(a),
                                      /*turnover*/ 0.2, /*returns*/ 0.3, /*drawdown*/ 0.4,
                                      /*margin*/ 0.5, /*fitness*/ 9.0 + static_cast<f64>(a),
                                      /*holding_days*/ 7.0});
    fx.canon_hashes.push_back(0xCAFEBABE00000000ull + a);
    fx.provenance.push_back(lib::Provenance{
        /*expr_source*/ "rank(close)",
        /*parent_hashes*/ std::vector<u64>{0x11ull + a, 0x22ull + a},
        /*mutation_op*/ static_cast<atx::u16>(a),
        /*seed*/ 1000ull + a});
  }
  return fx;
}

// Assemble a fully-sealed library segment in memory from the fixture, reusing
// the production one-pass writer in record.hpp. Returns the raw bytes.
[[nodiscard]] std::vector<std::byte> make_sealed_segment_bytes(const SmallFixture &fx) {
  return lib::write_segment_bytes(fx.n_alphas, fx.n_instruments, fx.n_periods, fx.base_alpha_id,
                                  fx.pnl, fx.pos, fx.metrics, fx.canon_hashes, fx.provenance);
}

[[nodiscard]] lib::SegmentHeader make_header(u32 n_alphas, u32 n_inst, u64 n_periods, u64 base) {
  return lib::make_header(n_alphas, n_inst, n_periods, base);
}

void write_header(std::array<std::byte, sizeof(lib::SegmentHeader)> &buf,
                  const lib::SegmentHeader &h) {
  std::memcpy(buf.data(), &h, sizeof(h));
}

[[nodiscard]] lib::SegmentHeader read_header(const std::array<std::byte, sizeof(lib::SegmentHeader)> &buf) {
  lib::SegmentHeader h{};
  std::memcpy(&h, buf.data(), sizeof(h));
  return h;
}

TEST(LibraryRecord, HeaderRoundTripsLittleEndian) {
  const lib::SegmentHeader h = make_header(/*n_alphas*/ 3, /*N*/ 4, /*T*/ 8, /*base*/ 100);
  std::array<std::byte, sizeof(lib::SegmentHeader)> buf{};
  write_header(buf, h);
  const lib::SegmentHeader g = read_header(buf);
  EXPECT_EQ(g.magic, lib::kLibMagic);
  EXPECT_EQ(g.base_alpha_id, 100u);
  EXPECT_EQ(g.n_alphas, 3u);
  EXPECT_EQ(g.n_instruments, 4u);
  EXPECT_EQ(g.n_periods, 8u);
  EXPECT_TRUE(lib::is_supported_version(g.format_version));
}

TEST(LibraryRecord, ProvenanceRoundTrips) {
  const lib::Provenance p{"add(rank(close), ts_mean(volume, 10))",
                          {0xABCDull, 0x1234ull},
                          /*op*/ 2,
                          /*seed*/ 777};
  std::vector<std::byte> b;
  lib::serialize(p, b);
  const lib::Provenance q = lib::deserialize_provenance(b);
  EXPECT_EQ(q.expr_source, p.expr_source);
  EXPECT_EQ(q.parent_hashes, p.parent_hashes);
  EXPECT_EQ(q.mutation_op, p.mutation_op);
  EXPECT_EQ(q.seed, 777u);
}

TEST(LibraryRecord, ProvenanceRoundTripsEmpty) {
  // A degenerate provenance (no expression, no parents) must still round-trip.
  const lib::Provenance p{"", {}, /*op*/ 0, /*seed*/ 0};
  std::vector<std::byte> b;
  lib::serialize(p, b);
  const lib::Provenance q = lib::deserialize_provenance(b);
  EXPECT_TRUE(q.expr_source.empty());
  EXPECT_TRUE(q.parent_hashes.empty());
  EXPECT_EQ(q.seed, 0u);
}

TEST(LibraryRecord, RejectsBadMagicAndCrc) {
  auto buf = make_sealed_segment_bytes(small_fixture());
  ASSERT_FALSE(buf.empty());
  // A pristine sealed segment attaches cleanly.
  ASSERT_TRUE(lib::SegmentReaderLite::attach_bytes(buf).has_value());

  // Corrupt the magic cookie (byte 0) — attach must reject before exposing.
  auto bad_magic = make_sealed_segment_bytes(small_fixture());
  bad_magic[0] ^= std::byte{0xFF};
  EXPECT_FALSE(lib::SegmentReaderLite::attach_bytes(bad_magic).has_value());

  // Corrupt a data byte near the tail (inside the integrity-crc'd region) —
  // attach must reject on the crc mismatch.
  auto bad_crc = make_sealed_segment_bytes(small_fixture());
  bad_crc[bad_crc.size() - 8] ^= std::byte{0x01};
  EXPECT_FALSE(lib::SegmentReaderLite::attach_bytes(bad_crc).has_value());
}

TEST(LibraryRecord, AttachExposesRowsAndDir) {
  const SmallFixture fx = small_fixture();
  auto buf = make_sealed_segment_bytes(fx);
  auto reader = lib::SegmentReaderLite::attach_bytes(buf);
  ASSERT_TRUE(reader.has_value());

  EXPECT_EQ(reader->n_alphas(), fx.n_alphas);
  EXPECT_EQ(reader->n_instruments(), fx.n_instruments);
  EXPECT_EQ(reader->n_periods(), fx.n_periods);
  EXPECT_EQ(reader->base_alpha_id(), fx.base_alpha_id);

  // PnL row 2 reads back the fixture values.
  const std::span<const f64> row2 = reader->pnl_row(2);
  ASSERT_EQ(row2.size(), fx.n_periods);
  EXPECT_DOUBLE_EQ(row2[0], 200.0);
  EXPECT_DOUBLE_EQ(row2[7], 207.0);

  // Position cross-section (alpha 1, period 3) reads back the fixture values.
  const std::span<const f64> cs = reader->pos_row(1, 3);
  ASSERT_EQ(cs.size(), fx.n_instruments);
  EXPECT_DOUBLE_EQ(cs[0], 1.0 + 0.1 * 3.0);
  EXPECT_DOUBLE_EQ(cs[3], 1.0 + 0.1 * 3.0 + 0.03);

  // Directory entry carries the global alpha id, canon hash, and metrics.
  const lib::AlphaDirEntry &e1 = reader->dir_entry(1);
  EXPECT_EQ(e1.alpha_id, fx.base_alpha_id + 1);
  EXPECT_EQ(e1.canon_hash, fx.canon_hashes[1]);
  EXPECT_DOUBLE_EQ(e1.metrics.fitness, fx.metrics[1].fitness);

  // Provenance round-trips through the blob slice.
  const lib::Provenance pr = reader->provenance(1);
  EXPECT_EQ(pr.expr_source, fx.provenance[1].expr_source);
  EXPECT_EQ(pr.parent_hashes, fx.provenance[1].parent_hashes);
  EXPECT_EQ(pr.seed, fx.provenance[1].seed);
}


}  // namespace atxtest_library_record_test
