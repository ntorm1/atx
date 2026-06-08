// library_lifecycle_test.cpp — S4-4: LifecycleJournal (PIT append-only lifecycle).
//
// LifecycleJournal is the point-in-time (PIT), append-only state machine that
// drives an alpha's library lifecycle (Candidate → Admitted → Live → Decaying →
// {Live | Dead} → Recycled). Every transition is an INSERT into a sqlite journal
// table — NEVER an UPDATE/DELETE — so a later transition can never retroactively
// relabel an earlier point-in-time state query (the dominant S4-4 risk).
//
// The proofs here:
//   * LegalTransitionsOnly / RejectsForwardSkipAndSelf — the strict LINEAR legal
//     table: only the six adjacent edges are accepted; forward-skips, backward
//     edges, and self-transitions are rejected with Err(InvalidArgument).
//   * DeadToRecycled / DecayingCanRecoverToLive — the two non-trivial edges the
//     S7 spine drives (the GC baton and the Decaying→Live recovery).
//   * TransitionsArePitIrreversible (LOAD-BEARING) — a later, legal transition
//     does NOT change an earlier state_as_of() query: the append-only journal +
//     PIT (as_of_period ≤ t) read make a retroactive relabel impossible.
//   * PersistsAcrossReopen — the journal is durable across a process restart.
//
// NOTE (plan deviation): the plan's TransitionsArePitIrreversible used a direct
// Admitted→Decaying later transition, which is a FORWARD-SKIP and is ILLEGAL
// under the strict linear table (Admitted must pass through Live to reach
// Decaying). It is rewritten here to a legal Admitted→Live later transition,
// which still proves the PIT no-retroactive-relabel property.

#include <filesystem> // per-test temp directory
#include <string>

#include <gtest/gtest.h>

#include "atx/core/types.hpp" // u64

#include "atx/engine/combine/store.hpp"      // combine::AlphaId
#include "atx/engine/library/lifecycle.hpp"  // the unit under test

namespace {

using atx::engine::combine::AlphaId;
using atx::engine::library::LifecycleJournal;
using atx::engine::library::LifecycleState;

// A per-test unique temp directory under the OS temp dir. remove_all on each call
// makes it fresh, so a test that REOPENS the same journal must capture the dir in
// a variable (do NOT call tmpdir() twice for the same journal) — mirrors the
// library_dedup_index_test.cpp helper.
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

// ---- tests ------------------------------------------------------------------

TEST(LibraryLifecycle, LegalTransitionsOnly) {
  LifecycleJournal j(tmpdir());
  EXPECT_TRUE(j.transition(AlphaId{0}, LifecycleState::Admitted, 10).has_value()); // Candidate->Admitted ok
  EXPECT_TRUE(j.transition(AlphaId{0}, LifecycleState::Live, 20).has_value());      // Admitted->Live ok
  EXPECT_FALSE(j.transition(AlphaId{0}, LifecycleState::Candidate, 30).has_value()); // Live->Candidate illegal
}

TEST(LibraryLifecycle, RejectsForwardSkipAndSelf) { // strengthen the table
  LifecycleJournal j(tmpdir());
  EXPECT_TRUE(j.transition(AlphaId{0}, LifecycleState::Admitted, 1).has_value());
  EXPECT_FALSE(j.transition(AlphaId{0}, LifecycleState::Decaying, 2).has_value()); // Admitted->Decaying forward-skip illegal
  EXPECT_FALSE(j.transition(AlphaId{0}, LifecycleState::Admitted, 3).has_value()); // self illegal
}

TEST(LibraryLifecycle, DeadToRecycled) { // S7 baton
  LifecycleJournal j(tmpdir());
  ASSERT_TRUE(j.transition(AlphaId{0}, LifecycleState::Admitted, 1).has_value());
  ASSERT_TRUE(j.transition(AlphaId{0}, LifecycleState::Live, 2).has_value());
  ASSERT_TRUE(j.transition(AlphaId{0}, LifecycleState::Decaying, 3).has_value());
  ASSERT_TRUE(j.transition(AlphaId{0}, LifecycleState::Dead, 4).has_value());
  EXPECT_TRUE(j.transition(AlphaId{0}, LifecycleState::Recycled, 5).has_value());
}

TEST(LibraryLifecycle, DecayingCanRecoverToLive) { // the Decaying->Live edge
  LifecycleJournal j(tmpdir());
  ASSERT_TRUE(j.transition(AlphaId{0}, LifecycleState::Admitted, 1).has_value());
  ASSERT_TRUE(j.transition(AlphaId{0}, LifecycleState::Live, 2).has_value());
  ASSERT_TRUE(j.transition(AlphaId{0}, LifecycleState::Decaying, 3).has_value());
  EXPECT_TRUE(j.transition(AlphaId{0}, LifecycleState::Live, 4).has_value());
}

TEST(LibraryLifecycle, TransitionsArePitIrreversible) { // LOAD-BEARING (PIT)
  LifecycleJournal j(tmpdir());
  ASSERT_TRUE(j.transition(AlphaId{0}, LifecycleState::Admitted, 100).has_value());
  EXPECT_EQ(j.state_as_of(AlphaId{0}, 50), LifecycleState::Candidate);  // before its birth
  EXPECT_EQ(j.state_as_of(AlphaId{0}, 150), LifecycleState::Admitted);
  ASSERT_TRUE(j.transition(AlphaId{0}, LifecycleState::Live, 200).has_value()); // a LATER, LEGAL transition
  EXPECT_EQ(j.state_as_of(AlphaId{0}, 150), LifecycleState::Admitted); // earlier query UNCHANGED (PIT)
  EXPECT_EQ(j.state_as_of(AlphaId{0}, 250), LifecycleState::Live);
}

TEST(LibraryLifecycle, PersistsAcrossReopen) { // append-only durable
  const std::string dir = tmpdir("rt"); // ONE dir for both opens
  {
    LifecycleJournal j(dir);
    ASSERT_TRUE(j.transition(AlphaId{0}, LifecycleState::Admitted, 1).has_value());
  }
  LifecycleJournal r(dir);
  EXPECT_EQ(r.state_as_of(AlphaId{0}, 999), LifecycleState::Admitted);
}

} // namespace
