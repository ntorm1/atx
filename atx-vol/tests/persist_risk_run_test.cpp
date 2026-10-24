// persist_risk_run_test.cpp — the RiskRun segment archive (api/persist/
// risk_run_archive.hpp; design wave3-persist D13, spec
// docs/storage/PERSIST_HISTORY_FORMAT.md): streaming writer, immutable segments behind
// a manifest, bit-exact compaction, the zstd cold tier and the schema-fingerprint
// guard against layout drift.
//
// Runs are recorded from REAL RiskStream steps over the data-free synthetic market of
// tests/support/risk_synthetic_market.hpp (and, for the end-to-end case, a synthetic
// SpiderRock store), so every column carries production-shaped numbers, NaNs of
// refused lanes included. Equality is always byte equality (memcmp / bit_cast).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/api/marketdata/spiderrock_store.hpp"
#include "atx/vol/api/persist/persist_file.hpp"
#include "atx/vol/api/persist/risk_run_archive.hpp"
#include "atx/vol/api/risk/historical_var.hpp"
#include "atx/vol/api/risk/pnl_explain.hpp"
#include "atx/vol/api/risk/risk_book.hpp"
#include "atx/vol/api/risk/risk_kernel.hpp"
#include "persist/risk_run_archive_publish.hpp"
#include "persist/risk_run_format.hpp"
#include "support/risk_synthetic_market.hpp"

#if defined(_WIN32)
#include <process.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
namespace rt = atx::vol::risk_test;
namespace rr = atx::vol::risk_run;
using namespace atx::vol; // NOLINT(google-build-using-namespace): test file

[[nodiscard]] fs::path fresh_dir(std::string_view tag) {
  const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string name = "atx-persist-rr-" + std::string(tag);
  if (info != nullptr) name += std::string("-") + info->name();
#if defined(_WIN32)
  name += "-" + std::to_string(_getpid());
#else
  name += "-" + std::to_string(getpid());
#endif
  const fs::path dir = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

template <class T> [[nodiscard]] bool same_bytes(std::span<const T> a, const std::vector<T> &b) {
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}
template <class T> [[nodiscard]] bool same_bytes(const std::vector<T> &a, const std::vector<T> &b) {
  return same_bytes(std::span<const T>{a}, b);
}
template <class T> [[nodiscard]] bool same_pod(const T &a, const T &b) {
  return std::memcmp(&a, &b, sizeof(T)) == 0;
}

// State k of a deterministic intraday path (half a grid day per step), every axis
// moving; the rate moves on odd steps so the rho / rate_move flags are exercised.
[[nodiscard]] rt::StateMove path_move(int k) {
  rt::StateMove m;
  m.time_units = static_cast<std::int64_t>(k) * (rt::kDayUnits / 2);
  const double x = static_cast<double>(k);
  m.spot_ret = {0.003 * std::sin(x), -0.002 * std::sin(0.7 * x), 0.004 * std::cos(x) - 0.004};
  m.vol_level = 0.002 * std::sin(0.5 * x);
  m.vol_skew = 0.005 * std::sin(0.3 * x);
  m.rate = (k % 2 == 1) ? 0.0005 : 0.0;
  return m;
}

// A recorded run and the live copies it must reproduce.
struct Recorded {
  std::vector<RiskFrame> frames;     // one per state
  std::vector<ExplainResult> explains; // one per step >= 1
  std::vector<std::uint64_t> leg_ids;  // explain row order
};

struct Market {
  std::vector<std::unique_ptr<rt::OwnedState>> states;
  RiskBook book;
};

[[nodiscard]] Market make_market(int n_states) {
  Market m{{}, RiskBook::create(rt::make_option_legs(), rt::make_share_legs()).value()};
  for (int k = 0; k < n_states; ++k) m.states.push_back(rt::make_state(path_move(k)));
  return m;
}

// Drive a RiskStream over `market` and record every frame and explain into `dir`.
[[nodiscard]] Recorded record_run(const Market &market, const fs::path &dir,
                                  const RiskRunOptions &options, bool finish = true) {
  Recorded out;
  RiskStream stream(market.book, RiskKernel::family(), RiskNeeds{}, ExplainOptions{});
  auto writer = RiskRunWriter::create(dir.string(), stream.prepared(), options);
  EXPECT_TRUE(writer.has_value()) << to_string(writer.error().status);
  if (!writer) return out;
  EXPECT_TRUE(stream.start(market.states[0]->state).has_value());
  EXPECT_TRUE(writer->append_frame(stream.current()).has_value());
  out.frames.push_back(stream.current());
  for (std::size_t k = 1; k < market.states.size(); ++k) {
    auto r = stream.step(market.states[k]->state);
    EXPECT_TRUE(r.has_value());
    if (!r) return out;
    EXPECT_TRUE(writer->append_frame(stream.current()).has_value()) << k;
    EXPECT_TRUE(writer->append_explain(**r).has_value()) << k;
    out.frames.push_back(stream.current());
    out.explains.push_back(**r);
  }
  out.leg_ids = out.explains.empty() ? std::vector<std::uint64_t>{} : out.explains.front().id;
  if (finish) EXPECT_TRUE(writer->finish().has_value());
  return out;
}

void expect_frame_equal(const RiskFrame &a, const RiskFrame &b, std::size_t k) {
  EXPECT_EQ(a.ts_ns, b.ts_ns) << k;
  EXPECT_EQ(a.book_logical_id, b.book_logical_id) << k;
  EXPECT_EQ(a.needs, b.needs) << k;
  for (const auto col : rr::kFrameMembers) EXPECT_TRUE(same_bytes(a.*col, b.*col)) << k;
  EXPECT_TRUE(same_bytes(a.route, b.route)) << k;
  EXPECT_TRUE(same_bytes(a.flags, b.flags)) << k;
  EXPECT_TRUE(same_bytes(a.status, b.status)) << k;
  EXPECT_TRUE(same_bytes(a.uid_spot, b.uid_spot)) << k;
  EXPECT_TRUE(same_bytes(a.uid_flat_rate, b.uid_flat_rate)) << k;
  EXPECT_TRUE(same_bytes(a.uid_status, b.uid_status)) << k;
  EXPECT_TRUE(same_bytes(a.share_mark, b.share_mark)) << k;
  EXPECT_TRUE(same_bytes(a.share_status, b.share_status)) << k;
  EXPECT_TRUE(same_pod(a.counters, b.counters)) << k;
}

void expect_buckets_equal(const std::vector<ExplainBucket> &a, const std::vector<ExplainBucket> &b,
                          std::size_t k) {
  ASSERT_EQ(a.size(), b.size()) << k;
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].key, b[i].key) << k;
    EXPECT_EQ(a[i].expiry_ns, b[i].expiry_ns) << k;
    EXPECT_TRUE(same_pod(a[i].totals, b[i].totals)) << k;
  }
}

