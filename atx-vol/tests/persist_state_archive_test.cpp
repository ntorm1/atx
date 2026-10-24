// persist_state_archive_test.cpp — the MarketState archive (kind MarketState, `.atxms`;
// api/persist/state_archive.hpp; design wave3-persist D10 / D14, spec
// docs/storage/PERSIST_FORMAT.md "MarketState kind").
//
// Fixtures: a hand-built storage exercising every section (unsorted underliers, a
// yield curve, term carry, cash dividends, partial vendor columns, extras), the risk
// engine's surface-backed synthetic market (risk_synthetic_market.hpp) for embedded
// surfaces, and a SpiderRock-SHAPED synthetic store (the shipped generator) for the
// loader path and the RiskStream equality. No licensed data is read here.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/api/marketdata/spiderrock_store.hpp"
#include "atx/vol/api/persist/persist_file.hpp"
#include "atx/vol/api/persist/state_archive.hpp"
#include "atx/vol/api/risk/pnl_explain.hpp"
#include "atx/vol/api/risk/risk_book.hpp"
#include "atx/vol/api/risk/risk_kernel.hpp"
#include "atx/vol/api/storage/surface_archive.hpp"
#include "persist/state_archive_format.hpp"
#include "support/risk_synthetic_market.hpp"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace atx::vol; // NOLINT(google-build-using-namespace): test file
namespace rt = atx::vol::risk_test;

[[nodiscard]] bool bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] std::string pid_tag() {
#if defined(_WIN32)
  return std::to_string(_getpid());
#else
  return std::to_string(getpid());
#endif
}

