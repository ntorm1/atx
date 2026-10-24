// Lane H1 (r6, 2026-10-23): live publication of serving records (api/fitting/serve_book.hpp)
// - versions and their atomic publication, deferred reclamation, readers x publisher with
// zero torn versions, lifetime / ABA, failure recovery (a publisher that dies mid-publish
// leaves the old version live), the async rebuild queue under a quote burst, geometry
// invalidation, the book's typed responses, the primary tier's no-cold-solve cap, the
// served chain, the serve_build stage, determinism across worker counts and the
// allocation-free query path.
#include "atx/vol/api/fitting/serve_book.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/fitting/slice_serve_record.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "fitting/deam_iv_cache.hpp"
#include "support/alloc_counter.hpp"

namespace atx::vol {
namespace {

constexpr double kSpot = 100.0;
constexpr double kRate = 0.03;
constexpr double kYield = 0.01;
constexpr std::int64_t kNow = 1'800'000'000'000'000'000; // Unix ns, 2027-01-15

std::uint64_t bits(double value) { return std::bit_cast<std::uint64_t>(value); }

double smile(double strike, double forward) {
  const double k = std::log(strike / forward);
  return 0.22 - 0.10 * k + 0.25 * k * k;
}

struct Chain {
  QuoteSliceMarket market;
  std::vector<QuoteSliceQuote> quotes;
};

// Accurate Andersen-Lake prices on a smile shifted by `vol_shift`, strikes 70-130 every
// 2.5, both sides, +/- 1 % spreads (the slice_serve_record_test fixture).
Chain smile_chain(double T, double vol_shift) {
  Chain out;
  out.market = QuoteSliceMarket{kSpot, T, T, kRate, kYield, 0.0};
  const double forward = kSpot * std::exp((kRate - kYield) * T);
  for (int i = 0; i < 25; ++i) {
    const double K = 70.0 + 2.5 * static_cast<double>(i);
    const double q = kYield + 2.0e-4 * std::sin(0.37 * K);
    for (const Side side : {Side::Call, Side::Put}) {
      const Result<double> price =
          andersen_lake(kSpot, K, T, smile(K, forward) + vol_shift, kRate, q, side);
      if (!price) continue;
      const double spread = std::max(0.01, 0.01 * *price);
      out.quotes.push_back({K, side, std::max(*price - spread, 0.0), *price + spread, q});
    }
  }
  return out;
}

QuoteSliceFit fit_value(double vol_shift, double T = 0.25) {
  const Chain chain = smile_chain(T, vol_shift);
  Result<QuoteSliceFit> fit = fit_quote_slice(chain.market, chain.quotes);
  EXPECT_TRUE(fit.has_value());
  return fit ? std::move(*fit) : QuoteSliceFit{};
}

std::shared_ptr<const QuoteSliceFit> fit_of(double vol_shift, double T = 0.25) {
  return std::make_shared<const QuoteSliceFit>(fit_value(vol_shift, T));
}

ServeRecordPolicy no_ladders() {
  ServeRecordPolicy p;
  p.ladders = false;
  return p;
}

std::shared_ptr<const SliceServeRecord> record_of(const QuoteSliceFit &fit,
                                                  const ServeRecordPolicy &policy) {
  Result<std::shared_ptr<const SliceServeRecord>> built = build_serve_record(fit, policy);
  EXPECT_TRUE(built.has_value());
  return built ? *built : nullptr;
}

// Two fits of one expiry 2 vol points apart and their tier-1 records (built once).
const QuoteSliceFit &fit_a() {
  static const auto fit = fit_of(0.0);
  return *fit;
}
const QuoteSliceFit &fit_b() {
  static const auto fit = fit_of(0.02);
  return *fit;
}
std::shared_ptr<const SliceServeRecord> rec_a() {
  static const auto r = record_of(fit_a(), no_ladders());
  return r;
}
std::shared_ptr<const SliceServeRecord> rec_b() {
  static const auto r = record_of(fit_b(), no_ladders());
  return r;
}
// The laddered record of fit A (the tick profile: its ladders accept IV roots).
std::shared_ptr<const SliceServeRecord> rec_ladder() {
  static const auto r = record_of(fit_a(), serve_record_policy_tick());
  return r;
}
// The laddered record of fit A under the default 12 x 6 planes: some of its ladders are
// served on a Sampled estimate (the fixture's put ladders; review-H1 F1).
std::shared_ptr<const SliceServeRecord> rec_default_ladders() {
  static const auto r = record_of(fit_a(), ServeRecordPolicy{});
  return r;
}

ServeBookEntry entry(std::uint64_t id, std::shared_ptr<const SliceServeRecord> record,
                     std::uint64_t tag = 0) {
  ServeBookEntry e;
  e.expiry_id = id;
  e.record = std::move(record);
  e.tag = tag;
  return e;
}

ServeRequest req(double strike, Side side, double spot, ServeTier tier, ServeTier tier_max,
                 std::int64_t now_ns = 0) {
  ServeRequest r;
  r.strike = strike;
  r.side = side;
  r.spot = spot;
  r.tier = tier;
  r.tier_max = tier_max;
  r.now_ns = now_ns;
  return r;
}

ServeBookPolicy inline_policy(std::size_t capacity = 64) {
  ServeBookPolicy p;
  p.build_workers = 0;
  p.queue_capacity = capacity;
  return p;
}

void expect_same(const ServeAnswer &a, const ServeAnswer &b) {
  EXPECT_EQ(bits(a.price), bits(b.price));
  EXPECT_EQ(bits(a.delta), bits(b.delta));
  EXPECT_EQ(bits(a.gamma), bits(b.gamma));
  EXPECT_EQ(bits(a.vega), bits(b.vega));
  EXPECT_EQ(bits(a.theta), bits(b.theta));
  EXPECT_EQ(bits(a.certificate_usd), bits(b.certificate_usd));
  EXPECT_EQ(a.status, b.status);
  EXPECT_EQ(a.route, b.route);
  EXPECT_EQ(a.tier, b.tier);
}

// ── Versions and the reader ──────────────────────────────────────────────────────────────

TEST(LivePublication, AnEmptyBookRefusesWithARouteAndQueuesTheMissingExpiry) {
  ServeBook bare(inline_policy());
  EXPECT_EQ(bare.version(), 0u);
  ServeBookReader r0 = bare.reader();
  const ServeAnswer none = r0.serve(7, req(100.0, Side::Call, kSpot, ServeTier::Ladder,
                                           ServeTier::Family));
  EXPECT_EQ(none.status, ServeStatus::TierUnavailable);
  EXPECT_EQ(none.route, ServeRoute::ColdPath);
  EXPECT_TRUE(std::isnan(none.price));
  EXPECT_EQ(bare.counters().enqueued, 0u); // no rebuild function: nothing to queue

  std::vector<ServeRebuildRequest> seen;
  ServeBook book(inline_policy(), [&seen](const ServeRebuildRequest &q) {
    seen.push_back(q);
    return std::optional<ServeBookEntry>(entry(q.expiry_id, rec_a(), 11));
  });
  ServeBookReader r = book.reader();
  const ServeAnswer miss = r.serve(7, req(100.0, Side::Call, kSpot, ServeTier::Ladder,
                                          ServeTier::Family));
  EXPECT_EQ(miss.status, ServeStatus::TierUnavailable);
  EXPECT_EQ(miss.route, ServeRoute::ColdPath);
  EXPECT_EQ(book.counters().enqueued, 1u);
  EXPECT_EQ(book.run_pending(), 1u);
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].expiry_id, 7u);
  EXPECT_EQ(seen[0].reason, ServeRebuildReason::Missing);
  EXPECT_EQ(book.version(), 1u);
  const ServeAnswer hit = r.serve(7, req(100.0, Side::Call, kSpot, ServeTier::Ladder,
                                         ServeTier::Family));
  EXPECT_EQ(hit.status, ServeStatus::Ok);
  EXPECT_EQ(r.pinned().number, 1u);
  ASSERT_NE(r.pinned().find(7), nullptr);
  EXPECT_EQ(r.pinned().find(7)->tag, 11u);
}