void expect_explain_equal(const ExplainResult &a, const ExplainResult &b, std::size_t k) {
  EXPECT_EQ(a.base_ts_ns, b.base_ts_ns) << k;
  EXPECT_EQ(a.shifted_ts_ns, b.shifted_ts_ns) << k;
  EXPECT_EQ(std::bit_cast<std::uint64_t>(a.dt_years), std::bit_cast<std::uint64_t>(b.dt_years));
  EXPECT_TRUE(same_bytes(a.id, b.id)) << k;
  EXPECT_TRUE(same_bytes(a.uid, b.uid)) << k;
  EXPECT_TRUE(same_bytes(a.kind, b.kind)) << k;
  for (const auto col : rr::kExplainMembers) {
    EXPECT_TRUE(same_bytes(a.*col, b.*col)) << k;
    EXPECT_EQ((a.*col).size(), a.size()) << k << ": a materialized column without one row per leg";
  }
  EXPECT_TRUE(same_bytes(a.pnl_carry, b.pnl_carry)) << k;
  EXPECT_TRUE(same_bytes(a.route_base, b.route_base)) << k;
  EXPECT_TRUE(same_bytes(a.route_shifted, b.route_shifted)) << k;
  EXPECT_TRUE(same_bytes(a.flags, b.flags)) << k;
  EXPECT_TRUE(same_bytes(a.status, b.status)) << k;
  EXPECT_TRUE(same_pod(a.total, b.total)) << k;
  expect_buckets_equal(a.by_uid, b.by_uid, k);
  expect_buckets_equal(a.by_expiry, b.by_expiry, k);
  EXPECT_EQ(a.excluded_count, b.excluded_count) << k;
  EXPECT_TRUE(same_pod(a.excluded_premium, b.excluded_premium)) << k;
  EXPECT_EQ(a.n_pivot_groups, b.n_pivot_groups) << k;
  EXPECT_EQ(a.n_pivot_edge_fallback, b.n_pivot_edge_fallback) << k;
  EXPECT_EQ(a.rate_move_without_rho, b.rate_move_without_rho) << k;
  EXPECT_EQ(std::bit_cast<std::uint64_t>(a.explain_seconds),
            std::bit_cast<std::uint64_t>(b.explain_seconds));
}

// Every column of every step of an open archive against the live copies.
void expect_archive_matches(const RiskRunArchive &a, const Recorded &rec) {
  ASSERT_EQ(a.n_steps(), rec.frames.size());
  EXPECT_TRUE(same_bytes(a.leg_ids(), rec.leg_ids));
  for (std::size_t k = 0; k < rec.frames.size(); ++k) {
    for (std::size_t c = 0; c < kRiskFrameColumnCount; ++c) {
      EXPECT_TRUE(same_bytes(a.frame(static_cast<RiskFrameColumn>(c), k),
                             rec.frames[k].*rr::kFrameMembers[c]))
          << "step " << k << " frame column " << c;
      EXPECT_EQ(a.frame(static_cast<RiskFrameColumn>(c), k).size(), rec.frames[k].size())
          << "step " << k << " frame column " << c << ": one row per unique option";
    }
    if (k == 0) {
      EXPECT_EQ(a.totals(0), nullptr) << "the first state has no explain";
      EXPECT_TRUE(a.explain(RiskExplainColumn::PnlTotal, 0).empty());
      continue;
    }
    const ExplainResult &live = rec.explains[k - 1];
    ASSERT_GT(live.size(), 0u);
    // Every recorded column, the 21st (pnl_carry, lane E) included, has one row per leg.
    static_assert(kRiskExplainColumnCount == 21);
    for (std::size_t c = 0; c < kRiskExplainColumnCount; ++c) {
      EXPECT_TRUE(same_bytes(a.explain(static_cast<RiskExplainColumn>(c), k),
                             live.*rr::kExplainMembers[c]))
          << "step " << k << " explain column " << c;
      EXPECT_EQ(a.explain(static_cast<RiskExplainColumn>(c), k).size(), live.size())
          << "step " << k << " explain column " << c << ": one row per leg";
    }
    EXPECT_TRUE(same_bytes(a.explain(RiskExplainColumn::PnlCarry, k), live.pnl_carry)) << k;
    ASSERT_NE(a.totals(k), nullptr);
    EXPECT_TRUE(same_pod(*a.totals(k), live.total)) << k;
  }
  EXPECT_TRUE(a.frame(RiskFrameColumn::Price, rec.frames.size()).empty());
  EXPECT_EQ(a.totals(rec.frames.size()), nullptr);
}

[[nodiscard]] ExplainTotals sample_totals(double base) {
  ExplainTotals t;
  t.pv_base = base;
  t.pv_target = base + 1.25;
  t.pnl_total = 1.25;
  t.pnl_delta = 0.75 + base * 1e-3;
  t.pnl_vega = -0.125;
  t.vol_level = 0.0625;
  t.shares_pnl = 3.0;
  t.n_ok = 17;
  t.n_excluded = 2;
  t.excluded_premium = 0.5;
  return t;
}

[[nodiscard]] HistoricalVarResult sample_var(int seed, std::size_t n_frames) {
  HistoricalVarResult r;
  for (std::size_t i = 0; i < n_frames; ++i) {
    HistoricalVarFrame f;
    f.base_ts_ns = rt::instant_ns(static_cast<std::int64_t>(i) * 45);
    f.shifted_ts_ns = f.base_ts_ns + 7;
    f.totals = sample_totals(static_cast<double>(seed) * 100.0 + static_cast<double>(i));
    f.n_legs_resolved = static_cast<std::uint32_t>(10 + i);
    f.n_legs_unresolved = static_cast<std::uint32_t>(i % 3);
    f.admitted = i % 4 != 3;
    r.frames.push_back(f);
  }
  for (const double c : {0.95, 0.99}) {
    HistoricalVarPoint p;
    p.confidence = c;
    p.value_at_risk = 1000.0 * c + seed;
    p.expected_shortfall = 1100.0 * c + seed;
    p.tail_scenarios = static_cast<std::size_t>(seed) + 2;
    p.tail_mean = sample_totals(-c);
    r.points.push_back(p);
  }
  r.n_admitted = n_frames - n_frames / 4;
  r.reference_pv = 12345.678 + seed;
  return r;
}

void expect_var_equal(const HistoricalVarResult &a, const HistoricalVarResult &b) {
  ASSERT_EQ(a.frames.size(), b.frames.size());
  for (std::size_t i = 0; i < a.frames.size(); ++i) {
    EXPECT_EQ(a.frames[i].base_ts_ns, b.frames[i].base_ts_ns);
    EXPECT_EQ(a.frames[i].shifted_ts_ns, b.frames[i].shifted_ts_ns);
    EXPECT_TRUE(same_pod(a.frames[i].totals, b.frames[i].totals));
    EXPECT_EQ(a.frames[i].n_legs_resolved, b.frames[i].n_legs_resolved);
    EXPECT_EQ(a.frames[i].n_legs_unresolved, b.frames[i].n_legs_unresolved);
    EXPECT_EQ(a.frames[i].admitted, b.frames[i].admitted);
  }
  ASSERT_EQ(a.points.size(), b.points.size());
  for (std::size_t i = 0; i < a.points.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a.points[i].confidence),
              std::bit_cast<std::uint64_t>(b.points[i].confidence));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a.points[i].value_at_risk),
              std::bit_cast<std::uint64_t>(b.points[i].value_at_risk));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a.points[i].expected_shortfall),
              std::bit_cast<std::uint64_t>(b.points[i].expected_shortfall));
    EXPECT_EQ(a.points[i].tail_scenarios, b.points[i].tail_scenarios);
    EXPECT_TRUE(same_pod(a.points[i].tail_mean, b.points[i].tail_mean));
  }
  EXPECT_EQ(a.n_admitted, b.n_admitted);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(a.reference_pv),
            std::bit_cast<std::uint64_t>(b.reference_pv));
}

