#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "atx/vol/data.hpp"
#include "atx/vol/universe.hpp"

// Acceptance evidence for P2-1: the ingest builders (`build_uid_list`,
// `build_expiry_inputs`) and `Universe::add_expiry` must be linear in the row
// count, not quadratic in the number of distinct keys.
//
// The three former hot spots each linear-scanned a growing vector on every row:
//   - build_uid_list      : O(rows * distinct_uids)
//   - build_expiry_inputs : O(rows * distinct_(uid,expiry))
//   - data_install/add_expiry stamp : O(chains * distinct_(uid,expiry))
// Over the synthetic frame below (100 uids, 1500 distinct (uid,expiry) cells,
// 90k rows) a quadratic build performs on the order of rows*distinct
// (~1e8..~1.4e8) string comparisons and takes seconds-to-minutes; the hashed
// implementations do ~90k O(1) probes and finish in single-digit milliseconds.
// The wall-clock ceiling below is set FAR above the linear cost and FAR below
// the quadratic cost, so it is not a tight/flaky timing gate — it can only fail
// if the O(n^2) behavior regresses.

namespace {

using atx::vol::build_expiry_inputs;
using atx::vol::build_uid_list;
using atx::vol::Chain;
using atx::vol::data_install;
using atx::vol::ExpiryInputField;
using atx::vol::ExpiryInputs;
using atx::vol::find_expiry_inputs;
using atx::vol::has_flag;
using atx::vol::QuoteFrame;
using atx::vol::QuoteRow;
using atx::vol::Side;
using atx::vol::Universe;
using atx::vol::UniverseStats;

constexpr int kUnders = 100;   // distinct uids
constexpr int kExpiries = 15;  // distinct expiries per uid
constexpr int kStrikes = 30;   // strikes per (uid, expiry)

// "U000".."U099" — 4 chars, comfortably under the data-plane uid cap (< 16).
std::string uid_for(int i) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "U%03d", i);
  return std::string{buf};
}

// 15 distinct, parseable ISO dates (all after the snapshot date so install's
// year-fraction is finite and positive). Shared across uids on purpose so the
// (uid, expiry_iso) join is what makes the 100*15 cells distinct.
std::string expiry_for(int e) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-06-19", 2026 + e);
  return std::string{buf};
}

// Per-expiry source inputs (independent of uid), so a sampled cell has a
// predictable rate/T and the Rate|T completeness bits set.
double rate_for(int e) { return 0.01 + 0.001 * static_cast<double>(e); }
double years_for(int e) { return 0.05 + 0.05 * static_cast<double>(e); }

QuoteFrame make_synthetic_frame() {
  QuoteFrame f; // leave f.uid empty: the row stream carries the uids
  f.snapshot_iso = "2026-01-01";
  f.spot = 100.0;
  f.yc_pillar_t = {0.5};
  f.yc_pillar_r = {0.03};

  f.rows.reserve(static_cast<std::size_t>(kUnders) * kExpiries * kStrikes * 2);
  // uid-major push order so build_uid_list's first-seen order is U000..U099.
  for (int u = 0; u < kUnders; ++u) {
    const std::string uid = uid_for(u);
    for (int e = 0; e < kExpiries; ++e) {
      const std::string exp = expiry_for(e);
      for (int k = 0; k < kStrikes; ++k) {
        for (int s = 0; s < 2; ++s) {
          QuoteRow r;
          r.uid = uid;
          r.expiry_iso = exp;
          r.strike = 100.0 + static_cast<double>(k);
          r.side = static_cast<Side>(s);
          r.bid = 1.0 + static_cast<double>(k);
          r.ask = 1.2 + static_cast<double>(k);
          r.bid_size = 10;
          r.ask_size = 11;
          r.rate_source = rate_for(e);
          r.years_source = years_for(e);
          f.rows.push_back(std::move(r));
        }
      }
    }
  }
  return f;
}