// A published record answers through the book exactly as it answers alone (the book adds
// only the version, the cap and the typed responses).
TEST(LivePublication, APublishedRecordAnswersBitForBitAsTheRecordItself) {
  ServeBook book(inline_policy());
  ASSERT_TRUE(book.publish_expiry(entry(1, rec_a())).has_value());
  ServeBookReader r = book.reader();
  std::size_t answered = 0;
  for (const ServedNode &node : rec_a()->nodes()) {
    for (const Side side : {Side::Call, Side::Put}) {
      for (const double S : {kSpot, 0.993 * kSpot}) {
        for (const double K : {node.strike, node.strike + 0.9}) {
          const ServeRequest q = req(K, side, S, ServeTier::Ladder, ServeTier::Family);
          const ServeAnswer via = r.serve(1, q);
          expect_same(via, rec_a()->serve(q));
          answered += via.status == ServeStatus::Ok ? 1u : 0u;
          ServeIvRequest iv;
          iv.strike = K;
          iv.side = side;
          iv.spot = S;
          iv.target_price = via.price;
          iv.tier = ServeTier::Family;
          iv.tier_max = ServeTier::Family;
          const ServeIvAnswer a = r.implied_vol(1, iv);
          const ServeIvAnswer b = rec_a()->implied_vol(iv);
          EXPECT_EQ(bits(a.sigma), bits(b.sigma));
          EXPECT_EQ(a.status, b.status);
        }
      }
    }
  }
  EXPECT_GT(answered, 50u);
}

// No reader-triggered synchronous cold solve: the primary tier (Family by default) caps
// every request; Certified's Fast confirm at a moved spot is a ColdPath route unless the
// book's policy allows it.
TEST(LivePublication, ThePrimaryTierNeverRunsASolveAboveItsCap) {
  ServeBook book(inline_policy());
  ASSERT_TRUE(book.publish_expiry(entry(1, rec_a())).has_value());
  ServeBookReader r = book.reader();
  const ServeRequest certified = req(101.3, Side::Put, 0.996 * kSpot, ServeTier::Certified,
                                     ServeTier::Certified);
  ASSERT_EQ(rec_a()->serve(certified).status, ServeStatus::Ok); // the record would solve
  const ServeAnswer capped = r.serve(1, certified);
  EXPECT_EQ(capped.status, ServeStatus::TierUnavailable);
  EXPECT_EQ(capped.route, ServeRoute::ColdPath);
  EXPECT_TRUE(std::isnan(capped.price));
  ServeIvRequest iv;
  iv.strike = 101.3;
  iv.side = Side::Put;
  iv.spot = 0.996 * kSpot;
  iv.target_price = rec_a()->serve(certified).price;
  iv.tier = ServeTier::Certified;
  iv.tier_max = ServeTier::Certified;
  const ServeIvAnswer iv_capped = r.implied_vol(1, iv);
  EXPECT_EQ(iv_capped.status, ServeStatus::TierUnavailable);
  EXPECT_EQ(iv_capped.route, ServeRoute::ColdPath);
  EXPECT_EQ(iv_capped.evaluations, 0u);
  // A request reaching past the cap is answered by the tiers under it.
  const ServeAnswer walked = r.serve(1, req(101.3, Side::Put, 0.996 * kSpot, ServeTier::Ladder,
                                            ServeTier::Certified));
  EXPECT_EQ(walked.status, ServeStatus::Ok);
  EXPECT_EQ(walked.tier, ServeTier::Family);

  ServeBookPolicy allow = inline_policy();
  allow.primary_tier_max = ServeTier::Certified;
  ServeBook open(allow);
  ASSERT_TRUE(open.publish_expiry(entry(1, rec_a())).has_value());
  ServeBookReader ro = open.reader();
  const ServeAnswer solved = ro.serve(1, certified);
  EXPECT_EQ(solved.status, ServeStatus::Ok);
  EXPECT_EQ(solved.tier, ServeTier::Certified);
  EXPECT_EQ(solved.certificate_class, ServeBoundClass::KernelTested);
}

// ── Failure recovery ─────────────────────────────────────────────────────────────────────

// A publisher that dies mid-publish (an exception between its puts and commit, or an
// abandoned transaction) publishes nothing: the old version stays live, whole.
TEST(LivePublication, APublisherThatDiesMidPublishLeavesTheOldVersionLive) {
  ServeBook book(inline_policy());
  ASSERT_TRUE(book.publish(std::array{entry(1, rec_a(), 1), entry(2, rec_a(), 1)}).has_value());
  ServeBookReader r = book.reader();
  const ServeRequest q = req(101.3, Side::Put, kSpot, ServeTier::Ladder, ServeTier::Family);
  const ServeAnswer before = r.serve(1, q);
  ASSERT_EQ(before.status, ServeStatus::Ok);

  std::jthread dying([&book] {
    try {
      ServeBookTransaction tx = book.transaction();
      ASSERT_TRUE(tx.put(entry(1, rec_b(), 2)).has_value());
      tx.retire(2);
      throw std::runtime_error("publisher died before commit");
    } catch (const std::runtime_error &) {
    }
  });
  dying.join();
  {
    ServeBookTransaction abandoned = book.transaction();
    ASSERT_TRUE(abandoned.put(entry(3, rec_b(), 2)).has_value());
  }
  EXPECT_EQ(book.version(), 1u);
  EXPECT_EQ(book.counters().aborted, 2u);
  expect_same(r.serve(1, q), before);
  ASSERT_NE(r.pinned().find(2), nullptr);
  EXPECT_EQ(r.pinned().find(3), nullptr);
  EXPECT_EQ(r.pinned().find(1)->tag, 1u);

  // An invalid batch is refused whole: nothing of it is published.
  ServeBookEntry hollow = entry(4, nullptr);
  const auto bad = book.publish(std::array{entry(1, rec_b(), 3), hollow});
  EXPECT_FALSE(bad.has_value());
  const auto dup = book.publish(std::array{entry(5, rec_b(), 3), entry(5, rec_a(), 3)});
  EXPECT_FALSE(dup.has_value());
  EXPECT_EQ(book.version(), 1u);
  EXPECT_EQ(r.pinned().find(1)->tag, 1u);
}