[[nodiscard]] std::size_t manifest_size(const fs::path &dir) {
  auto d = PersistDirectory::open(dir.string());
  return d ? d->entries().size() : 0u;
}

} // namespace

TEST(PersistRiskRun, FrameRoundTripsBitExact) {
  const Market market = make_market(5);
  const fs::path dir = fresh_dir("frame");
  const Recorded rec = record_run(market, dir, RiskRunOptions{});
  ASSERT_EQ(rec.frames.size(), 5u);
  for (const bool mapped : {true, false}) {
    PersistOpenOptions o;
    o.mapped = mapped;
    auto a = RiskRunArchive::open(dir.string(), o);
    ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
    expect_archive_matches(*a, rec);
    for (std::size_t k = 0; k < rec.frames.size(); ++k) {
      auto f = a->materialize_frame(k);
      ASSERT_TRUE(f.has_value()) << to_string(f.error().status);
      expect_frame_equal(*f, rec.frames[k], k);
      ASSERT_TRUE(a->step_info(k).has_value());
      EXPECT_EQ(a->step_info(k)->ts_ns, market.states[k]->state.ts_ns());
    }
    EXPECT_EQ(a->materialize_frame(5).error().status, PersistStatus::NotFound);
  }
  // A run without frame columns still records each step's instant and counters.
  const fs::path lean = fresh_dir("lean");
  RiskRunOptions no_frames;
  no_frames.frames = false;
  (void)record_run(market, lean, no_frames);
  auto a = RiskRunArchive::open(lean.string());
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->n_steps(), 5u);
  EXPECT_TRUE(a->frame(RiskFrameColumn::Price, 2).empty());
  EXPECT_NE(a->totals(2), nullptr);
}

TEST(PersistRiskRun, ExplainRoundTripsBitExact) {
  const Market market = make_market(6);
  const fs::path dir = fresh_dir("explain");
  const Recorded rec = record_run(market, dir, RiskRunOptions{});
  ASSERT_EQ(rec.explains.size(), 5u);
  auto a = RiskRunArchive::open(dir.string());
  ASSERT_TRUE(a.has_value());
  expect_archive_matches(*a, rec);
  for (std::size_t k = 1; k < rec.frames.size(); ++k) {
    auto e = a->materialize_explain(k);
    ASSERT_TRUE(e.has_value()) << to_string(e.error().status);
    expect_explain_equal(*e, rec.explains[k - 1], k);
  }
  EXPECT_EQ(a->materialize_explain(0).error().status, PersistStatus::NotFound);
  EXPECT_EQ(a->materialize_explain(99).error().status, PersistStatus::NotFound);

  // Buckets off: the explain columns stay, the bucket rows are not recorded.
  const fs::path lean = fresh_dir("nobuckets");
  RiskRunOptions no_buckets;
  no_buckets.buckets = false;
  (void)record_run(market, lean, no_buckets);
  auto r2 = RiskRunArchive::open(lean.string());
  ASSERT_TRUE(r2.has_value());
  auto e2 = r2->materialize_explain(3);
  ASSERT_TRUE(e2.has_value());
  EXPECT_TRUE(e2->by_uid.empty());
  EXPECT_TRUE(same_bytes(e2->pnl_total, rec.explains[2].pnl_total));
}

TEST(PersistRiskRun, AppendSealsSegmentsAtThreshold) {
  const Market market = make_market(11);
  // A 1-byte threshold seals before every new step: one step per segment.
  const fs::path tiny = fresh_dir("tiny");
  RiskRunOptions one;
  one.segment_bytes = 1;
  const Recorded rec = record_run(market, tiny, one);
  EXPECT_EQ(manifest_size(tiny), 11u);
  auto d = PersistDirectory::open(tiny.string());
  ASSERT_TRUE(d.has_value());
  for (const PersistManifestEntry &e : d->entries()) {
    EXPECT_EQ(e.records, 1u);
    EXPECT_EQ(std::string_view(e.file_name.data()).substr(0, 4), "seg-");
  }
  auto a = RiskRunArchive::open(tiny.string());
  ASSERT_TRUE(a.has_value());
  expect_archive_matches(*a, rec);

  // The default 16 MiB threshold keeps this small run in one segment.
  const fs::path big = fresh_dir("big");
  (void)record_run(market, big, RiskRunOptions{});
  EXPECT_EQ(manifest_size(big), 1u);

  // A threshold of about two one-step segment files: a few multi-step segments (a
  // segment seals only once its staged step data crossed the threshold), reassembled
  // in step order.
  auto one_step = rr::Reader::open(tiny.string());
  ASSERT_TRUE(one_step.has_value());
  const std::uint64_t step_bytes = one_step->segment(5).header().file_size;
  const fs::path mid = fresh_dir("mid");
  RiskRunOptions two;
  two.segment_bytes = 2 * step_bytes;
  const Recorded rec3 = record_run(market, mid, two);
  const std::size_t n_seg = manifest_size(mid);
  EXPECT_GE(n_seg, 2u);
  EXPECT_LT(n_seg, 11u);
  auto dm = PersistDirectory::open(mid.string());
  ASSERT_TRUE(dm.has_value());
  std::uint32_t steps_listed = 0;
  for (std::size_t i = 0; i < dm->entries().size(); ++i) {
    // Every segment but the last sealed because it crossed the threshold.
    if (i + 1 < dm->entries().size()) EXPECT_GE(dm->entries()[i].records, 2u) << i;
    steps_listed += dm->entries()[i].records;
  }
  EXPECT_EQ(steps_listed, 11u);
  auto a3 = RiskRunArchive::open(mid.string());
  ASSERT_TRUE(a3.has_value());
  expect_archive_matches(*a3, rec3);

  // Writer refusals: a second writer on a live run directory, a frame of another
  // book, and any append after finish().
  RiskStream stream(market.book, RiskKernel::family(), RiskNeeds{}, ExplainOptions{});
  EXPECT_EQ(
      RiskRunWriter::create(tiny.string(), stream.prepared(), RiskRunOptions{}).error().status,
      PersistStatus::AlreadyExists);
  const fs::path fresh = fresh_dir("refuse");
  auto w = RiskRunWriter::create(fresh.string(), stream.prepared(), RiskRunOptions{});
  ASSERT_TRUE(w.has_value());
  RiskFrame alien = rec.frames.front();
  alien.book_logical_id ^= 0x5Au;
  EXPECT_EQ(w->append_frame(alien).error().status, PersistStatus::InvalidArgument);
  RiskFrame short_frame = rec.frames.front();
  short_frame.price.pop_back();
  EXPECT_EQ(w->append_frame(short_frame).error().status, PersistStatus::InvalidArgument);
  ASSERT_TRUE(w->finish().has_value());
  EXPECT_EQ(w->append_frame(rec.frames.front()).error().status, PersistStatus::InvalidArgument);
  EXPECT_EQ(w->finish().error().status, PersistStatus::InvalidArgument);
  RiskRunOptions zero;
  zero.segment_bytes = 0;
  EXPECT_EQ(
      RiskRunWriter::create(fresh_dir("zero").string(), stream.prepared(), zero).error().status,
      PersistStatus::InvalidArgument);
}

