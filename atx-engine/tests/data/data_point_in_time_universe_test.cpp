// atx-engine/tests/data/data_point_in_time_universe_test.cpp
//
// Checkpoint 15, task T1: the point-in-time universe builder of
// `atx::engine::data::PitUniverseBuilder`. Case numbers and names are those of
// §9.1 of `atx-engine/reviews/2026-09-20-iteration15-point-in-time-universe-design.md`
// (Revision 3); case 26 prints the §8.2 `POINT_IN_TIME_UNIVERSE_MEASUREMENT` marker
// for every oracle family F1-F9 so `iteration15_native_comparator.py` can read one
// ctest log of `atx-engine-data-tests`.
//
// Synthetic sessions only via `observe_session`; keys are consecutive UTC midnights
// from 2013-01-02 unless a case says otherwise; no file I/O, no clock, no RNG.

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "atx/core/datetime.hpp" // days_from_civil for the oracle fixture dates
#include "atx/core/error.hpp"
#include "atx/core/sha256.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/data/point_in_time_universe.hpp"
#include "atx/engine/eval/cross_section_ic.hpp" // case 18 ONLY: eval::kValidationBeginNs

namespace atxtest_data_point_in_time_universe {

using atx::core::ErrorCode;
using atx::core::Result;
using atx::core::Status;
using namespace atx::engine::data;

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
constexpr atx::i64 kDay = kPitNanosPerDay;
// 2013-01-02T00:00:00Z: 15,707 days after the epoch (1970..2012 = 42 years, 10 of
// them leap = 15,340 days to 2012-01-01; 2012 is leap => 15,706 = 2013-01-01).
constexpr atx::i64 kBase = 15'707 * kDay;
constexpr atx::usize k2014 = 365; // 2014-01-02 offset in days from kBase
constexpr atx::usize k2015 = 730; // 2015-01-02

[[nodiscard]] atx::i64 key(atx::usize d) { return kBase + static_cast<atx::i64>(d) * kDay; }

struct Bar {
  atx::i64 id;
  atx::f64 close;
  atx::f64 volume;
  atx::f64 shares;
  atx::f64 gics;
};

// dv = close * volume = 2.0 * (dv / 2) exactly for the integer dv used here.
[[nodiscard]] Bar bar(atx::i64 id, atx::f64 dv, atx::f64 gics = kNaN) {
  return Bar{id, 2.0, dv / 2.0, kNaN, gics};
}
[[nodiscard]] Bar raw(atx::i64 id, atx::f64 close, atx::f64 volume) {
  return Bar{id, close, volume, kNaN, kNaN};
}

[[nodiscard]] PitUniverseConfig make_cfg(atx::usize window, atx::usize min_valid,
                                         std::initializer_list<atx::usize> top_n,
                                         std::initializer_list<atx::u32> bands) {
  PitUniverseConfig c;
  c.adv_window = window;
  c.min_valid_observations = min_valid;
  c.top_n_count = 0;
  for (const atx::usize n : top_n) {
    c.top_n[c.top_n_count] = n;
    ++c.top_n_count;
  }
  c.band_count = 0;
  for (const atx::u32 b : bands) {
    c.band_bp[c.band_count] = b;
    ++c.band_count;
  }
  c.max_source_ids = 64;
  c.max_rebalances = 16;
  c.max_sessions = 1024;
  return c;
}

void require(const Status &st, const char *what) {
  if (!st.has_value()) {
    throw std::runtime_error(std::string{what} + ": " + st.error().to_string());
  }
}

template <class T>
  requires(!std::is_void_v<T>)
[[nodiscard]] T require(Result<T> &&r, const char *what) {
  if (!r.has_value()) {
    throw std::runtime_error(std::string{what} + ": " + r.error().to_string());
  }
  return std::move(*r);
}

// One builder plus a running session ordinal; every helper throws on an
// unexpected error so a broken fixture fails the test with the engine's message.
class Feed {
public:
  explicit Feed(const PitUniverseConfig &c, std::span<const PitInstrumentTypeEvidence> types = {})
      : b_{require(PitUniverseBuilder::create(c, types), "create")} {}