[[nodiscard]] fs::path fresh_dir(std::string_view tag) {
  const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string name = "atx-persist-sa-" + std::string(tag);
  if (info != nullptr) name += std::string("-") + info->name();
  const fs::path dir = fs::temp_directory_path() / (name + "-" + pid_tag());
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

// ── Hand-built storage ───────────────────────────────────────────────────────

constexpr std::int64_t kTs = 1786716000000000000LL; // 2026-08-14 14:00 UTC

struct HandMarket {
  MarketStateStorage storage;
  std::vector<MarkExtrasColumns> extras; // one per storage.marks
  std::vector<std::string> tickers;      // index uid - 1
  BucketStateReport report;
};

[[nodiscard]] MarketStateStorage::MarksColumns marks_for(std::uint32_t uid, double spot,
                                                         bool vprice, bool vdelta) {
  MarketStateStorage::MarksColumns m;
  m.uid = uid;
  for (int e = 0; e < 2; ++e) {
    const std::int64_t expiry = kTs + (e + 1) * 86400000000000LL * 30;
    for (const Side side : {Side::Call, Side::Put}) {
      for (int k = 0; k < 3; ++k) {
        m.strike.push_back(spot * (0.9 + 0.1 * k));
        m.expiry_ns.push_back(expiry);
        m.side.push_back(side);
        m.sigma.push_back(0.2 + 0.01 * k + 0.02 * e + (side == Side::Put ? 0.005 : 0.0));
        if (vprice) m.vendor_price.push_back(1.0 + k + e);
        if (vdelta) m.vendor_delta.push_back(side == Side::Call ? 0.5 - 0.1 * k : -0.5 + 0.1 * k);
      }
    }
  }
  return m;
}

[[nodiscard]] std::unique_ptr<HandMarket> hand_market() {
  auto h = std::make_unique<HandMarket>();
  MarketStateStorage &s = h->storage;
  s.ts_ns = kTs;
  s.time.convention = TimeConvention::Calendar365;
  s.time.vol_time.alpha = 0.65; // carried verbatim even when unused
  s.underliers.resize(3);
  s.underliers[0] = UnderlierState{7, 101.5, 0.04, nullptr, 0.01, {}, {}, CarryFrom::State};
  s.underliers[1] = UnderlierState{2, 55.25, 0.035, nullptr, 0.0, {}, {}, CarryFrom::State};
  s.underliers[2] = UnderlierState{5, 210.0, 0.042, nullptr, 0.013, {}, {}, CarryFrom::State};
  const std::vector<double> pillar_t{0.1, 0.5, 1.0, 2.0};
  const std::vector<double> pillar_r{0.040, 0.041, 0.043, 0.045};
  s.yield_curves.push_back(YieldCurve::create(pillar_t, pillar_r).value());
  s.term_carries.resize(3);
  s.term_carries[1] = {{kTs + 86400000000000LL * 30, 0.12, 0.036, 0.001},
                       {kTs + 86400000000000LL * 60, 0.24, 0.037, 0.002},
                       {kTs + 86400000000000LL * 90, 0.31, 0.038, 0.002, 0.2475}};
  s.dividend_schedules.resize(3);
  constexpr std::int64_t kDay = 86400000000000LL;
  s.dividend_schedules[2] = {{kTs + kDay * 20, 1.25}, {kTs + kDay * 110, 1.3}};
  s.marks.push_back(marks_for(7, 101.5, false, true));
  s.marks.push_back(marks_for(2, 55.25, true, true));
  // Wire the borrowed views only now: every vector is final.
  s.underliers[0].yield = &s.yield_curves[0];
  s.underliers[1].term_carry = s.term_carries[1];
  s.underliers[2].cash_divs = s.dividend_schedules[2];
  for (std::size_t mi = 0; mi < s.marks.size(); ++mi) {
    MarkExtrasColumns x;
    const std::size_t n = s.marks[mi].sigma.size();
    for (std::size_t i = 0; i < n; ++i) {
      const double base = 1000.0 * static_cast<double>(mi + 1) + static_cast<double>(i);
      x.bid.push_back(base + 0.25);
      x.ask.push_back(base + 0.75);
      x.vendor_gamma.push_back(base * 1e-4);
      x.vendor_vega.push_back(base * 1e-3);
      x.vendor_theta.push_back(-base * 1e-5);
      x.uprc.push_back(s.marks[mi].uid == 7 ? 101.5 : 55.25);
    }
    // vendor_T left empty for the second entry: the archive fills NaN.
    if (mi == 0) x.vendor_T.assign(n, 0.0833);
    h->extras.push_back(std::move(x));
  }
  h->tickers = {"", "MSFT", "", "", "SPY", "", "AAPL"};
  h->report.n_uids = 3;
  h->report.n_marks = 24;
  h->report.max_ddiv_spread = 0.0125;
  h->report.n_rows_bad_sigma = 4;
  return h;
}

[[nodiscard]] MarketStateWriteInput input_of(const HandMarket &h) {
  MarketStateWriteInput in;
  in.storage = &h.storage;
  in.tickers_by_uid = h.tickers;
  in.extras = h.extras;
  in.report = &h.report;
  in.date = "2026-08-14";
  in.bucket_et = "1000";
  in.conventions_version = 3;
  in.source_fingerprint = 0xFEEDu;
  in.created_unix_ns = 1792000000000000000;
  return in;
}

// An 8-byte aligned owned copy of an image, opened as a MarketState archive.
[[nodiscard]] PersistResult<MarketStateArchive> open_image(std::span<const std::byte> image,
                                                           PersistVerify v = PersistVerify::Full) {
  auto words = std::make_shared<std::vector<std::uint64_t>>((image.size() + 7u) / 8u, 0u);
  std::memcpy(words->data(), image.data(), image.size());
  const auto bytes = std::as_bytes(std::span<const std::uint64_t>{*words}).first(image.size());
  return MarketStateArchive::open_bytes(bytes, words, v);
}

[[nodiscard]] bool inside(const void *p, std::span<const std::byte> region) noexcept {
  const auto *b = static_cast<const std::byte *>(p);
  return std::less_equal<const std::byte *>{}(region.data(), b) &&
         std::less<const std::byte *>{}(b, region.data() + region.size());
}

void expect_resolved_equal(const ResolvedContract &a, const ResolvedContract &b,
                           std::string_view what) {
  EXPECT_TRUE(bits(a.T, b.T)) << what;
  EXPECT_TRUE(bits(a.sigma, b.sigma)) << what;
  EXPECT_TRUE(bits(a.forward, b.forward)) << what;
  EXPECT_TRUE(bits(a.q_eff, b.q_eff)) << what;
  EXPECT_TRUE(bits(a.rate, b.rate)) << what;
  EXPECT_TRUE(bits(a.spot, b.spot)) << what;
  EXPECT_TRUE(bits(a.vendor_price, b.vendor_price)) << what;
  EXPECT_TRUE(bits(a.vendor_delta, b.vendor_delta)) << what;
  EXPECT_EQ(a.flags, b.flags) << what;
  EXPECT_EQ(a.status, b.status) << what;
}

template <class T> [[nodiscard]] bool same(std::span<const T> a, std::span<const T> b) {
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size_bytes()) == 0);
}

// ── Image surgery: change a column's bytes and re-seal every CRC that guards it ──

void reseal_section(std::vector<std::byte> &img, std::uint32_t type) {
  PersistHeader h{};
  std::memcpy(&h, img.data(), sizeof h);
  for (std::uint32_t i = 0; i < h.section_count; ++i) {
    PersistSectionEntry e{};
    std::memcpy(&e, img.data() + 256 + 64 * i, sizeof e);
    if (e.type != type) continue;
    e.crc32c = persist_crc32c(
        std::span<const std::byte>{img.data() + e.offset, static_cast<std::size_t>(e.length)});
    e.raw_crc32c = e.crc32c;
    std::memcpy(img.data() + 256 + 64 * i, &e, sizeof e);
  }
  h.section_table_crc32c =
      persist_crc32c(std::span<const std::byte>{img.data() + 256, 64u * h.section_count});
  h.header_crc32c = 0;
  h.header_crc32c = persist_crc32c(std::as_bytes(std::span{&h, 1}));
  std::memcpy(img.data(), &h, sizeof h);
  PersistFooter f{};
  const std::size_t at = img.size() - sizeof f;
  std::memcpy(&f, img.data() + at, sizeof f);
  f.header_crc32c = h.header_crc32c;
  f.file_crc32c = persist_crc32c(std::span<const std::byte>{img.data(), at});
  f.footer_crc32c = 0;
  f.footer_crc32c = persist_crc32c(std::as_bytes(std::span{&f, 1}));
  std::memcpy(img.data() + at, &f, sizeof f);
}