TEST(PersistRiskRun, CrashBetweenSegmentAndManifestIsInvisible) {
  const Market market = make_market(4);
  const fs::path dir = fresh_dir("crash");
  RiskRunOptions one;
  one.segment_bytes = 1;
  const Recorded rec = record_run(market, dir, one);
  ASSERT_EQ(manifest_size(dir), 4u);

  // A crash after a segment's publish but before its manifest rewrite leaves a valid,
  // complete segment on disk that no manifest entry names: the reader never sees it.
  auto d = PersistDirectory::open(dir.string());
  ASSERT_TRUE(d.has_value());
  const PersistManifestEntry last = d->entries().back();
  auto orphan_name = persist_file_name(PersistKind::RiskRun, "000000000099", last.seq + 7);
  ASSERT_TRUE(orphan_name.has_value());
  fs::copy_file(d->path(last), dir / *orphan_name);
  // ... and a crash mid-publish leaves a temp file behind.
  fs::copy_file(d->path(last), dir / "seg-000000000001.atxrr.tmp-4242");
  auto a = RiskRunArchive::open(dir.string());
  ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
  expect_archive_matches(*a, rec);

  // A writer that dies without finish() loses only its unsealed staging: every
  // sealed segment is listed, the staged step is not.
  const fs::path dead = fresh_dir("dead");
  {
    Recorded partial = record_run(market, dead, one, /*finish=*/false);
    ASSERT_EQ(partial.frames.size(), 4u);
  } // writer destroyed unsealed
  auto b = RiskRunArchive::open(dead.string());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b->n_steps(), 3u);
  for (std::size_t k = 0; k < 3; ++k) {
    EXPECT_TRUE(same_bytes(b->frame(RiskFrameColumn::Delta, k),
                           rec.frames[k].*rr::kFrameMembers[6]));
  }
  // A manifest entry whose file was swapped for another valid segment is refused.
  const fs::path swapped = fresh_dir("swapped");
  (void)record_run(market, swapped, one);
  auto ds = PersistDirectory::open(swapped.string());
  ASSERT_TRUE(ds.has_value());
  fs::remove(ds->path(ds->entries()[1]));
  fs::copy_file(ds->path(ds->entries()[2]), ds->path(ds->entries()[1]));
  auto c = RiskRunArchive::open(swapped.string());
  ASSERT_FALSE(c.has_value());
  EXPECT_TRUE(c.error().status == PersistStatus::SegmentMismatch ||
              c.error().status == PersistStatus::SizeMismatch)
      << to_string(c.error().status);
}

TEST(PersistRiskRun, HistoricalVarResultRoundTrips) {
  const Market market = make_market(3);
  for (const std::uint64_t threshold : {std::uint64_t{1}, std::uint64_t{64} << 20}) {
    const fs::path dir = fresh_dir(threshold == 1 ? "var-split" : "var-one");
    RiskStream stream(market.book, RiskKernel::family(), RiskNeeds{}, ExplainOptions{});
    RiskRunOptions o;
    o.segment_bytes = threshold;
    auto w = RiskRunWriter::create(dir.string(), stream.prepared(), o);
    ASSERT_TRUE(w.has_value());
    const HistoricalVarResult v0 = sample_var(1, 9);
    const HistoricalVarResult v1 = sample_var(2, 4);
    const HistoricalVarResult v2{}; // an empty result is a result
    ASSERT_TRUE(w->append_var(v0).has_value()); // before any step
    ASSERT_TRUE(stream.start(market.states[0]->state).has_value());
    ASSERT_TRUE(w->append_frame(stream.current()).has_value());
    ASSERT_TRUE(stream.step(market.states[1]->state).has_value());
    ASSERT_TRUE(w->append_frame(stream.current()).has_value());
    ASSERT_TRUE(w->append_var(v1).has_value());
    ASSERT_TRUE(stream.step(market.states[2]->state).has_value());
    ASSERT_TRUE(w->append_frame(stream.current()).has_value());
    ASSERT_TRUE(w->append_var(v2).has_value());
    ASSERT_TRUE(w->finish().has_value());

    auto reader = RiskRunArchive::open(dir.string());
    ASSERT_TRUE(reader.has_value()) << to_string(reader.error().status);
    ASSERT_EQ(reader->n_var(), 3u);
    expect_var_equal(reader->var(0).value(), v0);
    expect_var_equal(reader->var(1).value(), v1);
    expect_var_equal(reader->var(2).value(), v2);
    EXPECT_EQ(reader->var(3).error().status, PersistStatus::NotFound);
    EXPECT_EQ(reader->n_steps(), 3u);
  }
}

TEST(PersistRiskRun, CompactionIsBitExact) {
  const Market market = make_market(11);
  const fs::path dir = fresh_dir("compact");
  RiskRunOptions one;
  one.segment_bytes = 1;
  const Recorded rec = record_run(market, dir, one);
  ASSERT_EQ(manifest_size(dir), 11u);
  const std::vector<fs::path> before = [&] {
    std::vector<fs::path> out;
    for (const auto &e : fs::directory_iterator(dir)) out.push_back(e.path());
    return out;
  }();

  auto merged = compact_risk_run(dir.string(), PersistCodec::None, 0);
  ASSERT_TRUE(merged.has_value()) << to_string(merged.error().status);
  EXPECT_EQ(*merged, 11u);
  EXPECT_EQ(manifest_size(dir), 1u);
  for (const fs::path &p : before) EXPECT_TRUE(fs::exists(p)) << "superseded files stay (gc)";
  auto a = RiskRunArchive::open(dir.string());
  ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
  expect_archive_matches(*a, rec);
  for (std::size_t k = 0; k < rec.frames.size(); ++k) {
    expect_frame_equal(a->materialize_frame(k).value(), rec.frames[k], k);
    EXPECT_EQ(a->step_info(k).value().segment, 0u) << k;
    if (k > 0) expect_explain_equal(a->materialize_explain(k).value(), rec.explains[k - 1], k);
  }
  // Compacting a one-segment run is a rewrite of the same columns.
  ASSERT_EQ(compact_risk_run(dir.string(), PersistCodec::None, 0).value(), 1u);
  auto again = RiskRunArchive::open(dir.string());
  ASSERT_TRUE(again.has_value());
  expect_archive_matches(*again, rec);
  EXPECT_EQ(compact_risk_run(fresh_dir("empty").string(), PersistCodec::None, 0).error().status,
            PersistStatus::NotFound);
}

TEST(PersistRiskRun, ZstdColdTierRoundTrips) {
  const Market market = make_market(11);
  const fs::path plain = fresh_dir("plain");
  const fs::path cold = fresh_dir("cold");
  RiskRunOptions one;
  one.segment_bytes = 1;
  const Recorded rec = record_run(market, plain, one);
  (void)record_run(market, cold, one);
  ASSERT_EQ(compact_risk_run(plain.string(), PersistCodec::None, 0).value(), 11u);
  ASSERT_EQ(compact_risk_run(cold.string(), PersistCodec::Zstd, 3).value(), 11u);

  auto a = RiskRunArchive::open(cold.string());
  ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
  expect_archive_matches(*a, rec);
  auto reader = rr::Reader::open(cold.string());
  ASSERT_TRUE(reader.has_value());
  const PersistSectionEntry *price = reader->segment(0).entry(
      rr::type_of(rr::Section::FrameF64) + static_cast<std::uint32_t>(RiskFrameColumn::Price));
  ASSERT_NE(price, nullptr);
  EXPECT_EQ(price->codec, static_cast<std::uint8_t>(PersistCodec::Zstd));
  EXPECT_EQ(price->filter, static_cast<std::uint8_t>(PersistFilter::ByteShuffle));
  auto plain_reader = rr::Reader::open(plain.string());
  ASSERT_TRUE(plain_reader.has_value());
  EXPECT_LT(reader->segment(0).header().file_size, plain_reader->segment(0).header().file_size);

  // Levels outside zstd's [1, 19] (and an unknown codec) are refused untouched.
  EXPECT_EQ(compact_risk_run(cold.string(), PersistCodec::Zstd, 0).error().status,
            PersistStatus::InvalidArgument);
  EXPECT_EQ(compact_risk_run(cold.string(), PersistCodec::Zstd, 22).error().status,
            PersistStatus::InvalidArgument);
  EXPECT_EQ(compact_risk_run(cold.string(), static_cast<PersistCodec>(9), 3).error().status,
            PersistStatus::InvalidArgument);
  EXPECT_EQ(manifest_size(cold), 1u);
}