// A rebuild that throws or returns nothing publishes nothing and is counted; the book
// keeps serving the old record.
TEST(LivePublication, AFailedRebuildLeavesTheOldVersionLive) {
  int calls = 0;
  ServeBook book(inline_policy(), [&calls](const ServeRebuildRequest &q)
                     -> std::optional<ServeBookEntry> {
    ++calls;
    if (q.expiry_id == 1) throw std::runtime_error("rebuild died");
    return std::nullopt;
  });
  ASSERT_TRUE(book.publish_expiry(entry(1, rec_a(), 1)).has_value());
  ASSERT_TRUE(book.publish_expiry(entry(2, rec_a(), 1)).has_value());
  EXPECT_EQ(book.request_rebuild(1), ServeEnqueue::Queued);
  EXPECT_EQ(book.request_rebuild(2), ServeEnqueue::Queued);
  EXPECT_EQ(book.run_pending(), 2u);
  EXPECT_EQ(calls, 2);
  const ServeBookCounters c = book.counters();
  EXPECT_EQ(c.rebuild_failed, 2u);
  EXPECT_EQ(c.rebuilt, 0u);
  EXPECT_EQ(book.version(), 2u);
  ServeBookReader r = book.reader();
  EXPECT_EQ(r.serve(1, req(100.0, Side::Call, kSpot, ServeTier::Ladder, ServeTier::Family))
                .status,
            ServeStatus::Ok);
}

// ── Typed responses ──────────────────────────────────────────────────────────────────────

// Expiry and event instants: at or after either the entry is Invalidated (route Refit) and
// a rebuild (reason Invalidated) is queued; an entry with instants needs a clock.
TEST(LivePublication, ExpiryAndEventInstantsInvalidateTheGeometry) {
  std::vector<ServeRebuildReason> reasons;
  ServeBook book(inline_policy(), [&reasons](const ServeRebuildRequest &q) {
    reasons.push_back(q.reason);
    return std::optional<ServeBookEntry>{};
  });
  ServeBookEntry expiring = entry(1, rec_a());
  expiring.expiry_ns = kNow + 1000;
  ServeBookEntry dividend = entry(2, rec_a());
  dividend.next_event_ns = kNow + 500;
  ASSERT_TRUE(book.publish(std::array{expiring, dividend}).has_value());
  ServeBookReader r = book.reader();
  for (const std::uint64_t id : {1u, 2u}) {
    const ServeAnswer live =
        r.serve(id, req(100.0, Side::Call, kSpot, ServeTier::Ladder, ServeTier::Family, kNow));
    EXPECT_EQ(live.status, ServeStatus::Ok) << id;
    const ServeAnswer gone = r.serve(
        id, req(100.0, Side::Call, kSpot, ServeTier::Ladder, ServeTier::Family, kNow + 1000));
    EXPECT_EQ(gone.status, ServeStatus::Invalidated) << id;
    EXPECT_EQ(gone.route, ServeRoute::Refit);
    EXPECT_TRUE(std::isnan(gone.price) && std::isnan(gone.delta));
    ServeIvRequest iv;
    iv.strike = 100.0;
    iv.spot = kSpot;
    iv.target_price = live.price;
    iv.tier = ServeTier::Family;
    iv.tier_max = ServeTier::Family;
    iv.now_ns = kNow + 2000;
    EXPECT_EQ(r.implied_vol(id, iv).status, ServeStatus::Invalidated);
    const ServeAnswer clockless =
        r.serve(id, req(100.0, Side::Call, kSpot, ServeTier::Ladder, ServeTier::Family, 0));
    EXPECT_EQ(clockless.status, ServeStatus::Stale) << id;
    EXPECT_EQ(clockless.route, ServeRoute::ColdPath);
  }
  EXPECT_EQ(book.counters().deduplicated, 2u); // each IV query met its queued request
  EXPECT_EQ(book.run_pending(), 2u);
  ASSERT_EQ(reasons.size(), 2u);
  EXPECT_EQ(reasons[0], ServeRebuildReason::Invalidated);
  EXPECT_EQ(reasons[1], ServeRebuildReason::Invalidated);
}

// Stale records queue ONE rebuild per expiry; a full queue answers BudgetExhausted
// (nothing queued); the rebuilt record then serves.
TEST(LivePublication, StaleQueuesOneRebuildAndAFullQueueIsBudgetExhausted) {
  ServeRecordPolicy aging = no_ladders();
  aging.max_age_ns = 1000;
  const auto old = record_of(fit_a(), aging);
  ASSERT_NE(old, nullptr);
  ServeBook book(inline_policy(1), [](const ServeRebuildRequest &q) {
    return std::optional<ServeBookEntry>(entry(q.expiry_id, rec_a(), 2));
  });
  ASSERT_TRUE(book.publish(std::array{entry(1, old, 1), entry(2, old, 1)}).has_value());
  ServeBookReader r = book.reader();
  const std::int64_t late = old->version().as_of_ns + 5000;
  const ServeRequest q = req(100.0, Side::Call, kSpot, ServeTier::Ladder, ServeTier::Family, late);
  const ServeAnswer first = r.serve(1, q);
  EXPECT_EQ(first.status, ServeStatus::Stale);
  EXPECT_EQ(first.route, ServeRoute::Rebuild);
  EXPECT_EQ(first.age_ns, 5000);
  EXPECT_EQ(r.serve(1, q).status, ServeStatus::Stale); // deduplicated
  const ServeAnswer full = r.serve(2, q);
  EXPECT_EQ(full.status, ServeStatus::BudgetExhausted);
  EXPECT_EQ(full.route, ServeRoute::Rebuild);
  EXPECT_TRUE(std::isnan(full.price));
  ServeBookCounters c = book.counters();
  EXPECT_EQ(c.enqueued, 1u);
  EXPECT_EQ(c.deduplicated, 1u);
  EXPECT_EQ(c.rejected, 1u);
  EXPECT_EQ(c.queue_depth, 1u);
  EXPECT_EQ(book.run_pending(), 1u);
  EXPECT_EQ(r.serve(1, q).status, ServeStatus::Ok);
  EXPECT_EQ(r.serve(2, q).status, ServeStatus::Stale); // now it fits in the queue
  EXPECT_EQ(book.run_pending(), 1u);
  EXPECT_EQ(r.serve(2, q).status, ServeStatus::Ok);
  c = book.counters();
  EXPECT_EQ(c.rebuilt, 2u);
  EXPECT_GE(c.rebuild_max_ns, c.queue_age_max_ns);
}

