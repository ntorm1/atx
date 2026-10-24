// persist_alloc_test.cpp — PersistFile.NoAllocationAfterOpen (design wave3-persist
// D6 / T8): once `open` returns, every accessor of the container is allocation-free,
// and so is every accessor of an opened MarketStateArchive (review 1: the state, its
// lookups, tickers, extras and the provenance getters; `materialize` is a deep copy
// and allocates by design).
// Counted with tests/support/alloc_counter.* (thread-local armed tally over a
// replaced global operator new), linked ONLY into atx-vol-persist-alloc-tests, never
// the monolithic atx-vol-tests or a slice (which must keep the default operator new).
// Registered under the "persist/" prefix so the lane's anchored
// -R "^persist/PersistFile\." runs it.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/api/persist/persist_file.hpp"
#include "atx/vol/api/persist/state_archive.hpp"
#include "atx/vol/api/pricing/rates_curve.hpp"
#include "atx/vol/api/risk/market_state.hpp"
#include "support/alloc_counter.hpp"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace atx::vol; // NOLINT(google-build-using-namespace): test file

constexpr std::uint64_t kFp = 0x0A110CA7Eull;

[[nodiscard]] fs::path fresh_dir() {
#if defined(_WIN32)
  const std::string pid = std::to_string(_getpid());
#else
  const std::string pid = std::to_string(getpid());
#endif
  const fs::path dir = fs::temp_directory_path() / ("atx-persist-alloc-" + pid);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

constexpr std::int64_t kTs = 1786716000000000000LL;
constexpr std::int64_t kDay = 86400000000000LL;

// A small but complete state: a yield curve, term carry, cash dividends, vendor columns
// and the seven extras columns.
struct SmallMarket {
  MarketStateStorage storage;
  std::vector<MarkExtrasColumns> extras;
  std::vector<std::string> tickers{"AAA", "BBB"};
};

[[nodiscard]] std::unique_ptr<SmallMarket> small_market() {
  auto m = std::make_unique<SmallMarket>();
  MarketStateStorage &s = m->storage;
  s.ts_ns = kTs;
  s.underliers.resize(2);
  s.underliers[0] = UnderlierState{1, 100.0, 0.04, nullptr, 0.01, {}, {}, CarryFrom::State};
  s.underliers[1] = UnderlierState{2, 50.0, 0.03, nullptr, 0.0, {}, {}, CarryFrom::State};
  const std::vector<double> t{0.25, 1.0};
  const std::vector<double> r{0.040, 0.042};
  s.yield_curves.push_back(YieldCurve::create(t, r).value());
  s.term_carries.resize(2);
  s.term_carries[0] = {{kTs + kDay * 30, 0.0, 0.04, 0.01}};
  s.dividend_schedules.resize(2);
  s.dividend_schedules[1] = {{kTs + kDay * 20, 0.5}};
  for (const std::uint32_t uid : {1u, 2u}) {
    MarketStateStorage::MarksColumns c;
    c.uid = uid;
    MarkExtrasColumns x;
    for (const Side side : {Side::Call, Side::Put}) {
      for (int k = 0; k < 4; ++k) {
        c.strike.push_back(40.0 * uid + 5.0 * k);
        c.expiry_ns.push_back(kTs + kDay * 30);
        c.side.push_back(side);
        c.sigma.push_back(0.2 + 0.01 * k);
        c.vendor_price.push_back(1.0 + k);
        c.vendor_delta.push_back(side == Side::Call ? 0.5 : -0.5);
        for (std::vector<double> *col : {&x.bid, &x.ask, &x.vendor_gamma, &x.vendor_vega,
                                         &x.vendor_theta, &x.vendor_T, &x.uprc}) {
          col->push_back(0.5 + k);
        }
      }
    }
    s.marks.push_back(std::move(c));
    m->extras.push_back(std::move(x));
  }
  s.underliers[0].yield = &s.yield_curves[0];
  s.underliers[0].term_carry = s.term_carries[0];
  s.underliers[1].cash_divs = s.dividend_schedules[1];
  return m;
}

} // namespace