TEST(PersistRiskRun, RefusesLayoutDriftFingerprint) {
  // The fingerprint is a pure function of the pinned layout. This known answer moves
  // exactly when a RiskRun record, column or section changes (including ExplainTotals
  // / FrameCounters growing under another lane): re-pin it deliberately, then every
  // old file is refused with SchemaMismatch (no backward compatibility, persist D5).
  constexpr std::uint64_t kPinned = 0x07b280ae55b35c46ull; // re-pinned 2026-10-24 (lane E)
  EXPECT_EQ(risk_run_fingerprint(), rr::kFingerprint);
  EXPECT_EQ(risk_run_fingerprint(), kPinned);

  // A drifted layout (one ExplainTotals field moved, or one column restrided) folds to
  // another fingerprint.
  auto drifted = rr::kSchema;
  drifted[rr::slot_of(rr::type_of(rr::Section::StepTotals))].stride += 8;
  EXPECT_NE(persist_fingerprint(persist_salt(kRiskRunSalt), drifted), rr::kFingerprint);
  auto renamed = rr::kTotalsFields;
  renamed[3].name = "pnl_delta_v2";
  auto schema2 = rr::kSchema;
  schema2[rr::slot_of(rr::type_of(rr::Section::StepTotals))].fields = renamed;
  EXPECT_NE(persist_fingerprint(persist_salt(kRiskRunSalt), schema2), rr::kFingerprint);

  // A well-formed RiskRun segment stamped with another layout's fingerprint is refused
  // by the archive, not misread.
  const fs::path dir = fresh_dir("drift");
  const std::vector<double> col(8, 1.5);
  const PersistSectionInput in{rr::type_of(rr::Section::FrameF64), 1, 0, PersistDType::F64, 8,
                               true, PersistCodec::None, PersistFilter::None, 3,
                               std::as_bytes(std::span{col})};
  PersistFileInfo info;
  info.kind = PersistKind::RiskRun;
  info.schema_fingerprint = rr::kFingerprint ^ 0x1ull;
  auto image = build_persist_file(std::span{&in, 1}, info);
  ASSERT_TRUE(image.has_value());
  ASSERT_TRUE(PersistDirectory::publish(dir.string(), rr::segment_key(0), *image, 0).has_value());
  auto a = RiskRunArchive::open(dir.string());
  ASSERT_FALSE(a.has_value());
  EXPECT_EQ(a.error().status, PersistStatus::SchemaMismatch);
}

TEST(PersistRiskRun, RecordedRiskStreamMatchesLiveTotals) {
  // A SpiderRock-shaped synthetic day (11 half-hour buckets), loaded through the
  // production loader into MarketStates, streamed, recorded and reopened.
  const fs::path root = fresh_dir("sr-store");
  SyntheticSpiderRockSpec spec;
  spec.n_underliers = 3;
  spec.n_expiries = 3;
  spec.n_strikes = 5;
  spec.seed = 11;
  ASSERT_TRUE(write_synthetic_spiderrock_store(spec, root.string()).has_value());
  SpiderRockStoreSpec rspec;
  rspec.store_root = root.string();
  rspec.date = spec.date;
  UnderlierTable table;
  struct Owned {
    MarketStateStorage storage;
    MarketState state;
  };
  std::vector<std::unique_ptr<Owned>> states;
  for (const std::string &bucket : spec.buckets_et) {
    auto b = load_spiderrock_bucket(rspec, bucket, table);
    ASSERT_TRUE(b.has_value()) << b.error().to_string();
    auto st = market_state_from_bucket(*b, TimeSpec{});
    ASSERT_TRUE(st.has_value()) << st.error().to_string();
    auto owned = std::make_unique<Owned>();
    owned->storage = std::move(*st);
    owned->state = make_market_state(owned->storage).value();
    states.push_back(std::move(owned));
  }
  ASSERT_EQ(states.size(), 11u);
  // The book: every second marked contract of the first bucket, signed, plus a share
  // leg per underlier.
  std::vector<OptionLeg> legs;
  std::vector<ShareLeg> shares;
  std::uint64_t id = 1;
  for (const MarketStateStorage::MarksColumns &m : states.front()->storage.marks) {
    for (std::size_t i = 0; i < m.strike.size(); i += 2) {
      const double qty = static_cast<double>(id % 5 + 1) * (id % 2 == 0 ? -1.0 : 1.0);
      legs.push_back(OptionLeg{id++, m.uid, m.strike[i], m.expiry_ns[i], m.side[i], qty, 100.0});
    }
    shares.push_back(ShareLeg{100000 + m.uid, m.uid, -25.0});
  }
  const RiskBook book = RiskBook::create(legs, shares).value();
  RiskStream stream(book, RiskKernel::family(), RiskNeeds{}, ExplainOptions{});
  const fs::path dir = fresh_dir("sr-run");
  RiskRunOptions o;
  o.segment_bytes = 16u << 10; // several segments over the day
  auto w = RiskRunWriter::create(dir.string(), stream.prepared(), o);
  ASSERT_TRUE(w.has_value());
  ASSERT_TRUE(stream.start(states[0]->state).has_value());
  ASSERT_TRUE(w->append_frame(stream.current()).has_value());
  std::vector<ExplainTotals> live;
  std::vector<std::vector<double>> live_pnl;
  for (std::size_t k = 1; k < states.size(); ++k) {
    auto r = stream.step(states[k]->state);
    ASSERT_TRUE(r.has_value()) << r.error().to_string();
    ASSERT_TRUE(w->append_frame(stream.current()).has_value());
    ASSERT_TRUE(w->append_explain(**r).has_value());
    live.push_back((*r)->total);
    live_pnl.push_back((*r)->pnl_total);
    EXPECT_GT((*r)->total.n_ok, 0u) << k;
  }
  ASSERT_TRUE(w->finish().has_value());
  EXPECT_GT(manifest_size(dir), 1u);

  auto a = RiskRunArchive::open(dir.string());
  ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
  ASSERT_EQ(a->n_steps(), 11u);
  for (std::size_t k = 1; k < 11; ++k) {
    ASSERT_NE(a->totals(k), nullptr) << k;
    EXPECT_TRUE(same_pod(*a->totals(k), live[k - 1])) << "step " << k;
    EXPECT_TRUE(same_bytes(a->explain(RiskExplainColumn::PnlTotal, k), live_pnl[k - 1])) << k;
  }
  // The day's P&L re-summed from the archive in step order equals the live sum.
  double day_live = 0.0;
  double day_disk = 0.0;
  for (std::size_t k = 1; k < 11; ++k) {
    day_live += live[k - 1].pnl_total;
    day_disk += a->totals(k)->pnl_total;
  }
  EXPECT_EQ(std::bit_cast<std::uint64_t>(day_live), std::bit_cast<std::uint64_t>(day_disk));
}

