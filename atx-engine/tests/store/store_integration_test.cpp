// store_integration_test.cpp — end-to-end: mint alpha -> run -> metrics -> lifecycle ->
// segment -> query "how built" + "what changed". Proves the units compose (Task 9).
#include <gtest/gtest.h>
#include "atx/engine/store/db.hpp"
#include "atx/engine/store/alpha_catalog.hpp"
#include "atx/engine/store/universe_registry.hpp"
#include "atx/engine/store/fingerprint.hpp"
#include "atx/engine/store/run_recorder.hpp"
#include "atx/engine/store/event_log.hpp"
#include "atx/engine/store/segment_index.hpp"

namespace atxtest_store_integration_test {
using namespace atx::engine::store;
namespace cat = atx::engine::store::alpha_catalog;
namespace ur  = atx::engine::store::universe_registry;
namespace fp  = atx::engine::store::fingerprint;
namespace ev  = atx::engine::store::event_log;
namespace si  = atx::engine::store::segment_index;

TEST(StoreIntegration, FullAlphaLifecycle) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  const atx::u64 H = 0xABCDEFull;

  // 1. universe + snapshot
  ASSERT_TRUE(ur::define(db, "u1", "SP500", 20260101, "top500", 0xFEEDull).has_value());
  ASSERT_TRUE(ur::record_snapshot(db, "snap1", "ORATS", 20260101, 0xBEEFull).has_value());

  // 2. mint alpha + lineage + created event
  ASSERT_TRUE(cat::upsert(db, H, 1, "rank(close - ts_mean(close,5))", 1, "run1").has_value());
  ASSERT_TRUE(cat::add_lineage(db, H, 0x1ull, 3, 99).has_value());
  ASSERT_TRUE(ev::append(db, EventRow{1, H, "created", "run1", "system", "{}"}).has_value());

  // 3. run with fingerprint
  RunInputs in{"sha", "cfg", 0xFEEDull, 0xBEEFull, 7, "gates"};
  RunRow r; r.run_id = "run1"; r.fingerprint = fp::compute(in); r.kind = "backtest";
  r.universe_id = "u1"; r.snapshot_id = "snap1"; r.fit_start = 0; r.fit_end = 100;
  r.bt_start = 100; r.bt_end = 200; r.position_mode = "book"; r.sector_neutral = true;
  r.rebalance_every = 5; r.cost_model = "flat"; r.started_at = 2;
  auto rec = RunRecorder::begin(db, r); ASSERT_TRUE(rec.has_value());
  ASSERT_TRUE(rec->link_alpha(H, "admitted").has_value());
  ASSERT_TRUE(rec->record_metrics(AlphaMetricsRow{H, 1.8, 0.25, 0.08, 0.4, 0.6, 2.5}).has_value());
  ASSERT_TRUE(rec->commit(50, 0xD16E57ull).has_value());

  // 4. lifecycle Candidate->Admitted (dual write)
  ASSERT_TRUE(ev::transition(db, H, LifecycleState::Candidate, LifecycleState::Admitted,
                             100, "run1", 3).has_value());

  // 5. segment mapping for heavy arrays
  ASSERT_TRUE(si::register_segment(db, SegmentRow{"seg1", "/d/seg1", 0xC0DEull, 0, 1, 0x55ull, 1, "run1"}).has_value());
  ASSERT_TRUE(si::map_alpha(db, H, "seg1", 0).has_value());

  // ---- queries the design promised ----
  // "how was it built": parents present
  auto ps = cat::parents(db, H); ASSERT_TRUE(ps.has_value()); EXPECT_EQ(ps->size(), 1u);
  // "what changed over time": created + lifecycle = 2 events
  auto h = ev::history(db, H); ASSERT_TRUE(h.has_value()); EXPECT_EQ(h->size(), 2u);
  // state as-of
  auto st = ev::state_as_of(db, H, 150); ASSERT_TRUE(st.has_value()); EXPECT_EQ(*st, LifecycleState::Admitted);
  // locate heavy arrays
  auto loc = si::locate(db, H); ASSERT_TRUE(loc.has_value()); EXPECT_EQ(loc->segment_id, "seg1");
}

}  // namespace atxtest_store_integration_test
