// library_dedup_index_test.cpp — S4-2: DedupIndex (library-wide canonical-hash dedup).
//
// DedupIndex is the cross-run, cross-segment dedup gate keyed by the S3
// factory::canonical_hash. It is the persistent superset of factory::CanonSet:
// an in-memory HashMap<canon_hash, AlphaId> fronting a sqlite table so the dedup
// set survives a process restart. insert() returns true iff the hash was newly
// admitted (false ⇒ a structural duplicate the caller must skip).
//
// These are the soundness PROOFS against the dominant Sprint-4 risk (a dedup
// that DROPS a distinct alpha (false dup) or fails to reject a true dup):
//   * RejectsStructurallyEquivalentResubmission (NON-VACUOUS): an Add-operand
//     reorder is canon-equal (S3 commutative pass), so the resubmission is
//     rejected library-wide — and the test first ASSERTs the two hashes really
//     are equal, so the rejection is not vacuously passing on distinct keys.
//   * AdmitsGenuinelyNew: two distinct windows (ts_mean(close,5) vs (close,6))
//     hash differently and are BOTH admitted — no false dup.
//   * PersistsAcrossReopen: a hash inserted into one DedupIndex is still present
//     after the object is destroyed and a fresh one is opened on the SAME dir
//     (sqlite-backed; cache rebuilt from the table).
//   * Persists64BitHashFidelity: a high-bit-set hash round-trips through the
//     sqlite INTEGER (i64) column without losing the top bit.

#include <filesystem> // per-test temp directory
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "atx/core/types.hpp" // u64

#include "atx/engine/alpha/parser.hpp"   // parse_expr, Library
#include "atx/engine/alpha/registry.hpp" // OpSig, OpCode, DType, shape_elementwise
#include "atx/engine/alpha/typecheck.hpp" // analyze

#include "atx/engine/combine/store.hpp"     // combine::AlphaId
#include "atx/engine/factory/canonical.hpp" // factory::canonical_hash
#include "atx/engine/factory/genome.hpp"    // factory::Genome
#include "atx/engine/library/dedup_index.hpp" // the unit under test

namespace {

using atx::u64;
using atx::engine::alpha::analyze;
using atx::engine::alpha::DType;
using atx::engine::alpha::ExprId;
using atx::engine::alpha::Library;
using atx::engine::alpha::OpCode;
using atx::engine::alpha::OpSig;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::shape_elementwise;
using atx::engine::combine::AlphaId;
using atx::engine::factory::canonical_hash;
using atx::engine::factory::Genome;
using atx::engine::library::DedupIndex;

// A per-test unique temp directory under the OS temp dir. remove_all on each call
// makes it fresh, so a test that REOPENS the same library must capture the dir in
// a variable (do NOT call tmpdir() twice for the same library) — mirrors the
// library_store_test.cpp helper.
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

// Test-only op aliases so the plan's call syntax (add/sub/...) resolves through
// the parser. Mirrors factory_canonical_test.cpp's register_aliases EXACTLY.
inline void register_aliases(Library &lib) {
  const OpSig add{"add", 2, 2, OpCode::Add, DType::F64, true, {}, &shape_elementwise};
  const OpSig sub{"sub", 2, 2, OpCode::Sub, DType::F64, true, {}, &shape_elementwise};
  static_cast<void>(lib.register_op(add));
  static_cast<void>(lib.register_op(sub));
}

// Expression source -> canonical hash, via the SAME parse path the factory
// canonical test uses: register aliases, parse_expr -> analyze -> Genome, then
// canonical_hash over the genome's single root.
[[nodiscard]] u64 hash_of(std::string_view src, Library &lib) {
  register_aliases(lib);
  auto parsed = parse_expr(src, lib);
  EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  if (!parsed) {
    return 0;
  }
  auto info = analyze(*parsed);
  EXPECT_TRUE(info.has_value()) << (info ? "" : info.error().message());
  if (!info) {
    return 0;
  }
  Genome g{std::move(*parsed), std::move(*info), 0};
  return canonical_hash(g);
}

// ---- tests ------------------------------------------------------------------

TEST(LibraryDedup, RejectsStructurallyEquivalentResubmission) { // non-vacuous
  Library lib;
  const u64 h = hash_of("add(rank(close), ts_mean(volume, 10))", lib);
  const u64 h2 = hash_of("add(ts_mean(volume, 10), rank(close))", lib); // commutative reorder
  ASSERT_EQ(h, h2); // S3 canonicalization (Add is hash-commutative) — premise check
  ASSERT_NE(h, 0U); // the hash path actually produced a key

  DedupIndex idx(tmpdir());
  auto first = idx.insert(h, AlphaId{0});
  ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().message());
  EXPECT_TRUE(first.value()); // first sighting: newly inserted

  auto dup = idx.insert(h2, AlphaId{1});
  ASSERT_TRUE(dup.has_value()) << (dup ? "" : dup.error().message());
  EXPECT_FALSE(dup.value()); // commutative dup: rejected library-wide

  EXPECT_TRUE(idx.contains(h));
  ASSERT_TRUE(idx.find(h).has_value());
  EXPECT_EQ(idx.find(h)->value, AlphaId{0}.value); // the FIRST id wins
}

TEST(LibraryDedup, AdmitsGenuinelyNew) {
  Library lib;
  const u64 a = hash_of("ts_mean(close, 5)", lib);
  const u64 b = hash_of("ts_mean(close, 6)", lib);
  ASSERT_NE(a, b); // distinct windows must hash differently

  DedupIndex idx(tmpdir());
  auto ra = idx.insert(a, AlphaId{0});
  auto rb = idx.insert(b, AlphaId{1});
  ASSERT_TRUE(ra.has_value());
  ASSERT_TRUE(rb.has_value());
  EXPECT_TRUE(ra.value());
  EXPECT_TRUE(rb.value());
}

TEST(LibraryDedup, PersistsAcrossReopen) { // survives process restart
  const std::string dir = tmpdir("rt");    // ONE dir for both opens
  {
    DedupIndex idx(dir);
    auto r = idx.insert(0xDEADBEEFull, AlphaId{0});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r.value());
  }
  DedupIndex reopened(dir);
  EXPECT_TRUE(reopened.contains(0xDEADBEEFull)); // sqlite-backed, cache rebuilt
  ASSERT_TRUE(reopened.find(0xDEADBEEFull).has_value());
  EXPECT_EQ(reopened.find(0xDEADBEEFull)->value, AlphaId{0}.value);
  // A reopened-and-already-present hash must still be rejected as a dup.
  auto again = reopened.insert(0xDEADBEEFull, AlphaId{99});
  ASSERT_TRUE(again.has_value());
  EXPECT_FALSE(again.value());
}

TEST(LibraryDedup, Persists64BitHashFidelity) { // guards the i64 round-trip
  const std::string dir = tmpdir("hb");
  constexpr u64 kHighBit = 0xF0F0F0F0F0F0F0F0ull; // top bit set
  {
    DedupIndex idx(dir);
    auto r = idx.insert(kHighBit, AlphaId{7});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r.value());
  }
  DedupIndex reopened(dir);
  EXPECT_TRUE(reopened.contains(kHighBit)); // all 64 bits survived INTEGER round-trip
  ASSERT_TRUE(reopened.find(kHighBit).has_value());
  EXPECT_EQ(reopened.find(kHighBit)->value, AlphaId{7}.value);
  // A near-miss key that differs only in low bits must NOT be a false hit.
  EXPECT_FALSE(reopened.contains(kHighBit ^ 0x1ull));
}

} // namespace