  [[nodiscard]] Status observe(const std::vector<Bar> &bars, bool with_extra = true) {
    Status st = observe_key(key(next_), bars, with_extra);
    if (st.has_value()) {
      ++next_;
    }
    return st;
  }
  [[nodiscard]] Status observe_at(atx::usize d, const std::vector<Bar> &bars) {
    Status st = observe_key(key(d), bars, true);
    if (st.has_value()) {
      next_ = d + 1;
    }
    return st;
  }
  [[nodiscard]] Status observe_key(atx::i64 k, const std::vector<Bar> &bars, bool with_extra) {
    std::vector<atx::i64> ids;
    std::vector<atx::f64> close;
    std::vector<atx::f64> volume;
    std::vector<atx::f64> shares;
    std::vector<atx::f64> gics;
    for (const Bar &b : bars) {
      ids.push_back(b.id);
      close.push_back(b.close);
      volume.push_back(b.volume);
      shares.push_back(b.shares);
      gics.push_back(b.gics);
    }
    if (!with_extra) {
      shares.clear();
      gics.clear();
    }
    return b_.observe_session(k, ids, close, volume, shares, gics);
  }
  void feed(atx::usize n, const std::vector<Bar> &bars) {
    for (atx::usize i = 0; i < n; ++i) {
      require(observe(bars), "feed");
    }
  }
  [[nodiscard]] Result<PitRebalanceView> rebalance_last() {
    return b_.rebalance(b_.session_key(b_.sessions() - 1));
  }
  [[nodiscard]] PitRebalanceView rebalance_now() {
    return require(rebalance_last(), "rebalance");
  }
  [[nodiscard]] PitUniverseBuilder &b() { return b_; }
  [[nodiscard]] const PitUniverseBuilder &b() const { return b_; }
  [[nodiscard]] atx::usize next() const { return next_; }

private:
  [[nodiscard]] static PitUniverseBuilder make(const PitUniverseConfig &c) {
    return require(PitUniverseBuilder::create(c), "create");
  }
  PitUniverseBuilder b_;
  atx::usize next_{0};
};

[[nodiscard]] const PitRankedRow *find_ranked(const PitRebalanceView &v, atx::i64 id) {
  for (const PitRankedRow &row : v.ranked) {
    if (row.security_id == id) {
      return &row;
    }
  }
  return nullptr;
}

[[nodiscard]] const PitMemberRow *find_member(const PitRebalanceView &v, atx::usize c,
                                              atx::u32 slot) {
  for (const PitMemberRow &row : v.members[c]) {
    if (row.slot == slot) {
      return &row;
    }
  }
  return nullptr;
}

[[nodiscard]] std::vector<atx::i64> member_ids(const Feed &f, const PitRebalanceView &v,
                                               atx::usize c) {
  std::vector<atx::i64> out;
  for (const PitMemberRow &row : v.members[c]) {
    out.push_back(f.b().security_id(row.slot));
  }
  return out;
}

[[nodiscard]] std::vector<atx::u32> member_rank_list(const PitRebalanceView &v, atx::usize c) {
  std::vector<atx::u32> out;
  for (const PitMemberRow &row : v.members[c]) {
    out.push_back(row.rank);
  }
  return out;
}

[[nodiscard]] std::vector<atx::usize> member_status_list(const PitRebalanceView &v, atx::usize c) {
  std::vector<atx::usize> out;
  for (const PitMemberRow &row : v.members[c]) {
    out.push_back(static_cast<atx::usize>(row.status));
  }
  return out;
}

[[nodiscard]] std::vector<atx::u32> to_vec(std::span<const atx::u32> s) {
  return std::vector<atx::u32>(s.begin(), s.end());
}

// ===========================================================================
//  Fixtures shared by the numbered cases and the §8.2 marker export (case 26).
// ===========================================================================

// Case 1 / F1: one ID entering at session 10, rank at `rank_session`.
[[nodiscard]] Feed fixture_late_entrant(atx::usize rank_session) {
  Feed f(make_cfg(63, 57, {5}, {0}));
  for (atx::usize d = 0; d < 10; ++d) {
    require(f.observe({}), "empty");
  }
  for (atx::usize d = 10; d <= rank_session; ++d) {
    require(f.observe({bar(1, 200.0)}), "entrant");
  }
  return f;
}

// Case 2 / F1: sessions 0..62, ID 1 absent on the first `missing` sessions,
// dv = 2 * (s + 1) elsewhere.
[[nodiscard]] Feed fixture_gap(atx::usize missing) {
  Feed f(make_cfg(63, 57, {5}, {0}));
  for (atx::usize s = 0; s < 63; ++s) {
    if (s < missing) {
      require(f.observe({}), "gap");
    } else {
      require(f.observe({raw(1, 2.0, static_cast<atx::f64>(s + 1))}), "gap");
    }
  }
  return f;
}

// Case 2b / F1: six invalid bar shapes plus a zero-volume (valid) bar for ID 1;
// ID 2 valid throughout.
[[nodiscard]] Feed fixture_invalid_shapes() {
  Feed f(make_cfg(63, 57, {5}, {0}));
  constexpr atx::f64 kInf = std::numeric_limits<atx::f64>::infinity();
  for (atx::usize s = 0; s < 63; ++s) {
    Bar one = raw(1, 2.0, 10.0);
    switch (s) {
    case 1:
      one.close = kNaN;
      break;
    case 2:
      one.close = kInf;
      break;
    case 3:
      one.close = 0.0;
      break;
    case 4:
      one.close = -1.0;
      break;
    case 5:
      one.volume = -1.0;
      break;
    case 6:
      one.volume = kNaN;
      break;
    case 7:
      one.volume = 0.0; // valid: dv = 0
      break;
    default:
      break;
    }
    require(f.observe({one, raw(2, 2.0, 10.0)}), "shapes");
  }
  return f;
}

// Case 3 / F1: ID 1 all-zero volume, ID 2 positive.
[[nodiscard]] Feed fixture_zero_volume() {
  Feed f(make_cfg(63, 57, {5}, {0}));
  f.feed(63, {raw(1, 2.0, 0.0), raw(2, 2.0, 10.0)});
  return f;
}

// Case 4 / F1: ID 1 present on sessions 5..62 with dv 1..58; ID 2 on 6..62 with 1..57.
[[nodiscard]] Feed fixture_median() {
  Feed f(make_cfg(63, 57, {5}, {0}));
  for (atx::usize s = 0; s < 63; ++s) {
    std::vector<Bar> bars;
    if (s >= 5) {
      bars.push_back(bar(1, static_cast<atx::f64>(s - 4)));
    }
    if (s >= 6) {
      bars.push_back(bar(2, static_cast<atx::f64>(s - 5)));
    }
    require(f.observe(bars), "median");
  }
  return f;
}

// Case 5 / F1: identical dv, fed in order 30, 10, 20.
[[nodiscard]] Feed fixture_ties() {
  Feed f(make_cfg(63, 57, {5}, {0}));
  f.feed(63, {bar(30, 100.0), bar(10, 100.0), bar(20, 100.0)});
  return f;
}

// Case 6 / F2: close exactly 1.0 (fails) and nextafter(1.0, 2.0) (passes).
[[nodiscard]] Feed fixture_price_floor() {
  Feed f(make_cfg(63, 57, {5}, {0}));
  f.feed(63, {raw(1, 1.0, 10.0), raw(2, std::nextafter(1.0, 2.0), 10.0)});
  return f;
}

// Case 7 / F2: ID 1 valid 0..62, absent on rank session 63; ID 2 present throughout.
[[nodiscard]] Feed fixture_absent_on_rank() {
  Feed f(make_cfg(63, 57, {5}, {0}));
  f.feed(63, {raw(1, 2.0, 10.0), raw(2, 2.0, 10.0)});
  require(f.observe({raw(2, 2.0, 10.0)}), "rank session");
  return f;
}

enum class BandVariant { KeepAt11, DropAt12, BandZeroDropAt11 };

// Case 9 / F4: top_n 10; profile A ranks IDs 1..12 by id; profile B moves ID 10 to
// rank 11 (or 12) and ID 11 to rank 10. r0 is taken inside; the caller takes r1.
[[nodiscard]] Feed fixture_band(BandVariant variant) {
  const atx::u32 band = (variant == BandVariant::BandZeroDropAt11) ? 0u : 1000u;
  Feed f(make_cfg(4, 3, {10}, {band}));
  std::vector<Bar> a;
  for (atx::i64 id = 1; id <= 12; ++id) {
    a.push_back(bar(id, 1000.0 - static_cast<atx::f64>(id)));
  }
  f.feed(4, a);
  (void)f.rebalance_now();
  std::vector<Bar> b;
  for (atx::i64 id = 1; id <= 9; ++id) {
    b.push_back(bar(id, 1000.0 - static_cast<atx::f64>(id)));
  }
  if (variant == BandVariant::DropAt12) {
    b.push_back(bar(11, 990.0)); // rank 10
    b.push_back(bar(12, 989.0)); // rank 11
    b.push_back(bar(10, 988.0)); // rank 12
  } else {
    b.push_back(bar(11, 990.0)); // rank 10
    b.push_back(bar(10, 989.0)); // rank 11
    b.push_back(bar(12, 988.0)); // rank 12
  }
  f.feed(4, b);
  return f;
}

// Case 9c / F7 (the C-2 shape): incumbents 100 and 200 land at ranks 5 and 11.
[[nodiscard]] Feed fixture_c2() {
  Feed f(make_cfg(4, 3, {10}, {1000}));
  f.feed(4, {bar(100, 900.0), bar(200, 800.0)});
  (void)f.rebalance_now();
  std::vector<Bar> b;
  b.push_back(bar(100, 995.0)); // rank 5
  b.push_back(bar(200, 989.0)); // rank 11
  const std::array<atx::f64, 10> dv{999.0, 998.0, 997.0, 996.0, 994.0,
                                    993.0, 992.0, 991.0, 990.0, 988.0};
  for (atx::i64 id = 1; id <= 10; ++id) {
    b.push_back(bar(id, dv[static_cast<atx::usize>(id - 1)]));
  }
  f.feed(4, b);
  return f;
}

// Case 10 / F7: top_n 2, band 1000 => K_band 2; incumbents A, B fall to ranks 2, 3.
[[nodiscard]] Feed fixture_top2_band() {
  Feed f(make_cfg(4, 3, {2}, {1000}));
  f.feed(4, {bar(1, 30.0), bar(2, 20.0)});
  (void)f.rebalance_now();
  f.feed(4, {bar(1, 30.0), bar(2, 20.0), bar(3, 40.0)});
  return f;
}

// Case 10 / F7: top_n 3, band 1000 => K_band 3; incumbents at 1..3, entrant at 4.
[[nodiscard]] Feed fixture_top3_band() {
  Feed f(make_cfg(4, 3, {3}, {1000}));
  f.feed(4, {bar(1, 30.0), bar(2, 20.0), bar(3, 10.0)});
  (void)f.rebalance_now();
  f.feed(4, {bar(1, 40.0), bar(2, 30.0), bar(3, 20.0), bar(4, 10.0)});
  return f;
}

enum class DropVariant { Absent, NanClose, FloorFail, All };

// Case 11 / F5: incumbents A (absent on rank session), B (close NaN), C (close 0.5)
// and D (normal); `variant` selects which incumbent misbehaves (All = every one).
[[nodiscard]] Feed fixture_drop(DropVariant variant) {
  Feed f(make_cfg(4, 3, {5}, {0}));
  f.feed(4, {bar(1, 30.0), bar(2, 20.0), bar(3, 10.0), bar(4, 5.0)});
  (void)f.rebalance_now();
  f.feed(3, {bar(1, 30.0), bar(2, 20.0), bar(3, 10.0), bar(4, 5.0)});
  std::vector<Bar> last;
  if (variant != DropVariant::Absent && variant != DropVariant::All) {
    last.push_back(bar(1, 30.0));
  }
  if (variant == DropVariant::NanClose || variant == DropVariant::All) {
    last.push_back(raw(2, kNaN, 10.0));
  } else {
    last.push_back(bar(2, 20.0));
  }
  if (variant == DropVariant::FloorFail || variant == DropVariant::All) {
    last.push_back(raw(3, 0.5, 10.0));
  } else {
    last.push_back(bar(3, 10.0));
  }
  last.push_back(bar(4, 5.0));
  require(f.observe(last), "drop rank session");
  return f;
}

// Cases 12 / 13: six IDs with a deterministic per-session pattern; ID 4 enters at 5.
[[nodiscard]] std::vector<Bar> pattern_bars(atx::usize s) {
  std::vector<Bar> bars;
  for (atx::i64 id = 1; id <= 6; ++id) {
    if (id == 4 && s < 5) {
      continue;
    }
    const atx::f64 close = 2.0 + static_cast<atx::f64>(id % 3);
    const atx::f64 volume =
        100.0 + static_cast<atx::f64>((s * 7 + static_cast<atx::usize>(id) * 13) % 50);
    bars.push_back(raw(id, close, volume));
  }
  return bars;
}

[[nodiscard]] Feed fixture_long(atx::usize sessions, const std::vector<atx::usize> &rank_at) {
  Feed f(make_cfg(63, 57, {3}, {0}));
  atx::usize next_rank = 0;
  for (atx::usize s = 0; s < sessions; ++s) {
    require(f.observe(pattern_bars(s)), "long");
    if (next_rank < rank_at.size() && rank_at[next_rank] == s) {
      (void)f.rebalance_now();
      ++next_rank;
    }
  }
  return f;
}

// Cases 20 / 21 / F6: three calendar years, four sessions each, one rebalance per year.
// A = 1 (dv 10, gics NaN), B = 2 (dv 20, gics 10), C = 3 (dv 30, gics NaN).
[[nodiscard]] Feed fixture_coverage() {
  Feed f(make_cfg(4, 3, {2}, {0}));
  const Bar A = bar(1, 10.0);
  const Bar B = bar(2, 20.0, 10.0);
  const Bar C = bar(3, 30.0);
  require(f.observe_at(0, {A, B}), "2013");
  require(f.observe_at(1, {A, B}), "2013");
  require(f.observe_at(2, {A, B, C}), "2013");
  require(f.observe_at(3, {A, B, C}), "2013");
  (void)f.rebalance_now();
  for (atx::usize d = 0; d < 4; ++d) {
    require(f.observe_at(k2014 + d, {A, B, C}), "2014");
  }
  (void)f.rebalance_now();
  for (atx::usize d = 0; d < 3; ++d) {
    require(f.observe_at(k2015 + d, {B, C}), "2015");
  }
  (void)f.rebalance_now();
  require(f.observe_at(k2015 + 3, {B, C}), "2015 last");
  return f;
}

// Case 22 / F6 / F9: D stops trading in 2014 (last bar e1); A, B trade to the end.
[[nodiscard]] Feed fixture_survivorship() {
  Feed f(make_cfg(4, 3, {2}, {0}));
  const Bar A = bar(1, 10.0);
  const Bar B = bar(2, 20.0);
  const Bar D = bar(4, 40.0);
  for (atx::usize d = 0; d < 4; ++d) {
    require(f.observe_at(d, {A, B, D}), "2013");
  }
  (void)f.rebalance_now();
  require(f.observe_at(k2014 + 0, {A, B, D}), "2014");
  require(f.observe_at(k2014 + 1, {A, B, D}), "2014");
  require(f.observe_at(k2014 + 2, {A, B}), "2014");
  require(f.observe_at(k2014 + 3, {A, B}), "2014");
  (void)f.rebalance_now();
  for (atx::usize d = 0; d < 3; ++d) {
    require(f.observe_at(k2015 + d, {A, B}), "2015");
  }
  (void)f.rebalance_now();
  require(f.observe_at(k2015 + 3, {A, B}), "2015 last");
  return f;
}

// Cases 23 / 23b / F9: D rank-dropped in 2014 then absent in 2015; A's first bar is invalid.
[[nodiscard]] Feed fixture_exits() {
  Feed f(make_cfg(4, 3, {2}, {0}));
  const Bar A = bar(1, 10.0);
  const Bar B = bar(2, 20.0);
  const Bar D = bar(4, 40.0);
  require(f.observe_at(0, {raw(1, kNaN, 5.0), B, D}), "2013 invalid first bar");
  for (atx::usize d = 1; d < 4; ++d) {
    require(f.observe_at(d, {A, B, D}), "2013");
  }
  (void)f.rebalance_now();
  for (atx::usize d = 0; d < 4; ++d) {
    require(f.observe_at(k2014 + d, {A, B, bar(4, 5.0)}), "2014");
  }
  (void)f.rebalance_now();
  for (atx::usize d = 0; d < 3; ++d) {
    require(f.observe_at(k2015 + d, {A, B}), "2015");
  }
  (void)f.rebalance_now();
  require(f.observe_at(k2015 + 3, {A, B}), "2015 last");
  return f;
}

// Case 20b / F6: three members with valid counts 63 / 60 / 57 and one NaN gics.
[[nodiscard]] Feed fixture_valid_total() {
  Feed f(make_cfg(63, 57, {3}, {0}));
  for (atx::usize s = 0; s < 63; ++s) {
    std::vector<Bar> bars{bar(1, 10.0)};
    if (s >= 3) {
      bars.push_back(bar(2, 10.0, 10.0));
    }
    if (s >= 6) {
      bars.push_back(bar(3, 10.0, 20.0));
    }
    require(f.observe(bars), "valid total");
  }
  return f;
}

// Case 27 / F8: two rebalances x two cuts, both effective.
[[nodiscard]] Feed fixture_bin() {
  Feed f(make_cfg(4, 3, {2}, {0, 1000}));
  f.feed(4, {bar(1, 30.0), bar(2, 20.0), bar(3, 10.0)});
  (void)f.rebalance_now();
  f.feed(4, {bar(1, 30.0), bar(2, 20.0), bar(3, 10.0)});
  (void)f.rebalance_now();
  f.feed(1, {bar(1, 30.0), bar(2, 20.0), bar(3, 10.0)});
  return f;
}

// Case 28 / F3: 2013 Jan 2, Jan 15, Feb 3, Feb 27, Mar 1.
[[nodiscard]] std::vector<atx::i64> cadence_keys() {
  return {key(0), key(13), key(32), key(56), key(58)};
}

// ===========================================================================
//  §9.1 cases
// ===========================================================================

TEST(DataPointInTimeUniverse, Case01_LateEntrant_ExactlySixtyThreeObservations_IsEligible) {
  {
    Feed f = fixture_late_entrant(72);
    const PitRebalanceView v = f.rebalance_now();
    ASSERT_EQ(v.eligible, 1U);
    EXPECT_EQ(v.ranked[0].valid_observations, 63U);
    EXPECT_EQ(v.ranked[0].security_id, 1);
  }
  {
    Feed f = fixture_late_entrant(71);
    const PitRebalanceView v = f.rebalance_now();
    ASSERT_EQ(v.eligible, 1U);
    EXPECT_EQ(v.ranked[0].valid_observations, 62U);
  }
  {
    Feed f = fixture_late_entrant(65);
    const PitRebalanceView v = f.rebalance_now();
    EXPECT_EQ(v.eligible, 0U); // 56 valid < 57
    EXPECT_EQ(v.ids_seen, 1U);
    EXPECT_EQ(v.ids_with_valid_bar, 1U);
  }
}

TEST(DataPointInTimeUniverse, Case02_Gap_BreaksWindowButLaterBarsCount) {
  {
    Feed f = fixture_gap(7);
    const PitRebalanceView v = f.rebalance_now();
    EXPECT_EQ(v.eligible, 0U);
  }
  {
    Feed f = fixture_gap(6);
    const PitRebalanceView v = f.rebalance_now();
    ASSERT_EQ(v.eligible, 1U);
    EXPECT_EQ(v.ranked[0].valid_observations, 57U);
    EXPECT_EQ(v.ranked[0].adv63_usd, 70.0); // median of 2*(s+1), s = 6..62
  }
}

TEST(DataPointInTimeUniverse, Case02b_BarValidity_EveryInvalidShape_IsMissing) {
  Feed f = fixture_invalid_shapes();
  const PitRebalanceView v = f.rebalance_now();
  ASSERT_EQ(v.eligible, 2U);
  const PitRankedRow *one = find_ranked(v, 1);
  const PitRankedRow *two = find_ranked(v, 2);
  ASSERT_NE(one, nullptr);
  ASSERT_NE(two, nullptr);
  EXPECT_EQ(one->valid_observations, 57U); // 63 - 6 invalid; volume 0 still valid
  EXPECT_EQ(two->valid_observations, 63U);
}

TEST(DataPointInTimeUniverse, Case03_ZeroVolume_IsValidAndZeroAdvPassesFloor) {
  Feed f = fixture_zero_volume();
  const PitRebalanceView v = f.rebalance_now();
  ASSERT_EQ(v.eligible, 2U);
  EXPECT_EQ(v.ranked[0].security_id, 2);
  EXPECT_EQ(v.ranked[1].security_id, 1);
  EXPECT_EQ(v.ranked[1].adv63_usd, 0.0);
  EXPECT_EQ(v.ranked[1].valid_observations, 63U);
  EXPECT_EQ(v.ranked[1].rank, 2U);
}

TEST(DataPointInTimeUniverse, Case04_Median_EvenCount_IsHalfSum) {
  Feed f = fixture_median();
  const PitRebalanceView v = f.rebalance_now();
  ASSERT_EQ(v.eligible, 2U);
  const PitRankedRow *even = find_ranked(v, 1);
  const PitRankedRow *odd = find_ranked(v, 2);
  ASSERT_NE(even, nullptr);
  ASSERT_NE(odd, nullptr);
  EXPECT_EQ(even->valid_observations, 58U);
  EXPECT_EQ(even->adv63_usd, 29.5);
  EXPECT_EQ(odd->valid_observations, 57U);
  EXPECT_EQ(odd->adv63_usd, 29.0);
}

TEST(DataPointInTimeUniverse, Case05_Ties_BreakByFirstSeenSlotAscending) {
  Feed f = fixture_ties();
  const PitRebalanceView v = f.rebalance_now();
  ASSERT_EQ(v.eligible, 3U);
  EXPECT_EQ(v.ranked[0].security_id, 30);
  EXPECT_EQ(v.ranked[0].slot, 0U);
  EXPECT_EQ(v.ranked[0].rank, 1U);
  EXPECT_EQ(v.ranked[1].security_id, 10);
  EXPECT_EQ(v.ranked[1].slot, 1U);
  EXPECT_EQ(v.ranked[1].rank, 2U);
  EXPECT_EQ(v.ranked[2].security_id, 20);
  EXPECT_EQ(v.ranked[2].slot, 2U);
  EXPECT_EQ(v.ranked[2].rank, 3U);
}

TEST(DataPointInTimeUniverse, Case06_PriceFloor_RawCloseStrictlyAboveOne) {
  Feed f = fixture_price_floor();
  const PitRebalanceView v = f.rebalance_now();
  ASSERT_EQ(v.eligible, 1U);
  EXPECT_EQ(v.ranked[0].security_id, 2);
  EXPECT_EQ(v.ranked[0].raw_close, std::nextafter(1.0, 2.0));
}

TEST(DataPointInTimeUniverse, Case07_RankSession_NoBar_IsIneligibleEvenWithFullWindow) {
  Feed f = fixture_absent_on_rank();
  const PitRebalanceView v = f.rebalance_now();
  ASSERT_EQ(v.eligible, 1U);
  EXPECT_EQ(v.ranked[0].security_id, 2);
  EXPECT_EQ(f.b().last_bar(0), 62U);
}

TEST(DataPointInTimeUniverse, Case08_Effective_IsNextObservedSession_ViewStaysValid) {
  {
    Feed f(make_cfg(4, 3, {2}, {0}));
    f.feed(4, {bar(1, 10.0), bar(2, 20.0)});
    const PitRebalanceView v = f.rebalance_now();
    EXPECT_EQ(f.b().effective_key(0), 0);
    const Result<std::string> pending = encode_membership_bin(f.b());
    ASSERT_FALSE(pending.has_value());
    EXPECT_EQ(pending.error().code(), ErrorCode::Internal);
    const atx::i64 id0 = v.ranked[0].security_id;
    const atx::f64 adv0 = v.ranked[0].adv63_usd;
    const atx::u32 slot0 = v.members[0][0].slot;
    require(f.observe({bar(1, 10.0), bar(2, 20.0)}), "effective");
    EXPECT_EQ(f.b().effective_key(0), key(4));
    EXPECT_EQ(v.ranked[0].security_id, id0);
    EXPECT_EQ(v.ranked[0].adv63_usd, adv0);
    EXPECT_EQ(v.members[0][0].slot, slot0);
    EXPECT_EQ(v.ranked.size(), 2U);
  }
  {
    Feed f(make_cfg(4, 3, {2}, {0}));
    f.feed(4, {bar(1, 10.0), bar(2, 20.0)});
    (void)f.rebalance_now();
    require(f.observe_at(6, {bar(1, 10.0), bar(2, 20.0)}), "weekend gap");
    EXPECT_EQ(f.b().effective_key(0), key(6));
  }
}

TEST(DataPointInTimeUniverse, Case09_Band_KeepsIncumbentAtThresholdDropsAtPlusOne) {
  {
    Feed f = fixture_band(BandVariant::KeepAt11);
    const PitRebalanceView v = f.rebalance_now();
    const PitMemberRow *ten = find_member(v, 0, 9); // ID 10 is slot 9
    ASSERT_NE(ten, nullptr);
    EXPECT_EQ(ten->rank, 11U);
    EXPECT_EQ(ten->status, PitMemberStatus::Keep);
    EXPECT_EQ(find_member(v, 0, 10), nullptr); // ID 11 (rank 10) not admitted
    EXPECT_EQ(v.churn[0].adds, 0U);
    EXPECT_EQ(v.churn[0].kept, 10U);
    EXPECT_EQ(v.churn[0].drops_rank, 0U);
    EXPECT_EQ(v.churn[0].drops_last_bar, 0U);
    EXPECT_EQ(v.churn[0].members, 10U);
  }
  {
    Feed f = fixture_band(BandVariant::DropAt12);
    const PitRebalanceView v = f.rebalance_now();
    EXPECT_EQ(find_member(v, 0, 9), nullptr);
    const PitMemberRow *eleven = find_member(v, 0, 10);
    ASSERT_NE(eleven, nullptr);
    EXPECT_EQ(eleven->status, PitMemberStatus::Add);
    EXPECT_EQ(v.churn[0].drops_rank, 1U);
    EXPECT_EQ(v.churn[0].adds, 1U);
    EXPECT_EQ(v.churn[0].kept, 9U);
  }
  {
    Feed f = fixture_band(BandVariant::BandZeroDropAt11);
    const PitRebalanceView v = f.rebalance_now();
    EXPECT_EQ(find_member(v, 0, 9), nullptr);
    EXPECT_EQ(v.churn[0].drops_rank, 1U);
    EXPECT_EQ(v.churn[0].adds, 1U);
  }
}

TEST(DataPointInTimeUniverse, Case09b_FirstRebalance_AllAddNoDrops_ShortListLegal) {
  Feed f(make_cfg(4, 3, {5}, {0}));
  f.feed(4, {bar(1, 30.0), bar(2, 20.0), bar(3, 10.0)});
  const PitRebalanceView v = f.rebalance_now();
  ASSERT_EQ(v.members[0].size(), 3U);
  for (const PitMemberRow &row : v.members[0]) {
    EXPECT_EQ(row.status, PitMemberStatus::Add);
  }
  EXPECT_EQ(v.drops[0].size(), 0U);
  EXPECT_EQ(v.churn[0].adds, 3U);
  EXPECT_EQ(v.churn[0].kept, 0U);
  EXPECT_EQ(v.churn[0].drops_rank, 0U);
  EXPECT_EQ(v.churn[0].members, 3U);
}

TEST(DataPointInTimeUniverse, Case09c_MemberOrder_IsRankAscendingNotKeepThenFill) {
  Feed f = fixture_c2();
  const PitRebalanceView v = f.rebalance_now();
  const std::vector<atx::u32> ranks = member_rank_list(v, 0);
  const std::vector<atx::u32> want_ranks{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 11U};
  EXPECT_EQ(ranks, want_ranks);
  const std::vector<atx::i64> ids = member_ids(f, v, 0);
  const std::vector<atx::i64> want_ids{1, 2, 3, 4, 100, 5, 6, 7, 8, 200};
  EXPECT_EQ(ids, want_ids);
  const std::vector<atx::usize> status = member_status_list(v, 0);
  const std::vector<atx::usize> want_status{0U, 0U, 0U, 0U, 1U, 0U, 0U, 0U, 0U, 1U};
  EXPECT_EQ(status, want_status);
  EXPECT_EQ(v.churn[0].adds, 8U);
  EXPECT_EQ(v.churn[0].kept, 2U);
  const std::vector<atx::u32> slots = to_vec(f.b().members(1, 0));
  const std::vector<atx::u32> want_slots{0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U};
  EXPECT_EQ(slots, want_slots);
  const std::vector<atx::u32> slot_ranks = to_vec(f.b().member_ranks(1, 0));
  const std::vector<atx::u32> want_slot_ranks{5U, 11U, 1U, 2U, 3U, 4U, 6U, 7U, 8U, 9U};
  EXPECT_EQ(slot_ranks, want_slot_ranks);
}

TEST(DataPointInTimeUniverse, Case10_Band_IncumbentsExceedTopN_BestRankedKept) {
  {
    Feed f = fixture_top2_band();
    const PitRebalanceView v = f.rebalance_now();
    const std::vector<atx::i64> ids = member_ids(f, v, 0);
    const std::vector<atx::i64> want{3, 1};
    EXPECT_EQ(ids, want);
    EXPECT_EQ(v.members[0][0].status, PitMemberStatus::Add);
    EXPECT_EQ(v.members[0][1].status, PitMemberStatus::Keep);
    ASSERT_EQ(v.drops[0].size(), 1U);
    EXPECT_EQ(v.drops[0][0].slot, 1U);
    EXPECT_EQ(v.drops[0][0].kind, PitDropKind::Rank);
    EXPECT_EQ(v.churn[0].drops_rank, 1U);
  }
  {
    Feed f = fixture_top3_band();
    const PitRebalanceView v = f.rebalance_now();
    const std::vector<atx::i64> ids = member_ids(f, v, 0);
    const std::vector<atx::i64> want{1, 2, 3};
    EXPECT_EQ(ids, want);
    for (const PitMemberRow &row : v.members[0]) {
      EXPECT_EQ(row.status, PitMemberStatus::Keep);
    }
    EXPECT_EQ(v.churn[0].adds, 0U);
    EXPECT_EQ(v.churn[0].drops_rank, 0U);
    EXPECT_EQ(v.churn[0].kept, 3U);
  }
}

TEST(DataPointInTimeUniverse, Case11_Drop_KindByFormulaOnly) {
  {
    Feed f = fixture_drop(DropVariant::Absent);
    const PitRebalanceView v = f.rebalance_now();
    EXPECT_EQ(v.churn[0].drops_last_bar, 1U);
    EXPECT_EQ(v.churn[0].drops_rank, 0U);
  }
  {
    Feed f = fixture_drop(DropVariant::NanClose);
    const PitRebalanceView v = f.rebalance_now();
    EXPECT_EQ(v.churn[0].drops_last_bar, 1U);
    EXPECT_EQ(v.churn[0].drops_rank, 0U);
  }
  {
    Feed f = fixture_drop(DropVariant::FloorFail);
    const PitRebalanceView v = f.rebalance_now();
    EXPECT_EQ(v.churn[0].drops_rank, 1U);
    EXPECT_EQ(v.churn[0].drops_last_bar, 0U);
  }
  {
    Feed f = fixture_drop(DropVariant::All);
    const PitRebalanceView v = f.rebalance_now();
    ASSERT_EQ(v.drops[0].size(), 3U);
    EXPECT_EQ(v.drops[0][0].slot, 0U);
    EXPECT_EQ(v.drops[0][0].kind, PitDropKind::LastBar);
    EXPECT_EQ(v.drops[0][1].slot, 1U);
    EXPECT_EQ(v.drops[0][1].kind, PitDropKind::LastBar);
    EXPECT_EQ(v.drops[0][2].slot, 2U);
    EXPECT_EQ(v.drops[0][2].kind, PitDropKind::Rank);
    EXPECT_EQ(v.churn[0].kept, 1U);
    EXPECT_EQ(v.churn[0].members, 1U);
  }
}

void expect_same_rows(const PitRebalanceView &a, const PitRebalanceView &b) {
  ASSERT_EQ(a.ranked.size(), b.ranked.size());
  for (atx::usize i = 0; i < a.ranked.size(); ++i) {
    EXPECT_EQ(a.ranked[i].slot, b.ranked[i].slot);
    EXPECT_EQ(a.ranked[i].security_id, b.ranked[i].security_id);
    EXPECT_EQ(a.ranked[i].rank, b.ranked[i].rank);
    EXPECT_EQ(std::bit_cast<atx::u64>(a.ranked[i].adv63_usd),
              std::bit_cast<atx::u64>(b.ranked[i].adv63_usd));
    EXPECT_EQ(a.ranked[i].valid_observations, b.ranked[i].valid_observations);
    EXPECT_EQ(std::bit_cast<atx::u64>(a.ranked[i].raw_close),
              std::bit_cast<atx::u64>(b.ranked[i].raw_close));
  }
}

void expect_same_retained(const PitUniverseBuilder &a, const PitUniverseBuilder &b,
                          atx::usize r) {
  EXPECT_EQ(to_vec(a.members(r, 0)), to_vec(b.members(r, 0)));
  EXPECT_EQ(to_vec(a.member_ranks(r, 0)), to_vec(b.member_ranks(r, 0)));
  EXPECT_EQ(a.churn(r, 0).adds, b.churn(r, 0).adds);
  EXPECT_EQ(a.churn(r, 0).drops_rank, b.churn(r, 0).drops_rank);
  EXPECT_EQ(a.churn(r, 0).drops_last_bar, b.churn(r, 0).drops_last_bar);
  EXPECT_EQ(a.churn(r, 0).kept, b.churn(r, 0).kept);
  EXPECT_EQ(a.churn(r, 0).members, b.churn(r, 0).members);
  EXPECT_EQ(a.rank_key(r), b.rank_key(r));
}

TEST(DataPointInTimeUniverse,
     Case12_TruncationInvariance_LaterSessionsDoNotChangeEarlierMembership) {
  // Run A: sessions 0..99 with rebalances at 62 and 80; run B: sessions 0..80 only.
  Feed a = fixture_long(81, {62U});
  Feed b = fixture_long(81, {62U});
  const PitRebalanceView va = a.rebalance_now(); // r1 at session 80
  const PitRebalanceView vb = b.rebalance_now();
  for (atx::usize s = 81; s < 100; ++s) {
    require(a.observe(pattern_bars(s)), "run A tail");
  }
  ASSERT_EQ(a.b().sessions(), 100U);
  ASSERT_EQ(b.b().sessions(), 81U);
  ASSERT_EQ(a.b().rebalances(), 2U);
  ASSERT_EQ(b.b().rebalances(), 2U);
  expect_same_retained(a.b(), b.b(), 0);
  expect_same_retained(a.b(), b.b(), 1);
  EXPECT_EQ(a.b().members(0, 0).size(), 3U);
  expect_same_rows(va, vb); // A's r1 view is unchanged by sessions 81..99
  EXPECT_EQ(a.b().effective_key(1), key(81));
  EXPECT_EQ(b.b().effective_key(1), 0);
}

TEST(DataPointInTimeUniverse, Case13_Determinism_TwoBuildersIdentical) {
  Feed a = fixture_long(100, {62U, 80U});
  Feed b = fixture_long(100, {62U, 80U});
  expect_same_retained(a.b(), b.b(), 0);
  expect_same_retained(a.b(), b.b(), 1);
  const std::string ea = require(encode_membership_bin(a.b()), "encode a");
  const std::string eb = require(encode_membership_bin(b.b()), "encode b");
  EXPECT_EQ(ea, eb);
  EXPECT_EQ(a.b().effective_key(0), key(63));
  EXPECT_EQ(a.b().effective_key(1), key(81));
}

TEST(DataPointInTimeUniverse, Case14_Id_LargeArchiveIds_AcceptedNonPositiveRejected) {
  Feed f(make_cfg(4, 3, {5}, {0}));
  f.feed(4, {bar(4'163'749, 30.0), bar(1'001'001'001'070, 20.0)});
  EXPECT_EQ(f.b().security_id(0), 4'163'749);
  EXPECT_EQ(f.b().security_id(1), 1'001'001'001'070);
  const PitRebalanceView v = f.rebalance_now();
  ASSERT_EQ(v.eligible, 2U);
  EXPECT_EQ(v.ranked[0].security_id, 4'163'749);
  EXPECT_EQ(v.ranked[1].security_id, 1'001'001'001'070);
  const Status zero = f.observe({bar(0, 1.0)});
  ASSERT_FALSE(zero.has_value());
  EXPECT_EQ(zero.error().code(), ErrorCode::OutOfRange);
  const Status negative = f.observe({bar(-1, 1.0)});
  ASSERT_FALSE(negative.has_value());
  EXPECT_EQ(negative.error().code(), ErrorCode::OutOfRange);
  EXPECT_EQ(f.b().source_ids(), 2U);
  EXPECT_EQ(f.b().sessions(), 4U);
}

TEST(DataPointInTimeUniverse, Case14b_IdTable_FullLoad_NoProbeEscape) {
  PitUniverseConfig c = make_cfg(4, 3, {5}, {0});
  c.max_source_ids = 8; // H = 32
  Feed f(c);
  std::vector<Bar> eight;
  for (atx::i64 id = 1; id <= 8; ++id) {
    eight.push_back(bar(id * 7919, 10.0)); // spread keys
  }
  require(f.observe(eight), "eight ids");
  EXPECT_EQ(f.b().source_ids(), 8U);
  std::vector<Bar> nine = eight;
  nine.push_back(bar(9 * 7919, 10.0));
  const Status st = f.observe(nine);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::OutOfRange);
  EXPECT_NE(st.error().message().find("max_source_ids"), std::string::npos);
  EXPECT_EQ(f.b().source_ids(), 8U);
  EXPECT_EQ(f.b().sessions(), 1U);
  require(f.observe(eight), "re-find");
  EXPECT_EQ(f.b().source_ids(), 8U);
  for (atx::u32 slot = 0; slot < 8; ++slot) {
    EXPECT_EQ(f.b().security_id(slot), static_cast<atx::i64>(slot + 1) * 7919);
    EXPECT_EQ(f.b().last_bar(slot), 1U);
  }
}