[[nodiscard]] PersistSectionEntry entry_of(const std::vector<std::byte> &img, std::uint32_t type) {
  PersistHeader h{};
  std::memcpy(&h, img.data(), sizeof h);
  for (std::uint32_t i = 0; i < h.section_count; ++i) {
    PersistSectionEntry e{};
    std::memcpy(&e, img.data() + 256 + 64 * i, sizeof e);
    if (e.type == type) return e;
  }
  ADD_FAILURE() << "no section " << type;
  return {};
}

// ── SpiderRock-shaped synthetic store (written once per process) ─────────────

[[nodiscard]] const fs::path &synthetic_store() {
  static const fs::path root = [] {
    const fs::path dir = fs::temp_directory_path() / ("atx-persist-sa-store-" + pid_tag());
    std::error_code ec;
    fs::remove_all(dir, ec);
    SyntheticSpiderRockSpec spec;
    spec.n_underliers = 6;
    spec.n_expiries = 4;
    spec.n_strikes = 9;
    spec.seed = 3;
    const Status st = write_synthetic_spiderrock_store(spec, dir.string());
    EXPECT_TRUE(st.has_value()) << (st ? "" : st.error().to_string());
    return dir;
  }();
  return root;
}

[[nodiscard]] SpiderRockStoreSpec store_spec() {
  SpiderRockStoreSpec spec;
  spec.store_root = synthetic_store().string();
  spec.date = SyntheticSpiderRockSpec{}.date;
  return spec;
}

struct LoadedBucket {
  SpiderRockBucket bucket;
  MarketStateStorage storage;
  BucketStateReport report;
};

[[nodiscard]] std::unique_ptr<LoadedBucket> load_bucket(std::string_view bucket,
                                                       UnderlierTable &table) {
  auto out = std::make_unique<LoadedBucket>();
  out->bucket = load_spiderrock_bucket(store_spec(), bucket, table).value();
  out->storage = market_state_from_bucket(out->bucket, TimeSpec{}, &out->report).value();
  return out;
}

} // namespace

// ── PersistStateArchive ──────────────────────────────────────────────────────

