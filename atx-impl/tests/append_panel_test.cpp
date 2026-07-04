// atx::engine::data::append_history_panel — byte-identity tests (p7 S6-1).
//
// The CENTRAL correctness gate for Sprint 6: an incremental append must be
// byte-identical to a full rebuild over the same combined date range. We prove it
// by serializing both panels via write_panel and comparing the on-disk bytes (and
// the fnv1a64 digest) — not by assertion inside the code under test.
//
// Fixtures are tiny, deterministic synthetic ORATS segments (no real data): a few
// dates x a few instruments, volume/shares sized so every instrument clears the
// (disabled) ADV screen. Segments are written per-date into three directories:
//   - combined/ : every date (the full-rebuild source + the append's combined_cfg)
//   - old/      : all dates except the last batch (the pre-append panel)
//   - new/      : only the appended dates (append's new_seg_dir)

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/segment_panel.hpp" // alpha::TimeWindow
#include "atx/engine/data/history_panel.hpp"  // build/append_history_panel, HistoryDataConfig
#include "atx/engine/data/orats_history.hpp"  // kOratsFields
#include "atx/tsdb/load_parquet.hpp"          // build_from_long, LongColumns

#include "serialize_panel.hpp" // write_panel

namespace atxtest_append_panel {

namespace fs = std::filesystem;
using atx::engine::alpha::Panel;
using atx::engine::data::HistoryDataConfig;

// One ISO trading day in unix nanos; day 0 == 2020-01-02 midnight UTC.
constexpr atx::i64 kDayNanos = 86400LL * 1'000'000'000LL;
constexpr atx::i64 kDay0 = 18263LL * kDayNanos;

// Write one date's .seg with all ORATS fields for `n_instr` symbols into `dir`.
// close[i] = base_close + i; cumReturnFactor = 1.0 (TRI == raw close);
// shares = 2e8, volume = 1e6 (market_cap / dollar_volume >> any disabled floor).
// Filenames are zero-padded by day index so they sort chronologically.
void write_seg_day(const fs::path &dir, int day_index, atx::i64 dn, int n_instr,
                   atx::f64 base_close) {
  const auto r = static_cast<atx::usize>(n_instr);
  atx::tsdb::LongColumns cols;
  cols.field_names.assign(atx::engine::data::kOratsFields.begin(),
                          atx::engine::data::kOratsFields.end());
  cols.times.assign(r, dn);
  cols.symbols.reserve(r);
  for (int i = 0; i < n_instr; ++i) {
    cols.symbols.push_back(std::to_string(10001 + i)); // stable symbol set every day
  }
  cols.values.assign(atx::engine::data::kOratsFields.size(), std::vector<atx::f64>(r, 0.0));
  for (int i = 0; i < n_instr; ++i) {
    cols.values[3][static_cast<atx::usize>(i)] = base_close + static_cast<atx::f64>(i); // close
  }
  cols.values[6].assign(r, 1.0e6); // volume
  cols.values[7].assign(r, 2.0e8); // shares
  cols.values[10].assign(r, 1.0);  // cumReturnFactor

  char name[32];
  std::snprintf(name, sizeof(name), "day_%04d.seg", day_index);
  auto ok = atx::tsdb::build_from_long(cols, (dir / name).string(), 0);
  ASSERT_TRUE(ok.has_value()) << "write_seg_day failed for day " << day_index;
}

// Default HistoryDataConfig over `dir` with the universe screen disabled so every
// synthetic instrument is admitted on every date (focuses the test on the append
// mechanics, not the screen).
HistoryDataConfig cfg_for(const std::string &dir) {
  HistoryDataConfig hc;
  hc.seg_dir = dir;
  hc.window = atx::engine::alpha::TimeWindow{}; // all dates
  hc.universe.min_adv_usd = 0.0;
  hc.universe.top_n_by_adv = 0;
  hc.compact_to_universe = false;
  return hc;
}

std::string read_file_bytes(const std::string &path) {
  std::ifstream f{path, std::ios::binary};
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Build a unique scratch root per test, populate combined/old/new with the given
// total date count and the count of trailing dates that are "new".
struct Dirs {
  fs::path root;
  fs::path combined;
  fs::path old_dir;
  fs::path new_dir;
};

Dirs make_dirs(const char *tag) {
  Dirs d;
  d.root = fs::temp_directory_path() / (std::string("atx_append_") + tag);
  d.combined = d.root / "combined";
  d.old_dir = d.root / "old";
  d.new_dir = d.root / "new";
  std::error_code ec;
  fs::remove_all(d.root, ec);
  fs::create_directories(d.combined, ec);
  fs::create_directories(d.old_dir, ec);
  fs::create_directories(d.new_dir, ec);
  return d;
}

// Populate combined/old/new. Dates [0, total) go to combined; [0, total-n_new) to
// old; [total-n_new, total) to new. base_close grows with the day index so each
// date row is distinct.
void populate(const Dirs &d, int total, int n_new, int n_instr) {
  for (int day = 0; day < total; ++day) {
    const atx::i64 dn = kDay0 + static_cast<atx::i64>(day) * kDayNanos;
    const atx::f64 base = 50.0 + static_cast<atx::f64>(day) * 0.25;
    ASSERT_NO_FATAL_FAILURE(write_seg_day(d.combined, day, dn, n_instr, base));
    if (day < total - n_new) {
      ASSERT_NO_FATAL_FAILURE(write_seg_day(d.old_dir, day, dn, n_instr, base));
    } else {
      ASSERT_NO_FATAL_FAILURE(write_seg_day(d.new_dir, day, dn, n_instr, base));
    }
  }
}

void cleanup(const Dirs &d) {
  std::error_code ec;
  fs::remove_all(d.root, ec);
}

// ---------------------------------------------------------------------------
// AppendHistoryPanel_ByteIdentical — append one new day == full rebuild.
// ---------------------------------------------------------------------------
TEST(AppendHistoryPanel, ByteIdentical) {
  const Dirs d = make_dirs("byteid");
  ASSERT_NO_FATAL_FAILURE(populate(d, /*total=*/8, /*n_new=*/1, /*n_instr=*/4));

  // Full rebuild over the combined range.
  auto full = atx::engine::data::build_history_panel(cfg_for(d.combined.string()));
  ASSERT_TRUE(full.has_value()) << full.error().message();

  // Build over old dates, then append the single new day.
  auto old_p = atx::engine::data::build_history_panel(cfg_for(d.old_dir.string()));
  ASSERT_TRUE(old_p.has_value()) << old_p.error().message();
  auto appended = atx::engine::data::append_history_panel(
      old_p->panel, d.new_dir.string(), cfg_for(d.combined.string()));
  ASSERT_TRUE(appended.has_value()) << appended.error().message();

  // Serialize both and compare bytes + digest.
  const std::string p_full = (d.root / "full.bin").string();
  const std::string p_app = (d.root / "app.bin").string();
  auto wf = atx::impl::write_panel(full->panel, p_full);
  auto wa = atx::impl::write_panel(appended->panel, p_app);
  ASSERT_TRUE(wf.has_value()) << wf.error().message();
  ASSERT_TRUE(wa.has_value()) << wa.error().message();

  EXPECT_EQ(*wf, *wa) << "append fnv1a64 digest must equal full-rebuild digest";
  const std::string bytes_full = read_file_bytes(p_full);
  const std::string bytes_app = read_file_bytes(p_app);
  EXPECT_FALSE(bytes_full.empty());
  EXPECT_EQ(bytes_full.size(), bytes_app.size()) << "serialized byte length must match";
  EXPECT_EQ(bytes_full, bytes_app) << "append must be byte-identical to full rebuild";

  cleanup(d);
}

// ---------------------------------------------------------------------------
// AppendHistoryPanel_MultiDayAppend — append 3 new days in one call.
// ---------------------------------------------------------------------------
TEST(AppendHistoryPanel, MultiDayAppend) {
  const Dirs d = make_dirs("multi");
  ASSERT_NO_FATAL_FAILURE(populate(d, /*total=*/10, /*n_new=*/3, /*n_instr=*/5));

  auto full = atx::engine::data::build_history_panel(cfg_for(d.combined.string()));
  ASSERT_TRUE(full.has_value()) << full.error().message();
  auto old_p = atx::engine::data::build_history_panel(cfg_for(d.old_dir.string()));
  ASSERT_TRUE(old_p.has_value()) << old_p.error().message();
  auto appended = atx::engine::data::append_history_panel(
      old_p->panel, d.new_dir.string(), cfg_for(d.combined.string()));
  ASSERT_TRUE(appended.has_value()) << appended.error().message();

  const std::string p_full = (d.root / "full.bin").string();
  const std::string p_app = (d.root / "app.bin").string();
  auto wf = atx::impl::write_panel(full->panel, p_full);
  auto wa = atx::impl::write_panel(appended->panel, p_app);
  ASSERT_TRUE(wf.has_value()) << wf.error().message();
  ASSERT_TRUE(wa.has_value()) << wa.error().message();

  EXPECT_EQ(*wf, *wa) << "multi-day append digest must equal full rebuild";
  EXPECT_EQ(read_file_bytes(p_full), read_file_bytes(p_app))
      << "multi-day append must be byte-identical to full rebuild";

  cleanup(d);
}

// ---------------------------------------------------------------------------
// AppendHistoryPanel_EmptyNewSegs — empty new dir returns the original panel.
// ---------------------------------------------------------------------------
TEST(AppendHistoryPanel, EmptyNewSegs) {
  const Dirs d = make_dirs("empty");
  // All 6 dates are "old"; the new dir stays empty.
  ASSERT_NO_FATAL_FAILURE(populate(d, /*total=*/6, /*n_new=*/0, /*n_instr=*/4));

  auto old_p = atx::engine::data::build_history_panel(cfg_for(d.old_dir.string()));
  ASSERT_TRUE(old_p.has_value()) << old_p.error().message();

  // combined == old here (no new dates), and new_dir is empty -> no-op.
  auto appended = atx::engine::data::append_history_panel(
      old_p->panel, d.new_dir.string(), cfg_for(d.combined.string()));
  ASSERT_TRUE(appended.has_value()) << appended.error().message();

  const std::string p_old = (d.root / "old.bin").string();
  const std::string p_app = (d.root / "app.bin").string();
  auto wo = atx::impl::write_panel(old_p->panel, p_old);
  auto wa = atx::impl::write_panel(appended->panel, p_app);
  ASSERT_TRUE(wo.has_value()) << wo.error().message();
  ASSERT_TRUE(wa.has_value()) << wa.error().message();

  EXPECT_EQ(*wo, *wa) << "empty-append digest must equal the original panel";
  EXPECT_EQ(read_file_bytes(p_old), read_file_bytes(p_app))
      << "empty new-seg append must return the original panel byte-for-byte";

  cleanup(d);
}

// ---------------------------------------------------------------------------
// AppendHistoryPanel_OverlapRejectsInvalidArgument — a new date <= last existing
// date is a fail-closed Err(InvalidArgument).
// ---------------------------------------------------------------------------
TEST(AppendHistoryPanel, OverlapRejectsInvalidArgument) {
  const Dirs d = make_dirs("overlap");
  // Build a combined partition of 6 dates; the "old" panel covers all 6.
  ASSERT_NO_FATAL_FAILURE(populate(d, /*total=*/6, /*n_new=*/0, /*n_instr=*/4));

  auto old_p = atx::engine::data::build_history_panel(cfg_for(d.old_dir.string()));
  ASSERT_TRUE(old_p.has_value()) << old_p.error().message();

  // Put a seg for an EXISTING date (day 5, the last old date) into the new dir:
  // it overlaps the existing range and must be rejected.
  const atx::i64 dup_dn = kDay0 + 5LL * kDayNanos;
  ASSERT_NO_FATAL_FAILURE(write_seg_day(d.new_dir, 5, dup_dn, 4, 50.0 + 5.0 * 0.25));

  auto appended = atx::engine::data::append_history_panel(
      old_p->panel, d.new_dir.string(), cfg_for(d.combined.string()));
  ASSERT_FALSE(appended.has_value()) << "overlapping new date must be rejected";
  EXPECT_EQ(appended.error().code(), atx::core::ErrorCode::InvalidArgument);

  cleanup(d);
}

// ---------------------------------------------------------------------------
// AppendHistoryPanel_CompactRejected — compact_to_universe on the append path is
// a documented unsupported precondition (fail closed).
// ---------------------------------------------------------------------------
TEST(AppendHistoryPanel, CompactRejected) {
  const Dirs d = make_dirs("compact");
  ASSERT_NO_FATAL_FAILURE(populate(d, /*total=*/6, /*n_new=*/1, /*n_instr=*/4));

  auto old_p = atx::engine::data::build_history_panel(cfg_for(d.old_dir.string()));
  ASSERT_TRUE(old_p.has_value()) << old_p.error().message();

  HistoryDataConfig hc = cfg_for(d.combined.string());
  hc.compact_to_universe = true;
  auto appended =
      atx::engine::data::append_history_panel(old_p->panel, d.new_dir.string(), hc);
  ASSERT_FALSE(appended.has_value()) << "compact_to_universe must be rejected on append";
  EXPECT_EQ(appended.error().code(), atx::core::ErrorCode::InvalidArgument);

  cleanup(d);
}

} // namespace atxtest_append_panel
