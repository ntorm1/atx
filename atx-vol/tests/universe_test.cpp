#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/universe.hpp"

// Universe / SoA-chain-registry coverage, ported from the C ats-vol tests:
//   test_universe_hashmap.c   -> ticker interning (dedupe, distinct, growth,
//                                reverse lookup)
//   test_universe_eviction.c  -> LRU residency policy (default opts, oldest
//                                first, min-residency floor, no-op under cap,
//                                restore via mark_live)
//   test_universe_scale.c     -> the scale/stress build+ingest loop, asserting
//                                on universe counts/state (NOT wall-clock)
//
// Plus the contract-id codec round-trip and the chain-index helper the C keeps
// in ats_vol_types.h / ats_universe.h.
//
// Deliberately omitted:
//   - the C hashmap perf microbench `lookup_under_1us_at_6k_uids` (an
//     environment-specific timing gate, not a correctness check — per the port
//     task's "skip raw wall-clock/perf-gate" instruction; the capacity-growth
//     case below keeps the 8000-ticker correctness sweep).
//   - test_universe_cadence.c in full: it exercises `AtsVolCadenceQueue` and
//     `ats_vol_profile_tier_priority`, which live in the calibrate-pool /
//     profile modules, not the universe registry ported here.
//   - the scale test's rebuild/calibration/surface/profile plumbing
//     (`ats_vol_universe_rebuild`, curve/surface creation), which belongs to
//     out-of-scope modules; only the universe-construction correctness is kept.