TEST(PersistFile, NoAllocationAfterOpen) {
  std::vector<double> plain(4096, 2.5);
  std::vector<double> packed(4096);
  for (std::size_t i = 0; i < packed.size(); ++i) packed[i] = static_cast<double>(i % 17);
  std::vector<std::uint8_t> flags(4096, 1);
  const PersistSectionInput inputs[] = {
      {16, 1, 0, PersistDType::F64, 8, true, {}, {}, 3, std::as_bytes(std::span{plain})},
      {17, 1, 0, PersistDType::F64, 8, true, PersistCodec::Zstd, PersistFilter::ByteShuffle, 3,
       std::as_bytes(std::span{packed})},
      {18, 1, 0, PersistDType::U8, 1, false, {}, {}, 3, std::as_bytes(std::span{flags})},
  };
  PersistFileInfo info;
  info.kind = PersistKind::RiskRun;
  info.schema_fingerprint = kFp;
  auto image = build_persist_file(inputs, info);
  ASSERT_TRUE(image.has_value());
  const fs::path path = fresh_dir() / "alloc.atxrr";
  ASSERT_TRUE(publish_persist_file(path.string(), *image).has_value());

  static constexpr PersistSectionSpec kSchema[] = {
      {16, 1, PersistDType::F64, 8, "plain", {}, true},
      {17, 1, PersistDType::F64, 8, "packed", {}, true},
      {18, 1, PersistDType::U8, 1, "flags", {}, false},
  };
  for (const bool mapped : {true, false}) {
    for (const PersistVerify v :
         {PersistVerify::Framing, PersistVerify::Lazy, PersistVerify::Full}) {
      PersistOpenOptions o;
      o.mapped = mapped;
      o.verify = v;
      auto f = PersistFile::open(path.string(), PersistKind::RiskRun, kFp, o);
      ASSERT_TRUE(f.has_value());
      double sink = 0.0;
      const std::uint64_t n = atx::vol::testkit::count_allocations([&] {
        sink += static_cast<double>(f->header().file_size + f->footer().file_size);
        sink += static_cast<double>(f->sections().size() + f->bytes().size());
        for (const std::uint32_t t : {16u, 17u, 18u, 99u}) {
          auto s = f->section(t);
          sink += s ? static_cast<double>(s->size()) : 1.0;
          sink += f->entry(t) != nullptr ? 1.0 : 0.0;
        }
        auto d = f->typed<double>(17);
        sink += d ? (*d)[5] : 0.0;
        auto u = f->typed<std::uint8_t>(18);
        sink += u ? (*u)[0] : 0.0;
        sink += f->check_schema(kSchema).has_value() ? 1.0 : 0.0;
        sink += f->verify_all().has_value() ? 1.0 : 0.0;
        const std::shared_ptr<const void> keep = f->keepalive();
        sink += keep != nullptr ? 1.0 : 0.0;
      });
      EXPECT_EQ(n, 0u) << "mapped=" << mapped << " verify=" << static_cast<int>(v);
      EXPECT_GT(sink, 0.0);
    }
  }

  // The MarketState archive over the same container.
  const auto market = small_market();
  MarketStateWriteInput in;
  in.storage = &market->storage;
  in.tickers_by_uid = market->tickers;
  in.extras = market->extras;
  in.date = "2026-08-14";
  in.bucket_et = "1000";
  in.conventions_version = 1;
  auto state_image = encode_market_state(in);
  ASSERT_TRUE(state_image.has_value());
  const fs::path state_path = path.parent_path() / "alloc.atxms";
  ASSERT_TRUE(publish_persist_file(state_path.string(), *state_image).has_value());
  const std::int64_t expiry = kTs + kDay * 30;
  for (const bool mapped : {true, false}) {
    for (const PersistVerify v :
         {PersistVerify::Framing, PersistVerify::Lazy, PersistVerify::Full}) {
      PersistOpenOptions o = kMarketStateOpenDefaults;
      o.mapped = mapped;
      o.verify = v;
      auto a = MarketStateArchive::open(state_path.string(), o);
      ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
      double sink = 0.0;
      const std::uint64_t n = atx::vol::testkit::count_allocations([&] {
        const MarketState &st = a->state();
        sink += static_cast<double>(st.n_underliers() + st.underliers().size());
        for (const std::uint32_t uid : {1u, 2u, 9u}) {
          const UnderlierState *u = st.underlier(uid);
          sink += u != nullptr ? u->spot : 0.0;
          const ContractMarks *cm = st.marks(uid);
          sink += cm != nullptr ? static_cast<double>(cm->size()) : 0.0;
          const MarkExtras x = a->extras(uid);
          sink += static_cast<double>(x.bid.size() + x.uprc.size());
          sink += static_cast<double>(a->ticker(uid).size());
        }
        const ResolvedContract rc = st.resolve(1, 45.0, expiry, Side::Call);
        sink += rc.sigma;
        sink += st.carry_at(2, 0.1).forward;
        sink += static_cast<double>(a->source_fingerprint() + a->n_marks() +
                                    a->uid_table_size() + a->conventions_version());
        sink += static_cast<double>(a->date().size() + a->bucket_et().size());
        sink += a->has_extras() ? 1.0 : 0.0;
        sink += static_cast<double>(a->report().n_marks);
        sink += static_cast<double>(a->file().header().file_size);
      });
      EXPECT_EQ(n, 0u) << "archive mapped=" << mapped << " verify=" << static_cast<int>(v);
      EXPECT_GT(sink, 0.0);
    }
  }
}