// The record's own typed refusals pass through the book unchanged: out of domain, a
// price-only node, unsupported risk (NaN with a reason, never zero).
TEST(LivePublication, TheRecordsTypedRefusalsPassThroughTheBook) {
  QuoteSliceFit fit = fit_value(0.0);
  const std::size_t n = fit.prices->nodes().size();
  const std::size_t i = n / 2;
  fit.price_only_nodes.assign(n, 0u);
  fit.price_only_nodes[i] = 2u; // the put
  const auto priced_only = record_of(fit, no_ladders());
  ASSERT_NE(priced_only, nullptr);
  ServeBook book(inline_policy());
  ASSERT_TRUE(book.publish(std::array{entry(1, priced_only), entry(2, rec_ladder())}).has_value());
  ServeBookReader r = book.reader();

  const ServeAnswer outside =
      r.serve(1, req(50.0, Side::Call, kSpot, ServeTier::Ladder, ServeTier::Family));
  EXPECT_EQ(outside.status, ServeStatus::OutOfDomain);
  EXPECT_EQ(outside.route, ServeRoute::Refit);

  const double K = priced_only->nodes()[i].strike;
  const ServeAnswer here = r.serve(1, req(K, Side::Put, kSpot, ServeTier::Ladder,
                                          ServeTier::Family));
  EXPECT_EQ(here.status, ServeStatus::PriceOnly);
  EXPECT_EQ(bits(here.price), bits(priced_only->nodes()[i].put.price));
  const ServeAnswer moved = r.serve(1, req(K, Side::Put, 1.01 * kSpot, ServeTier::Ladder,
                                           ServeTier::Family));
  EXPECT_EQ(moved.status, ServeStatus::PriceOnly);
  EXPECT_EQ(moved.route, ServeRoute::Refit);
  EXPECT_TRUE(std::isnan(moved.price));

  std::size_t ladders = 0;
  for (const ServedNode &node : rec_ladder()->nodes()) {
    if (!(node.call.tiers & kServeNodeLadder)) continue;
    ServeRequest all = req(node.strike, Side::Call, 1.001 * kSpot, ServeTier::Ladder,
                           ServeTier::Ladder);
    all.greeks = ServeGreekMask::all();
    const ServeAnswer a = r.serve(2, all);
    if (a.status != ServeStatus::Ok) continue;
    EXPECT_TRUE(std::isnan(a.rho) && std::isnan(a.phi));
    EXPECT_EQ(a.reason[static_cast<std::size_t>(ServeGreek::Rho)],
              ServeGreekReason::TierHasNoGreek);
    ++ladders;
  }
  EXPECT_GT(ladders, 5u);
}

// ── The served chain ─────────────────────────────────────────────────────────────────────

struct ChainColumns {
  std::vector<std::uint64_t> id;
  std::vector<double> strike;
  std::vector<Side> side;
  std::vector<double> price, delta, gamma, vega, theta;
  std::vector<TickStatus> tick;
  std::vector<ServedChainStatement> served; // each answer's statement (review-H1 F1)

  explicit ChainColumns(std::size_t n)
      : price(n), delta(n), gamma(n), vega(n), theta(n), tick(n), served(n) {
    id.reserve(n);
    strike.reserve(n);
    side.reserve(n);
  }
  [[nodiscard]] ServedChainView view() const { return {id, strike, side}; }
  [[nodiscard]] ChainTickOutputs out() { return {price, delta, gamma, vega, theta, tick}; }
};

TEST(LivePublication, WithheldSupportRefusesBookIvAndChainWithoutFallbackPrices) {
  QuoteSliceFit fit = fit_value(0.0);
  const auto nodes = fit.prices->nodes();
  const std::size_t i = nodes.size() / 2;
  fit.withheld_nodes.assign(nodes.size(), 0u);
  fit.withheld_nodes[i] = 2u;
  const auto record = record_of(fit, no_ladders());
  ASSERT_NE(record, nullptr);
  ServeBook book(inline_policy());
  ASSERT_TRUE(book.publish_expiry(entry(7, record)).has_value());
  auto reader = book.reader();
  ChainColumns chain(6);
  for (const double strike : {nodes[i].strike,
                             (nodes[i - 1].strike + nodes[i].strike) / 2.0,
                             (nodes[i].strike + nodes[i + 1].strike) / 2.0}) {
    for (const Side side : {Side::Call, Side::Put}) {
      chain.id.push_back(7);
      chain.strike.push_back(strike);
      chain.side.push_back(side);
    }
    const auto answer = reader.serve(7, req(strike, Side::Put, kSpot,
                                           ServeTier::Ladder, ServeTier::Certified));
    EXPECT_EQ(answer.status, ServeStatus::OutOfDomain);
    EXPECT_EQ(answer.route, ServeRoute::Refit);
    EXPECT_TRUE(std::isnan(answer.price));
    ServeIvRequest iv;
    iv.strike = strike;
    iv.side = Side::Put;
    iv.spot = kSpot;
    iv.target_price = 4.0;
    const auto identified = reader.implied_vol(7, iv);
    EXPECT_EQ(identified.status, ServeStatus::OutOfDomain);
    EXPECT_EQ(identified.route, ServeRoute::Refit);
    EXPECT_TRUE(std::isnan(identified.sigma));
  }
  for (const double spot : {kSpot, 1.002 * kSpot}) {
    Status status;
    const auto allocations = testkit::count_allocations([&] {
      status = reader.serve_chain(spot, 0, chain.view(), ServeTier::Ladder,
                                 ServeTier::Family, chain.out(), chain.served);
    });
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(allocations, 0u);
    for (std::size_t k = 0; k < chain.strike.size(); ++k) {
      if (chain.side[k] == Side::Put) {
        EXPECT_EQ(chain.served[k].status, ServeStatus::OutOfDomain);
        EXPECT_EQ(chain.served[k].route, ServeRoute::Refit);
        EXPECT_EQ(chain.tick[k], TickStatus::Unpriceable);
        EXPECT_TRUE(std::isnan(chain.price[k]));
      } else {
        EXPECT_EQ(chain.served[k].status, ServeStatus::Ok);
        EXPECT_TRUE(std::isfinite(chain.price[k]));
      }
    }
  }
  EXPECT_EQ(book.counters().enqueued, 0u);
}

// Every node strike and a between-node strike of each expiry in `ids`, both sides.
ChainColumns chain_over(const SliceServeRecord &record, std::span<const std::uint64_t> ids) {
  std::size_t n = 0;
  for (std::size_t k = 0; k < ids.size(); ++k) n += 4 * record.nodes().size();
  ChainColumns c(n);
  for (const std::uint64_t id : ids) {
    for (const ServedNode &node : record.nodes()) {
      for (const Side side : {Side::Call, Side::Put}) {
        for (const double K : {node.strike, node.strike + 1.1}) {
          c.id.push_back(id);
          c.strike.push_back(K);
          c.side.push_back(side);
        }
      }
    }
  }
  return c;
}