TEST(DataPointInTimeUniverse, Case15_Session_OutOfOrder_Rejected) {
  Feed f(make_cfg(4, 3, {5}, {0}));
  require(f.observe_key(key(1), {bar(1, 10.0)}, true), "first");
  const Status same = f.observe_key(key(1), {bar(1, 10.0)}, true);
  ASSERT_FALSE(same.has_value());
  EXPECT_EQ(same.error().code(), ErrorCode::InvalidArgument);
  const Status earlier = f.observe_key(key(0), {bar(1, 10.0)}, true);
  ASSERT_FALSE(earlier.has_value());
  EXPECT_EQ(earlier.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(f.b().sessions(), 1U);
  require(f.observe_key(key(2), {bar(1, 10.0)}, true), "next valid");
  EXPECT_EQ(f.b().sessions(), 2U);
}

TEST(DataPointInTimeUniverse, Case16_Session_DuplicateIdWithinCall_RejectedWithoutMutation) {
  {
    Feed f(make_cfg(4, 1, {5}, {0}));
    const Status st = f.observe({bar(5, 10.0), bar(7, 20.0), bar(5, 10.0)});
    ASSERT_FALSE(st.has_value());
    EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(f.b().source_ids(), 0U);
    EXPECT_EQ(f.b().sessions(), 0U);
    require(f.observe({bar(5, 10.0), bar(7, 20.0)}), "valid after rejection");
    Feed g(make_cfg(4, 1, {5}, {0}));
    require(g.observe({bar(5, 10.0), bar(7, 20.0)}), "fresh");
    EXPECT_EQ(f.b().source_ids(), g.b().source_ids());
    const PitRebalanceView vf = f.rebalance_now();
    const PitRebalanceView vg = g.rebalance_now();
    EXPECT_EQ(vf.ids_seen, vg.ids_seen);
    expect_same_rows(vf, vg);
    EXPECT_EQ(vf.ranked[0].security_id, 7);
  }
  {
    Feed f(make_cfg(4, 1, {5}, {0}));
    require(f.observe({bar(5, 10.0)}), "known");
    const Status st = f.observe({bar(5, 10.0), bar(7, 20.0), bar(5, 10.0)});
    ASSERT_FALSE(st.has_value());
    EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(f.b().source_ids(), 1U);
    EXPECT_EQ(f.b().sessions(), 1U);
  }
}

TEST(DataPointInTimeUniverse,
     Case16b_Observe_MaxSessionsMaxSourceIdsSpanMismatch_RejectedNoMutation) {
  {
    PitUniverseConfig c = make_cfg(4, 3, {5}, {0});
    c.max_sessions = 2;
    Feed f(c);
    f.feed(2, {bar(1, 10.0)});
    const Status st = f.observe({bar(1, 10.0)});
    ASSERT_FALSE(st.has_value());
    EXPECT_EQ(st.error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(f.b().sessions(), 2U);
  }
  {
    PitUniverseConfig c = make_cfg(4, 3, {5}, {0});
    c.max_source_ids = 2;
    Feed f(c);
    const Status st = f.observe({bar(1, 10.0), bar(2, 10.0), bar(3, 10.0)});
    ASSERT_FALSE(st.has_value());
    EXPECT_EQ(st.error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(f.b().source_ids(), 0U);
    EXPECT_EQ(f.b().sessions(), 0U);
    require(f.observe({bar(1, 10.0), bar(2, 10.0)}), "two fit");
    EXPECT_EQ(f.b().source_ids(), 2U);
  }
  {
    Feed f(make_cfg(4, 3, {5}, {0}));
    const std::vector<atx::i64> ids{1, 2};
    const std::vector<atx::f64> close{2.0, 2.0};
    const std::vector<atx::f64> volume{5.0};
    const Status st = f.b().observe_session(key(0), ids, close, volume, {}, {});
    ASSERT_FALSE(st.has_value());
    EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(f.b().source_ids(), 0U);
    EXPECT_EQ(f.b().sessions(), 0U);
  }
}

TEST(DataPointInTimeUniverse, Case16c_Observe_EmptySharesAndGics_ReadNaN) {
  Feed f(make_cfg(4, 3, {5}, {0}));
  for (atx::usize s = 0; s < 4; ++s) {
    require(f.observe({bar(1, 10.0, 10.0), bar(2, 20.0, 20.0)}, false), "no extras");
  }
  const PitRebalanceView v = f.rebalance_now();
  ASSERT_EQ(v.eligible, 2U);
  EXPECT_TRUE(std::isnan(v.ranked[0].vendor_market_cap_usd));
  EXPECT_TRUE(std::isnan(v.ranked[0].gics));
  EXPECT_TRUE(std::isnan(v.ranked[1].gics));
  EXPECT_EQ(v.gics_missing_members[0], 2U);
  EXPECT_EQ(v.churn[0].members, 2U);
}

TEST(DataPointInTimeUniverse, Case17_Rebalance_KeyNotLastObserved_Rejected) {
  Feed f(make_cfg(4, 3, {5}, {0}));
  f.feed(5, {bar(1, 10.0)});
  const Result<PitRebalanceView> stale = f.b().rebalance(key(3));
  ASSERT_FALSE(stale.has_value());
  EXPECT_EQ(stale.error().code(), ErrorCode::InvalidArgument);
  Feed g(make_cfg(4, 3, {5}, {0}));
  const Result<PitRebalanceView> none = g.b().rebalance(key(0));
  ASSERT_FALSE(none.has_value());
  EXPECT_EQ(none.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(f.b().rebalances(), 0U);
}

TEST(DataPointInTimeUniverse, Case17b_Rebalance_SameSessionTwiceOrBeyondMax_Rejected) {
  {
    Feed f(make_cfg(4, 3, {5}, {0}));
    f.feed(4, {bar(1, 10.0)});
    (void)f.rebalance_now();
    const Result<PitRebalanceView> again = f.rebalance_last();
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(f.b().rebalances(), 1U);
  }
  {
    PitUniverseConfig c = make_cfg(4, 3, {5}, {0});
    c.max_rebalances = 1;
    Feed f(c);
    f.feed(4, {bar(1, 10.0)});
    (void)f.rebalance_now();
    f.feed(1, {bar(1, 10.0)});
    const Result<PitRebalanceView> second = f.rebalance_last();
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code(), ErrorCode::OutOfRange);
  }
}

TEST(DataPointInTimeUniverse, Case18_Seal_SessionAtOrAfter2020_Rejected) {
  EXPECT_EQ(kPitSessionKeyEndExclusive, atx::engine::eval::kValidationBeginNs);
  Feed f(make_cfg(4, 3, {5}, {0}));
  const Status sealed = f.observe_key(kPitSessionKeyEndExclusive, {bar(1, 10.0)}, true);
  ASSERT_FALSE(sealed.has_value());
  EXPECT_EQ(sealed.error().code(), ErrorCode::PermissionDenied);
  EXPECT_EQ(f.b().sessions(), 0U);
  require(f.observe_key(kPitSessionKeyEndExclusive - kDay, {bar(1, 10.0)}, true), "2019-12-31");
  EXPECT_EQ(f.b().sessions(), 1U);
  EXPECT_EQ(pit_year_of(kPitSessionKeyEndExclusive - kDay), 2019);
  EXPECT_EQ(pit_year_of(kPitSessionKeyEndExclusive), 2020);
  EXPECT_EQ(pit_year_of(kBase), 2013);
  const PitCivilDate d = pit_civil_of(kBase);
  EXPECT_EQ(d.month, 1U);
  EXPECT_EQ(d.day, 2U);
}

TEST(DataPointInTimeUniverse, Case19_Create_OverflowingMaxima_RejectedBeforeAllocation) {
  {
    PitUniverseConfig c = make_cfg(4, 3, {5}, {0});
    c.max_source_ids = kPitMaxSourceIds + 1;
    const Result<PitUniverseBuilder> r = PitUniverseBuilder::create(c);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  }
  {
    PitUniverseConfig c = make_cfg(4, 3, {atx::usize{1} << 52}, {0, 1000});
    c.max_rebalances = kPitMaxRebalances; // 4096 * 2 * 2^52 = 2^65 overflows u64
    const Result<PitUniverseBuilder> r = PitUniverseBuilder::create(c);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
  }
  {
    PitUniverseConfig c = make_cfg(4, 3, {5}, {0});
    c.top_n_count = 0;
    const Result<PitUniverseBuilder> r = PitUniverseBuilder::create(c);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  }
  {
    PitUniverseConfig c = make_cfg(4, 3, {5, 5}, {0});
    const Result<PitUniverseBuilder> r = PitUniverseBuilder::create(c);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument); // not distinct
  }
}

TEST(DataPointInTimeUniverse, Case20_Coverage_ThreeYearFixture_ExactCounts) {
  Feed f = fixture_coverage();
  std::array<PitCoverageYear, kPitMaxYears> rows{};
  atx::usize n = 0;
  require(f.b().coverage_by_year(rows, n), "coverage");
  ASSERT_EQ(n, 3U);
  EXPECT_EQ(rows[0].year, 2013);
  EXPECT_EQ(rows[0].sessions, 4U);
  EXPECT_EQ(rows[0].ids_seen, 3U);
  EXPECT_EQ(rows[0].ids_with_valid_bar_median, 2.5);
  EXPECT_EQ(rows[0].rebalances, 1U);
  EXPECT_EQ(rows[0].eligible_median, 2.0);
  EXPECT_EQ(rows[0].members_median[0], 2.0);
  EXPECT_EQ(rows[0].gics_missing_members_median[0], 1.0);
  EXPECT_EQ(rows[0].nonmissing_fraction_median[0], 1.0);
  EXPECT_EQ(rows[1].year, 2014);
  EXPECT_EQ(rows[1].sessions, 4U);
  EXPECT_EQ(rows[1].ids_seen, 3U);
  EXPECT_EQ(rows[1].ids_with_valid_bar_median, 3.0);
  EXPECT_EQ(rows[1].rebalances, 1U);
  EXPECT_EQ(rows[1].eligible_median, 3.0);
  EXPECT_EQ(rows[1].members_median[0], 2.0);
  EXPECT_EQ(rows[1].gics_missing_members_median[0], 1.0);
  EXPECT_EQ(rows[2].year, 2015);
  EXPECT_EQ(rows[2].sessions, 4U);
  EXPECT_EQ(rows[2].ids_seen, 2U);
  EXPECT_EQ(rows[2].ids_with_valid_bar_median, 2.0);
  EXPECT_EQ(rows[2].rebalances, 1U);
  EXPECT_EQ(rows[2].eligible_median, 2.0);
  EXPECT_EQ(rows[2].members_median[0], 2.0);
  EXPECT_EQ(rows[2].gics_missing_members_median[0], 1.0);
  std::array<PitCoverageYear, 2> small{};
  atx::usize m = 0;
  const Status undersized = f.b().coverage_by_year(small, m);
  ASSERT_FALSE(undersized.has_value());
  EXPECT_EQ(undersized.error().code(), ErrorCode::OutOfRange);
}

TEST(DataPointInTimeUniverse, Case20b_Rebalance_ValidTotalAndGicsMissing_AreIntegers) {
  Feed f = fixture_valid_total();
  const PitRebalanceView v = f.rebalance_now();
  ASSERT_EQ(v.eligible, 3U);
  EXPECT_EQ(v.churn[0].members, 3U);
  EXPECT_EQ(v.valid_observations_total[0], 180U);
  EXPECT_EQ(v.gics_missing_members[0], 1U);
}

TEST(DataPointInTimeUniverse, Case21_Union_DistinctAndCumulative_ByEffectiveYear) {
  Feed f = fixture_coverage();
  std::array<PitUnionYear, kPitMaxYears> rows{};
  atx::usize n = 0;
  require(f.b().union_by_year(rows, n), "union");
  ASSERT_EQ(n, 2U);
  EXPECT_EQ(rows[0].year, 2014); // rank date 2013-01-05 -> effective 2014-01-02
  EXPECT_EQ(rows[0].distinct[0], 2U);
  EXPECT_EQ(rows[0].cumulative[0], 2U);
  EXPECT_EQ(rows[1].year, 2015);
  EXPECT_EQ(rows[1].distinct[0], 2U);
  EXPECT_EQ(rows[1].cumulative[0], 3U);
  EXPECT_GE(rows[1].cumulative[0], rows[0].cumulative[0]);
}

TEST(DataPointInTimeUniverse, Case22_Survivorship_ArchiveEndGuard_Censors) {
  Feed f = fixture_survivorship();
  const PitSurvivorship s = require(f.b().survivorship(), "survivorship");
  ASSERT_EQ(s.cuts, 1U);
  const PitSurvivorshipCut &c = s.cut[0];
  EXPECT_EQ(c.ever_members, 3U);
  EXPECT_EQ(c.ended_before_window_end, 1U); // D, last bar 2014-01-03
  EXPECT_EQ(c.censored, 2U);                // A, B: last bar == final session
  ASSERT_EQ(c.year_count, 2U); // 2013 is warmup: no rebalance is effective on its first session
  EXPECT_EQ(c.years[0], 2014);
  EXPECT_EQ(c.year_start_members[0], 2U);
  EXPECT_EQ(c.year_exits[0], 1U);
  EXPECT_EQ(c.years[1], 2015);
  EXPECT_EQ(c.year_start_members[1], 2U);
  EXPECT_EQ(c.year_exits[1], 0U);
  EXPECT_EQ(f.b().churn(1, 0).adds, 1U);
  EXPECT_EQ(f.b().churn(1, 0).drops_last_bar, 1U);
}

TEST(DataPointInTimeUniverse, Case23_Exits_KindFollowsLastDropNotLastBar) {
  Feed f = fixture_exits();
  std::array<PitExitRecord, 8> rows{};
  atx::usize n = 0;
  require(f.b().exits(0, rows, n), "exits");
  ASSERT_EQ(n, 3U);
  EXPECT_EQ(rows[2].security_id, 4);
  EXPECT_EQ(rows[2].exit_kind, PitExitKind::RankDrop);
  EXPECT_EQ(rows[2].last_bar, 7U); // 2014-01-05, after the 2014 rank drop
  EXPECT_EQ(rows[0].exit_kind, PitExitKind::WindowEnd);
  EXPECT_EQ(rows[1].exit_kind, PitExitKind::WindowEnd);
  EXPECT_EQ(f.b().churn(1, 0).drops_rank, 1U);
}

TEST(DataPointInTimeUniverse, Case23b_Exits_OrderedByIdWithFirstSeenFirstBarAndMemberRebalances) {
  Feed f = fixture_exits();
  std::array<PitExitRecord, 8> rows{};
  atx::usize n = 0;
  require(f.b().exits(0, rows, n), "exits");
  ASSERT_EQ(n, 3U);
  EXPECT_EQ(rows[0].security_id, 1);
  EXPECT_EQ(rows[1].security_id, 2);
  EXPECT_EQ(rows[2].security_id, 4);
  EXPECT_EQ(rows[0].first_seen, 0U);
  EXPECT_EQ(rows[0].first_bar, 1U);
  EXPECT_LT(rows[0].first_seen, rows[0].first_bar);
  EXPECT_EQ(rows[0].last_bar, 11U);
  EXPECT_EQ(rows[0].first_member_rebalance, 1U);
  EXPECT_EQ(rows[0].last_member_rebalance, 2U);
  EXPECT_EQ(rows[1].first_seen, 0U);
  EXPECT_EQ(rows[1].first_bar, 0U);
  EXPECT_EQ(rows[1].first_member_rebalance, 0U);
  EXPECT_EQ(rows[1].last_member_rebalance, 2U);
  EXPECT_EQ(rows[2].first_member_rebalance, 0U);
  EXPECT_EQ(rows[2].last_member_rebalance, 0U);
  std::array<PitExitRecord, 2> small{};
  atx::usize m = 0;
  const Status undersized = f.b().exits(0, small, m);
  ASSERT_FALSE(undersized.has_value());
  EXPECT_EQ(undersized.error().code(), ErrorCode::OutOfRange);
  const Status bad_cut = f.b().exits(1, rows, m);
  ASSERT_FALSE(bad_cut.has_value());
  EXPECT_EQ(bad_cut.error().code(), ErrorCode::InvalidArgument);
}

TEST(DataPointInTimeUniverse, Case24_VendorMarketCap_NaNOrNonPositiveShares_IsNaN) {
  Feed f(make_cfg(4, 3, {5}, {0}));
  for (atx::usize s = 0; s < 4; ++s) {
    require(f.observe({Bar{1, 2.0, 10.0, kNaN, kNaN}, Bar{2, 2.0, 10.0, 0.0, kNaN},
                       Bar{3, 2.0, 10.0, -1.0, kNaN}, Bar{4, 2.5, 10.0, 100.0, kNaN}}),
            "shares");
  }
  const PitRebalanceView v = f.rebalance_now();
  ASSERT_EQ(v.eligible, 4U);
  EXPECT_TRUE(std::isnan(find_ranked(v, 1)->vendor_market_cap_usd));
  EXPECT_TRUE(std::isnan(find_ranked(v, 2)->vendor_market_cap_usd));
  EXPECT_TRUE(std::isnan(find_ranked(v, 3)->vendor_market_cap_usd));
  EXPECT_EQ(find_ranked(v, 4)->vendor_market_cap_usd, 250.0);
}

TEST(DataPointInTimeUniverse, Case25_DuplicateDatesCoverage_2018Shape) {
  Feed f(make_cfg(63, 57, {5}, {0}));
  for (atx::usize s = 0; s < 63; ++s) {
    std::vector<Bar> bars{bar(1, 10.0), bar(2, 10.0)};
    if (s != 30) {
      bars.push_back(bar(3, 10.0));
      bars.push_back(bar(4, 10.0));
    }
    require(f.observe(bars), "2018 shape");
  }
  EXPECT_EQ(f.b().ids_with_valid_bar(29), 4U);
  EXPECT_EQ(f.b().ids_with_valid_bar(30), 2U);
  EXPECT_EQ(f.b().ids_with_valid_bar(31), 4U);
  const PitRebalanceView v = f.rebalance_now();
  ASSERT_EQ(v.eligible, 4U);
  EXPECT_EQ(find_ranked(v, 1)->valid_observations, 63U);
  EXPECT_EQ(find_ranked(v, 3)->valid_observations, 62U);
  EXPECT_EQ(find_ranked(v, 4)->valid_observations, 62U);
  std::array<PitCoverageYear, kPitMaxYears> rows{};
  atx::usize n = 0;
  require(f.b().coverage_by_year(rows, n), "coverage");
  ASSERT_EQ(n, 1U);
  EXPECT_EQ(rows[0].ids_seen, 4U); // missing, not unseen
  EXPECT_EQ(rows[0].sessions, 63U);
}

TEST(DataPointInTimeUniverse, Case27_MembershipBin_RoundTripAndRefusals) {
  Feed f = fixture_bin();
  const std::string bytes = require(encode_membership_bin(f.b()), "encode");
  EXPECT_EQ(bytes.size(), 204U);
  const PitMembershipImage img = require(decode_membership_bin(bytes), "decode");
  EXPECT_EQ(img.adv_window, 4U);
  EXPECT_EQ(img.min_valid_observations, 3U);
  EXPECT_EQ(img.min_raw_price_exclusive, 1.0);
  ASSERT_EQ(img.top_n.size(), 1U);
  EXPECT_EQ(img.top_n[0], 2U);
  ASSERT_EQ(img.band_bp.size(), 2U);
  EXPECT_EQ(img.band_bp[0], 0U);
  EXPECT_EQ(img.band_bp[1], 1000U);
  ASSERT_EQ(img.rebalances.size(), 2U);
  EXPECT_EQ(img.rebalances[0].rank_session_key, key(3));
  EXPECT_EQ(img.rebalances[0].effective_session_key, key(4));
  EXPECT_EQ(img.rebalances[1].rank_session_key, key(7));
  EXPECT_EQ(img.rebalances[1].effective_session_key, key(8));
  for (const PitMembershipRebalance &reb : img.rebalances) {
    ASSERT_EQ(reb.cuts.size(), 2U);
    for (const PitMembershipCut &cut : reb.cuts) {
      const std::vector<atx::i64> want_ids{1, 2};
      const std::vector<atx::u32> want_ranks{1U, 2U};
      EXPECT_EQ(cut.security_ids, want_ids);
      EXPECT_EQ(cut.ranks, want_ranks);
    }
  }
  EXPECT_EQ(img.fnv1a64, pit_fnv1a64(std::string_view{bytes}.substr(0, bytes.size() - 8)));
  {
    std::string bad = bytes;
    bad[0] = 'B';
    const Result<PitMembershipImage> r = decode_membership_bin(bad);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  }
  {
    std::string bad = bytes;
    bad[8] = 2;
    const Result<PitMembershipImage> r = decode_membership_bin(bad);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  }
  {
    const Result<PitMembershipImage> r =
        decode_membership_bin(std::string_view{bytes}.substr(0, 20));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  }
  {
    std::string bad = bytes;
    bad.back() = static_cast<char>(static_cast<unsigned char>(bad.back()) ^ 0x01u);
    const Result<PitMembershipImage> r = decode_membership_bin(bad);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::Internal);
  }
  {
    // A truncated body whose trailer is recomputed is a short read, not a mismatch.
    std::string cut = bytes.substr(0, bytes.size() - 8 - 4);
    const atx::u64 fnv = pit_fnv1a64(cut);
    for (atx::usize i = 0; i < 8; ++i) {
      cut.push_back(static_cast<char>((fnv >> (8 * i)) & 0xFFu));
    }
    const Result<PitMembershipImage> r = decode_membership_bin(cut);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  }
  // FNV-1a-64 pins: the empty string hashes to the offset basis; "a" to 0xaf63dc4c8601ec8c.
  EXPECT_EQ(pit_fnv1a64(""), 14695981039346656037ull);
  EXPECT_EQ(pit_fnv1a64("a"), 0xaf63dc4c8601ec8cull);
}

TEST(DataPointInTimeUniverse, Case28_SelectMonthlyRankSessions_LastByDataNeverLastAttached) {
  const std::vector<atx::i64> keys = cadence_keys();
  std::array<atx::i64, 8> out{};
  atx::usize n = 0;
  require(select_monthly_rank_sessions(keys, key(0) - kDay, key(88), out, n), "jan-mar");
  ASSERT_EQ(n, 2U);
  EXPECT_EQ(out[0], key(13)); // Jan 15
  EXPECT_EQ(out[1], key(56)); // Feb 27; Mar 1 is the last attached, never selected
  require(select_monthly_rank_sessions(keys, key(0) - kDay, key(55), out, n), "to feb 26");
  ASSERT_EQ(n, 1U);
  EXPECT_EQ(out[0], key(13));
  require(select_monthly_rank_sessions(keys, key(0) - kDay, key(13), out, n), "to jan 15");
  ASSERT_EQ(n, 1U);
  EXPECT_EQ(out[0], key(13));
  std::array<atx::i64, 1> tiny{};
  const Status small = select_monthly_rank_sessions(keys, key(0) - kDay, key(88), tiny, n);
  ASSERT_FALSE(small.has_value());
  EXPECT_EQ(small.error().code(), ErrorCode::OutOfRange);
  const std::vector<atx::i64> unsorted{key(1), key(0)};
  const Status bad = select_monthly_rank_sessions(unsorted, key(0), key(9), out, n);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code(), ErrorCode::InvalidArgument);
}