namespace {

using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::cid_expiry;
using atx::vol::cid_side;
using atx::vol::cid_strike_idx;
using atx::vol::cid_uid;
using atx::vol::ContractId;
using atx::vol::ErrorCode;
using atx::vol::EvictionOptions;
using atx::vol::EvictionStats;
using atx::vol::ExpiryId;
using atx::vol::make_contract_id;
using atx::vol::QuoteBatch;
using atx::vol::Side;
using atx::vol::Uid;
using atx::vol::Underlying;
using atx::vol::Universe;
using atx::vol::UniverseStats;

// Sentinel used only when an EXPECT_TRUE(has_value()) precondition fails inside
// a non-void helper (gtest ASSERT_* cannot early-return from a value-returning
// function). All test inputs are valid, so this is never actually returned.
constexpr Uid kBadUid = 0xFFFF'FFFFu;

// 4-letter base-26 ticker, mirroring the C test's make_ticker_n. Produces
// 26^4 = 456 976 unique strings.
std::string make_ticker_n(int i) {
  std::string out(4, 'A');
  out[0] = static_cast<char>('A' + (i / (26 * 26 * 26)) % 26);
  out[1] = static_cast<char>('A' + (i / (26 * 26)) % 26);
  out[2] = static_cast<char>('A' + (i / 26) % 26);
  out[3] = static_cast<char>('A' + i % 26);
  return out;
}

// ── Ticker interning (test_universe_hashmap.c) ──────────────────────────────

TEST(UniverseTicker, InternTicker_SameTickerTwice_ReturnsSameUid) {
  Universe u;
  const auto spy1 = u.intern_ticker("SPY");
  const auto spy2 = u.intern_ticker("SPY");
  const auto qqq = u.intern_ticker("QQQ");
  ASSERT_TRUE(spy1.has_value());
  ASSERT_TRUE(spy2.has_value());
  ASSERT_TRUE(qqq.has_value());
  EXPECT_EQ(*spy1, *spy2);
  EXPECT_NE(*spy1, *qqq);
}

TEST(UniverseTicker, TickerFor_KnownUid_ReturnsTicker) {
  Universe u;
  const auto spy = u.intern_ticker("SPY");
  ASSERT_TRUE(spy.has_value());

  const auto t = u.ticker_for(*spy);
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(*t, std::string_view{"SPY"});
}

TEST(UniverseTicker, TickerFor_UnknownUid_ReturnsNotFound) {
  const Universe u;
  const auto zero = u.ticker_for(0u);
  ASSERT_FALSE(zero.has_value());
  EXPECT_EQ(zero.error().code(), ErrorCode::NotFound);

  const auto missing = u.ticker_for(42u);
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
}

TEST(UniverseTicker, InternTicker_EmptyTicker_ReturnsInvalidArgument) {
  Universe u;
  const auto res = u.intern_ticker("");
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(UniverseTicker, InternTicker_TooLong_ReturnsInvalidArgument) {
  Universe u;
  const auto res = u.intern_ticker("SEVENTEEN_CHARS!!"); // 17 bytes > 16
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(UniverseTicker, InternTicker_CapacityGrowth_AllLookupsStable) {
  // 8000 unique 4-letter tickers force multiple rehashes; each must
  // round-trip to a stable, positive uid (C: capacity_growth_holds_lookups).
  Universe u;
  constexpr int kN = 8000;
  for (int i = 0; i < kN; ++i) {
    const auto uid = u.intern_ticker(make_ticker_n(i));
    ASSERT_TRUE(uid.has_value());
  }
  for (int i = 0; i < kN; ++i) {
    const std::string tk = make_ticker_n(i);
    const auto a = u.intern_ticker(tk);
    const auto b = u.intern_ticker(tk);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(*a, *b);
    EXPECT_GT(*a, 0u);
  }
  EXPECT_EQ(u.n_underlyings(), static_cast<std::uint32_t>(kN));
}

// ── Contract-id codec ───────────────────────────────────────────────────────

TEST(UniverseContractId, EncodeDecode_RoundTrips) {
  struct Case {
    Uid uid;
    ExpiryId exp;
    std::uint16_t strike;
    Side side;
  };
  constexpr std::array<Case, 4> cases{{
      {1u, 0u, 0u, Side::Call},
      {12345u, 7u, 42u, Side::Put},
      {0x00FF'FFFFu, 0xFFFFu, 0xFFFFu, Side::Put}, // all fields at max width
      {0u, 0u, 0u, Side::Call},
  }};
  for (const Case &tc : cases) {
    const ContractId cid = make_contract_id(tc.uid, tc.exp, tc.strike, tc.side);
    EXPECT_EQ(cid_uid(cid), tc.uid);
    EXPECT_EQ(cid_expiry(cid), tc.exp);
    EXPECT_EQ(cid_strike_idx(cid), tc.strike);
    EXPECT_EQ(cid_side(cid), tc.side);
  }
}

TEST(UniverseContractId, Encoding_MatchesCBitLayout) {
  // Bit-for-bit against the C `ats_vol_cid_make` formula.
  const ContractId cid = make_contract_id(0x00AB'CDEFu, 0x1234u, 0x5678u, Side::Put);
  const ContractId expected = (static_cast<ContractId>(0x00AB'CDEFu) << 33) |
                              (static_cast<ContractId>(0x1234u) << 17) |
                              (static_cast<ContractId>(0x5678u) << 1) | 1ull;
  EXPECT_EQ(cid, expected);
}

// ── Chain-index helper ──────────────────────────────────────────────────────

TEST(UniverseChain, ChainIndex_CallPut_Interleaves) {
  EXPECT_EQ(chain_index(0u, Side::Call), std::size_t{0});
  EXPECT_EQ(chain_index(0u, Side::Put), std::size_t{1});
  EXPECT_EQ(chain_index(5u, Side::Call), std::size_t{10});
  EXPECT_EQ(chain_index(5u, Side::Put), std::size_t{11});
}

// ── Expiry / strike growth ──────────────────────────────────────────────────

TEST(UniverseChain, AddExpiry_NewExpiries_AssignSequentialIds) {
  Universe u;
  const auto uid = u.intern_ticker("AAPL");
  ASSERT_TRUE(uid.has_value());

  const auto e0 = u.add_expiry(*uid, 1'000);
  const auto e1 = u.add_expiry(*uid, 2'000);
  const auto e2 = u.add_expiry(*uid, 3'000);
  ASSERT_TRUE(e0.has_value() && e1.has_value() && e2.has_value());
  EXPECT_EQ(*e0, ExpiryId{0});
  EXPECT_EQ(*e1, ExpiryId{1});
  EXPECT_EQ(*e2, ExpiryId{2});

  const auto un = u.get_underlying(*uid);
  ASSERT_TRUE(un.has_value());
  EXPECT_EQ((*un)->chains.size(), std::size_t{3});
}

TEST(UniverseChain, AddExpiry_DuplicateExpiryNs_IsIdempotent) {
  Universe u;
  const auto uid = u.intern_ticker("AAPL");
  ASSERT_TRUE(uid.has_value());

  const auto first = u.add_expiry(*uid, 42'000);
  const auto again = u.add_expiry(*uid, 42'000);
  ASSERT_TRUE(first.has_value() && again.has_value());
  EXPECT_EQ(*first, *again);

  const auto un = u.get_underlying(*uid);
  ASSERT_TRUE(un.has_value());
  EXPECT_EQ((*un)->chains.size(), std::size_t{1}); // not duplicated
}

TEST(UniverseChain, AddExpiry_UnknownUid_ReturnsNotFound) {
  Universe u;
  const auto res = u.add_expiry(999u, 1'000);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::NotFound);
}

TEST(UniverseChain, AddStrike_GrowsStrikeGrid) {
  Universe u;
  const auto uid = u.intern_ticker("AAPL");
  ASSERT_TRUE(uid.has_value());
  const auto eid = u.add_expiry(*uid, 1'000);
  ASSERT_TRUE(eid.has_value());

  constexpr int kK = 40; // forces growth past the initial capacity
  for (int k = 0; k < kK; ++k) {
    const auto sidx = u.add_strike(*uid, *eid, 80.0 + static_cast<double>(k));
    ASSERT_TRUE(sidx.has_value());
    EXPECT_EQ(*sidx, static_cast<std::uint16_t>(k));
  }

  const auto un = u.get_underlying(*uid);
  ASSERT_TRUE(un.has_value());
  const Chain &c = (*un)->chains[0];
  EXPECT_EQ(c.n_strikes(), static_cast<std::size_t>(kK));
  // Per-side axes are exactly twice the strike axis.
  EXPECT_EQ(c.bids.size(), static_cast<std::size_t>(2 * kK));
  EXPECT_EQ(c.mids.size(), static_cast<std::size_t>(2 * kK));
  EXPECT_EQ(c.flags.size(), static_cast<std::size_t>(2 * kK));
  EXPECT_DOUBLE_EQ(c.strikes[7], 87.0);
}

TEST(UniverseChain, AddStrike_UnknownExpiry_ReturnsNotFound) {
  Universe u;
  const auto uid = u.intern_ticker("AAPL");
  ASSERT_TRUE(uid.has_value());
  const auto res = u.add_strike(*uid, ExpiryId{5}, 100.0); // no expiries yet
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::NotFound);
}

// ── Quote ingest ────────────────────────────────────────────────────────────

// Build a one-underlier universe with `n_exp` expiries and `n_k` strikes.
Uid build_one(Universe &u, std::string_view ticker, std::uint16_t n_exp, std::uint16_t n_k) {
  const auto uid_res = u.intern_ticker(ticker);
  EXPECT_TRUE(uid_res.has_value());
  const Uid uid = uid_res.has_value() ? *uid_res : kBadUid;
  for (std::uint16_t e = 0; e < n_exp; ++e) {
    const auto eid = u.add_expiry(uid, static_cast<std::int64_t>(e + 1) * 1'000'000'000ll);
    EXPECT_TRUE(eid.has_value());
    const ExpiryId eid_v = eid.has_value() ? *eid : ExpiryId{0};
    for (std::uint16_t k = 0; k < n_k; ++k) {
      const auto sidx = u.add_strike(uid, eid_v, 80.0 + static_cast<double>(k));
      EXPECT_TRUE(sidx.has_value());
    }
  }
  return uid;
}

TEST(UniverseQuotes, ApplyQuotes_KnownContracts_UpdatesMidsAndFlags) {
  Universe u;
  const Uid uid = build_one(u, "SPY", /*n_exp=*/2, /*n_k=*/4);

  // One call quote on (expiry 1, strike 2) and one put quote on the same.
  constexpr std::uint8_t kFlagStale = 0x04u;
  const std::array<ContractId, 2> contracts{
      make_contract_id(uid, ExpiryId{1}, 2u, Side::Call),
      make_contract_id(uid, ExpiryId{1}, 2u, Side::Put),
  };
  const std::array<double, 2> bids{1.00, 2.00};
  const std::array<double, 2> asks{1.20, 2.40};
  const std::array<std::int32_t, 2> bsz{10, 20};
  const std::array<std::int32_t, 2> asz{11, 21};
  const std::array<std::int64_t, 2> ts{500, 600};
  const std::array<std::uint8_t, 2> flags{kFlagStale, 0u};

  const QuoteBatch batch{contracts, bids, asks, bsz, asz, ts, flags};
  ASSERT_TRUE(u.apply_quotes(batch).has_value());

  const auto un = u.get_underlying(uid);
  ASSERT_TRUE(un.has_value());
  const Chain &c = (*un)->chains[1];
  const std::size_t call_idx = chain_index(2u, Side::Call);
  const std::size_t put_idx = chain_index(2u, Side::Put);

  EXPECT_DOUBLE_EQ(c.mids[call_idx], 0.5 * (1.00 + 1.20));
  EXPECT_DOUBLE_EQ(c.mids[put_idx], 0.5 * (2.00 + 2.40));
  EXPECT_EQ(c.flags[call_idx], kFlagStale);
  EXPECT_EQ(c.bid_sizes[put_idx], 20);
  EXPECT_EQ(c.ts_ns[put_idx], std::int64_t{600});

  const UniverseStats st = u.stats();
  EXPECT_EQ(st.n_quotes_applied, std::uint64_t{2});
  EXPECT_EQ(st.n_quotes_dropped, std::uint64_t{0});
}

TEST(UniverseQuotes, ApplyQuotes_UnknownContracts_CountedAsDropped) {
  Universe u;
  const Uid uid = build_one(u, "SPY", /*n_exp=*/1, /*n_k=*/2);

  const std::array<ContractId, 4> contracts{
      make_contract_id(uid, ExpiryId{0}, 0u, Side::Call),      // valid
      make_contract_id(999u, ExpiryId{0}, 0u, Side::Call),     // unknown uid
      make_contract_id(uid, ExpiryId{9}, 0u, Side::Call),      // expiry OOR
      make_contract_id(uid, ExpiryId{0}, 99u, Side::Call),     // strike OOR
  };
  const std::array<double, 4> bids{1.0, 1.0, 1.0, 1.0};
  const std::array<double, 4> asks{1.2, 1.2, 1.2, 1.2};
  const std::array<std::int32_t, 4> bsz{1, 1, 1, 1};
  const std::array<std::int32_t, 4> asz{1, 1, 1, 1};
  const std::array<std::int64_t, 4> ts{1, 1, 1, 1};
  const std::array<std::uint8_t, 4> flags{0u, 0u, 0u, 0u};

  const QuoteBatch batch{contracts, bids, asks, bsz, asz, ts, flags};
  ASSERT_TRUE(u.apply_quotes(batch).has_value());

  const UniverseStats st = u.stats();
  EXPECT_EQ(st.n_quotes_applied, std::uint64_t{1});
  EXPECT_EQ(st.n_quotes_dropped, std::uint64_t{3});
}

TEST(UniverseQuotes, ApplyQuotes_UpdatesLastTouched) {
  Universe u;
  const Uid uid = build_one(u, "SPY", /*n_exp=*/1, /*n_k=*/1);

  const std::array<ContractId, 2> contracts{
      make_contract_id(uid, ExpiryId{0}, 0u, Side::Call),
      make_contract_id(uid, ExpiryId{0}, 0u, Side::Put),
  };
  const std::array<double, 2> bids{1.0, 1.0};
  const std::array<double, 2> asks{1.2, 1.2};
  const std::array<std::int32_t, 2> bsz{1, 1};
  const std::array<std::int32_t, 2> asz{1, 1};
  const std::array<std::int64_t, 2> ts{700, 300}; // max is 700
  const std::array<std::uint8_t, 2> flags{0u, 0u};

  const QuoteBatch batch{contracts, bids, asks, bsz, asz, ts, flags};
  ASSERT_TRUE(u.apply_quotes(batch).has_value());

  const auto un = u.get_underlying(uid);
  ASSERT_TRUE(un.has_value());
  EXPECT_EQ((*un)->last_touched_ns, std::int64_t{700});
}

TEST(UniverseQuotes, ApplyQuotes_MismatchedColumns_ReturnsInvalidArgument) {
  Universe u;
  const Uid uid = build_one(u, "SPY", /*n_exp=*/1, /*n_k=*/1);

  const std::array<ContractId, 2> contracts{
      make_contract_id(uid, ExpiryId{0}, 0u, Side::Call),
      make_contract_id(uid, ExpiryId{0}, 0u, Side::Put),
  };
  const std::array<double, 1> bids{1.0}; // wrong length
  const std::array<double, 2> asks{1.2, 1.2};
  const std::array<std::int32_t, 2> bsz{1, 1};
  const std::array<std::int32_t, 2> asz{1, 1};
  const std::array<std::int64_t, 2> ts{1, 1};
  const std::array<std::uint8_t, 2> flags{0u, 0u};

  const QuoteBatch batch{contracts, bids, asks, bsz, asz, ts, flags};
  const auto res = u.apply_quotes(batch);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

// ── Stats ───────────────────────────────────────────────────────────────────

TEST(UniverseStatsSuite, NUnderlyings_Empty_ReturnsZero) {
  const Universe u;
  EXPECT_EQ(u.n_underlyings(), std::uint32_t{0});
  const UniverseStats st = u.stats();
  EXPECT_EQ(st.n_underlyings, std::uint32_t{0});
  EXPECT_EQ(st.n_chains, std::uint32_t{0});
  EXPECT_EQ(st.n_strikes, std::uint64_t{0});
}

TEST(UniverseStatsSuite, Stats_AfterBuild_CountsUnderlyingsChainsStrikes) {
  Universe u;
  build_one(u, "AAA", /*n_exp=*/3, /*n_k=*/5);
  build_one(u, "BBB", /*n_exp=*/2, /*n_k=*/4);

  const UniverseStats st = u.stats();
  EXPECT_EQ(st.n_underlyings, std::uint32_t{2});
  EXPECT_EQ(st.n_chains, static_cast<std::uint32_t>(3 + 2));
  EXPECT_EQ(st.n_strikes, static_cast<std::uint64_t>(3 * 5 + 2 * 4));
}

// ── LRU residency policy (test_universe_eviction.c) ─────────────────────────

// Register `n` distinct underliers, matching the C build_universe_n helper.
Universe build_universe_n(int n) {
  Universe u;
  for (int i = 0; i < n; ++i) {
    std::string tk(3, 'A');
    tk[0] = static_cast<char>('A' + (i / (26 * 26)) % 26);
    tk[1] = static_cast<char>('A' + (i / 26) % 26);
    tk[2] = static_cast<char>('A' + i % 26);
    EXPECT_TRUE(u.intern_ticker(tk).has_value());
  }
  return u;
}

TEST(UniverseEviction, DefaultOptions_Have4GbCapAnd60sMinResidency) {
  const EvictionOptions o{};
  EXPECT_EQ(o.cap_bytes / (1024ull * 1024ull * 1024ull), std::uint64_t{4});
  EXPECT_EQ(o.min_residency_ns, std::int64_t{60} * 1'000'000'000ll);
}

TEST(UniverseEviction, EvictLru_OldestUnderliersEvictedFirst) {
  Universe u = build_universe_n(10);

  const std::int64_t base_ns = 1'000'000'000ll * 1'000; // 1000 s on a fake clock
  const std::int64_t stride = 1'000'000'000ll * 30;     // 30 s apart
  const std::uint64_t bytes_each = 10ull * 1024ull * 1024ull;
  for (int i = 0; i < 10; ++i) {
    u.mark_live(static_cast<Uid>(i + 1), bytes_each, base_ns + static_cast<std::int64_t>(i) * stride);
  }
  EXPECT_EQ(u.bytes_live(), 10ull * bytes_each);

  // Cap 50 MB; 10 x 10 MB = 100 MB. Must shed enough to fall under 45 MB.
  EvictionOptions o{};
  o.cap_bytes = 50ull * 1024ull * 1024ull;
  o.min_residency_ns = 0;
  o.now_ns = base_ns + static_cast<std::int64_t>(100) * stride; // far future

  const EvictionStats stats = u.evict_lru(o);
  EXPECT_GE(stats.n_evicted, std::uint32_t{6});
  EXPECT_LE(stats.bytes_remaining, 50ull * 1024ull * 1024ull * 9ull / 10ull);

  // Youngest (uid 10) still live; oldest (uid 1) evicted.
  const auto youngest = u.get_underlying(10u);
  ASSERT_TRUE(youngest.has_value());
  EXPECT_FALSE((*youngest)->evicted);
  const auto oldest = u.get_underlying(1u);
  ASSERT_TRUE(oldest.has_value());
  EXPECT_TRUE((*oldest)->evicted);
}

TEST(UniverseEviction, EvictLru_MinResidencyFloor_ProtectsRecentTouches) {
  Universe u = build_universe_n(5);

  const std::int64_t one_s = 1'000'000'000ll;
  for (int i = 0; i < 5; ++i) {
    u.mark_live(static_cast<Uid>(i + 1), 50ull * 1024ull * 1024ull, 100ll * one_s);
  }

  EvictionOptions o{};
  o.cap_bytes = 100ull * 1024ull * 1024ull; // tight
  o.min_residency_ns = 60ll * one_s;         // default
  o.now_ns = 110ll * one_s;                  // 10 s after touch -> all protected

  const EvictionStats stats = u.evict_lru(o);
  EXPECT_EQ(stats.n_evicted, std::uint32_t{0});
  EXPECT_GE(stats.n_skipped_min_residency, std::uint32_t{1});
}

TEST(UniverseEviction, EvictLru_UnderCap_IsNoOp) {
  Universe u = build_universe_n(3);
  for (int i = 0; i < 3; ++i) {
    u.mark_live(static_cast<Uid>(i + 1), 1ull << 20, 1'000'000);
  }
  EvictionOptions o{};
  o.cap_bytes = 100ull * 1024ull * 1024ull;

  const EvictionStats stats = u.evict_lru(o);
  EXPECT_EQ(stats.n_evicted, std::uint32_t{0});
  EXPECT_EQ(stats.bytes_remaining, static_cast<std::uint64_t>(3) * (1ull << 20));
}

TEST(UniverseEviction, MarkLive_AfterEviction_RestoresUnderlier) {
  Universe u = build_universe_n(4);
  for (int i = 0; i < 4; ++i) {
    u.mark_live(static_cast<Uid>(i + 1), 25ull * 1024ull * 1024ull,
                100ll * 1'000'000'000ll + static_cast<std::int64_t>(i));
  }
  EvictionOptions o{};
  o.cap_bytes = 50ull * 1024ull * 1024ull;
  o.min_residency_ns = 0;
  o.now_ns = static_cast<std::int64_t>(1e18);
  (void)u.evict_lru(o);

  const auto un = u.get_underlying(1u);
  ASSERT_TRUE(un.has_value());
  EXPECT_TRUE((*un)->evicted);

  // Restore: simulates the next refit rebuilding surface + cache.
  u.mark_live(1u, 25ull * 1024ull * 1024ull, static_cast<std::int64_t>(1e18));
  EXPECT_FALSE((*un)->evicted);
  EXPECT_EQ((*un)->bytes_live, 25ull * 1024ull * 1024ull);
}

TEST(UniverseEviction, BytesLive_ExcludesEvicted) {
  Universe u = build_universe_n(2);
  u.mark_live(1u, 10ull * 1024ull * 1024ull, 100);
  u.mark_live(2u, 20ull * 1024ull * 1024ull, 200);
  EXPECT_EQ(u.bytes_live(), 30ull * 1024ull * 1024ull);

  // Evict everything (huge age, zero residency floor, tiny cap).
  EvictionOptions o{};
  o.cap_bytes = 1ull; // forces eviction of all
  o.min_residency_ns = 0;
  o.now_ns = static_cast<std::int64_t>(1e18);
  (void)u.evict_lru(o);
  EXPECT_EQ(u.bytes_live(), std::uint64_t{0});
}

// ── Scale / stress (test_universe_scale.c, correctness only) ────────────────

TEST(UniverseScale, BuildAndApply_LargeSyntheticUniverse_StateConsistent) {
  Universe u;
  constexpr int kUnders = 24;
  constexpr std::uint16_t kExp = 6;
  constexpr std::uint16_t kStrikes = 12;

  std::vector<Uid> uids;
  uids.reserve(kUnders);
  for (int i = 0; i < kUnders; ++i) {
    const Uid uid = build_one(u, make_ticker_n(i), kExp, kStrikes);
    ASSERT_GT(uid, 0u);
    uids.push_back(uid);
  }

  // A full call+put batch across every (uid, expiry, strike).
  std::vector<ContractId> contracts;
  std::vector<double> bids;
  std::vector<double> asks;
  std::vector<std::int32_t> bsz;
  std::vector<std::int32_t> asz;
  std::vector<std::int64_t> ts;
  std::vector<std::uint8_t> flags;
  for (const Uid uid : uids) {
    for (std::uint16_t e = 0; e < kExp; ++e) {
      for (std::uint16_t k = 0; k < kStrikes; ++k) {
        for (std::uint8_t s = 0; s < 2; ++s) {
          const Side side = static_cast<Side>(s);
          contracts.push_back(make_contract_id(uid, static_cast<ExpiryId>(e), k, side));
          bids.push_back(1.0 + static_cast<double>(k));
          asks.push_back(1.2 + static_cast<double>(k));
          bsz.push_back(10);
          asz.push_back(11);
          ts.push_back(100 + static_cast<std::int64_t>(k));
          flags.push_back(std::uint8_t{0});
        }
      }
    }
  }

  const QuoteBatch batch{contracts, bids, asks, bsz, asz, ts, flags};
  ASSERT_TRUE(u.apply_quotes(batch).has_value());

  const UniverseStats st = u.stats();
  EXPECT_EQ(st.n_underlyings, static_cast<std::uint32_t>(kUnders));
  EXPECT_EQ(st.n_chains, static_cast<std::uint32_t>(kUnders * kExp));
  EXPECT_EQ(st.n_strikes, static_cast<std::uint64_t>(kUnders) * kExp * kStrikes);
  EXPECT_EQ(st.n_quotes_applied, static_cast<std::uint64_t>(contracts.size()));
  EXPECT_EQ(st.n_quotes_dropped, std::uint64_t{0});

  // Spot-check a derived mid at (uid 0, expiry 2, strike 3, put).
  const auto un = u.get_underlying(uids[0]);
  ASSERT_TRUE(un.has_value());
  const Chain &c = (*un)->chains[2];
  const std::size_t idx = chain_index(3u, Side::Put);
  EXPECT_DOUBLE_EQ(c.mids[idx], 0.5 * ((1.0 + 3.0) + (1.2 + 3.0)));

  // Stress: re-applying a batch that references a now-unknown high uid must
  // all drop, leaving applied-count untouched and bumping the drop count.
  const std::array<ContractId, 3> bad{
      make_contract_id(9999u, ExpiryId{0}, 0u, Side::Call),
      make_contract_id(9999u, ExpiryId{1}, 1u, Side::Put),
      make_contract_id(9999u, ExpiryId{2}, 2u, Side::Call),
  };
  const std::array<double, 3> z_d{0.0, 0.0, 0.0};
  const std::array<std::int32_t, 3> z_i{0, 0, 0};
  const std::array<std::int64_t, 3> z_ts{0, 0, 0};
  const std::array<std::uint8_t, 3> z_f{0u, 0u, 0u};
  const QuoteBatch bad_batch{bad, z_d, z_d, z_i, z_i, z_ts, z_f};
  ASSERT_TRUE(u.apply_quotes(bad_batch).has_value());

  const UniverseStats st2 = u.stats();
  EXPECT_EQ(st2.n_quotes_applied, st.n_quotes_applied); // unchanged
  EXPECT_EQ(st2.n_quotes_dropped, std::uint64_t{3});
}

} // namespace
