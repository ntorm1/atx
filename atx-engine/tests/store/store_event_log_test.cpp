// store_event_log_test.cpp — append-only event log + lifecycle projection (Task 6).
#include <gtest/gtest.h>
#include "atx/engine/store/db.hpp"
#include "atx/engine/store/event_log.hpp"

namespace atxtest_store_event_log_test {
using atx::engine::store::StoreDb;
using atx::engine::store::LifecycleState;
using atx::engine::store::EventRow;
namespace ev = atx::engine::store::event_log;

TEST(EventLog, TransitionDualWritesAndStateAsOf) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  ASSERT_TRUE(ev::transition(db, 0xA1ull, LifecycleState::Candidate, LifecycleState::Admitted,
                             /*as_of*/100, "run1", /*ts*/10).has_value());
  auto st = ev::state_as_of(db, 0xA1ull, 150); ASSERT_TRUE(st.has_value());
  EXPECT_EQ(*st, LifecycleState::Admitted);
  // PIT: before its birth it is Candidate
  auto before = ev::state_as_of(db, 0xA1ull, 50); ASSERT_TRUE(before.has_value());
  EXPECT_EQ(*before, LifecycleState::Candidate);
  // the transition also wrote an alpha_event row
  auto h = ev::history(db, 0xA1ull); ASSERT_TRUE(h.has_value());
  ASSERT_EQ(h->size(), 1u);
  EXPECT_EQ((*h)[0].event_type, "lifecycle");
}

TEST(EventLog, HistoryIsOrderedAppendOnly) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  ASSERT_TRUE(ev::append(db, EventRow{1, 0xB2ull, "created",  "run1", "system", "{}"}).has_value());
  ASSERT_TRUE(ev::append(db, EventRow{2, 0xB2ull, "evaluated","run1", "run",    "{\"sharpe\":1.2}"}).has_value());
  auto h = ev::history(db, 0xB2ull); ASSERT_TRUE(h.has_value());
  ASSERT_EQ(h->size(), 2u);
  EXPECT_EQ((*h)[0].event_type, "created");
  EXPECT_EQ((*h)[1].event_type, "evaluated");
}

}  // namespace atxtest_store_event_log_test