// ===========================================================================
//  Case 26 — §8.2 native export the oracle comparator consumes.
//
//  The oracle `build-equity/audits/iteration15-universe-oracle-v1.json` (T3) fixes
//  the case ids, the fixture encoding (`input_encoding`) and the value keys per
//  family; this section reproduces every one of its 42 fixtures through a generic
//  runner, prints one `POINT_IN_TIME_UNIVERSE_MEASUREMENT ` line per case with the
//  closed key list {schema, case_id, family, inputs, values}, and pins the `values`
//  JSON text against the oracle's expectation (integers exact, medians as `_x2`,
//  reals as shortest round-trip text with `.0` forced on integral values).
// ===========================================================================

[[nodiscard]] std::string jnum(atx::f64 v) {
  if (std::isnan(v)) {
    return "\"nan\"";
  }
  if (std::isinf(v)) {
    return v > 0.0 ? "\"inf\"" : "\"-inf\"";
  }
  std::array<char, 64> buf{};
  const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  std::string out(buf.data(), static_cast<std::size_t>(res.ptr - buf.data()));
  if (out.find_first_of(".e") == std::string::npos) {
    out += ".0"; // integral reals print as `.0` (DR15-5, cp14 convention)
  }
  return out;
}

[[nodiscard]] std::string jint(atx::usize v) { return std::to_string(v); }
[[nodiscard]] std::string ji64(atx::i64 v) { return std::to_string(v); }
[[nodiscard]] std::string ju64(atx::u64 v) { return std::to_string(v); }
[[nodiscard]] std::string jstr(std::string_view v) { return "\"" + std::string{v} + "\""; }