// One version for the whole chain; each contract's columns are the record's answer to the
// chain path's request, bit for bit.
TEST(LivePublication, TheServedChainIsTheRecordsAnswerPerContract) {
  ServeBook book(inline_policy());
  ASSERT_TRUE(book.publish(std::array{entry(1, rec_ladder()), entry(2, rec_a())}).has_value());
  ServeBookReader r = book.reader();
  const std::array<std::uint64_t, 2> ids{1, 2};
  ChainColumns c = chain_over(*rec_ladder(), ids);
  const double S = 1.002 * kSpot;
  ASSERT_TRUE(r.serve_chain(S, 0, c.view(), ServeTier::Ladder, ServeTier::Family, c.out(),
                            c.served)
                  .has_value());
  std::size_t ok = 0;
  std::size_t ladder = 0;
  for (std::size_t k = 0; k < c.strike.size(); ++k) {
    ServeRequest q = req(c.strike[k], c.side[k], S, ServeTier::Ladder, ServeTier::Family);
    q.family_bound = false;
    const auto &rec = c.id[k] == 1 ? rec_ladder() : rec_a();
    const ServeAnswer a = rec->serve(q);
    EXPECT_EQ(c.served[k].status, a.status) << k;
    EXPECT_EQ(c.served[k].tier, a.tier) << k;
    EXPECT_EQ(c.served[k].sampled, a.sampled) << k;
    EXPECT_EQ(bits(c.price[k]), bits(a.price)) << k;
    EXPECT_EQ(bits(c.delta[k]), bits(a.delta)) << k;
    EXPECT_EQ(bits(c.gamma[k]), bits(a.gamma)) << k;
    EXPECT_EQ(bits(c.vega[k]), bits(a.vega)) << k;
    EXPECT_EQ(bits(c.theta[k]), bits(a.theta)) << k;
    if (a.status == ServeStatus::Ok) {
      EXPECT_EQ(c.tick[k], a.sampled ? TickStatus::Fallback : TickStatus::Ok);
      ++ok;
      ladder += a.tier == ServeTier::Ladder ? 1u : 0u;
    } else if (a.status == ServeStatus::Exercised) {
      EXPECT_EQ(c.tick[k], TickStatus::Exercised);
    } else {
      // A refusal is Unpriceable (NaN); a price-only answer keeps its price (Fallback).
      EXPECT_EQ(c.tick[k], std::isnan(a.price) ? TickStatus::Unpriceable : TickStatus::Fallback);
    }
  }
  EXPECT_GT(ok, c.strike.size() / 2);
  EXPECT_GT(ladder, 20u);
  ChainColumns shortc = c;
  shortc.served.pop_back();
  EXPECT_FALSE(r.serve_chain(S, 0, shortc.view(), ServeTier::Ladder, ServeTier::Family,
                             shortc.out(), shortc.served)
                   .has_value());
}

// review-H1 F1: the served chain carries each answer's statement. A Sampled answer is an
// estimate, never certified: its TickStatus is Fallback (the record routes' value "NOT
// covered by any certificate"), never Ok, so a reader of the status span alone cannot take
// it for a certified one.
TEST(LivePublication, TheServedChainCarriesEachAnswersStatement) {
  ServeBook book(inline_policy());
  ASSERT_TRUE(book.publish_expiry(entry(1, rec_default_ladders())).has_value());
  ServeBookReader r = book.reader();
  const std::array<std::uint64_t, 1> ids{1};
  ChainColumns c = chain_over(*rec_default_ladders(), ids);
  const double S = 1.002 * kSpot;
  ASSERT_TRUE(r.serve_chain(S, 0, c.view(), ServeTier::Ladder, ServeTier::Family, c.out(),
                            c.served)
                  .has_value());
  std::size_t sampled = 0;
  std::size_t not_sampled = 0;
  for (std::size_t k = 0; k < c.strike.size(); ++k) {
    ServeRequest q = req(c.strike[k], c.side[k], S, ServeTier::Ladder, ServeTier::Family);
    q.family_bound = false;
    const ServeAnswer a = rec_default_ladders()->serve(q);
    const ServedChainStatement &st = c.served[k];
    EXPECT_EQ(st.status, a.status) << k;
    EXPECT_EQ(st.route, a.route) << k;
    EXPECT_EQ(st.tier, a.tier) << k;
    EXPECT_EQ(st.sampled, a.sampled) << k;
    EXPECT_EQ(st.certificate_class, a.certificate_class) << k;
    EXPECT_EQ(bits(st.certificate_usd), bits(a.certificate_usd)) << k;
    EXPECT_EQ(st.map_certificate_class, a.map_certificate_class) << k;
    EXPECT_EQ(bits(st.map_certificate_usd), bits(a.map_certificate_usd)) << k;
    if (a.status != ServeStatus::Ok) continue;
    if (a.sampled) {
      ++sampled;
      EXPECT_EQ(c.tick[k], TickStatus::Fallback) << k;
      // The statement says what the status span no longer hides: an estimate.
      EXPECT_TRUE(st.certificate_class == ServeBoundClass::Sampled ||
                  st.map_certificate_class == ServeBoundClass::Sampled)
          << k;
    } else {
      ++not_sampled;
      EXPECT_EQ(c.tick[k], TickStatus::Ok) << k;
      EXPECT_NE(st.certificate_class, ServeBoundClass::Sampled) << k;
      EXPECT_NE(st.map_certificate_class, ServeBoundClass::Sampled) << k;
    }
  }
  EXPECT_GT(sampled, 0u);
  EXPECT_GT(not_sampled, 0u);

  // C0's price-only answer travels too: the node's reason and IV interval, on the answer
  // and on the chain's statement (at the fit spot the price is kept: Fallback).
  QuoteSliceFit fit = fit_value(0.0);
  const std::size_t n = fit.prices->nodes().size();
  const std::size_t i = n / 2;
  fit.price_only_nodes.assign(n, 0u);
  fit.price_only_nodes[i] = 2u; // the put
  const auto priced_only = record_of(fit, no_ladders());
  ASSERT_NE(priced_only, nullptr);
  const ServedNodeSide &node = priced_only->nodes()[i].put;
  ASSERT_EQ(node.status, QuoteSliceEvaluationStatus::PriceOnly);
  ASSERT_TRUE(book.publish_expiry(entry(2, priced_only)).has_value());
  const ServeAnswer direct = priced_only->serve(
      req(priced_only->nodes()[i].strike, Side::Put, kSpot, ServeTier::Ladder, ServeTier::Family));
  EXPECT_EQ(direct.status, ServeStatus::PriceOnly);
  EXPECT_EQ(direct.price_only_reason, node.reason);
  EXPECT_EQ(bits(direct.iv_lower), bits(node.iv_lower));
  EXPECT_EQ(bits(direct.iv_upper), bits(node.iv_upper));
  const std::array<std::uint64_t, 1> ids2{2};
  ChainColumns p = chain_over(*priced_only, ids2);
  ASSERT_TRUE(r.serve_chain(kSpot, 0, p.view(), ServeTier::Ladder, ServeTier::Family, p.out(),
                            p.served)
                  .has_value());
  std::size_t price_only = 0;
  for (std::size_t k = 0; k < p.strike.size(); ++k) {
    if (p.served[k].status != ServeStatus::PriceOnly) {
      EXPECT_EQ(p.served[k].price_only_reason, QuoteSlicePriceOnlyReason::None) << k;
      continue;
    }
    ++price_only;
    EXPECT_EQ(p.side[k], Side::Put) << k;
    EXPECT_EQ(p.served[k].price_only_reason, node.reason) << k;
    EXPECT_EQ(bits(p.served[k].iv_lower), bits(node.iv_lower)) << k;
    EXPECT_EQ(bits(p.served[k].iv_upper), bits(node.iv_upper)) << k;
    EXPECT_EQ(p.tick[k], TickStatus::Fallback) << k;
    EXPECT_EQ(bits(p.price[k]), bits(node.price)) << k;
  }
  EXPECT_EQ(price_only, 1u);
  std::printf("[ statement ] sampled %zu not_sampled %zu price_only_reason %d iv [%g, %g]\n",
              sampled, not_sampled, static_cast<int>(node.reason), node.iv_lower,
              node.iv_upper);
}