TEST(IngestScale, BuildersAreLinear_LargeSyntheticFrame) {
  QuoteFrame f = make_synthetic_frame();
  ASSERT_EQ(f.rows.size(),
            static_cast<std::size_t>(kUnders) * kExpiries * kStrikes * 2);

  // Time only the two builders under test.
  const auto t0 = std::chrono::steady_clock::now();
  const auto st = build_uid_list(f);
  build_expiry_inputs(f);
  const auto t1 = std::chrono::steady_clock::now();
  const double ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();

  ASSERT_TRUE(st.has_value());

  // ── Correctness ─────────────────────────────────────────────────────────
  // Distinct uids, in first-seen (uid-major) order.
  ASSERT_EQ(f.uid_strs.size(), static_cast<std::size_t>(kUnders));
  for (int i = 0; i < kUnders; ++i) {
    EXPECT_EQ(f.uid_strs[static_cast<std::size_t>(i)], uid_for(i));
  }
  EXPECT_EQ(f.uid, uid_for(0)); // default backfilled from first distinct uid

  // One cell per distinct (uid, expiry): 100 * 15.
  ASSERT_EQ(f.expiry_inputs.size(),
            static_cast<std::size_t>(kUnders) * kExpiries);

  // Sample a few cells: values collapsed from the row stream, completeness set.
  for (const auto sample : {std::pair<int, int>{0, 0}, {7, 3}, {99, 14}}) {
    const std::string uid = uid_for(sample.first);
    const std::string exp = expiry_for(sample.second);
    const ExpiryInputs *cell = find_expiry_inputs(f, uid, exp);
    ASSERT_NE(cell, nullptr) << uid << " / " << exp;
    EXPECT_TRUE(has_flag(cell->completeness, ExpiryInputField::Rate));
    EXPECT_TRUE(has_flag(cell->completeness, ExpiryInputField::T));
    EXPECT_FALSE(has_flag(cell->completeness, ExpiryInputField::AtmVol));
    EXPECT_DOUBLE_EQ(cell->rate, rate_for(sample.second));
    EXPECT_DOUBLE_EQ(cell->T_vol, years_for(sample.second));
  }

  // ── Non-quadratic wall-clock ceiling ────────────────────────────────────
  // See the file header: 2 s is orders of magnitude above the observed linear
  // cost (~ms) and far below any quadratic build over ~90k rows x thousands of
  // distinct keys. A tight assert would be flaky; this one only trips on an
  // O(n^2) regression.
  EXPECT_LT(ms, 2000.0) << "builders took " << ms << " ms (expected << 2000)";
}

TEST(IngestScale, InstallAddsChainsLinearly_LargeSyntheticFrame) {
  QuoteFrame f = make_synthetic_frame();
  ASSERT_TRUE(build_uid_list(f).has_value());
  build_expiry_inputs(f);

  Universe u;
  // data_install calls add_expiry once per row (90k calls); only 100*15 create
  // a chain, the rest hit the O(1) expiry_index idempotency probe. A quadratic
  // add_expiry would rescan each underlier's chains on every one of its rows.
  const auto t0 = std::chrono::steady_clock::now();
  const auto uid = data_install(u, f);
  const auto t1 = std::chrono::steady_clock::now();
  const double ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();

  ASSERT_TRUE(uid.has_value());
  const UniverseStats stats = u.stats();
  EXPECT_EQ(stats.n_underlyings, static_cast<std::uint32_t>(kUnders));
  EXPECT_EQ(stats.n_chains,
            static_cast<std::uint32_t>(kUnders * kExpiries));
  EXPECT_EQ(stats.n_strikes,
            static_cast<std::uint64_t>(kUnders) * kExpiries * kStrikes);

  // Idempotency after install's chain sort: re-installing the same frame must
  // not duplicate chains (the rebuilt expiry_index still resolves each
  // expiry_ns to its current post-sort id).
  const auto uid2 = data_install(u, f);
  ASSERT_TRUE(uid2.has_value());
  const UniverseStats stats2 = u.stats();
  EXPECT_EQ(stats2.n_chains, stats.n_chains);

  EXPECT_LT(ms, 2000.0) << "install took " << ms << " ms (expected << 2000)";
}

} // namespace