template <class T, class F> [[nodiscard]] std::string jlist(const std::vector<T> &values, F f) {
  std::string out = "[";
  for (atx::usize k = 0U; k < values.size(); ++k) {
    if (k != 0U) {
      out += ",";
    }
    out += f(values[k]);
  }
  return out + "]";
}

class Obj {
public:
  Obj &raw(const char *key_name, const std::string &value) {
    if (!body_.empty()) {
      body_ += ",";
    }
    body_ += "\"";
    body_ += key_name;
    body_ += "\":";
    body_ += value;
    return *this;
  }
  Obj &num(const char *key_name, atx::f64 value) { return raw(key_name, jnum(value)); }
  Obj &integer(const char *key_name, atx::usize value) { return raw(key_name, jint(value)); }
  Obj &int64(const char *key_name, atx::i64 value) { return raw(key_name, ji64(value)); }
  Obj &str(const char *key_name, std::string_view value) { return raw(key_name, jstr(value)); }
  [[nodiscard]] std::string json() const { return "{" + body_ + "}"; }

private:
  std::string body_;
};

// Whitespace-free copy of an expected-JSON literal (the literals below are wrapped
// to the column limit; the serializer never emits whitespace).
[[nodiscard]] std::string packed(std::string_view text) {
  std::string out;
  for (const char ch : text) {
    if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') {
      out.push_back(ch);
    }
  }
  return out;
}

void emit_and_check(const char *case_id, const char *family, const std::string &inputs_json,
                    const std::string &values_json, std::string_view expected_values) {
  std::cout << "POINT_IN_TIME_UNIVERSE_MEASUREMENT "
            << "{\"schema\":\"atx-point-in-time-universe-measurement-v1\",\"case_id\":\"" << case_id
            << "\",\"family\":\"" << family << "\",\"inputs\":" << inputs_json
            << ",\"values\":" << values_json << "}\n";
  EXPECT_EQ(values_json, packed(expected_values)) << case_id;
}

// ---------------------------------------------------------------------------
//  The oracle's fixture encoding (`input_encoding` of the oracle document).
// ---------------------------------------------------------------------------

struct OracleBar {
  atx::usize from;
  atx::usize to;
  atx::f64 close;
  atx::f64 volume;
};
struct OracleOverride {
  atx::usize ordinal;
  atx::f64 close;
  atx::f64 volume;
};
struct OracleName {
  atx::i64 id;
  std::vector<OracleBar> bars;
  std::vector<OracleOverride> overrides;
  std::vector<atx::usize> absent;
  atx::f64 shares;
  atx::f64 gics;
};
struct OracleFixture {
  atx::usize adv_window;
  atx::usize min_valid_observations;
  atx::f64 min_raw_price_exclusive;
  atx::f64 min_adv_usd;
  std::vector<atx::usize> top_n;
  std::vector<atx::u32> band_bp;
  std::string first_session_date; // empty when session_dates is used
  atx::usize session_count;
  std::vector<std::string> session_dates;
  std::vector<atx::usize> rank_sessions;
  std::vector<OracleName> names;
  atx::i64 target_security_id; // 0 = none (families F4+)
};

[[nodiscard]] OracleName oname(atx::i64 id, std::vector<OracleBar> bars, atx::f64 gics = kNaN) {
  return OracleName{id, std::move(bars), {}, {}, kNaN, gics};
}

// One 63-window fixture shape shared by F4/F5/F7/F8: bars [0,62] at volume `a` and
// [63,126] at volume `b`, close 2.0.
[[nodiscard]] OracleName two_phase(atx::i64 id, atx::f64 a, atx::f64 b) {
  return oname(id, {OracleBar{0, 62, 2.0, a}, OracleBar{63, 126, 2.0, b}});
}

[[nodiscard]] OracleFixture frozen_fixture(std::vector<atx::usize> top_n,
                                           std::vector<atx::u32> band_bp,
                                           atx::usize session_count,
                                           std::vector<atx::usize> rank_sessions,
                                           std::vector<OracleName> names,
                                           atx::i64 target = 0) {
  return OracleFixture{63,
                       57,
                       1.0,
                       0.0,
                       std::move(top_n),
                       std::move(band_bp),
                       "2013-01-02",
                       session_count,
                       {},
                       std::move(rank_sessions),
                       std::move(names),
                       target};
}

[[nodiscard]] OracleFixture short_window_fixture(std::vector<atx::usize> top_n,
                                                 std::vector<std::string> session_dates,
                                                 std::vector<atx::usize> rank_sessions,
                                                 std::vector<OracleName> names) {
  return OracleFixture{3,
                       2,
                       1.0,
                       0.0,
                       std::move(top_n),
                       {0U},
                       "",
                       0,
                       std::move(session_dates),
                       std::move(rank_sessions),
                       std::move(names),
                       0};
}

[[nodiscard]] atx::i64 date_key(std::string_view ymd) {
  // YYYY-MM-DD; the fixtures are all well-formed literals.
  const int y = std::stoi(std::string{ymd.substr(0, 4)});
  const int mo = std::stoi(std::string{ymd.substr(5, 2)});
  const int d = std::stoi(std::string{ymd.substr(8, 2)});
  return atx::core::time::days_from_civil(static_cast<atx::i32>(y), static_cast<atx::u32>(mo),
                                          static_cast<atx::u32>(d)) *
         kDay;
}

[[nodiscard]] std::vector<atx::i64> fixture_keys(const OracleFixture &fx) {
  std::vector<atx::i64> keys;
  if (!fx.session_dates.empty()) {
    for (const std::string &d : fx.session_dates) {
      keys.push_back(date_key(d));
    }
    return keys;
  }
  const atx::i64 first = date_key(fx.first_session_date);
  for (atx::usize s = 0; s < fx.session_count; ++s) {
    keys.push_back(first + static_cast<atx::i64>(s) * kDay);
  }
  return keys;
}

[[nodiscard]] std::string fixture_inputs_json(const OracleFixture &fx) {
  Obj config;
  config.integer("adv_window", fx.adv_window)
      .integer("min_valid_observations", fx.min_valid_observations)
      .num("min_raw_price_exclusive", fx.min_raw_price_exclusive)
      .num("min_adv_usd", fx.min_adv_usd)
      .raw("top_n", jlist(fx.top_n, jint))
      .raw("band_bp", jlist(fx.band_bp, [](atx::u32 b) { return jint(b); }));
  Obj in;
  in.raw("config", config.json());
  if (!fx.session_dates.empty()) {
    in.raw("session_dates", jlist(fx.session_dates, [](const std::string &d) { return jstr(d); }));
  } else {
    in.str("first_session_date", fx.first_session_date).integer("session_count", fx.session_count);
  }
  in.raw("rank_sessions", jlist(fx.rank_sessions, jint));
  std::string names = "[";
  for (atx::usize k = 0; k < fx.names.size(); ++k) {
    const OracleName &nm = fx.names[k];
    if (k != 0) {
      names += ",";
    }
    Obj n;
    n.int64("id", nm.id)
        .raw("bars", jlist(nm.bars,
                           [](const OracleBar &b) {
                             return "[" + jint(b.from) + "," + jint(b.to) + "," + jnum(b.close) +
                                    "," + jnum(b.volume) + "]";
                           }))
        .raw("overrides", jlist(nm.overrides,
                                [](const OracleOverride &o) {
                                  return "[" + jint(o.ordinal) + "," + jnum(o.close) + "," +
                                         jnum(o.volume) + "]";
                                }))
        .raw("absent", jlist(nm.absent, jint))
        .num("shares", nm.shares)
        .num("gics", nm.gics);
    names += n.json();
  }
  names += "]";
  in.raw("names", names);
  if (fx.target_security_id != 0) {
    in.int64("target_security_id", fx.target_security_id);
  }
  return in.json();
}

struct CutCapture {
  std::vector<atx::i64> members; // rank order
  std::vector<atx::u32> ranks;
  std::vector<atx::usize> status;
  std::vector<std::pair<atx::i64, atx::usize>> drops; // [security_id, kind], slot order
  PitChurn churn;
  atx::usize gics_missing;
  atx::u64 valid_total;
};
struct RebalanceCapture {
  std::vector<CutCapture> cuts;
  std::vector<atx::i64> ranked_ids;
  atx::usize eligible;
  bool target_eligible;
  atx::u32 target_rank;
  atx::f64 target_adv;
  atx::u32 target_valid;
};
struct OracleRun {
  Feed feed;
  std::vector<RebalanceCapture> rebalances;
};

[[nodiscard]] RebalanceCapture capture(const Feed &f, const PitRebalanceView &v, atx::i64 target) {
  RebalanceCapture cap{};
  cap.eligible = v.eligible;
  for (const PitRankedRow &row : v.ranked) {
    cap.ranked_ids.push_back(row.security_id);
  }
  for (atx::usize c = 0; c < v.members.size(); ++c) {
    CutCapture cut{};
    cut.members = member_ids(f, v, c);
    cut.ranks = member_rank_list(v, c);
    cut.status = member_status_list(v, c);
    for (const PitDropRow &d : v.drops[c]) {
      cut.drops.emplace_back(f.b().security_id(d.slot), static_cast<atx::usize>(d.kind));
    }
    cut.churn = v.churn[c];
    cut.gics_missing = v.gics_missing_members[c];
    cut.valid_total = v.valid_observations_total[c];
    cap.cuts.push_back(std::move(cut));
  }
  if (target != 0) {
    const PitRankedRow *row = find_ranked(v, target);
    cap.target_eligible = row != nullptr;
    if (row != nullptr) {
      cap.target_rank = row->rank;
      cap.target_adv = row->adv63_usd;
      cap.target_valid = row->valid_observations;
    }
  }
  return cap;
}

[[nodiscard]] OracleRun run_fixture(const OracleFixture &fx) {
  PitUniverseConfig c;
  c.adv_window = fx.adv_window;
  c.min_valid_observations = fx.min_valid_observations;
  c.min_raw_price_exclusive = fx.min_raw_price_exclusive;
  c.min_adv_usd = fx.min_adv_usd;
  c.top_n_count = fx.top_n.size();
  for (atx::usize i = 0; i < fx.top_n.size(); ++i) {
    c.top_n[i] = fx.top_n[i];
  }
  c.band_count = fx.band_bp.size();
  for (atx::usize i = 0; i < fx.band_bp.size(); ++i) {
    c.band_bp[i] = fx.band_bp[i];
  }
  c.max_source_ids = 64;
  c.max_rebalances = 16;
  c.max_sessions = 1024;
  OracleRun run{Feed(c), {}};
  const std::vector<atx::i64> keys = fixture_keys(fx);
  for (atx::usize s = 0; s < keys.size(); ++s) {
    std::vector<Bar> bars;
    for (const OracleName &nm : fx.names) {
      bool present = false;
      atx::f64 close = 0.0;
      atx::f64 volume = 0.0;
      for (const OracleBar &b : nm.bars) {
        if (b.from <= s && s <= b.to) {
          present = true;
          close = b.close;
          volume = b.volume;
        }
      }
      for (const OracleOverride &o : nm.overrides) {
        if (o.ordinal == s) {
          present = true;
          close = o.close;
          volume = o.volume;
        }
      }
      for (const atx::usize a : nm.absent) {
        if (a == s) {
          present = false;
        }
      }
      if (present) {
        bars.push_back(Bar{nm.id, close, volume, nm.shares, nm.gics});
      }
    }
    require(run.feed.observe_key(keys[s], bars, true), "oracle fixture session");
    for (const atx::usize r : fx.rank_sessions) {
      if (r == s) {
        const PitRebalanceView v = run.feed.rebalance_now();
        run.rebalances.push_back(capture(run.feed, v, fx.target_security_id));
      }
    }
  }
  return run;
}

// ---- family serializers ---------------------------------------------------------

[[nodiscard]] std::string f1_values(const RebalanceCapture &cap) {
  Obj v;
  v.integer("eligible", cap.target_eligible ? 1U : 0U)
      .integer("rank", cap.target_eligible ? cap.target_rank : 0U)
      .integer("eligible_count", cap.eligible)
      .raw("ranked_ids", jlist(cap.ranked_ids, ji64));
  if (cap.target_eligible) {
    v.num("adv63_usd", cap.target_adv).integer("valid_observations", cap.target_valid);
  }
  return v.json();
}

[[nodiscard]] std::string churn_json(const PitChurn &c) {
  Obj o;
  o.integer("adds", c.adds)
      .integer("drops_rank", c.drops_rank)
      .integer("drops_last_bar", c.drops_last_bar)
      .integer("kept", c.kept)
      .integer("members", c.members);
  return o.json();
}

[[nodiscard]] std::string membership_values(const OracleRun &run) {
  std::string rebs = "[";
  for (atx::usize r = 0; r < run.rebalances.size(); ++r) {
    const CutCapture &cut = run.rebalances[r].cuts[0];
    if (r != 0) {
      rebs += ",";
    }
    Obj o;
    o.raw("members", jlist(cut.members, ji64))
        .raw("ranks", jlist(cut.ranks, [](atx::u32 x) { return jint(x); }))
        .raw("status", jlist(cut.status, jint))
        .raw("drops", jlist(cut.drops,
                            [](const std::pair<atx::i64, atx::usize> &d) {
                              return "[" + ji64(d.first) + "," + jint(d.second) + "]";
                            }))
        .raw("churn", churn_json(cut.churn));
    rebs += o.json();
  }
  Obj v;
  v.raw("rebalances", rebs + "]");
  return v.json();
}

[[nodiscard]] atx::usize x2(atx::f64 median) { return static_cast<atx::usize>(median * 2.0); }