TEST(PersistRiskRun, StepInfoDescribesEveryStep) {
  const Market market = make_market(5);
  const fs::path dir = fresh_dir("info");
  RiskRunOptions one;
  one.segment_bytes = 1; // one step per segment: step k lives in segment k
  const Recorded rec = record_run(market, dir, one);
  auto a = RiskRunArchive::open(dir.string());
  ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
  ASSERT_EQ(a->n_steps(), 5u);
  for (std::size_t k = 0; k < 5; ++k) {
    const std::optional<RiskRunStepInfo> s = a->step_info(k);
    ASSERT_TRUE(s.has_value()) << k;
    EXPECT_EQ(s->step, k);
    EXPECT_EQ(s->segment, k);
    EXPECT_EQ(s->ts_ns, rec.frames[k].ts_ns);
    EXPECT_EQ(s->book_logical_id, rec.frames[k].book_logical_id);
    EXPECT_TRUE(s->has_frame);
    EXPECT_EQ(s->needs, rec.frames[k].needs) << k;
    EXPECT_TRUE(same_pod(s->counters, rec.frames[k].counters)) << k;
    EXPECT_EQ(s->has_explain, k > 0);
    if (k == 0) {
      EXPECT_EQ(s->base_ts_ns, 0);
      EXPECT_EQ(s->explain_rows, 0u);
      EXPECT_EQ(std::bit_cast<std::uint64_t>(s->dt_years), 0u);
      continue;
    }
    const ExplainResult &e = rec.explains[k - 1];
    EXPECT_EQ(s->base_ts_ns, e.base_ts_ns);
    EXPECT_EQ(s->ts_ns, e.shifted_ts_ns);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(s->dt_years), std::bit_cast<std::uint64_t>(e.dt_years));
    EXPECT_EQ(s->explain_rows, e.id.size());
  }
  EXPECT_FALSE(a->step_info(5).has_value());

  // Frames off: every step and its explain stay, no step has a frame.
  const fs::path lean = fresh_dir("info-lean");
  RiskRunOptions no_frames;
  no_frames.frames = false;
  (void)record_run(market, lean, no_frames);
  auto b = RiskRunArchive::open(lean.string());
  ASSERT_TRUE(b.has_value());
  const std::optional<RiskRunStepInfo> s2 = b->step_info(2);
  ASSERT_TRUE(s2.has_value());
  EXPECT_FALSE(s2->has_frame);
  EXPECT_TRUE(s2->has_explain);
  EXPECT_EQ(s2->segment, 0u);
  EXPECT_EQ(b->materialize_frame(2).error().status, PersistStatus::NotFound);
}

TEST(PersistRiskRun, SealDurabilityPublishesIdenticalBytes) {
  const RiskRunOptions defaults;
  EXPECT_EQ(defaults.seal_durability, SealDurability::Publish);
  EXPECT_EQ(defaults.finish_durability, SealDurability::Flush);

  // One stream, recorded through writers that differ only in durability: the bytes
  // (every segment's file CRC, then the compacted segment's) are identical.
  const Market market = make_market(5);
  RiskStream stream(market.book, RiskKernel::family(), RiskNeeds{}, ExplainOptions{});
  Recorded rec;
  ASSERT_TRUE(stream.start(market.states[0]->state).has_value());
  rec.frames.push_back(stream.current());
  for (std::size_t k = 1; k < market.states.size(); ++k) {
    auto r = stream.step(market.states[k]->state);
    ASSERT_TRUE(r.has_value());
    rec.frames.push_back(stream.current());
    rec.explains.push_back(**r);
  }
  rec.leg_ids = rec.explains.front().id;
  const std::pair<SealDurability, SealDurability> modes[] = {
      {SealDurability::Publish, SealDurability::Flush},
      {SealDurability::Flush, SealDurability::Flush},
      {SealDurability::Publish, SealDurability::Publish}};
  std::vector<std::vector<std::uint32_t>> crcs;
  std::vector<std::uint32_t> compacted;
  for (const auto &[seal, last] : modes) {
    const fs::path dir = fresh_dir("durability-" + std::to_string(crcs.size()));
    RiskRunOptions o;
    o.segment_bytes = 1;
    o.seal_durability = seal;
    o.finish_durability = last;
    auto w = RiskRunWriter::create(dir.string(), stream.prepared(), o);
    ASSERT_TRUE(w.has_value());
    ASSERT_TRUE(w->append_frame(rec.frames[0]).has_value());
    for (std::size_t k = 1; k < rec.frames.size(); ++k) {
      ASSERT_TRUE(w->append_frame(rec.frames[k]).has_value());
      ASSERT_TRUE(w->append_explain(rec.explains[k - 1]).has_value());
    }
    ASSERT_TRUE(w->finish().has_value());
    auto d = PersistDirectory::open(dir.string());
    ASSERT_TRUE(d.has_value());
    ASSERT_EQ(d->entries().size(), 5u);
    std::vector<std::uint32_t> c;
    for (const PersistManifestEntry &e : d->entries()) c.push_back(e.file_crc32c);
    crcs.push_back(std::move(c));
    auto a = RiskRunArchive::open(dir.string());
    ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
    expect_archive_matches(*a, rec);
    // The temp files of either path are gone once the seal returns.
    for (const auto &f : fs::directory_iterator(dir)) {
      EXPECT_EQ(f.path().filename().string().find(".tmp"), std::string::npos) << f.path();
    }
    // Compaction at the same durability as the streaming seals.
    ASSERT_EQ(compact_risk_run(dir.string(), PersistCodec::None, 0, seal).value(), 5u);
    auto merged = PersistDirectory::open(dir.string());
    ASSERT_TRUE(merged.has_value());
    ASSERT_EQ(merged->entries().size(), 1u);
    compacted.push_back(merged->entries().front().file_crc32c);
    auto m = RiskRunArchive::open(dir.string());
    ASSERT_TRUE(m.has_value());
    expect_archive_matches(*m, rec);
  }
  EXPECT_EQ(crcs[1], crcs[0]);
  EXPECT_EQ(crcs[2], crcs[0]);
  EXPECT_EQ(compacted[1], compacted[0]);
  EXPECT_EQ(compacted[2], compacted[0]);

  // An out-of-range durability is refused before anything is written.
  RiskRunOptions bad;
  bad.seal_durability = static_cast<SealDurability>(7);
  EXPECT_EQ(RiskRunWriter::create(fresh_dir("bad-seal").string(), stream.prepared(), bad)
                .error()
                .status,
            PersistStatus::InvalidArgument);
  RiskRunOptions bad_last;
  bad_last.finish_durability = static_cast<SealDurability>(2);
  EXPECT_EQ(RiskRunWriter::create(fresh_dir("bad-last").string(), stream.prepared(), bad_last)
                .error()
                .status,
            PersistStatus::InvalidArgument);
  const fs::path keep = fresh_dir("durability-0");
  EXPECT_EQ(compact_risk_run(keep.string(), PersistCodec::None, 0, static_cast<SealDurability>(5))
                .error()
                .status,
            PersistStatus::InvalidArgument);
}