TEST(PersistStateArchive, RoundTripsStorageBitExact) {
  const auto h = hand_market();
  const MarketStateWriteInput in = input_of(*h);
  auto image = encode_market_state(in);
  ASSERT_TRUE(image.has_value()) << to_string(image.error().status);
  auto again = encode_market_state(in);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*image, *again) << "encoding is deterministic";

  const fs::path path = fresh_dir("rt") / "s.atxms";
  ASSERT_TRUE(publish_persist_file(path.string(), *image).has_value());
  auto mapped = MarketStateArchive::open(path.string(), kMarketStateOpenDefaults);
  ASSERT_TRUE(mapped.has_value()) << to_string(mapped.error().status);
  const MarketStateArchive &a = *mapped;
  EXPECT_EQ(a.source_fingerprint(), 0xFEEDu);
  EXPECT_EQ(a.conventions_version(), 3u);
  EXPECT_EQ(a.date(), "2026-08-14");
  EXPECT_EQ(a.bucket_et(), "1000");
  EXPECT_TRUE(a.has_extras());
  EXPECT_EQ(a.n_marks(), 24u);
  EXPECT_EQ(a.ticker(7), "AAPL");
  EXPECT_EQ(a.ticker(2), "MSFT");
  EXPECT_EQ(a.ticker(5), "SPY");
  EXPECT_EQ(a.ticker(99), "");
  EXPECT_EQ(a.report().n_rows_bad_sigma, 4u);
  EXPECT_TRUE(bits(a.report().max_ddiv_spread, 0.0125));
  EXPECT_EQ(a.file().header().created_unix_ns, 1792000000000000000);

  auto copy = a.materialize();
  ASSERT_TRUE(copy.has_value());
  const MarketStateStorage &m = *copy;
  EXPECT_EQ(m.ts_ns, kTs);
  EXPECT_EQ(m.time, h->storage.time);
  ASSERT_EQ(m.underliers.size(), 3u);
  EXPECT_EQ(m.underliers[0].uid, 2u); // canonical: uid order
  EXPECT_EQ(m.underliers[1].uid, 5u);
  EXPECT_EQ(m.underliers[2].uid, 7u);
  for (const UnderlierState &got : m.underliers) {
    const auto want = std::find_if(h->storage.underliers.begin(), h->storage.underliers.end(),
                                   [&](const UnderlierState &u) { return u.uid == got.uid; });
    ASSERT_NE(want, h->storage.underliers.end());
    EXPECT_TRUE(bits(got.spot, want->spot) && bits(got.flat_rate, want->flat_rate) &&
                bits(got.flat_q, want->flat_q));
    EXPECT_EQ(got.carry, want->carry);
    EXPECT_TRUE(same(got.term_carry, want->term_carry));
    EXPECT_TRUE(same(got.cash_divs, want->cash_divs));
    EXPECT_EQ(got.yield == nullptr, want->yield == nullptr);
  }
  // Lane E (design D22): ExpiryCarry::T_override round-trips bit-exactly, NaN (no override)
  // and finite alike, through the materialized copy and the zero-copy mapped state.
  const std::vector<ExpiryCarry> &want_carry = h->storage.term_carries[1];
  ASSERT_EQ(want_carry.size(), 3u);
  ASSERT_TRUE(std::isnan(want_carry[0].T_override) && std::isnan(want_carry[1].T_override));
  ASSERT_EQ(want_carry[2].T_override, 0.2475);
  const UnderlierState *mapped_two = a.state().underlier(2);
  ASSERT_TRUE(mapped_two != nullptr);
  const std::span<const ExpiryCarry> copied = m.underliers[0].term_carry;
  for (const std::span<const ExpiryCarry> got : {copied, mapped_two->term_carry}) {
    ASSERT_EQ(got.size(), 3u);
    for (std::size_t r = 0; r < 3; ++r) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(got[r].T_override),
                std::bit_cast<std::uint64_t>(want_carry[r].T_override))
          << "row " << r;
    }
  }
  ASSERT_EQ(m.marks.size(), 2u);
  for (const auto &got : m.marks) {
    const auto want = std::find_if(h->storage.marks.begin(), h->storage.marks.end(),
                                   [&](const auto &x) { return x.uid == got.uid; });
    ASSERT_NE(want, h->storage.marks.end());
    EXPECT_EQ(got.strike, want->strike);
    EXPECT_EQ(got.expiry_ns, want->expiry_ns);
    EXPECT_EQ(got.side, want->side);
    EXPECT_TRUE(same<double>(got.sigma, want->sigma));
    EXPECT_TRUE(same<double>(got.vendor_price, want->vendor_price)) << "empty stays empty";
    EXPECT_TRUE(same<double>(got.vendor_delta, want->vendor_delta));
  }
  // Re-encoding the materialized copy reproduces the original bytes exactly.
  std::vector<MarkExtrasColumns> extras;
  for (const auto &mc : m.marks) {
    const MarkExtras x = a.extras(mc.uid);
    MarkExtrasColumns c;
    c.bid.assign(x.bid.begin(), x.bid.end());
    c.ask.assign(x.ask.begin(), x.ask.end());
    c.vendor_gamma.assign(x.vendor_gamma.begin(), x.vendor_gamma.end());
    c.vendor_vega.assign(x.vendor_vega.begin(), x.vendor_vega.end());
    c.vendor_theta.assign(x.vendor_theta.begin(), x.vendor_theta.end());
    c.vendor_T.assign(x.vendor_T.begin(), x.vendor_T.end());
    c.uprc.assign(x.uprc.begin(), x.uprc.end());
    extras.push_back(std::move(c));
  }
  MarketStateWriteInput re = in;
  re.storage = &m;
  re.extras = extras;
  auto re_image = encode_market_state(re);
  ASSERT_TRUE(re_image.has_value()) << to_string(re_image.error().status);
  EXPECT_EQ(*re_image, *image);

  // Refusals at encode.
  MarketStateWriteInput bad = in;
  bad.storage = nullptr;
  EXPECT_EQ(encode_market_state(bad).error().status, PersistStatus::InvalidArgument);
  bad = in;
  bad.date = "2026-08-14T10:00:00";
  EXPECT_EQ(encode_market_state(bad).error().status, PersistStatus::InvalidArgument);
  bad = in;
  bad.extras = std::span<const MarkExtrasColumns>{h->extras}.first(1);
  EXPECT_EQ(encode_market_state(bad).error().status, PersistStatus::InvalidArgument);
}