[[nodiscard]] std::string f6_values(const OracleRun &run) {
  const PitUniverseBuilder &b = run.feed.b();
  std::array<PitCoverageYear, kPitMaxYears> cov{};
  atx::usize ncov = 0;
  require(b.coverage_by_year(cov, ncov), "f6 coverage");
  std::string coverage = "[";
  for (atx::usize k = 0; k < ncov; ++k) {
    if (k != 0) {
      coverage += ",";
    }
    std::string cuts = "[";
    for (atx::usize c = 0; c < cov[k].cuts; ++c) {
      if (c != 0) {
        cuts += ",";
      }
      Obj cut;
      cut.integer("members_median_x2", x2(cov[k].members_median[c]))
          .integer("gics_missing_members_median_x2", x2(cov[k].gics_missing_members_median[c]));
      cuts += cut.json();
    }
    Obj row;
    row.integer("year", static_cast<atx::usize>(cov[k].year))
        .integer("sessions", cov[k].sessions)
        .integer("ids_seen", cov[k].ids_seen)
        .integer("ids_with_valid_bar_median_x2", x2(cov[k].ids_with_valid_bar_median))
        .integer("rebalances", cov[k].rebalances)
        .integer("eligible_median_x2", x2(cov[k].eligible_median))
        .raw("cuts", cuts + "]");
    coverage += row.json();
  }
  std::array<PitUnionYear, kPitMaxYears> uni{};
  atx::usize nuni = 0;
  require(b.union_by_year(uni, nuni), "f6 union");
  std::string union_rows = "[";
  for (atx::usize k = 0; k < nuni; ++k) {
    if (k != 0) {
      union_rows += ",";
    }
    std::string cuts = "[";
    for (atx::usize c = 0; c < uni[k].cuts; ++c) {
      if (c != 0) {
        cuts += ",";
      }
      Obj cut;
      cut.integer("distinct", uni[k].distinct[c]).integer("cumulative", uni[k].cumulative[c]);
      cuts += cut.json();
    }
    Obj row;
    row.integer("year", static_cast<atx::usize>(uni[k].year)).raw("cuts", cuts + "]");
    union_rows += row.json();
  }
  const PitSurvivorship s = require(b.survivorship(), "f6 survivorship");
  std::string surv_cuts = "[";
  for (atx::usize c = 0; c < s.cuts; ++c) {
    if (c != 0) {
      surv_cuts += ",";
    }
    std::string years = "[";
    for (atx::usize k = 0; k < s.cut[c].year_count; ++k) {
      if (k != 0) {
        years += ",";
      }
      Obj y;
      y.integer("year", static_cast<atx::usize>(s.cut[c].years[k]))
          .integer("members_at_first_rebalance", s.cut[c].year_start_members[k])
          .integer("exited_within_year", s.cut[c].year_exits[k]);
      years += y.json();
    }
    Obj cut;
    cut.integer("ever_members", s.cut[c].ever_members)
        .integer("ended_before_window_end", s.cut[c].ended_before_window_end)
        .integer("censored", s.cut[c].censored)
        .raw("per_year", years + "]");
    surv_cuts += cut.json();
  }
  Obj surv;
  surv.raw("cuts", surv_cuts + "]");
  std::string rebs = "[";
  for (atx::usize r = 0; r < run.rebalances.size(); ++r) {
    if (r != 0) {
      rebs += ",";
    }
    std::string cuts = "[";
    for (atx::usize c = 0; c < run.rebalances[r].cuts.size(); ++c) {
      if (c != 0) {
        cuts += ",";
      }
      const CutCapture &cut = run.rebalances[r].cuts[c];
      Obj o;
      o.integer("valid_observations_total", static_cast<atx::usize>(cut.valid_total))
          .integer("gics_missing_members", cut.gics_missing)
          .integer("members", cut.churn.members);
      cuts += o.json();
    }
    Obj o;
    o.raw("cuts", cuts + "]");
    rebs += o.json();
  }
  Obj v;
  v.raw("coverage_by_year", coverage + "]")
      .raw("union_by_year", union_rows + "]")
      .raw("survivorship", surv.json())
      .raw("rebalances", rebs + "]");
  return v.json();
}

[[nodiscard]] std::string hex_of(std::string_view bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  for (const char ch : bytes) {
    const auto b = static_cast<unsigned char>(ch);
    out.push_back(kDigits[b >> 4]);
    out.push_back(kDigits[b & 0x0Fu]);
  }
  return out;
}

[[nodiscard]] std::string f8_values(const OracleRun &run) {
  const std::string bytes = require(encode_membership_bin(run.feed.b()), "f8 encode");
  const std::string sha = require(atx::core::sha256_hex(std::string_view{bytes}), "f8 sha");
  const PitMembershipImage img = require(decode_membership_bin(bytes), "f8 decode");
  Obj v;
  v.str("sha256_hex", sha)
      .integer("byte_length", bytes.size())
      .raw("fnv1a64_trailer", ju64(img.fnv1a64))
      .integer("rebalance_count", img.rebalances.size())
      .str("header_bytes_hex", hex_of(std::string_view{bytes}.substr(0, 28)));
  return v.json();
}

[[nodiscard]] std::string f9_exit_values(const OracleRun &run) {
  std::array<PitExitRecord, 16> rows{};
  atx::usize n = 0;
  require(run.feed.b().exits(0, rows, n), "f9 exits");
  std::string exits = "[";
  for (atx::usize k = 0; k < n; ++k) {
    if (k != 0) {
      exits += ",";
    }
    Obj o;
    o.int64("security_id", rows[k].security_id)
        .integer("first_seen", rows[k].first_seen)
        .integer("first_bar", rows[k].first_bar)
        .integer("last_bar", rows[k].last_bar)
        .integer("first_member_rebalance", rows[k].first_member_rebalance)
        .integer("last_member_rebalance", rows[k].last_member_rebalance)
        .integer("exit_kind", static_cast<atx::usize>(rows[k].exit_kind));
    exits += o.json();
  }
  Obj v;
  v.raw("exits", exits + "]");
  return v.json();
}

// ---- the oracle fixtures --------------------------------------------------------

[[nodiscard]] std::vector<OracleBar> single_bars(atx::usize from, atx::usize to, atx::f64 close,
                                                  atx::f64 volume) {
  return {OracleBar{from, to, close, volume}};
}

[[nodiscard]] std::vector<std::string> twelve_dates() {
  return {"2013-01-02", "2013-01-03", "2013-06-28", "2013-12-31", "2014-01-02", "2014-01-03",
          "2014-06-30", "2014-12-31", "2015-01-02", "2015-01-05", "2015-06-30", "2015-12-31"};
}

constexpr std::string_view kIneligible = R"({"eligible":0,"rank":0,"eligible_count":0,
  "ranked_ids":[]})";

void run_f1_f2() {
  {
    OracleFixture fx =
        frozen_fixture({1}, {0}, 73, {72}, {oname(30, single_bars(10, 72, 2.0, 100.0))}, 30);
    const OracleRun run = run_fixture(fx);
    emit_and_check("f1_late_entrant_rank72_63valid_eligible", "F1", fixture_inputs_json(fx),
                   f1_values(run.rebalances[0]),
                   R"({"eligible":1,"rank":1,"eligible_count":1,"ranked_ids":[30],
                       "adv63_usd":200.0,"valid_observations":63})");
  }
  {
    OracleFixture fx =
        frozen_fixture({1}, {0}, 72, {71}, {oname(30, single_bars(10, 71, 2.0, 100.0))}, 30);
    const OracleRun run = run_fixture(fx);
    emit_and_check("f1_late_entrant_rank71_62valid_eligible", "F1", fixture_inputs_json(fx),
                   f1_values(run.rebalances[0]),
                   R"({"eligible":1,"rank":1,"eligible_count":1,"ranked_ids":[30],
                       "adv63_usd":200.0,"valid_observations":62})");
  }
  {
    OracleFixture fx =
        frozen_fixture({1}, {0}, 66, {65}, {oname(30, single_bars(10, 65, 2.0, 100.0))}, 30);
    const OracleRun run = run_fixture(fx);
    emit_and_check("f1_late_entrant_rank65_56valid_ineligible", "F1", fixture_inputs_json(fx),
                   f1_values(run.rebalances[0]), kIneligible);
  }
  {
    OracleName seven = oname(7, single_bars(0, 62, 2.0, 100.0));
    seven.absent = {5, 12, 19, 26, 33, 40, 47};
    OracleFixture fx = frozen_fixture({1}, {0}, 63, {62}, {seven}, 7);
    const OracleRun run = run_fixture(fx);
    emit_and_check("f1_gap_seven_missing_56_ineligible", "F1", fixture_inputs_json(fx),
                   f1_values(run.rebalances[0]), kIneligible);
  }
  {
    std::vector<OracleBar> bars;
    for (atx::usize s = 0; s < 63; ++s) {
      bars.push_back(OracleBar{s, s, static_cast<atx::f64>(s) + 2.0, 1.0});
    }
    OracleName seven = oname(7, bars);
    seven.absent = {5, 12, 19, 26, 33, 40};
    OracleFixture fx = frozen_fixture({1}, {0}, 63, {62}, {seven}, 7);
    const OracleRun run = run_fixture(fx);
    emit_and_check("f1_gap_six_missing_57_eligible_median_of_57", "F1", fixture_inputs_json(fx),
                   f1_values(run.rebalances[0]),
                   R"({"eligible":1,"rank":1,"eligible_count":1,"ranked_ids":[7],
                       "adv63_usd":34.0,"valid_observations":57})");
  }
  {
    constexpr atx::f64 kInf = std::numeric_limits<atx::f64>::infinity();
    OracleName eleven = oname(11, single_bars(0, 62, 2.0, 100.0));
    eleven.overrides = {OracleOverride{1, kNaN, 100.0}, OracleOverride{2, kInf, 100.0},
                        OracleOverride{3, 0.0, 100.0},  OracleOverride{4, -1.0, 100.0},
                        OracleOverride{5, 2.0, -1.0},   OracleOverride{6, 2.0, kNaN},
                        OracleOverride{7, 2.0, 0.0}};
    OracleFixture fx = frozen_fixture({1}, {0}, 63, {62}, {eleven}, 11);
    const OracleRun run = run_fixture(fx);
    emit_and_check("f1_bar_validity_every_invalid_shape_is_missing", "F1",
                   fixture_inputs_json(fx), f1_values(run.rebalances[0]),
                   R"({"eligible":1,"rank":1,"eligible_count":1,"ranked_ids":[11],
                       "adv63_usd":200.0,"valid_observations":57})");
  }
  {
    OracleFixture fx = frozen_fixture(
        {2}, {0}, 63, {62},
        {oname(1, single_bars(0, 62, 2.0, 0.0)), oname(2, single_bars(0, 62, 2.0, 100.0))}, 1);
    const OracleRun run = run_fixture(fx);
    emit_and_check("f1_zero_volume_adv_zero_eligible_ranks_last", "F1", fixture_inputs_json(fx),
                   f1_values(run.rebalances[0]),
                   R"({"eligible":1,"rank":2,"eligible_count":2,"ranked_ids":[2,1],
                       "adv63_usd":0.0,"valid_observations":63})");
  }
  {
    std::vector<OracleBar> bars;
    for (atx::usize s = 5; s < 63; ++s) {
      bars.push_back(OracleBar{s, s, 2.0, static_cast<atx::f64>(s - 4) * 0.5});
    }
    OracleFixture fx = frozen_fixture({1}, {0}, 63, {62}, {oname(4, bars)}, 4);
    const OracleRun run = run_fixture(fx);
    emit_and_check("f1_median_even_58_is_half_sum", "F1", fixture_inputs_json(fx),
                   f1_values(run.rebalances[0]),
                   R"({"eligible":1,"rank":1,"eligible_count":1,"ranked_ids":[4],
                       "adv63_usd":29.5,"valid_observations":58})");
  }
  {
    std::vector<OracleBar> bars;
    for (atx::usize s = 6; s < 63; ++s) {
      bars.push_back(OracleBar{s, s, 2.0, static_cast<atx::f64>(s - 5) * 0.5});
    }
    OracleFixture fx = frozen_fixture({1}, {0}, 63, {62}, {oname(4, bars)}, 4);
    const OracleRun run = run_fixture(fx);
    emit_and_check("f1_median_odd_57_is_middle", "F1", fixture_inputs_json(fx),
                   f1_values(run.rebalances[0]),
                   R"({"eligible":1,"rank":1,"eligible_count":1,"ranked_ids":[4],
                       "adv63_usd":29.0,"valid_observations":57})");
  }
  {
    OracleFixture fx = frozen_fixture({3}, {0}, 63, {62},
                                      {oname(30, single_bars(0, 62, 2.0, 100.0)),
                                       oname(10, single_bars(0, 62, 2.0, 100.0)),
                                       oname(20, single_bars(0, 62, 2.0, 100.0))},
                                      10);
    const OracleRun run = run_fixture(fx);
    emit_and_check("f1_ties_break_by_first_seen_slot_ascending", "F1", fixture_inputs_json(fx),
                   f1_values(run.rebalances[0]),
                   R"({"eligible":1,"rank":2,"eligible_count":3,"ranked_ids":[30,10,20],
                       "adv63_usd":200.0,"valid_observations":63})");
  }
  // ---- F2 ----
  {
    OracleFixture fx =
        frozen_fixture({1}, {0}, 63, {62}, {oname(6, single_bars(0, 62, 1.0, 100.0))}, 6);
    const OracleRun run = run_fixture(fx);
    emit_and_check("f2_price_floor_close_exactly_one_ineligible", "F2", fixture_inputs_json(fx),
                   f1_values(run.rebalances[0]), kIneligible);
  }
  {
    OracleFixture fx = frozen_fixture(
        {1}, {0}, 63, {62}, {oname(6, single_bars(0, 62, std::nextafter(1.0, 2.0), 100.0))}, 6);
    const OracleRun run = run_fixture(fx);
    emit_and_check("f2_price_floor_nextafter_one_eligible", "F2", fixture_inputs_json(fx),
                   f1_values(run.rebalances[0]),
                   R"({"eligible":1,"rank":1,"eligible_count":1,"ranked_ids":[6],
                       "adv63_usd":100.00000000000003,"valid_observations":63})");
  }
  {
    OracleFixture fx =
        frozen_fixture({1}, {0}, 64, {63}, {oname(6, single_bars(0, 62, 2.0, 100.0))}, 6);
    const OracleRun run = run_fixture(fx);
    emit_and_check("f2_absent_on_rank_session_ineligible", "F2", fixture_inputs_json(fx),
                   f1_values(run.rebalances[0]), kIneligible);
  }
}

void run_f3_case(const char *case_id, const std::vector<std::string> &dates, const char *start,
                 const char *end, std::string_view expected) {
  std::vector<atx::i64> keys;
  for (const std::string &d : dates) {
    keys.push_back(date_key(d));
  }
  std::array<atx::i64, 8> out{};
  atx::usize n = 0;
  require(select_monthly_rank_sessions(keys, date_key(start), date_key(end), out, n), case_id);
  std::vector<atx::i64> ranks(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n));
  std::vector<atx::i64> effective;
  for (const atx::i64 rk : ranks) {
    for (atx::usize i = 0; i + 1 < keys.size(); ++i) {
      if (keys[i] == rk) {
        effective.push_back(keys[i + 1]);
      }
    }
  }
  Obj in;
  in.raw("session_dates", jlist(dates, [](const std::string &d) { return jstr(d); }))
      .str("start_date", start)
      .str("end_date", end);
  Obj v;
  v.raw("rank_session_keys", jlist(ranks, ji64))
      .raw("effective_session_keys", jlist(effective, ji64))
      .integer("rank_session_count", n);
  emit_and_check(case_id, "F3", in.json(), v.json(), expected);
}

void run_f3() {
  const std::vector<std::string> a{"2013-01-02", "2013-01-15", "2013-02-03", "2013-02-27",
                                   "2013-03-01"};
  run_f3_case("f3_last_by_data_mid_month_never_last_attached", a, "2013-01-01", "2013-03-31",
              R"({"rank_session_keys":[1358208000000000000,1361923200000000000],
                  "effective_session_keys":[1359849600000000000,1362096000000000000],
                  "rank_session_count":2})");
  run_f3_case("f3_end_filter_excludes_feb27", a, "2013-01-01", "2013-02-26",
              R"({"rank_session_keys":[1358208000000000000],
                  "effective_session_keys":[1359849600000000000],"rank_session_count":1})");
  run_f3_case("f3_end_filter_inclusive_on_jan15", a, "2013-01-01", "2013-01-15",
              R"({"rank_session_keys":[1358208000000000000],
                  "effective_session_keys":[1359849600000000000],"rank_session_count":1})");
  run_f3_case("f3_start_filter_excludes_jan15", a, "2013-01-16", "2013-03-31",
              R"({"rank_session_keys":[1361923200000000000],
                  "effective_session_keys":[1362096000000000000],"rank_session_count":1})");
  const std::vector<std::string> b{"2013-01-30", "2013-01-31", "2013-02-03", "2013-02-28",
                                   "2013-03-04"};
  run_f3_case("f3_effective_is_next_observed_session_weekend_gap", b, "2013-01-01", "2013-02-28",
              R"({"rank_session_keys":[1359590400000000000,1362009600000000000],
                  "effective_session_keys":[1359849600000000000,1362355200000000000],
                  "rank_session_count":2})");
}