TEST(PersistRiskRun, CompactionRefusesAStaleManifest) {
  // The compaction contract: the merged manifest replaces the one the run was read
  // from, or nothing. A segment a live writer published in between makes it refuse.
  const Market market = make_market(4);
  const fs::path dir = fresh_dir("stale");
  RiskRunOptions one;
  one.segment_bytes = 1;
  (void)record_run(market, dir, one);
  auto seen = PersistDirectory::open(dir.string());
  ASSERT_TRUE(seen.has_value());
  ASSERT_EQ(seen->entries().size(), 4u);
  std::ifstream in(seen->path(seen->entries().back()), std::ios::binary);
  const std::vector<char> raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  std::vector<std::byte> image(raw.size());
  std::memcpy(image.data(), raw.data(), raw.size());

  // A late seal (by the run's writer: the manifest still ends with its last row) lands
  // after the compactor read the manifest.
  const PersistManifestEntry &newest = seen->entries().back();
  const std::optional<rr::WriterTail> tail = rr::WriterTail{newest.seq, newest.file_crc32c};
  auto late = rr::publish_writer_segment(dir.string(), rr::segment_key(99), image, 1,
                                         SealDurability::Publish, tail);
  ASSERT_TRUE(late.has_value()) << to_string(late.error().status);
  EXPECT_EQ(manifest_size(dir), 5u);
  // A listed key is never replaced, even by its owner.
  const std::optional<rr::WriterTail> tail2 = rr::WriterTail{late->seq, late->file_crc32c};
  EXPECT_EQ(rr::publish_writer_segment(dir.string(), rr::segment_key(99), image, 1,
                                       SealDurability::Publish, tail2)
                .error()
                .status,
            PersistStatus::DuplicateKey);
  const auto refused =
      rr::publish_compacted(dir.string(), seen->entries(), image, 1, SealDurability::Flush);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().status, PersistStatus::SegmentMismatch);
  EXPECT_EQ(manifest_size(dir), 5u); // untouched

  // Against the manifest as it now stands the same publish goes through.
  auto now = PersistDirectory::open(dir.string());
  ASSERT_TRUE(now.has_value());
  ASSERT_TRUE(
      rr::publish_compacted(dir.string(), now->entries(), image, 1, SealDurability::Publish)
          .has_value());
  EXPECT_EQ(manifest_size(dir), 1u);
}

namespace {

// Re-frame the run's only segment with section `type` replaced by `bytes` and publish
// it as the run's only segment: a crafted but correctly sealed file (build_persist_file
// recomputes every section / table / header / file CRC, publish_compacted writes the
// matching manifest row), the shape a resealing adversary produces.
void reseal_section(const fs::path &dir, std::uint32_t type, std::span<const std::byte> bytes) {
  auto d = PersistDirectory::open(dir.string());
  ASSERT_TRUE(d.has_value());
  ASSERT_EQ(d->entries().size(), 1u);
  auto f = PersistFile::open(d->path(d->entries().front()), PersistKind::RiskRun,
                             risk_run_fingerprint(),
                             PersistOpenOptions{false, PersistVerify::Full, false, 0});
  ASSERT_TRUE(f.has_value()) << to_string(f.error().status);
  std::vector<PersistSectionInput> in;
  for (const PersistSectionEntry &e : f->sections()) {
    auto b = f->section(e.type);
    ASSERT_TRUE(b.has_value());
    in.push_back(PersistSectionInput{e.type, e.type_major, e.type_minor,
                                     static_cast<PersistDType>(e.dtype), e.element_stride, true,
                                     PersistCodec::None, PersistFilter::None, 3,
                                     e.type == type ? bytes : *b});
  }
  PersistFileInfo info;
  info.kind = PersistKind::RiskRun;
  info.schema_fingerprint = f->header().schema_fingerprint;
  info.ts_min_ns = f->header().ts_min_ns;
  info.ts_max_ns = f->header().ts_max_ns;
  auto image = build_persist_file(in, info);
  ASSERT_TRUE(image.has_value()) << to_string(image.error().status);
  ASSERT_TRUE(rr::publish_compacted(dir.string(), d->entries(), *image,
                                    d->entries().front().records, SealDurability::Flush)
                  .has_value());
}

// Every way a run is opened: the four verify levels mapped, and Read mode at Full.
[[nodiscard]] std::vector<PersistOpenOptions> every_open() {
  std::vector<PersistOpenOptions> out;
  for (const PersistVerify v : {PersistVerify::Framing, PersistVerify::Lazy, PersistVerify::Full,
                                PersistVerify::Paranoid}) {
    PersistOpenOptions o;
    o.verify = v;
    out.push_back(o);
  }
  PersistOpenOptions read;
  read.mapped = false;
  read.verify = PersistVerify::Full;
  out.push_back(read);
  return out;
}

} // namespace

TEST(PersistRiskRun, RefusesWrappingVarIndex) {
  // A resealed VarIndex whose middle count wraps the running u64 sum back to the start
  // ({0, n}, {n, 2^64 - n}, {0, n}) tiles the column by addition. It must be refused at
  // open (IndexBounds) at every level and in Read mode, and no exception may escape.
  const Market market = make_market(1);
  const std::uint32_t vidx = rr::type_of(rr::Section::VarIndex);
  for (const bool points : {false, true}) {
    const fs::path dir = fresh_dir(points ? "var-wrap-points" : "var-wrap-frames");
    RiskStream stream(market.book, RiskKernel::family(), RiskNeeds{}, ExplainOptions{});
    {
      auto w = RiskRunWriter::create(dir.string(), stream.prepared(), RiskRunOptions{});
      ASSERT_TRUE(w.has_value());
      for (int k = 0; k < 3; ++k) ASSERT_TRUE(w->append_var(sample_var(k + 1, 2)).has_value());
      ASSERT_TRUE(w->finish().has_value());
    }
    std::vector<rr::RiskRunVar> rows;
    {
      auto reader =
          rr::Reader::open(dir.string(), PersistOpenOptions{false, PersistVerify::Full, false, 0});
      ASSERT_TRUE(reader.has_value());
      auto t = reader->segment(0).typed<rr::RiskRunVar>(vidx);
      ASSERT_TRUE(t.has_value());
      rows.assign(t->begin(), t->end());
    }
    ASSERT_EQ(rows.size(), 3u);
    // Control: the same rows re-framed untouched open and serve every result.
    reseal_section(dir, vidx, std::as_bytes(std::span{rows}));
    {
      auto a = RiskRunArchive::open(dir.string());
      ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
      ASSERT_EQ(a->n_var(), 3u);
      for (std::size_t i = 0; i < 3; ++i) EXPECT_TRUE(a->var(i).has_value()) << i;
    }
    std::uint64_t rr::RiskRunVar::*begin =
        points ? &rr::RiskRunVar::point_begin : &rr::RiskRunVar::frame_begin;
    std::uint64_t rr::RiskRunVar::*count =
        points ? &rr::RiskRunVar::point_count : &rr::RiskRunVar::frame_count;
    std::uint64_t total = 0;
    for (const rr::RiskRunVar &r : rows) total += r.*count;
    ASSERT_GT(total, 0u);
    rows[0].*begin = 0;
    rows[0].*count = total;
    rows[1].*begin = total;
    rows[1].*count = std::numeric_limits<std::uint64_t>::max() - total + 1; // 2^64 - total
    rows[2].*begin = 0;
    rows[2].*count = total;
    reseal_section(dir, vidx, std::as_bytes(std::span{rows}));
    for (const PersistOpenOptions &o : every_open()) {
      EXPECT_NO_THROW({
        auto a = RiskRunArchive::open(dir.string(), o);
        EXPECT_FALSE(a.has_value()) << "verify " << static_cast<int>(o.verify);
        if (!a) EXPECT_EQ(a.error().status, PersistStatus::IndexBounds);
      });
    }
  }
}