// Lane E re-pin: the term-carry row is the 40-byte ExpiryCarry (T_override appended), so
// the MarketState fingerprint differs from the 32-byte layout's: an archive written before
// is refused with SchemaMismatch (and rebuilt by the state store), never misread. The value
// is pinned so the next layout change is deliberate.
TEST(PersistStateArchive, FingerprintFollowsTermCarryRow) {
  namespace sf = atx::vol::state_format;
  static_assert(sizeof(ExpiryCarry) == 40);
  const auto carry =
      std::find_if(std::begin(sf::kSchema), std::end(sf::kSchema),
                   [](const PersistSectionSpec &s) { return s.name == "term-carry"; });
  ASSERT_NE(carry, std::end(sf::kSchema));
  EXPECT_EQ(carry->stride, 40u);
  ASSERT_EQ(carry->fields.size(), 5u);
  EXPECT_EQ(carry->fields[4].name, "T_override");
  EXPECT_EQ(carry->fields[4].offset, 32u);
  EXPECT_EQ(market_state_fingerprint(), sf::kFingerprint);
  // The pre-lane-E layout (four fields, stride 32) folds to another fingerprint.
  std::vector<PersistSectionSpec> old(std::begin(sf::kSchema), std::end(sf::kSchema));
  PersistSectionSpec &row = old[static_cast<std::size_t>(carry - std::begin(sf::kSchema))];
  row.stride = 32;
  row.fields = row.fields.first(4);
  const std::uint64_t old_fp = persist_fingerprint(persist_salt(kMarketStateSalt), old);
  EXPECT_NE(old_fp, sf::kFingerprint);
  std::printf("[persist] MarketState fingerprint 0x%016llx (32-byte carry row: 0x%016llx)\n",
              static_cast<unsigned long long>(sf::kFingerprint),
              static_cast<unsigned long long>(old_fp));
  // Pinned 2026-10-24 (lane E: ExpiryCarry 40 B).
  constexpr std::uint64_t kPinned = 0xb9738a6408b9ba89ull;
  EXPECT_EQ(market_state_fingerprint(), kPinned);
}

TEST(PersistStateArchive, ResolveMatchesInMemoryStateOnEveryMark) {
  UnderlierTable table;
  const auto loaded = load_bucket("1000", table);
  MarketStateWriteInput in;
  in.storage = &loaded->storage;
  in.tickers_by_uid = table.tickers();
  auto image = encode_market_state(in);
  ASSERT_TRUE(image.has_value());
  auto a = open_image(*image);
  ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
  const MarketState mem = make_market_state(loaded->storage).value();
  const MarketState &arc = a->state();
  ASSERT_EQ(arc.n_underliers(), mem.n_underliers());
  std::size_t checked = 0;
  for (const auto &mc : loaded->storage.marks) {
    for (std::size_t i = 0; i < mc.sigma.size(); ++i) {
      const ResolvedContract x = mem.resolve(mc.uid, mc.strike[i], mc.expiry_ns[i], mc.side[i]);
      const ResolvedContract y = arc.resolve(mc.uid, mc.strike[i], mc.expiry_ns[i], mc.side[i]);
      expect_resolved_equal(x, y, "mark");
      const auto sx = mem.sigma_at(mc.uid, mc.strike[i], mc.expiry_ns[i], mc.side[i], 0.3);
      const auto sy = arc.sigma_at(mc.uid, mc.strike[i], mc.expiry_ns[i], mc.side[i], 0.3);
      EXPECT_TRUE(bits(sx.sigma, sy.sigma));
      // An unlisted strike falls through identically (no surface: refused alike).
      expect_resolved_equal(mem.resolve(mc.uid, mc.strike[i] + 0.37, mc.expiry_ns[i], mc.side[i]),
                            arc.resolve(mc.uid, mc.strike[i] + 0.37, mc.expiry_ns[i], mc.side[i]),
                            "unlisted");
      ++checked;
    }
  }
  EXPECT_EQ(checked, loaded->report.n_marks);
  EXPECT_GT(checked, 300u);
  for (const UnderlierState &u : mem.underliers()) {
    for (const double T : {0.02, 0.1, 0.5, 1.5}) {
      const auto cx = mem.carry_at(u.uid, T);
      const auto cy = arc.carry_at(u.uid, T);
      EXPECT_TRUE(bits(cx.forward, cy.forward) && bits(cx.q_eff, cy.q_eff) &&
                  bits(cx.rate, cy.rate));
      EXPECT_EQ(cx.status, cy.status);
    }
  }
}

TEST(PersistStateArchive, MarksSpansPointIntoMapping) {
  const auto h = hand_market();
  auto image = encode_market_state(input_of(*h));
  ASSERT_TRUE(image.has_value());
  const fs::path path = fresh_dir("zc") / "s.atxms";
  ASSERT_TRUE(publish_persist_file(path.string(), *image).has_value());
  for (const bool mapped : {true, false}) {
    PersistOpenOptions o = kMarketStateOpenDefaults;
    o.mapped = mapped;
    auto a = MarketStateArchive::open(path.string(), o);
    ASSERT_TRUE(a.has_value());
    const std::span<const std::byte> backing = a->file().bytes();
    for (const std::uint32_t uid : {2u, 7u}) {
      const ContractMarks *m = a->state().marks(uid);
      ASSERT_NE(m, nullptr);
      for (const void *p : {static_cast<const void *>(m->strike.data()),
                            static_cast<const void *>(m->expiry_ns.data()),
                            static_cast<const void *>(m->side.data()),
                            static_cast<const void *>(m->sigma.data()),
                            static_cast<const void *>(m->vendor_delta.data())}) {
        EXPECT_TRUE(inside(p, backing)) << "uid " << uid << " mapped=" << mapped;
      }
    }
    EXPECT_EQ(a->state().marks(5), nullptr);
    EXPECT_TRUE(a->state().marks(7)->vendor_price.empty());
  }
}