// The twelve-name F4 shape: ids 1..12, volumes a/b per id (close 2.0).
[[nodiscard]] std::vector<OracleName> band_names(atx::f64 ten_b, atx::f64 eleven_b) {
  return {two_phase(1, 600.0, 600.0),  two_phase(2, 550.0, 550.0),    two_phase(3, 500.0, 500.0),
          two_phase(4, 450.0, 450.0),  two_phase(5, 400.0, 400.0),    two_phase(6, 350.0, 350.0),
          two_phase(7, 300.0, 300.0),  two_phase(8, 250.0, 250.0),    two_phase(9, 200.0, 200.0),
          two_phase(10, 150.0, ten_b), two_phase(11, 100.0, eleven_b), two_phase(12, 50.0, 50.0)};
}

constexpr std::string_view kR0TenAdds =
    R"({"members":[1,2,3,4,5,6,7,8,9,10],"ranks":[1,2,3,4,5,6,7,8,9,10],
        "status":[0,0,0,0,0,0,0,0,0,0],"drops":[],
        "churn":{"adds":10,"drops_rank":0,"drops_last_bar":0,"kept":0,"members":10}})";

[[nodiscard]] std::string two_rebalances(std::string_view r0, std::string_view r1) {
  return "{\"rebalances\":[" + std::string{r0} + "," + std::string{r1} + "]}";
}

void run_f4() {
  {
    OracleFixture fx = frozen_fixture({10}, {1000}, 127, {62, 125}, band_names(100.0, 125.0));
    emit_and_check("f4_band1000_top10_incumbent_at_rank11_kept", "F4", fixture_inputs_json(fx),
                   membership_values(run_fixture(fx)),
                   two_rebalances(kR0TenAdds,
                                  R"({"members":[1,2,3,4,5,6,7,8,9,10],
                                      "ranks":[1,2,3,4,5,6,7,8,9,11],
                                      "status":[1,1,1,1,1,1,1,1,1,1],"drops":[],
                                      "churn":{"adds":0,"drops_rank":0,"drops_last_bar":0,
                                               "kept":10,"members":10}})"));
  }
  {
    OracleFixture fx = frozen_fixture({10}, {1000}, 127, {62, 125}, band_names(25.0, 125.0));
    emit_and_check("f4_band1000_top10_incumbent_at_rank12_dropped", "F4",
                   fixture_inputs_json(fx), membership_values(run_fixture(fx)),
                   two_rebalances(kR0TenAdds,
                                  R"({"members":[1,2,3,4,5,6,7,8,9,11],
                                      "ranks":[1,2,3,4,5,6,7,8,9,10],
                                      "status":[1,1,1,1,1,1,1,1,1,0],"drops":[[10,0]],
                                      "churn":{"adds":1,"drops_rank":1,"drops_last_bar":0,
                                               "kept":9,"members":10}})"));
  }
  {
    OracleFixture fx = frozen_fixture({10}, {0}, 127, {62, 125}, band_names(100.0, 125.0));
    emit_and_check("f4_band0_top10_incumbent_at_rank11_dropped", "F4", fixture_inputs_json(fx),
                   membership_values(run_fixture(fx)),
                   two_rebalances(kR0TenAdds,
                                  R"({"members":[1,2,3,4,5,6,7,8,9,11],
                                      "ranks":[1,2,3,4,5,6,7,8,9,10],
                                      "status":[1,1,1,1,1,1,1,1,1,0],"drops":[[10,0]],
                                      "churn":{"adds":1,"drops_rank":1,"drops_last_bar":0,
                                               "kept":9,"members":10}})"));
  }
  {
    OracleFixture fx = frozen_fixture(
        {2}, {1000}, 127, {62, 125},
        {two_phase(1, 150.0, 100.0), two_phase(2, 100.0, 50.0), two_phase(3, 50.0, 150.0)});
    emit_and_check("f4_band1000_top2_kband_is_two", "F4", fixture_inputs_json(fx),
                   membership_values(run_fixture(fx)),
                   two_rebalances(R"({"members":[1,2],"ranks":[1,2],"status":[0,0],"drops":[],
                                      "churn":{"adds":2,"drops_rank":0,"drops_last_bar":0,
                                               "kept":0,"members":2}})",
                                  R"({"members":[3,1],"ranks":[1,2],"status":[0,1],
                                      "drops":[[2,0]],
                                      "churn":{"adds":1,"drops_rank":1,"drops_last_bar":0,
                                               "kept":1,"members":2}})"));
  }
}

constexpr std::string_view kR0ThreeAdds =
    R"({"members":[1,2,3],"ranks":[1,2,3],"status":[0,0,0],"drops":[],
        "churn":{"adds":3,"drops_rank":0,"drops_last_bar":0,"kept":0,"members":3}})";

void run_f5() {
  const auto four = [](OracleName one) {
    return std::vector<OracleName>{std::move(one), two_phase(2, 150.0, 150.0),
                                   two_phase(3, 100.0, 100.0), two_phase(4, 50.0, 50.0)};
  };
  {
    OracleName one = two_phase(1, 200.0, 200.0);
    one.absent = {125};
    OracleFixture fx = frozen_fixture({3}, {0}, 127, {62, 125}, four(one));
    emit_and_check("f5_incumbent_absent_on_rank_session_drops_last_bar", "F5",
                   fixture_inputs_json(fx), membership_values(run_fixture(fx)),
                   two_rebalances(kR0ThreeAdds,
                                  R"({"members":[2,3,4],"ranks":[1,2,3],"status":[1,1,0],
                                      "drops":[[1,1]],
                                      "churn":{"adds":1,"drops_rank":0,"drops_last_bar":1,
                                               "kept":2,"members":3}})"));
  }
  {
    OracleName one = two_phase(1, 200.0, 200.0);
    one.overrides = {OracleOverride{125, kNaN, 200.0}};
    OracleFixture fx = frozen_fixture({3}, {0}, 127, {62, 125}, four(one));
    emit_and_check("f5_incumbent_close_nan_on_rank_session_drops_last_bar", "F5",
                   fixture_inputs_json(fx), membership_values(run_fixture(fx)),
                   two_rebalances(kR0ThreeAdds,
                                  R"({"members":[2,3,4],"ranks":[1,2,3],"status":[1,1,0],
                                      "drops":[[1,1]],
                                      "churn":{"adds":1,"drops_rank":0,"drops_last_bar":1,
                                               "kept":2,"members":3}})"));
  }
  {
    OracleName one = two_phase(1, 200.0, 200.0);
    one.overrides = {OracleOverride{125, 0.5, 200.0}};
    OracleFixture fx = frozen_fixture({3}, {0}, 127, {62, 125}, four(one));
    emit_and_check("f5_incumbent_close_half_on_rank_session_drops_rank", "F5",
                   fixture_inputs_json(fx), membership_values(run_fixture(fx)),
                   two_rebalances(kR0ThreeAdds,
                                  R"({"members":[2,3,4],"ranks":[1,2,3],"status":[1,1,0],
                                      "drops":[[1,0]],
                                      "churn":{"adds":1,"drops_rank":1,"drops_last_bar":0,
                                               "kept":2,"members":3}})"));
  }
  {
    OracleName one = two_phase(1, 300.0, 300.0);
    one.absent = {125};
    OracleName two = two_phase(2, 250.0, 250.0);
    two.overrides = {OracleOverride{125, kNaN, 250.0}};
    OracleName three = two_phase(3, 200.0, 200.0);
    three.overrides = {OracleOverride{125, 0.5, 200.0}};
    OracleFixture fx = frozen_fixture({3}, {0}, 127, {62, 125},
                                      {one, two, three, two_phase(4, 150.0, 150.0),
                                       two_phase(5, 100.0, 100.0), two_phase(6, 50.0, 50.0)});
    emit_and_check("f5_three_shapes_one_rebalance_drops_slot_ascending", "F5",
                   fixture_inputs_json(fx), membership_values(run_fixture(fx)),
                   two_rebalances(kR0ThreeAdds,
                                  R"({"members":[4,5,6],"ranks":[1,2,3],"status":[0,0,0],
                                      "drops":[[1,1],[2,1],[3,0]],
                                      "churn":{"adds":3,"drops_rank":1,"drops_last_bar":2,
                                               "kept":0,"members":3}})"));
  }
}

void run_f6() {
  {
    OracleName n105 = oname(105, single_bars(0, 11, 10.0, 7.0), 35.0);
    n105.overrides = {OracleOverride{7, 0.5, 140.0}};
    OracleName n106 = oname(106, single_bars(0, 11, 10.0, 6.0), 35.0);
    n106.overrides = {OracleOverride{10, 0.5, 120.0}};
    OracleName n108 = oname(108, single_bars(1, 11, 10.0, 5.0));
    n108.overrides = {OracleOverride{1, kNaN, 5.0}};
    OracleFixture fx = short_window_fixture(
        {3, 7}, twelve_dates(), {3, 7, 10},
        {oname(101, single_bars(0, 11, 10.0, 10.0), 45.0),
         oname(102, single_bars(0, 11, 10.0, 9.0)), oname(103, single_bars(0, 3, 10.0, 8.0), 20.0),
         oname(104, single_bars(5, 11, 10.0, 9.5)), n105, n106,
         oname(107, single_bars(0, 9, 10.0, 4.0)), n108});
    emit_and_check(
        "f6_three_year_panel_coverage_union_survivorship", "F6", fixture_inputs_json(fx),
        f6_values(run_fixture(fx)),
        R"({"coverage_by_year":[
              {"year":2013,"sessions":4,"ids_seen":7,"ids_with_valid_bar_median_x2":13,
               "rebalances":1,"eligible_median_x2":14,
               "cuts":[{"members_median_x2":6,"gics_missing_members_median_x2":2},
                       {"members_median_x2":14,"gics_missing_members_median_x2":6}]},
              {"year":2014,"sessions":4,"ids_seen":7,"ids_with_valid_bar_median_x2":14,
               "rebalances":1,"eligible_median_x2":12,
               "cuts":[{"members_median_x2":6,"gics_missing_members_median_x2":4},
                       {"members_median_x2":12,"gics_missing_members_median_x2":8}]},
              {"year":2015,"sessions":4,"ids_seen":7,"ids_with_valid_bar_median_x2":13,
               "rebalances":1,"eligible_median_x2":10,
               "cuts":[{"members_median_x2":6,"gics_missing_members_median_x2":4},
                       {"members_median_x2":10,"gics_missing_members_median_x2":6}]}],
            "union_by_year":[
              {"year":2014,"cuts":[{"distinct":3,"cumulative":3},{"distinct":7,"cumulative":7}]},
              {"year":2015,"cuts":[{"distinct":3,"cumulative":4},{"distinct":7,"cumulative":8}]}],
            "survivorship":{"cuts":[
              {"ever_members":4,"ended_before_window_end":1,"censored":3,
               "per_year":[{"year":2014,"members_at_first_rebalance":3,"exited_within_year":1},
                           {"year":2015,"members_at_first_rebalance":3,"exited_within_year":0}]},
              {"ever_members":8,"ended_before_window_end":2,"censored":6,
               "per_year":[{"year":2014,"members_at_first_rebalance":7,"exited_within_year":1},
                           {"year":2015,"members_at_first_rebalance":6,"exited_within_year":1}]}]},
            "rebalances":[
              {"cuts":[{"valid_observations_total":9,"gics_missing_members":1,"members":3},
                       {"valid_observations_total":20,"gics_missing_members":3,"members":7}]},
              {"cuts":[{"valid_observations_total":9,"gics_missing_members":2,"members":3},
                       {"valid_observations_total":18,"gics_missing_members":4,"members":6}]},
              {"cuts":[{"valid_observations_total":9,"gics_missing_members":2,"members":3},
                       {"valid_observations_total":15,"gics_missing_members":3,"members":5}]}]})");
  }
  {
    OracleName one = oname(1, single_bars(0, 3, 10.0, 30.0));
    one.absent = {1};
    OracleName two = oname(2, single_bars(0, 3, 10.0, 20.0));
    two.absent = {1};
    OracleFixture fx =
        short_window_fixture({3}, {"2013-01-02", "2013-01-03", "2013-06-28", "2013-12-31"}, {3},
                             {one, two, oname(3, single_bars(0, 3, 10.0, 10.0))});
    emit_and_check(
        "f6_absent_ids_thin_one_session_not_unseen", "F6", fixture_inputs_json(fx),
        f6_values(run_fixture(fx)),
        R"({"coverage_by_year":[{"year":2013,"sessions":4,"ids_seen":3,
              "ids_with_valid_bar_median_x2":6,"rebalances":1,"eligible_median_x2":6,
              "cuts":[{"members_median_x2":6,"gics_missing_members_median_x2":6}]}],
            "union_by_year":[],
            "survivorship":{"cuts":[{"ever_members":3,"ended_before_window_end":0,"censored":3,
                                     "per_year":[]}]},
            "rebalances":[{"cuts":[{"valid_observations_total":7,"gics_missing_members":3,
                                    "members":3}]}]})");
  }
  {
    OracleFixture fx = frozen_fixture({3}, {0}, 63, {62},
                                      {oname(1, single_bars(0, 62, 2.0, 100.0), 10.0),
                                       oname(2, single_bars(3, 62, 2.0, 50.0), 20.0),
                                       oname(3, single_bars(6, 62, 2.0, 25.0))});
    emit_and_check(
        "f6_valid_total_and_gics_missing_are_integers", "F6", fixture_inputs_json(fx),
        f6_values(run_fixture(fx)),
        R"({"coverage_by_year":[{"year":2013,"sessions":63,"ids_seen":3,
              "ids_with_valid_bar_median_x2":6,"rebalances":1,"eligible_median_x2":6,
              "cuts":[{"members_median_x2":6,"gics_missing_members_median_x2":2}]}],
            "union_by_year":[],
            "survivorship":{"cuts":[{"ever_members":3,"ended_before_window_end":0,"censored":3,
                                     "per_year":[]}]},
            "rebalances":[{"cuts":[{"valid_observations_total":180,"gics_missing_members":1,
                                    "members":3}]}]})");
  }
}

void run_f7() {
  {
    OracleFixture fx = frozen_fixture(
        {2}, {1000}, 127, {62, 125},
        {two_phase(1, 150.0, 100.0), two_phase(2, 100.0, 50.0), two_phase(3, 50.0, 150.0)});
    emit_and_check("f7_top2_band1000_outranked_incumbent_dropped_as_rank", "F7",
                   fixture_inputs_json(fx), membership_values(run_fixture(fx)),
                   two_rebalances(R"({"members":[1,2],"ranks":[1,2],"status":[0,0],"drops":[],
                                      "churn":{"adds":2,"drops_rank":0,"drops_last_bar":0,
                                               "kept":0,"members":2}})",
                                  R"({"members":[3,1],"ranks":[1,2],"status":[0,1],
                                      "drops":[[2,0]],
                                      "churn":{"adds":1,"drops_rank":1,"drops_last_bar":0,
                                               "kept":1,"members":2}})"));
  }
  {
    OracleFixture fx = frozen_fixture({3}, {1000}, 127, {62, 125},
                                      {two_phase(1, 200.0, 100.0), two_phase(2, 150.0, 200.0),
                                       two_phase(3, 100.0, 150.0), two_phase(4, 50.0, 75.0)});
    emit_and_check("f7_top3_band1000_all_incumbents_kept_rank4_entrant_not_added", "F7",
                   fixture_inputs_json(fx), membership_values(run_fixture(fx)),
                   two_rebalances(kR0ThreeAdds,
                                  R"({"members":[2,3,1],"ranks":[1,2,3],"status":[1,1,1],
                                      "drops":[],
                                      "churn":{"adds":0,"drops_rank":0,"drops_last_bar":0,
                                               "kept":3,"members":3}})"));
  }
  {
    const std::array<atx::f64, 20> a{950.0, 900.0, 850.0, 800.0, 750.0, 700.0, 650.0,
                                     600.0, 550.0, 500.0, 475.0, 450.0, 425.0, 400.0,
                                     375.0, 350.0, 325.0, 300.0, 275.0, 250.0};
    const std::array<atx::f64, 20> b{450.0, 400.0, 350.0, 300.0,  800.0, 250.0, 200.0,
                                     150.0, 100.0, 500.0, 1000.0, 950.0, 900.0, 850.0,
                                     750.0, 700.0, 650.0, 600.0,  550.0, 50.0};
    std::vector<OracleName> names;
    for (atx::usize i = 0; i < 20; ++i) {
      names.push_back(two_phase(static_cast<atx::i64>(i + 1), a[i], b[i]));
    }
    OracleFixture fx = frozen_fixture({10}, {1000}, 127, {62, 125}, names);
    emit_and_check("f7_c2_shape_members_rank_ascending_not_keep_then_fill", "F7",
                   fixture_inputs_json(fx), membership_values(run_fixture(fx)),
                   two_rebalances(kR0TenAdds,
                                  R"({"members":[11,12,13,14,5,15,16,17,18,10],
                                      "ranks":[1,2,3,4,5,6,7,8,9,11],
                                      "status":[0,0,0,0,1,0,0,0,0,1],
                                      "drops":[[1,0],[2,0],[3,0],[4,0],[6,0],[7,0],[8,0],[9,0]],
                                      "churn":{"adds":8,"drops_rank":8,"drops_last_bar":0,
                                               "kept":2,"members":10}})"));
  }
}