TEST(PersistRiskRun, SecondWriterOnOneDirectoryIsRefused) {
  const Market market = make_market(3);
  const fs::path dir = fresh_dir("two-writers");
  RiskStream sa(market.book, RiskKernel::family(), RiskNeeds{}, ExplainOptions{});
  RiskStream sb(market.book, RiskKernel::family(), RiskNeeds{}, ExplainOptions{});
  RiskRunOptions one;
  one.segment_bytes = 1;
  // Both created before either sealed: create() cannot tell them apart yet.
  auto wa = RiskRunWriter::create(dir.string(), sa.prepared(), one);
  auto wb = RiskRunWriter::create(dir.string(), sb.prepared(), one);
  ASSERT_TRUE(wa.has_value());
  ASSERT_TRUE(wb.has_value());
  Recorded rec;
  ASSERT_TRUE(sa.start(market.states[0]->state).has_value());
  ASSERT_TRUE(sb.start(market.states[0]->state).has_value());
  ASSERT_TRUE(wa->append_frame(sa.current()).has_value());
  ASSERT_TRUE(wb->append_frame(sb.current()).has_value());
  rec.frames.push_back(sa.current());
  // a seals first (its second step seals its first segment)...
  auto ra = sa.step(market.states[1]->state);
  ASSERT_TRUE(ra.has_value());
  ASSERT_TRUE(wa->append_frame(sa.current()).has_value());
  ASSERT_TRUE(wa->append_explain(**ra).has_value());
  rec.frames.push_back(sa.current());
  rec.explains.push_back(**ra);
  // ... so b's first seal finds a run it did not write: refused, and b is done.
  auto rb = sb.step(market.states[1]->state);
  ASSERT_TRUE(rb.has_value());
  const auto refused = wb->append_frame(sb.current());
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().status, PersistStatus::SegmentMismatch);
  EXPECT_EQ(wb->append_explain(**rb).error().status, PersistStatus::SegmentMismatch);
  EXPECT_EQ(wb->finish().error().status, PersistStatus::SegmentMismatch);
  // a carries on and finishes; the run holds a's steps only.
  auto rc = sa.step(market.states[2]->state);
  ASSERT_TRUE(rc.has_value());
  ASSERT_TRUE(wa->append_frame(sa.current()).has_value());
  ASSERT_TRUE(wa->append_explain(**rc).has_value());
  rec.frames.push_back(sa.current());
  rec.explains.push_back(**rc);
  rec.leg_ids = rec.explains.front().id;
  ASSERT_TRUE(wa->finish().has_value());
  auto a = RiskRunArchive::open(dir.string());
  ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
  expect_archive_matches(*a, rec);
  // A third writer on the now-listed run is refused at create().
  RiskStream sc(market.book, RiskKernel::family(), RiskNeeds{}, ExplainOptions{});
  EXPECT_EQ(RiskRunWriter::create(dir.string(), sc.prepared(), one).error().status,
            PersistStatus::AlreadyExists);
}

TEST(PersistRiskRun, CompactionUnderALiveWriterRefusesTheWriter) {
  const Market market = make_market(4);
  const fs::path dir = fresh_dir("compact-live");
  RiskStream stream(market.book, RiskKernel::family(), RiskNeeds{}, ExplainOptions{});
  RiskRunOptions one;
  one.segment_bytes = 1;
  auto w = RiskRunWriter::create(dir.string(), stream.prepared(), one);
  ASSERT_TRUE(w.has_value());
  Recorded rec;
  ASSERT_TRUE(stream.start(market.states[0]->state).has_value());
  ASSERT_TRUE(w->append_frame(stream.current()).has_value());
  rec.frames.push_back(stream.current());
  for (std::size_t k = 1; k < 3; ++k) {
    auto r = stream.step(market.states[k]->state);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(w->append_frame(stream.current()).has_value());
    ASSERT_TRUE(w->append_explain(**r).has_value());
    if (k < 2) {
      rec.frames.push_back(stream.current());
      rec.explains.push_back(**r);
    }
  }
  ASSERT_EQ(manifest_size(dir), 2u); // steps 0 and 1 sealed, step 2 staged
  ASSERT_EQ(compact_risk_run(dir.string(), PersistCodec::None, 0).value(), 2u);
  // The writer's next seal finds the compacted manifest: refused, and the writer is done.
  auto r3 = stream.step(market.states[3]->state);
  ASSERT_TRUE(r3.has_value());
  const auto refused = w->append_frame(stream.current());
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().status, PersistStatus::SegmentMismatch);
  EXPECT_EQ(w->finish().error().status, PersistStatus::SegmentMismatch);
  // The run still opens on the compacted manifest: the sealed steps, bit-exact.
  EXPECT_EQ(manifest_size(dir), 1u);
  rec.leg_ids = rec.explains.front().id;
  auto a = RiskRunArchive::open(dir.string());
  ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
  expect_archive_matches(*a, rec);
}

TEST(PersistRiskRun, FlushFinishFlushesEarlierPublishSegments) {
  // Defaults: segment seals at Publish, finish() at Flush. finish() flushes the
  // writer's Publish segments before its last seal, so a power loss after it returns
  // cannot leave the flushed manifest naming an unflushed segment.
  const Market market = make_market(4);
  const fs::path dir = fresh_dir("flush-finish");
  RiskStream stream(market.book, RiskKernel::family(), RiskNeeds{}, ExplainOptions{});
  RiskRunOptions one;
  one.segment_bytes = 1;
  auto w = RiskRunWriter::create(dir.string(), stream.prepared(), one);
  ASSERT_TRUE(w.has_value());
  Recorded rec;
  ASSERT_TRUE(stream.start(market.states[0]->state).has_value());
  ASSERT_TRUE(w->append_frame(stream.current()).has_value());
  rec.frames.push_back(stream.current());
  for (std::size_t k = 1; k < market.states.size(); ++k) {
    auto r = stream.step(market.states[k]->state);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(w->append_frame(stream.current()).has_value());
    ASSERT_TRUE(w->append_explain(**r).has_value());
    rec.frames.push_back(stream.current());
    rec.explains.push_back(**r);
  }
  rec.leg_ids = rec.explains.front().id;
  ASSERT_EQ(manifest_size(dir), 3u); // three Publish seals, the last step staged
#if defined(_WIN32)
  {
    // A handle denying write sharing on a Publish segment (what a mapped reader holds):
    // finish() cannot flush it, says Busy and changes nothing; a retry completes.
    auto d = PersistDirectory::open(dir.string());
    ASSERT_TRUE(d.has_value());
    const std::wstring held = fs::path{d->path(d->entries().front())}.wstring();
    HANDLE h = ::CreateFileW(held.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(h, INVALID_HANDLE_VALUE);
    const auto busy = w->finish();
    ::CloseHandle(h);
    ASSERT_FALSE(busy.has_value());
    EXPECT_EQ(busy.error().status, PersistStatus::Busy);
    EXPECT_EQ(manifest_size(dir), 3u);
  }
#endif
  ASSERT_TRUE(w->finish().has_value());
  EXPECT_EQ(manifest_size(dir), 4u);
  auto a = RiskRunArchive::open(dir.string());
  ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
  expect_archive_matches(*a, rec);
}