TEST(PersistStateArchive, TermCarryAndDividendsAreZeroCopy) {
  const auto h = hand_market();
  auto image = encode_market_state(input_of(*h));
  ASSERT_TRUE(image.has_value());
  auto a = open_image(*image);
  ASSERT_TRUE(a.has_value());
  const std::span<const std::byte> backing = a->file().bytes();
  const UnderlierState *two = a->state().underlier(2);
  const UnderlierState *five = a->state().underlier(5);
  ASSERT_TRUE(two != nullptr && five != nullptr);
  ASSERT_EQ(two->term_carry.size(), 3u);
  ASSERT_EQ(five->cash_divs.size(), 2u);
  EXPECT_TRUE(inside(two->term_carry.data(), backing));
  EXPECT_TRUE(inside(five->cash_divs.data(), backing));
  EXPECT_TRUE(same(two->term_carry, std::span<const ExpiryCarry>{h->storage.term_carries[1]}));
  EXPECT_TRUE(
      same(five->cash_divs, std::span<const DividendEvent>{h->storage.dividend_schedules[2]}));
}

TEST(PersistStateArchive, YieldCurveRebuiltAtOpen) {
  const auto h = hand_market();
  auto image = encode_market_state(input_of(*h));
  ASSERT_TRUE(image.has_value());
  auto a = open_image(*image);
  ASSERT_TRUE(a.has_value());
  const UnderlierState *u = a->state().underlier(7);
  ASSERT_NE(u, nullptr);
  ASSERT_NE(u->yield, nullptr);
  EXPECT_FALSE(inside(u->yield, a->file().bytes())) << "a YieldCurve is rebuilt, not mapped";
  const YieldCurve &want = h->storage.yield_curves[0];
  for (int i = 0; i <= 60; ++i) {
    const double T = 0.01 + 0.05 * i;
    EXPECT_TRUE(bits(u->yield->disc(T), want.disc(T))) << T;
    EXPECT_TRUE(bits(u->yield->zero(T), want.zero(T))) << T;
  }
  EXPECT_EQ(a->state().underlier(2)->yield, nullptr);
  // The encoder refuses a curve with no pillars to rebuild from (a default-constructed
  // curve some underlier points at).
  HandMarket empty_curve = *h;
  empty_curve.storage.yield_curves[0] = YieldCurve{};
  empty_curve.storage.underliers[0].yield = &empty_curve.storage.yield_curves[0];
  EXPECT_EQ(encode_market_state(input_of(empty_curve)).error().status,
            PersistStatus::InvalidArgument);
}

TEST(PersistStateArchive, ExtrasAlignWithMarks) {
  const auto h = hand_market();
  auto image = encode_market_state(input_of(*h));
  ASSERT_TRUE(image.has_value());
  auto a = open_image(*image);
  ASSERT_TRUE(a.has_value());
  for (std::size_t mi = 0; mi < h->storage.marks.size(); ++mi) {
    const std::uint32_t uid = h->storage.marks[mi].uid;
    const MarkExtras x = a->extras(uid);
    const MarkExtrasColumns &want = h->extras[mi];
    ASSERT_EQ(x.bid.size(), a->state().marks(uid)->size());
    EXPECT_TRUE(same<double>(x.bid, want.bid));
    EXPECT_TRUE(same<double>(x.ask, want.ask));
    EXPECT_TRUE(same<double>(x.vendor_gamma, want.vendor_gamma));
    EXPECT_TRUE(same<double>(x.vendor_vega, want.vendor_vega));
    EXPECT_TRUE(same<double>(x.vendor_theta, want.vendor_theta));
    EXPECT_TRUE(same<double>(x.uprc, want.uprc));
    if (want.vendor_T.empty()) {
      ASSERT_EQ(x.vendor_T.size(), x.bid.size());
      EXPECT_TRUE(std::all_of(x.vendor_T.begin(), x.vendor_T.end(),
                              [](double v) { return std::isnan(v); }));
    } else {
      EXPECT_TRUE(same<double>(x.vendor_T, want.vendor_T));
    }
    EXPECT_TRUE(inside(x.bid.data(), a->file().bytes()));
  }
  EXPECT_TRUE(a->extras(5).bid.empty()) << "a uid without marks has no extras rows";
  EXPECT_TRUE(a->extras(99).ask.empty());
  MarketStateWriteInput lean = input_of(*h);
  lean.extras = {};
  auto lean_image = encode_market_state(lean);
  ASSERT_TRUE(lean_image.has_value());
  EXPECT_LT(lean_image->size(), image->size() + 1);
  auto b = open_image(*lean_image);
  ASSERT_TRUE(b.has_value());
  EXPECT_FALSE(b->has_extras());
  EXPECT_TRUE(b->extras(7).bid.empty());
  EXPECT_FALSE(selection_bucket_from_archive(*b).has_value());
  // A misaligned extras column is refused at encode.
  std::vector<MarkExtrasColumns> short_cols = h->extras;
  short_cols[1].ask.pop_back();
  MarketStateWriteInput bad = input_of(*h);
  bad.extras = short_cols;
  EXPECT_EQ(encode_market_state(bad).error().status, PersistStatus::InvalidArgument);
}