// ── Concurrency and lifetime ─────────────────────────────────────────────────────────────

// Readers x publisher: one publisher alternates whole versions between two record sets
// (tag = version number; even: A, odd: B) on three expiries while four readers serve the
// chain. Every chain must be one version's: the pinned entries' tags agree, and every
// contract's price is that set's answer bit for bit. Versions never go backwards.
TEST(LivePublication, ReadersNeverSeeATornVersion) {
  ServeBook book(inline_policy());
  const std::array<std::uint64_t, 3> ids{1, 2, 3};
  const auto publish = [&book, &ids](std::uint64_t tag) {
    const auto &rec = tag % 2 == 0 ? rec_a() : rec_b();
    std::array<ServeBookEntry, 3> e{entry(ids[0], rec, tag), entry(ids[1], rec, tag),
                                    entry(ids[2], rec, tag)};
    return book.publish(e);
  };
  ASSERT_TRUE(publish(1).has_value()); // version 1, tag 1 (B)
  // The expected answers of each set (a sparse chain: every 3rd node).
  ChainColumns proto(0);
  for (const std::uint64_t id : ids) {
    for (std::size_t n = 0; n < rec_a()->nodes().size(); n += 3) {
      for (const Side side : {Side::Call, Side::Put}) {
        proto.id.push_back(id);
        proto.strike.push_back(rec_a()->nodes()[n].strike + 0.7);
        proto.side.push_back(side);
      }
    }
  }
  const std::size_t m = proto.strike.size();
  const double S = 0.998 * kSpot;
  std::array<std::vector<std::uint64_t>, 2> expected;
  for (int set = 0; set < 2; ++set) {
    const auto &rec = set == 0 ? rec_a() : rec_b();
    for (std::size_t k = 0; k < m; ++k) {
      ServeRequest q = req(proto.strike[k], proto.side[k], S, ServeTier::Ladder,
                           ServeTier::Family);
      q.family_bound = false;
      expected[static_cast<std::size_t>(set)].push_back(bits(rec->serve(q).price));
    }
  }
  ASSERT_NE(expected[0], expected[1]);

  // The publisher keeps publishing until every reader has served kChains chains, so the
  // readers' chains interleave with thousands of publications.
  constexpr int kReaders = 4;
  constexpr std::uint64_t kChains = 400;
  std::atomic<bool> done{false};
  std::array<std::atomic<std::uint64_t>, kReaders> chains{};
  std::array<std::uint64_t, kReaders> torn{};
  std::array<std::uint64_t, kReaders> backwards{};
  std::array<std::uint64_t, kReaders> distinct{};
  std::uint64_t versions = 1;
  {
    std::vector<std::jthread> readers;
    for (int t = 0; t < kReaders; ++t) {
      readers.emplace_back([&, t] {
        ServeBookReader r = book.reader();
        ChainColumns c(m);
        c.id = proto.id;
        c.strike = proto.strike;
        c.side = proto.side;
        std::uint64_t last = 0;
        while (!done.load(std::memory_order_acquire)) {
          if (!r.serve_chain(S, 0, c.view(), ServeTier::Ladder, ServeTier::Family, c.out(),
                             c.served)
                   .has_value()) {
            ++torn[t];
            continue;
          }
          const ServeBookVersion &v = r.pinned();
          if (v.number < last) ++backwards[t];
          if (v.number != last) ++distinct[t];
          last = v.number;
          const std::uint64_t tag = v.entries.front().tag;
          bool whole = tag == v.number;
          for (const ServeBookEntry &e : v.entries) whole = whole && e.tag == tag;
          const std::vector<std::uint64_t> &want = expected[tag % 2];
          for (std::size_t k = 0; k < m; ++k) whole = whole && bits(c.price[k]) == want[k];
          torn[t] += whole ? 0u : 1u;
          chains[t].fetch_add(1, std::memory_order_release);
        }
      });
    }
    const auto slowest = [&chains] {
      std::uint64_t low = ~std::uint64_t{0};
      for (const auto &n : chains) low = std::min(low, n.load(std::memory_order_acquire));
      return low;
    };
    while (slowest() < kChains && versions < 5'000'000) {
      if (!publish(versions + 1).has_value()) {
        ADD_FAILURE() << "publish " << versions + 1;
        break;
      }
      ++versions;
    }
    done.store(true, std::memory_order_release);
  }
  std::uint64_t all_chains = 0;
  std::uint64_t all_torn = 0;
  for (int t = 0; t < kReaders; ++t) {
    all_torn += torn[t];
    EXPECT_EQ(torn[t], 0u) << t;
    EXPECT_EQ(backwards[t], 0u) << t;
    EXPECT_GT(distinct[t], 20u) << t; // the reader followed many versions
    all_chains += chains[t].load();
  }
  EXPECT_EQ(book.version(), versions);
  EXPECT_GE(all_chains, kReaders * kChains);
  EXPECT_GT(versions, 100u);
  // Every retired version is freed once the readers are gone.
  book.reclaim();
  const ServeBookCounters c = book.counters();
  EXPECT_EQ(c.published, versions);
  EXPECT_EQ(c.retired_held, 0u);
  EXPECT_EQ(c.reclaimed, versions); // versions 0 (the empty book) .. versions - 1
  std::printf("[ torn-check ] readers %d chains %llu versions %llu torn %llu\n", kReaders,
              static_cast<unsigned long long>(all_chains),
              static_cast<unsigned long long>(versions),
              static_cast<unsigned long long>(all_torn));
}