void run_f8() {
  {
    OracleFixture fx = frozen_fixture({2, 3}, {0}, 127, {62, 125},
                                      {two_phase(9, 200.0, 50.0), two_phase(5, 150.0, 200.0),
                                       two_phase(7, 100.0, 150.0), two_phase(3, 50.0, 100.0)});
    emit_and_check(
        "f8_membership_bin_two_rebalances_two_cuts", "F8", fixture_inputs_json(fx),
        f8_values(run_fixture(fx)),
        R"({"sha256_hex":"6f8e059c5902c37064cbf4e00fae8b5ee85d49bbf37e1b6b6c9131e7615bf0ad",
            "byte_length":228,"fnv1a64_trailer":10158349540683142892,"rebalance_count":2,
            "header_bytes_hex":"4154585049545531010000003f00000039000000000000000000f03f"})");
  }
  {
    OracleFixture fx = frozen_fixture(
        {2, 3}, {0}, 127, {62, 125},
        {two_phase(42, 250.0, 250.0), oname(41, single_bars(63, 126, 2.0, 50.0))});
    emit_and_check(
        "f8_membership_bin_short_lists_below_top_n", "F8", fixture_inputs_json(fx),
        f8_values(run_fixture(fx)),
        R"({"sha256_hex":"caf53d7f9d6c8d7bd0b9b32a2112ce748f8e2998f8f25193f338ff4e41217f0a",
            "byte_length":180,"fnv1a64_trailer":13380942798210064020,"rebalance_count":2,
            "header_bytes_hex":"4154585049545531010000003f00000039000000000000000000f03f"})");
  }
}

void run_f9_turnover(const char *case_id, atx::usize adds, atx::usize drops_rank,
                     atx::usize drops_last_bar, atx::usize top_n, std::string_view expected) {
  // §4.2: one binary64 division of two exactly representable integers.
  const atx::f64 turnover = static_cast<atx::f64>(adds + drops_rank + drops_last_bar) /
                            static_cast<atx::f64>(2 * top_n);
  Obj in;
  in.integer("adds", adds)
      .integer("drops_rank", drops_rank)
      .integer("drops_last_bar", drops_last_bar)
      .integer("top_n", top_n);
  Obj v;
  v.num("one_way_turnover", turnover);
  emit_and_check(case_id, "F9", in.json(), v.json(), expected);
}

void run_f9() {
  run_f9_turnover("f9_turnover_8_8_0_over_2x10", 8, 8, 0, 10, R"({"one_way_turnover":0.8})");
  run_f9_turnover("f9_turnover_1_0_1_over_2x3", 1, 0, 1, 3,
                  R"({"one_way_turnover":0.3333333333333333})");
  run_f9_turnover("f9_turnover_3_1_2_over_2x3", 3, 1, 2, 3, R"({"one_way_turnover":1.0})");
  run_f9_turnover("f9_turnover_0_0_0_over_2x1000", 0, 0, 0, 1000, R"({"one_way_turnover":0.0})");
  run_f9_turnover("f9_turnover_1_1_0_over_2x2", 1, 1, 0, 2, R"({"one_way_turnover":0.5})");
  run_f9_turnover("f9_turnover_2_1_0_over_2x7", 2, 1, 0, 7,
                  R"({"one_way_turnover":0.21428571428571427})");
  {
    OracleName n200 = oname(200, single_bars(0, 11, 10.0, 9.0));
    n200.overrides = {OracleOverride{7, 0.5, 180.0}};
    OracleName n100 = oname(100, single_bars(1, 11, 10.0, 7.0));
    n100.overrides = {OracleOverride{1, kNaN, 7.0}};
    OracleFixture fx = short_window_fixture({2}, twelve_dates(), {3, 7, 10},
                                            {oname(300, single_bars(0, 11, 10.0, 10.0)), n200,
                                             oname(400, single_bars(0, 5, 10.0, 8.0)), n100});
    emit_and_check(
        "f9_exit_kind_readmitted_member_is_window_end", "F9", fixture_inputs_json(fx),
        f9_exit_values(run_fixture(fx)),
        R"({"exits":[
             {"security_id":100,"first_seen":1,"first_bar":2,"last_bar":11,
              "first_member_rebalance":1,"last_member_rebalance":1,"exit_kind":0},
             {"security_id":200,"first_seen":0,"first_bar":0,"last_bar":11,
              "first_member_rebalance":0,"last_member_rebalance":2,"exit_kind":2},
             {"security_id":300,"first_seen":0,"first_bar":0,"last_bar":11,
              "first_member_rebalance":0,"last_member_rebalance":2,"exit_kind":2}]})");
  }
  {
    OracleName n200 = oname(200, single_bars(0, 11, 10.0, 9.0));
    n200.overrides = {OracleOverride{10, 0.5, 180.0}};
    OracleName n100 = oname(100, single_bars(1, 11, 10.0, 7.0));
    n100.overrides = {OracleOverride{1, kNaN, 7.0}};
    OracleFixture fx =
        short_window_fixture({3}, twelve_dates(), {3, 7, 10},
                             {oname(300, single_bars(0, 11, 10.0, 10.0), 1.0), n200,
                              oname(400, single_bars(0, 5, 10.0, 8.0)), n100,
                              oname(500, single_bars(0, 11, 10.0, 6.0))});
    emit_and_check(
        "f9_exit_kinds_all_three_with_first_seen_before_first_bar", "F9",
        fixture_inputs_json(fx), f9_exit_values(run_fixture(fx)),
        R"({"exits":[
             {"security_id":100,"first_seen":1,"first_bar":2,"last_bar":11,
              "first_member_rebalance":1,"last_member_rebalance":2,"exit_kind":2},
             {"security_id":200,"first_seen":0,"first_bar":0,"last_bar":11,
              "first_member_rebalance":0,"last_member_rebalance":1,"exit_kind":0},
             {"security_id":300,"first_seen":0,"first_bar":0,"last_bar":11,
              "first_member_rebalance":0,"last_member_rebalance":2,"exit_kind":2},
             {"security_id":400,"first_seen":0,"first_bar":0,"last_bar":5,
              "first_member_rebalance":0,"last_member_rebalance":0,"exit_kind":1},
             {"security_id":500,"first_seen":0,"first_bar":0,"last_bar":11,
              "first_member_rebalance":2,"last_member_rebalance":2,"exit_kind":2}]})");
  }
}

TEST(DataPointInTimeUniverse, Case26_OracleMarkers_EmitAllFamilies) {
  run_f1_f2(); // 13 cases
  run_f3();    // 5
  run_f4();    // 4
  run_f5();    // 4
  run_f6();    // 3
  run_f7();    // 3
  run_f8();    // 2
  run_f9();    // 8  -> 42 oracle cases
}

PitUniverseConfig v2_config() {
  auto cfg = make_cfg(1, 1, {16}, {0});
  cfg.rule = PitUniverseRule::CommonStockV2;
  cfg.min_adv_usd = kPitV2MinAdvUsd;
  cfg.instrument_types_sha256.fill('a');
  return cfg;
}

PitInstrumentTypeEvidence type_proof(atx::i64 id, atx::u32 row,
                                    PitInstrumentType type = PitInstrumentType::CommonStock) {
  return {id, key(0), kPitSessionKeyEndExclusive, key(0) - kDay, key(0) - kDay,
          type, PitTypeSource::Vendor, true, row, true};
}

TEST(DataPointInTimeUniverseV2, InclusiveFloorsAndExplicitTypeExclusions) {
  std::vector<PitInstrumentTypeEvidence> types{type_proof(1, 1), type_proof(2, 2, PitInstrumentType::Etf),
      type_proof(4, 3), type_proof(5, 4), type_proof(6, 5), type_proof(6, 6, PitInstrumentType::Fund),
      type_proof(7, 7), type_proof(8, 8)};
  types[2].verified = false;
  types[3].available_at = key(0); // equality is unavailable
  types[5].source = PitTypeSource::Sec;
  Feed feed(v2_config(), types);
  const std::vector<Bar> bars{raw(1, 5.0, 1'000'000), raw(2, 10, 1'000'000), raw(3, 10, 1'000'000),
      raw(4, 10, 1'000'000), raw(5, 10, 1'000'000), raw(6, 10, 1'000'000),
      raw(7, 4.999, 2'000'000), raw(8, 10, 499'999.9)};
  require(feed.observe(bars), "observe");
  const auto first = feed.rebalance_now();
  ASSERT_EQ(first.ranked.size(), 1U);
  EXPECT_EQ(first.ranked[0].security_id, 1);
  EXPECT_EQ(first.ranked[0].adv63_usd, 5'000'000);
  ASSERT_EQ(first.excluded.size(), 7U);
  const auto reasons = [&](atx::i64 id) {
    for (const auto& row : first.excluded) if (row.security_id == id) return row.reasons;
    return atx::u32{0};
  };
  EXPECT_NE(reasons(2) & PitNotCommonStock, 0U);
  EXPECT_NE(reasons(3) & PitTypeUnknown, 0U);
  EXPECT_NE(reasons(4) & PitTypeUnverified, 0U);
  EXPECT_NE(reasons(5) & PitTypeUnavailable, 0U);
  EXPECT_NE(reasons(6) & PitTypeConflict, 0U);
  EXPECT_NE(reasons(7) & PitBelowPrice, 0U);
  EXPECT_NE(reasons(8) & PitBelowAdv, 0U);
  require(feed.observe(bars), "next session");
  const auto second = feed.rebalance_now();
  EXPECT_EQ(second.ranked.size(), 2U);
  EXPECT_NE(find_ranked(second, 5), nullptr);
}

TEST(DataPointInTimeUniverseV2, StrictClockExpiryAndFutureEvidencePreserveEarlierMembership) {
  auto cfg = v2_config();
  auto old = type_proof(1, 1);
  auto future = type_proof(1, 2, PitInstrumentType::Fund);
  future.available_at = key(2);
  auto expires = type_proof(2, 3);
  expires.valid_to = key(1); // this endpoint was known under the old source clock
  const std::vector<PitInstrumentTypeEvidence> types{old, future, expires};
  Feed full(cfg, types);
  expires.source_row = 2;
  const std::vector<PitInstrumentTypeEvidence> prefix_types{old, expires};
  Feed prefix(cfg, prefix_types);
  const std::vector<Bar> bars{raw(1, 10, 1'000'000), raw(2, 10, 1'000'000)};
  for (atx::usize t = 0; t < 4; ++t) {
    require(full.observe(bars), "full");
    const auto actual = full.rebalance_now();
    if (t <= 2) {
      require(prefix.observe(bars), "prefix");
      const auto expected = prefix.rebalance_now();
      EXPECT_EQ(member_ids(full, actual, 0), member_ids(prefix, expected, 0));
      EXPECT_NE(find_ranked(actual, 1), nullptr); // equality t=2 remains prior state
    } else EXPECT_EQ(find_ranked(actual, 1), nullptr);
    EXPECT_EQ(find_ranked(actual, 2) != nullptr, t == 0);
  }
}

TEST(DataPointInTimeUniverseV2, EvidenceValidationAndVersionedCodecPreserveLegacy) {
  auto cfg = v2_config();
  EXPECT_FALSE(PitUniverseBuilder::create(cfg));
  auto proof = type_proof(1, 1);
  auto bad = proof; bad.available_at = kPitSessionKeyEndExclusive;
  EXPECT_FALSE(PitUniverseBuilder::create(cfg, std::span{&bad, 1U}));
  bad = proof; bad.verified = false;
  EXPECT_FALSE(PitUniverseBuilder::create(cfg, std::span{&bad, 1U}));
  bad = proof; bad.clock_verified = false;
  EXPECT_FALSE(PitUniverseBuilder::create(cfg, std::span{&bad, 1U}));
  bad = proof; bad.source_published_at = 0;
  EXPECT_FALSE(PitUniverseBuilder::create(cfg, std::span{&bad, 1U}));
  bad = proof; bad.valid_to = bad.valid_from;
  EXPECT_FALSE(PitUniverseBuilder::create(cfg, std::span{&bad, 1U}));
  Feed v2(cfg, std::span{&proof, 1U});
  auto legacy_cfg = cfg;
  legacy_cfg.rule = PitUniverseRule::LegacyV1;
  Feed legacy(legacy_cfg);
  const std::vector<Bar> bars{raw(1, 10, 1'000'000)};
  require(v2.observe(bars), "v2"); require(legacy.observe(bars), "legacy");
  const auto new_view = v2.rebalance_now(), old_view = legacy.rebalance_now();
  EXPECT_EQ(member_ids(v2, new_view, 0), member_ids(legacy, old_view, 0));
  require(v2.observe(bars), "effective"); require(legacy.observe(bars), "effective");
  const auto old_bytes = require(encode_membership_bin(legacy.b()), "legacy encode");
  EXPECT_EQ(old_bytes.substr(0, 8), "ATXPITU1");
  const auto old_image = require(decode_membership_bin(old_bytes), "legacy decode");
  EXPECT_EQ(old_image.rule, PitUniverseRule::LegacyV1);
  const auto bytes = require(encode_membership_bin(v2.b()), "v2 encode");
  EXPECT_EQ(bytes.substr(0, 8), "ATXPITU2");
  const auto image = require(decode_membership_bin(bytes), "v2 decode");
  EXPECT_EQ(image.rule, PitUniverseRule::CommonStockV2);
  EXPECT_EQ(image.min_raw_price_inclusive, 5.0);
  EXPECT_EQ(image.min_adv_usd, 5'000'000.0);
  EXPECT_EQ(image.instrument_types_sha256, cfg.instrument_types_sha256);
  EXPECT_EQ(image.rebalances[0].cuts[0].security_ids, old_image.rebalances[0].cuts[0].security_ids);
  auto corrupt = bytes; corrupt[50] ^= 1;
  EXPECT_FALSE(decode_membership_bin(corrupt));
  EXPECT_FALSE(decode_membership_bin(bytes.substr(0, 64)));
}

TEST(DataPointInTimeUniverseV2, UnknownClockCannotRewriteQualifiedMembership) {
  const auto cfg = v2_config();
  const auto common = type_proof(1, 1);
  auto undated = type_proof(1, 2, PitInstrumentType::Etf);
  undated.verified = false;
  undated.clock_verified = false;
  undated.source_published_at = 0;
  undated.available_at = 0;
  auto lone_undated = undated;
  lone_undated.security_id = 2;
  lone_undated.source_row = 3;
  auto dated_ambiguous = type_proof(1, 4, PitInstrumentType::Unknown);
  dated_ambiguous.verified = false;
  dated_ambiguous.available_at = key(2);
  const std::vector<PitInstrumentTypeEvidence> types{common, undated, lone_undated, dated_ambiguous};
  Feed full(cfg, types);
  Feed prefix(cfg, std::span{&common, 1U});
  const std::vector<Bar> bars{raw(1, 10, 1'000'000), raw(2, 10, 1'000'000)};
  for (atx::usize t = 0; t < 4; ++t) {
    require(full.observe(bars), "full");
    const auto actual = full.rebalance_now();
    if (t <= 2) {
      require(prefix.observe(bars), "prefix");
      const auto expected = prefix.rebalance_now();
      EXPECT_EQ(member_ids(full, actual, 0), member_ids(prefix, expected, 0));
      EXPECT_NE(find_ranked(actual, 1), nullptr);
    } else {
      EXPECT_EQ(find_ranked(actual, 1), nullptr);
      for (const auto& excluded : actual.excluded)
        if (excluded.security_id == 1) EXPECT_NE(excluded.reasons & PitTypeUnverified, 0U);
    }
    EXPECT_EQ(find_ranked(actual, 2), nullptr);
    for (const auto& excluded : actual.excluded)
      if (excluded.security_id == 2) EXPECT_NE(excluded.reasons & PitTypeUnavailable, 0U);
  }
}

} // namespace atxtest_data_point_in_time_universe