TEST(PersistStateArchive, EmbeddedSurfacesServeViewsBitExact) {
  const auto owned = rt::make_state(rt::StateMove{});
  MarketStateWriteInput in;
  in.storage = &owned->storage;
  auto image = encode_market_state(in);
  ASSERT_TRUE(image.has_value()) << to_string(image.error().status);
  auto a = open_image(*image);
  ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
  // The reference: views over the SAME surfaces through the existing ATXVSA2 path.
  std::vector<std::string> symbols;
  std::vector<SurfaceArchiveItem> items;
  symbols.reserve(owned->surfaces.size());
  for (const PricedSurface &s : owned->surfaces) {
    symbols.push_back("U" + std::to_string(s.uid()));
    items.push_back(SurfaceArchiveItem{symbols.back(), &s, std::nullopt});
  }
  auto ref_archive = SurfaceArchiveV2::open(write_surface_archive_v2(items).value()).value();
  std::vector<PricedSurfaceView> ref_views;
  ref_views.reserve(items.size());
  std::vector<SurfaceRef> ref_refs;
  for (const std::string &sym : symbols) {
    ref_views.push_back(ref_archive.map_symbol(sym).value());
    ref_refs.emplace_back(&ref_views.back());
  }
  const MarketState ref_state = MarketState::create(owned->storage.ts_ns, owned->storage.time,
                                                    owned->storage.underliers, {}, ref_refs)
                                    .value();
  for (const PricedSurface &s : owned->surfaces) {
    const SurfaceRef got = a->state().surface(s.uid());
    ASSERT_TRUE(got.valid());
    EXPECT_TRUE(got.is_view());
    EXPECT_TRUE(inside(got.view(), a->file().bytes()) == false) << "the view object is owned";
    const SurfaceRef want = ref_state.surface(s.uid());
    for (const double T : {0.03, 0.1, 0.25, 0.6, 1.1}) {
      EXPECT_TRUE(bits(got.forward_at(T), want.forward_at(T)));
      EXPECT_TRUE(bits(got.q_eff_at(T), want.q_eff_at(T)));
      EXPECT_TRUE(bits(got.rate_at(T), want.rate_at(T)));
      for (const double rel : {0.8, 0.95, 1.0, 1.07, 1.25}) {
        const double K = s.pricing().S * rel;
        EXPECT_TRUE(bits(got.iv(K, T), want.iv(K, T))) << K << " " << T;
        EXPECT_NEAR(got.iv(K, T), s.iv(K, T), 1e-12) << "owned surface agrees";
      }
    }
    for (std::size_t e = 0; e < rt::kExpiryUnits.size(); ++e) {
      const double K = s.pricing().S * 1.03;
      expect_resolved_equal(a->state().resolve(s.uid(), K, rt::expiry_ns(e), Side::Call),
                            ref_state.resolve(s.uid(), K, rt::expiry_ns(e), Side::Call), "surface");
    }
  }
  // A view-backed surface has no writer: refused.
  MarketStateWriteInput views = in;
  views.surfaces = ref_refs;
  EXPECT_EQ(encode_market_state(views).error().status, PersistStatus::InvalidArgument);
}

TEST(PersistStateArchive, MovedArchiveKeepsStateValid) {
  const auto h = hand_market();
  auto image = encode_market_state(input_of(*h));
  ASSERT_TRUE(image.has_value());
  auto opened = open_image(*image);
  ASSERT_TRUE(opened.has_value());
  MarketStateArchive a = std::move(*opened);
  const MarketState *where = &a.state();
  const MarketState copy = a.state(); // a copy of the views: borrows the archive
  const ContractMarks *m7 = a.state().marks(7);
  ASSERT_NE(m7, nullptr);
  const ResolvedContract before = copy.resolve(7, m7->strike[4], m7->expiry_ns[4], m7->side[4]);
  MarketStateArchive b = std::move(a);
  EXPECT_EQ(&b.state(), where) << "the PIMPL state never moves";
  const ResolvedContract after = copy.resolve(7, m7->strike[4], m7->expiry_ns[4], m7->side[4]);
  expect_resolved_equal(before, after, "copy after move");
  expect_resolved_equal(before, b.state().resolve(7, m7->strike[4], m7->expiry_ns[4], m7->side[4]),
                        "moved archive");
  EXPECT_EQ(b.ticker(7), "AAPL");
  auto other = open_image(*image);
  ASSERT_TRUE(other.has_value());
  *other = std::move(b);
  EXPECT_EQ(&other->state(), where);
  EXPECT_TRUE(bits(other->state().underlier(7)->yield->disc(0.7),
                   h->storage.yield_curves[0].disc(0.7)));
}