// Lifetime and ABA: a reader keeps its pinned version (and its records) alive across any
// number of publications; the publisher frees it only once the reader re-pins; a reader
// re-pins on every new number, never on an address.
TEST(LivePublication, RetiredVersionsAreFreedOnlyAfterTheLastReaderLeaves) {
  ServeBook book(inline_policy());
  std::weak_ptr<const SliceServeRecord> watched;
  {
    auto doomed = record_of(fit_a(), no_ladders());
    watched = doomed;
    ASSERT_TRUE(book.publish_expiry(entry(1, std::move(doomed), 1)).has_value());
  }
  ServeBookReader r = book.reader();
  const ServeRequest q = req(101.3, Side::Put, kSpot, ServeTier::Ladder, ServeTier::Family);
  const ServeAnswer held = r.serve(1, q);
  ASSERT_EQ(held.status, ServeStatus::Ok);
  for (std::uint64_t tag = 2; tag <= 50; ++tag) {
    ASSERT_TRUE(book.publish_expiry(entry(1, rec_b(), tag)).has_value());
  }
  EXPECT_FALSE(watched.expired()); // the reader's pinned version holds it
  EXPECT_EQ(book.counters().retired_held, 1u);
  EXPECT_EQ(r.pinned().number, 1u);
  expect_same(r.pinned().find(1)->record->serve(q), held);
  // The next query re-pins (the number moved) and serves the newest version.
  const ServeAnswer now = r.serve(1, q);
  EXPECT_EQ(r.pinned().number, 50u);
  expect_same(now, rec_b()->serve(q));
  EXPECT_EQ(book.reclaim(), 1u);
  EXPECT_TRUE(watched.expired());
  EXPECT_EQ(book.counters().retired_held, 0u);
  // ABA: versions are freed and their memory reused; the reader follows the numbers.
  for (std::uint64_t tag = 51; tag <= 400; ++tag) {
    ASSERT_TRUE(book.publish_expiry(entry(1, tag % 2 == 0 ? rec_a() : rec_b(), tag)).has_value());
    if (tag % 3 != 0) continue;
    const ServeAnswer a = r.serve(1, q);
    EXPECT_EQ(r.pinned().number, tag);
    EXPECT_EQ(r.pinned().find(1)->tag, tag);
    expect_same(a, (tag % 2 == 0 ? rec_a() : rec_b())->serve(q));
  }
  // The book's state outlives the book for a reader that still holds it.
  std::optional<ServeBook> scoped(std::in_place, inline_policy());
  ASSERT_TRUE(scoped->publish_expiry(entry(9, rec_a(), 1)).has_value());
  ServeBookReader orphan = scoped->reader();
  ASSERT_EQ(orphan.serve(9, q).status, ServeStatus::Ok);
  scoped.reset();
  EXPECT_EQ(orphan.serve(9, q).status, ServeStatus::Ok);
}

// A quote burst against stale expiries from four threads with a two-slot queue: each
// expiry is queued at most once at a time, the overflow is typed BudgetExhausted, the
// worker drains the queue, and afterwards every expiry serves.
TEST(LivePublication, AQuoteBurstQueuesTypedAndDrains) {
  ServeRecordPolicy aging = no_ladders();
  aging.max_age_ns = 1000;
  const auto old = record_of(fit_a(), aging);
  ASSERT_NE(old, nullptr);
  ServeBookPolicy policy;
  policy.build_workers = 1;
  policy.queue_capacity = 2;
  std::atomic<int> builds{0};
  ServeBook book(policy, [&builds](const ServeRebuildRequest &q) {
    builds.fetch_add(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(5)); // a rebuild's cost
    return std::optional<ServeBookEntry>(entry(q.expiry_id, rec_a(), 2));
  });
  constexpr std::uint64_t kExpiries = 8;
  std::vector<ServeBookEntry> all;
  for (std::uint64_t id = 1; id <= kExpiries; ++id) all.push_back(entry(id, old, 1));
  ASSERT_TRUE(book.publish(all).has_value());
  const std::int64_t late = old->version().as_of_ns + 1'000'000;
  std::array<std::array<std::uint64_t, 4>, 4> statuses{}; // [thread][Ok, Stale, Budget, other]
  {
    std::vector<std::jthread> burst;
    for (std::size_t t = 0; t < 4; ++t) {
      burst.emplace_back([&, t] {
        ServeBookReader r = book.reader();
        for (int round = 0; round < 40; ++round) {
          for (std::uint64_t id = 1; id <= kExpiries; ++id) {
            const ServeAnswer a = r.serve(id, req(100.0, Side::Call, kSpot, ServeTier::Ladder,
                                                  ServeTier::Family, late));
            const std::size_t k = a.status == ServeStatus::Ok                ? 0
                                  : a.status == ServeStatus::Stale           ? 1
                                  : a.status == ServeStatus::BudgetExhausted ? 2
                                                                             : 3;
            ++statuses[t][k];
          }
        }
      });
    }
  }
  std::uint64_t budget = 0;
  std::uint64_t other = 0;
  for (const auto &s : statuses) {
    budget += s[2];
    other += s[3];
  }
  EXPECT_GT(budget, 0u);
  EXPECT_EQ(other, 0u);
  // Drain: re-ask each still-stale expiry until all serve.
  ServeBookReader r = book.reader();
  for (int pass = 0; pass < 50; ++pass) {
    ASSERT_TRUE(book.wait_idle(std::chrono::milliseconds(10'000)));
    std::size_t fresh = 0;
    for (std::uint64_t id = 1; id <= kExpiries; ++id) {
      fresh += r.serve(id, req(100.0, Side::Call, kSpot, ServeTier::Ladder, ServeTier::Family,
                               late))
                           .status == ServeStatus::Ok
                   ? 1u
                   : 0u;
    }
    if (fresh == kExpiries) break;
  }
  ASSERT_TRUE(book.wait_idle(std::chrono::milliseconds(10'000)));
  const ServeBookCounters c = book.counters();
  EXPECT_EQ(c.rebuilt, kExpiries);
  EXPECT_EQ(builds.load(), static_cast<int>(kExpiries));
  EXPECT_GT(c.rejected, 0u);
  EXPECT_GT(c.deduplicated, 0u);
  EXPECT_EQ(c.queue_depth, 0u);
  EXPECT_GT(c.queue_age_max_ns, 0);
  for (std::uint64_t id = 1; id <= kExpiries; ++id) {
    EXPECT_EQ(r.serve(id, req(100.0, Side::Call, kSpot, ServeTier::Ladder, ServeTier::Family,
                              late))
                  .status,
              ServeStatus::Ok)
        << id;
  }
}

// P0's contract on the query path: rebuilt books answer bit-identically and finitely for
// any number of rebuild workers.
TEST(LivePublication, RebuildsAreDeterministicAcrossWorkerCounts) {
  std::map<std::uint64_t, std::shared_ptr<const QuoteSliceFit>> fits;
  for (std::uint64_t id = 1; id <= 4; ++id) {
    const double k = static_cast<double>(id);
    fits.emplace(id, fit_of(0.01 * (k - 1.0), 0.2 + 0.05 * k));
  }
  const auto answers = [&fits](unsigned workers) {
    ServeBookPolicy policy;
    policy.build_workers = workers;
    ServeBook book(policy, [&fits](const ServeRebuildRequest &q) {
      return std::optional<ServeBookEntry>(
          entry(q.expiry_id, record_of(*fits.at(q.expiry_id), no_ladders()), q.expiry_id));
    });
    for (std::uint64_t id = 1; id <= 4; ++id) {
      EXPECT_EQ(book.request_rebuild(id), ServeEnqueue::Queued);
    }
    EXPECT_TRUE(book.wait_idle(std::chrono::milliseconds(120'000)));
    EXPECT_EQ(book.counters().rebuilt, 4u);
    ServeBookReader r = book.reader();
    std::vector<std::uint64_t> out;
    for (std::uint64_t id = 1; id <= 4; ++id) {
      for (const double K : {82.5, 96.1, 100.0, 104.4, 117.5}) {
        for (const Side side : {Side::Call, Side::Put}) {
          const ServeAnswer a =
              r.serve(id, req(K, side, 1.004 * kSpot, ServeTier::Ladder, ServeTier::Family));
          EXPECT_EQ(a.status, ServeStatus::Ok) << id << " " << K;
          EXPECT_TRUE(std::isfinite(a.price) && std::isfinite(a.delta) && std::isfinite(a.vega));
          out.push_back(bits(a.price));
          out.push_back(bits(a.delta));
          out.push_back(bits(a.vega));
        }
      }
    }
    return out;
  };
  const std::vector<std::uint64_t> one = answers(1);
  EXPECT_EQ(answers(2), one);
  EXPECT_EQ(answers(4), one);
}

// ── The serve_build stage ────────────────────────────────────────────────────────────────

// The fit's hand-off -> the first usable bundle (tiers 1-2, no ladders) -> the laddered
// record, each an atomic version; the records serve the fitter's own families.
TEST(LivePublication, ServeBuildPublishesTheFirstUsableBundleThenTheLadders) {
  const Chain chain = smile_chain(0.25, 0.0);
  QuoteSliceConfig config;
  config.dividend_model = QuoteSliceDividendModel::ContinuousYield;
  config.channel.repair_half_spreads = 0.5;
  config.channel.fit_space = QuoteSliceFitSpace::VolChannel;
  config.channel.deam_profile = QuoteSliceDeamProfile::Screen;
  config.channel.deam_cache = true;
  config.channel.deam_inversion = QuoteSliceDeamInversion::Premium;
  config.channel.deam_confirm_kernel = QuoteSliceDeamConfirmKernel::Fast;
  config.channel.deam_family = QuoteSliceDeamFamily::Always;
  config.channel.deam_family_q = true;
  config.channel.deam_family_native = true;
  config.grid_kernel = QuoteSliceGridKernel::Family;
  deam_iv_memo_clear();
  const Result<QuoteSliceFit> fit =
      fit_quote_slice_channel(chain.market, chain.quotes, config, nullptr);
  ASSERT_TRUE(fit.has_value());
  ASSERT_NE(fit->serve_handoff, nullptr);

  ServeBook book(inline_policy());
  ServeBookEntry key = entry(4, nullptr, 77);
  key.expiry_ns = kNow + 86'400'000'000'000;
  const Result<ServeBuildStage> stage = serve_build(book, key, *fit);
  ASSERT_TRUE(stage.has_value());
  EXPECT_EQ(stage->first_version, 1u);
  EXPECT_EQ(stage->final_version, 2u);
  EXPECT_GT(stage->first_usable_us, 0.0);
  EXPECT_LT(stage->first_usable_us, stage->total_us);
  ServeBookReader r = book.reader();
  const ServeAnswer a =
      r.serve(4, req(101.3, Side::Call, 1.003 * kSpot, ServeTier::Ladder, ServeTier::Family, kNow));
  EXPECT_EQ(a.status, ServeStatus::Ok);
  const ServeBookEntry *e = r.pinned().find(4);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->record, stage->record);
  EXPECT_EQ(e->tag, 77u);
  EXPECT_EQ(e->expiry_ns, key.expiry_ns);
  const ServeSupport &s = e->record->support();
  EXPECT_GT(s.call.ladders + s.put.ladders, 0u);
  for (const ServeSideSupport *side : {&s.call, &s.put}) {
    if (side->family) EXPECT_EQ(side->origin, ServeFamilyOrigin::Handed);
  }
  // One phase: the laddered record straight away.
  ServeBook single(inline_policy());
  const Result<ServeBuildStage> once = serve_build(single, key, *fit, {}, false);
  ASSERT_TRUE(once.has_value());
  EXPECT_EQ(once->first_version, 1u);
  EXPECT_EQ(once->final_version, 1u);
  // A fit without a certified slice publishes nothing.
  QuoteSliceFit empty;
  EXPECT_FALSE(serve_build(single, key, empty).has_value());
  EXPECT_EQ(single.version(), 1u);
}

// The chain columns of a counted region (built outside it).
struct ServeChainColumnsRef {
  ServedChainView view;
  ChainTickOutputs out;
  std::span<ServedChainStatement> served;
};

// ── Allocation ───────────────────────────────────────────────────────────────────────────

// The normal query path allocates nothing: serve, implied_vol, the served chain, a re-pin
// after a publication on another thread, and a miss that queues a rebuild.
TEST(LivePublicationAlloc, TheQueryPathAllocatesNothing) {
  ServeBook book(inline_policy(), [](const ServeRebuildRequest &q) {
    return std::optional<ServeBookEntry>(entry(q.expiry_id, rec_a(), 3));
  });
  ASSERT_TRUE(book.publish(std::array{entry(1, rec_ladder(), 1), entry(2, rec_a(), 1)})
                  .has_value());
  ServeBookReader r = book.reader();
  const std::array<std::uint64_t, 2> ids{1, 2};
  ChainColumns c = chain_over(*rec_ladder(), ids);
  const ServeRequest ladder_q =
      req(rec_ladder()->nodes()[12].strike, Side::Call, 1.002 * kSpot, ServeTier::Ladder,
          ServeTier::Family);
  const ServeRequest family_q =
      req(101.3, Side::Put, 0.996 * kSpot, ServeTier::Family, ServeTier::Family);
  ServeIvRequest iv;
  iv.strike = 101.3;
  iv.side = Side::Put;
  iv.spot = 0.996 * kSpot;
  iv.target_price = rec_a()->serve(family_q).price;
  iv.tier = ServeTier::Family;
  iv.tier_max = ServeTier::Family;
  ASSERT_EQ(r.serve(1, ladder_q).status, ServeStatus::Ok); // warm: the first pin
  ASSERT_EQ(r.serve(1, ladder_q).tier, ServeTier::Ladder);
  const ServeChainColumnsRef cols{c.view(), c.out(), c.served};
  std::size_t ok = 0;
  std::uint64_t n = testkit::count_allocations([&] {
    for (int i = 0; i < 50; ++i) {
      ok += r.serve(1, ladder_q).status == ServeStatus::Ok ? 1u : 0u;
      ok += r.serve(2, family_q).status == ServeStatus::Ok ? 1u : 0u;
      ok += r.implied_vol(2, iv).tier == ServeTier::Family ? 1u : 0u;
      ok += r.serve_chain(1.002 * kSpot, 0, cols.view, ServeTier::Ladder, ServeTier::Family,
                          cols.out, cols.served)
                    .has_value()
                ? 1u
                : 0u;
    }
  });
  EXPECT_EQ(n, 0u);
  EXPECT_EQ(ok, 200u);
  std::jthread([&book] {
    ASSERT_TRUE(book.publish_expiry(entry(2, rec_b(), 2)).has_value());
  }).join();
  ServeStatus repinned = ServeStatus::InvalidArgument;
  ServeStatus missing = ServeStatus::Ok;
  n = testkit::count_allocations([&] {
    repinned = r.serve(2, family_q).status; // re-pins version 2
    missing = r.serve(9, family_q).status;  // queues a Missing rebuild
  });
  EXPECT_EQ(n, 0u);
  EXPECT_EQ(repinned, ServeStatus::Ok);
  EXPECT_EQ(missing, ServeStatus::TierUnavailable);
  EXPECT_EQ(r.pinned().number, 2u);
  EXPECT_EQ(book.counters().enqueued, 1u);
}

} // namespace
} // namespace atx::vol