TEST(PersistStateArchive, RefusesUnsortedMarks) {
  const auto h = hand_market();
  auto image = encode_market_state(input_of(*h));
  ASSERT_TRUE(image.has_value());
  std::vector<std::byte> img = *image;
  const PersistSectionEntry strike =
      entry_of(img, static_cast<std::uint32_t>(MarketStateSection::Strike));
  // Rows 0..2 of the first uid are ascending strikes of one (expiry, side): swap 1 and 2.
  double k1 = 0.0;
  double k2 = 0.0;
  std::memcpy(&k1, img.data() + strike.offset + 8, 8);
  std::memcpy(&k2, img.data() + strike.offset + 16, 8);
  ASSERT_LT(k1, k2);
  std::memcpy(img.data() + strike.offset + 8, &k2, 8);
  std::memcpy(img.data() + strike.offset + 16, &k1, 8);
  EXPECT_EQ(open_image(img).error().status, PersistStatus::SectionChecksum) << "unsealed damage";
  reseal_section(img, static_cast<std::uint32_t>(MarketStateSection::Strike));
  auto a = open_image(img);
  ASSERT_FALSE(a.has_value());
  EXPECT_EQ(a.error().status, PersistStatus::IndexUnsorted);
  EXPECT_EQ(a.error().detail, 2u);
}

TEST(PersistStateArchive, RefusesInvalidSideByte) {
  const auto h = hand_market();
  auto image = encode_market_state(input_of(*h));
  ASSERT_TRUE(image.has_value());
  std::vector<std::byte> img = *image;
  const auto side_type = static_cast<std::uint32_t>(MarketStateSection::Side);
  const PersistSectionEntry side = entry_of(img, side_type);
  img[side.offset + 5] = std::byte{2};
  reseal_section(img, side_type);
  auto a = open_image(img);
  ASSERT_FALSE(a.has_value());
  EXPECT_EQ(a.error().status, PersistStatus::RecordInvalid);
  EXPECT_EQ(a.error().section, side_type);
  EXPECT_EQ(a.error().detail, 5u);
}

TEST(PersistStateArchive, ExplainOverArchivedStatesMatchesInMemory) {
  const std::vector<std::string> buckets = SyntheticSpiderRockSpec{}.buckets_et;
  ASSERT_EQ(buckets.size(), 11u);
  UnderlierTable table; // ONE table in bucket order: the bench's uid assignment
  std::vector<std::unique_ptr<LoadedBucket>> loaded;
  std::vector<MarketState> mem;
  for (const std::string &b : buckets) {
    loaded.push_back(load_bucket(b, table));
    mem.push_back(make_market_state(loaded.back()->storage).value());
  }
  const fs::path dir = fresh_dir("explain");
  std::vector<MarketStateArchive> archives;
  for (std::size_t k = 0; k < buckets.size(); ++k) {
    MarketStateWriteInput in;
    in.storage = &loaded[k]->storage;
    in.tickers_by_uid = table.tickers();
    auto image = encode_market_state(in);
    ASSERT_TRUE(image.has_value());
    const fs::path path = dir / (buckets[k] + ".atxms");
    ASSERT_TRUE(publish_persist_file(path.string(), *image).has_value());
    auto a = MarketStateArchive::open(path.string(), kMarketStateOpenDefaults);
    ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
    archives.push_back(std::move(*a));
  }
  // A book over the first bucket's marks: every third contract, alternating sign.
  std::vector<OptionLeg> legs;
  std::uint64_t id = 1;
  for (const auto &mc : loaded[0]->storage.marks) {
    for (std::size_t i = 0; i < mc.sigma.size(); i += 3) {
      const double qty = (id % 2u == 0u ? -1.0 : 1.0) * static_cast<double>(1 + id % 4u);
      legs.push_back(
          OptionLeg{id++, mc.uid, mc.strike[i], mc.expiry_ns[i], mc.side[i], qty, 100.0});
    }
  }
  const std::vector<ShareLeg> shares{{9001u, 1u, 120.0}, {9002u, 2u, -35.0}};
  ASSERT_GT(legs.size(), 100u);
  const RiskBook book = RiskBook::create(legs, shares).value();
  RiskStream from_mem(book, RiskKernel::family());
  RiskStream from_arc(book, RiskKernel::family());
  ASSERT_TRUE(from_mem.start(mem[0]).has_value());
  ASSERT_TRUE(from_arc.start(archives[0].state()).has_value());
  std::size_t steps = 0;
  for (std::size_t k = 1; k < buckets.size(); ++k) {
    auto x = from_mem.step(mem[k]);
    auto y = from_arc.step(archives[k].state());
    ASSERT_TRUE(x.has_value() && y.has_value());
    const ExplainResult &rx = **x;
    const ExplainResult &ry = **y;
    EXPECT_EQ(std::memcmp(&rx.total, &ry.total, sizeof(ExplainTotals)), 0) << "step " << k;
    EXPECT_TRUE(same<double>(rx.pnl_total, ry.pnl_total));
    EXPECT_TRUE(same<double>(rx.pnl_unexplained, ry.pnl_unexplained));
    EXPECT_GT(rx.total.n_ok, 0u);
    ++steps;
  }
  EXPECT_EQ(steps, 10u);
}
