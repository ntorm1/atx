// persist_state_store_test.cpp — the SpiderRock-derived MarketState store
// (api/persist/state_archive.hpp "SpiderRock-derived state store"; design
// wave3-persist D11 / D15 / T6 / T7).
//
// PersistStateStore.* run on a SpiderRock-SHAPED synthetic store written by the
// shipped generator into a per-process temp dir. PersistStateStoreReal.* are gated
// on ATX_VOL_SPIDERROCK_STORE (the licensed store root, READ-ONLY; the date defaults
// to the 2026-10-14 drop, override with ATX_VOL_SPIDERROCK_DATE) and GTEST_SKIP
// otherwise; they write only to the test scratch dir.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
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
#include "risk_bench.hpp"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace atx::vol; // NOLINT(google-build-using-namespace): test file

constexpr std::uint32_t kConventions = 1;

[[nodiscard]] std::string pid_tag() {
#if defined(_WIN32)
  return std::to_string(_getpid());
#else
  return std::to_string(getpid());
#endif
}

[[nodiscard]] fs::path fresh_dir(std::string_view tag) {
  const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string name = "atx-persist-ss-" + std::string(tag);
  if (info != nullptr) name += std::string("-") + info->name();
  const fs::path dir = fs::temp_directory_path() / (name + "-" + pid_tag());
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

[[nodiscard]] std::optional<std::string> env(const char *name) {
#ifdef _MSC_VER
  char *value = nullptr;
  std::size_t size = 0;
  const int status = ::_dupenv_s(&value, &size, name);
  const std::unique_ptr<char, decltype(&std::free)> owned(value, &std::free);
  if (status != 0 || value == nullptr || *value == '\0') return std::nullopt;
  return std::string(value);
#else
  const char *value = std::getenv(name);
  return value == nullptr || *value == '\0' ? std::nullopt : std::optional<std::string>(value);
#endif
}

// A small SpiderRock-shaped day: 8 underliers so a top-N cap changes membership.
[[nodiscard]] fs::path write_store(const fs::path &dir) {
  SyntheticSpiderRockSpec spec;
  spec.n_underliers = 8;
  spec.n_expiries = 4;
  spec.n_strikes = 7;
  spec.seed = 5;
  const Status st = write_synthetic_spiderrock_store(spec, dir.string());
  EXPECT_TRUE(st.has_value()) << (st ? "" : st.error().to_string());
  return dir;
}

[[nodiscard]] const fs::path &shared_store() {
  static const fs::path root = [] {
    const fs::path dir = fs::temp_directory_path() / ("atx-persist-ss-store-" + pid_tag());
    std::error_code ec;
    fs::remove_all(dir, ec);
    return write_store(dir);
  }();
  return root;
}

[[nodiscard]] SpiderRockStoreSpec spec_of(const fs::path &root, std::size_t top = 0) {
  SpiderRockStoreSpec spec;
  spec.store_root = root.string();
  spec.date = SyntheticSpiderRockSpec{}.date;
  spec.max_underliers = top;
  return spec;
}

[[nodiscard]] StateStoreBuildOptions options_of(const fs::path &out, unsigned parallel = 2) {
  StateStoreBuildOptions o;
  o.out_root = out.string();
  o.max_parallel_buckets = parallel;
  o.created_unix_ns = 1792000000000000000;
  o.conventions_version = kConventions;
  return o;
}

[[nodiscard]] std::vector<std::byte> slurp(const fs::path &p) {
  std::ifstream is(p, std::ios::binary);
  const std::vector<char> raw{std::istreambuf_iterator<char>(is), std::istreambuf_iterator<char>()};
  std::vector<std::byte> out(raw.size());
  std::memcpy(out.data(), raw.data(), raw.size());
  return out;
}

template <class T> [[nodiscard]] bool same(std::span<const T> a, std::span<const T> b) {
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size_bytes()) == 0);
}

// The archive's state equals a serial Parquet load with ONE table, mark for mark.
void expect_state_equals(const MarketState &arc, const MarketStateArchive &archive,
                         const MarketStateStorage &mem, const UnderlierTable &table) {
  ASSERT_EQ(arc.n_underliers(), mem.underliers.size());
  for (const UnderlierState &u : mem.underliers) {
    const UnderlierState *got = arc.underlier(u.uid);
    ASSERT_NE(got, nullptr) << u.uid;
    EXPECT_EQ(archive.ticker(u.uid), table.ticker(u.uid));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(got->spot), std::bit_cast<std::uint64_t>(u.spot));
    EXPECT_TRUE(same(got->term_carry, u.term_carry)) << u.uid;
  }
  std::size_t marks = 0;
  for (const auto &m : mem.marks) {
    const ContractMarks *got = arc.marks(m.uid);
    ASSERT_NE(got, nullptr) << m.uid;
    EXPECT_TRUE(same(got->strike, std::span<const double>{m.strike})) << m.uid;
    EXPECT_TRUE(same(got->expiry_ns, std::span<const std::int64_t>{m.expiry_ns})) << m.uid;
    EXPECT_TRUE(same(got->side, std::span<const Side>{m.side})) << m.uid;
    EXPECT_TRUE(same(got->sigma, std::span<const double>{m.sigma})) << m.uid;
    EXPECT_TRUE(same(got->vendor_price, std::span<const double>{m.vendor_price})) << m.uid;
    EXPECT_TRUE(same(got->vendor_delta, std::span<const double>{m.vendor_delta})) << m.uid;
    marks += m.sigma.size();
  }
  EXPECT_EQ(archive.n_marks(), marks);
}

} // namespace

// ── PersistStateStore ────────────────────────────────────────────────────────

TEST(PersistStateStore, BuildsSyntheticDayAndListsBuckets) {
  const SpiderRockStoreSpec spec = spec_of(shared_store(), 5); // top-5 of 8: membership moves
  const std::vector<std::string> buckets =
      list_spiderrock_buckets(spec.store_root, spec.date).value();
  ASSERT_EQ(buckets.size(), 11u);
  const fs::path out = fresh_dir("build");
  auto built = build_state_store_from_spiderrock(spec, buckets, options_of(out, 4));
  ASSERT_TRUE(built.has_value()) << to_string(built.error().status);
  ASSERT_EQ(built->size(), buckets.size());
  for (std::size_t k = 0; k < buckets.size(); ++k) {
    const StateStoreBucketReport &r = (*built)[k];
    EXPECT_EQ(r.bucket_et, buckets[k]);
    EXPECT_FALSE(r.skipped);
    EXPECT_GT(r.marks, 0u);
    EXPECT_GT(r.bytes, 0u);
    EXPECT_TRUE(fs::exists(r.path)) << r.path;
    EXPECT_EQ(r.source_fingerprint,
              spiderrock_state_source_fingerprint(spec, buckets[k], kConventions).value());
  }
  auto store = StateStore::open(out.string());
  ASSERT_TRUE(store.has_value());
  EXPECT_EQ(store->buckets(spec.date).value(), buckets);
  EXPECT_EQ(store->buckets("2026-01-01").error().status, PersistStatus::NotFound);
  // Parallel loads, serial uids: every bucket equals a serial load with ONE table.
  UnderlierTable table;
  for (std::size_t k = 0; k < buckets.size(); ++k) {
    const SpiderRockBucket bucket = load_spiderrock_bucket(spec, buckets[k], table).value();
    const MarketStateStorage mem = market_state_from_bucket(bucket, spec.time).value();
    auto a = store->open_bucket(spec.date, buckets[k]);
    ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
    expect_state_equals(a->state(), *a, mem, table);
    EXPECT_EQ(a->source_fingerprint(), (*built)[k].source_fingerprint);
    EXPECT_EQ(a->conventions_version(), kConventions);
    EXPECT_EQ(a->bucket_et(), buckets[k]);
    EXPECT_EQ(a->uid_table_size(), table.size()) << "the table after bucket " << buckets[k];
    EXPECT_EQ(a->n_marks(), (*built)[k].marks);
  }
  EXPECT_EQ(store->open_bucket(spec.date, "0930").error().status, PersistStatus::NotFound);
}

TEST(PersistStateStore, RebuildSkipsUpToDateBuckets) {
  const SpiderRockStoreSpec spec = spec_of(shared_store());
  const std::vector<std::string> buckets{"1000", "1030", "1100", "1130"};
  const fs::path out = fresh_dir("skip");
  auto first = build_state_store_from_spiderrock(spec, buckets, options_of(out));
  ASSERT_TRUE(first.has_value());
  const auto files_after_first = std::distance(fs::recursive_directory_iterator(out),
                                               fs::recursive_directory_iterator{});
  auto second = build_state_store_from_spiderrock(spec, buckets, options_of(out));
  ASSERT_TRUE(second.has_value()) << to_string(second.error().status);
  for (std::size_t k = 0; k < buckets.size(); ++k) {
    EXPECT_TRUE((*second)[k].skipped) << buckets[k];
    EXPECT_EQ((*second)[k].path, (*first)[k].path);
    EXPECT_EQ((*second)[k].marks, (*first)[k].marks);
  }
  EXPECT_EQ(
      std::distance(fs::recursive_directory_iterator(out), fs::recursive_directory_iterator{}),
            files_after_first)
      << "an up-to-date build writes nothing";
  // force: a new generation, byte-identical (T6 determinism).
  StateStoreBuildOptions force = options_of(out, 1);
  force.force = true;
  auto forced = build_state_store_from_spiderrock(spec, buckets, force);
  ASSERT_TRUE(forced.has_value());
  for (std::size_t k = 0; k < buckets.size(); ++k) {
    EXPECT_FALSE((*forced)[k].skipped);
    EXPECT_NE((*forced)[k].path, (*first)[k].path);
    EXPECT_EQ(slurp((*forced)[k].path), slurp((*first)[k].path))
        << buckets[k] << ": 1 vs 2 parallel buckets, two builds, same bytes";
  }
  // Changing the extras choice is a content change: everything rebuilds.
  StateStoreBuildOptions lean = options_of(out);
  lean.include_extras = false;
  auto rebuilt = build_state_store_from_spiderrock(spec, buckets, lean);
  ASSERT_TRUE(rebuilt.has_value());
  for (const StateStoreBucketReport &r : *rebuilt) EXPECT_FALSE(r.skipped);
  auto store = StateStore::open(out.string());
  ASSERT_TRUE(store.has_value());
  EXPECT_FALSE(store->open_bucket(spec.date, "1000").value().has_extras());
}

TEST(PersistStateStore, ChangedSourceFileTriggersRebuild) {
  const fs::path src = write_store(fresh_dir("src"));
  const SpiderRockStoreSpec spec = spec_of(src);
  const std::vector<std::string> buckets{"1000", "1030", "1100"};
  const fs::path out = fresh_dir("out");
  auto first = build_state_store_from_spiderrock(spec, buckets, options_of(out));
  ASSERT_TRUE(first.has_value());
  const std::uint64_t before =
      spiderrock_state_source_fingerprint(spec, "1030", kConventions).value();
  // Touch one parquet file of 1030: same bytes, a new last-write time.
  const fs::path bucket_dir = src / ("date=" + spec.date) / "bucket_et=1030";
  std::size_t touched = 0;
  for (const auto &entry : fs::directory_iterator(bucket_dir)) {
    if (entry.path().extension() != ".parquet") continue;
    fs::last_write_time(entry.path(), fs::last_write_time(entry.path()) + std::chrono::hours(1));
    ++touched;
  }
  ASSERT_GT(touched, 0u);
  const std::uint64_t after =
      spiderrock_state_source_fingerprint(spec, "1030", kConventions).value();
  EXPECT_NE(before, after);
  EXPECT_NE(spiderrock_state_source_fingerprint(spec, "1030", kConventions + 1).value(), after)
      << "the conventions version is a fingerprint input";
  auto second = build_state_store_from_spiderrock(spec, buckets, options_of(out));
  ASSERT_TRUE(second.has_value());
  EXPECT_TRUE((*second)[0].skipped);
  EXPECT_FALSE((*second)[1].skipped);
  EXPECT_TRUE((*second)[2].skipped) << "same tickers: the later bucket's uids still agree";
  EXPECT_EQ((*second)[1].source_fingerprint, after);
  auto store = StateStore::open(out.string());
  ASSERT_TRUE(store.has_value());
  EXPECT_EQ(store->open_bucket(spec.date, "1030").value().source_fingerprint(), after);
  // A missing bucket directory is NotFound, not an empty archive.
  EXPECT_EQ(spiderrock_state_source_fingerprint(spec, "0900", kConventions).error().status,
            PersistStatus::NotFound);
}

TEST(PersistStateStore, RefusesOutputInsideSourceRoot) {
  const SpiderRockStoreSpec spec = spec_of(shared_store());
  const std::vector<std::string> one{"1000"};
  for (const fs::path &out :
       {shared_store(), shared_store() / "derived", shared_store() / "a" / "b"}) {
    auto r = build_state_store_from_spiderrock(spec, one, options_of(out));
    ASSERT_FALSE(r.has_value()) << out.string();
    EXPECT_EQ(r.error().status, PersistStatus::InvalidArgument);
  }
#if defined(_WIN32)
  std::string upper = (shared_store() / "derived").string();
  std::transform(upper.begin(), upper.end(), upper.begin(),
                 [](char c) { return static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c); });
  EXPECT_EQ(build_state_store_from_spiderrock(spec, one, options_of(upper)).error().status,
            PersistStatus::InvalidArgument)
      << "Windows paths compare case-insensitively";
#endif
  EXPECT_FALSE(fs::exists(shared_store() / "derived"));
  StateStoreBuildOptions no_version = options_of(fresh_dir("ok"));
  no_version.conventions_version = 0;
  EXPECT_EQ(build_state_store_from_spiderrock(spec, one, no_version).error().status,
            PersistStatus::InvalidArgument);
  StateStoreBuildOptions sibling = options_of(fs::path(shared_store().string() + "-derived"));
  EXPECT_TRUE(build_state_store_from_spiderrock(spec, one, sibling).has_value())
      << "a sibling whose name merely starts with the root's is outside it";
}

// Ruling 2026-10-24: Full verification up to kMarketStateFullVerifyMaxBytes, Framing above
// it (Lazy would cost what Full costs: the state build touches every section at open).
TEST(PersistStateStore, DefaultVerifyIsFullUpToThresholdFramingAbove) {
  static_assert(market_state_open_options(kMarketStateFullVerifyMaxBytes).verify ==
                PersistVerify::Full);
  static_assert(market_state_open_options(kMarketStateFullVerifyMaxBytes + 1).verify ==
                PersistVerify::Framing);
  EXPECT_TRUE(market_state_open_options(kMarketStateFullVerifyMaxBytes + 1).mapped);

  // Applied by default: a small archive with one flipped payload byte (its stored CRC left
  // alone) is refused by the default opens and served by an explicit Framing open.
  const SpiderRockStoreSpec spec = spec_of(shared_store());
  const fs::path out = fresh_dir("policy");
  const std::vector<std::string> one{"1000"};
  auto built = build_state_store_from_spiderrock(spec, one, options_of(out));
  ASSERT_TRUE(built.has_value());
  const std::string path = built->front().path;
  const StateStore store = StateStore::open(out.string()).value();
  std::uint64_t at = 0;
  {
    auto a = store.open_bucket(spec.date, "1000");
    ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
    ASSERT_LE(a->file().header().file_size, kMarketStateFullVerifyMaxBytes);
    const PersistSectionEntry *e =
        a->file().entry(static_cast<std::uint32_t>(MarketStateSection::Sigma));
    ASSERT_NE(e, nullptr);
    ASSERT_GT(e->length, 0u);
    at = e->offset; // the low byte of the first sigma: still a valid positive vol
  }
  {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(f.good());
    char c = 0;
    f.seekg(static_cast<std::streamoff>(at));
    f.read(&c, 1);
    c = static_cast<char>(c ^ 0x01);
    f.seekp(static_cast<std::streamoff>(at));
    f.write(&c, 1);
  }
  auto by_store = store.open_bucket(spec.date, "1000");
  ASSERT_FALSE(by_store.has_value());
  EXPECT_EQ(by_store.error().status, PersistStatus::SectionChecksum);
  auto by_path = MarketStateArchive::open(path);
  ASSERT_FALSE(by_path.has_value());
  EXPECT_EQ(by_path.error().status, PersistStatus::SectionChecksum);
  PersistOpenOptions framing = kMarketStateOpenDefaults;
  framing.verify = PersistVerify::Framing;
  auto lax = store.open_bucket(spec.date, "1000", framing);
  ASSERT_TRUE(lax.has_value()) << to_string(lax.error().status);
  EXPECT_FALSE(lax->file().verify_all().has_value());
}

// Review 2: the build's skip check verifies Full at ANY size (it is off the latency path:
// ~50 ms against ~1.8 s for the load it saves), so a published bucket whose payload was
// damaged is rebuilt even though its source fingerprint still matches. The damage here is
// payload-only (a Framing open serves it); the repair is byte-identical to the original.
TEST(PersistStateStore, PayloadDamageWithMatchingFingerprintIsRebuilt) {
  const SpiderRockStoreSpec spec = spec_of(shared_store());
  const fs::path out = fresh_dir("repair");
  const std::vector<std::string> one{"1000"};
  auto first = build_state_store_from_spiderrock(spec, one, options_of(out));
  ASSERT_TRUE(first.has_value());
  const std::string path = first->front().path;
  const std::vector<std::byte> pristine = slurp(path);
  PersistOpenOptions framing = kMarketStateOpenDefaults;
  framing.verify = PersistVerify::Framing;
  std::uint64_t at = 0;
  {
    auto a = MarketStateArchive::open(path, framing);
    ASSERT_TRUE(a.has_value());
    const PersistSectionEntry *e =
        a->file().entry(static_cast<std::uint32_t>(MarketStateSection::Sigma));
    ASSERT_NE(e, nullptr);
    at = e->offset;
  }
  {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(f.good());
    char c = 0;
    f.seekg(static_cast<std::streamoff>(at));
    f.read(&c, 1);
    c = static_cast<char>(c ^ 0x01);
    f.seekp(static_cast<std::streamoff>(at));
    f.write(&c, 1);
  }
  ASSERT_TRUE(MarketStateArchive::open(path, framing).has_value()) << "damage must be payload-only";
  auto second = build_state_store_from_spiderrock(spec, one, options_of(out));
  ASSERT_TRUE(second.has_value());
  EXPECT_FALSE(second->front().skipped) << "a payload-damaged bucket was skipped";
  EXPECT_NE(second->front().path, path) << "the repair is a new generation";
  EXPECT_TRUE(slurp(second->front().path) == pristine) << "the repair differs from the first build";
  auto healed = StateStore::open(out.string()).value().open_bucket(spec.date, "1000");
  ASSERT_TRUE(healed.has_value()) << to_string(healed.error().status);
}

TEST(PersistStateStore, SelectionBucketFeedsBenchBookSelector) {
  const SpiderRockStoreSpec spec = spec_of(shared_store());
  const fs::path out = fresh_dir("sel");
  const std::vector<std::string> one{"1000"};
  ASSERT_TRUE(build_state_store_from_spiderrock(spec, one, options_of(out)).has_value());
  auto archive = StateStore::open(out.string()).value().open_bucket(spec.date, "1000");
  ASSERT_TRUE(archive.has_value());
  auto selection = selection_bucket_from_archive(*archive);
  ASSERT_TRUE(selection.has_value()) << selection.error().to_string();
  UnderlierTable table;
  const SpiderRockBucket parquet = load_spiderrock_bucket(spec, "1000", table).value();
  // The synthetic store drops no row, so the archive's rows are the loader's rows.
  ASSERT_EQ(selection->size(), parquet.size());
  EXPECT_EQ(selection->uid, parquet.uid);
  EXPECT_TRUE(same(std::span<const double>{selection->bid}, std::span<const double>{parquet.bid}));
  EXPECT_TRUE(same(std::span<const double>{selection->vendor_delta},
                   std::span<const double>{parquet.vendor_delta}));
  EXPECT_TRUE(selection->rate.empty() && selection->sdiv.empty() && selection->ddiv.empty());
  riskbench::RiskBenchArgs args;
  args.underliers = 4;
  args.options = 60;
  args.shares = 0;
  std::mt19937_64 rng_a(17);
  std::mt19937_64 rng_b(17);
  auto from_archive = riskbench::select_bench_book(*selection, args, rng_a);
  auto from_parquet = riskbench::select_bench_book(parquet, args, rng_b);
  ASSERT_TRUE(from_archive.has_value() && from_parquet.has_value());
  EXPECT_EQ(from_archive->options, from_parquet->options);
  EXPECT_EQ(from_archive->uids, from_parquet->uids);
  ASSERT_FALSE(from_archive->options.empty());
  for (const OptionLeg &leg : from_archive->options) {
    const ResolvedContract rc = archive->state().resolve(leg.uid, leg.K, leg.expiry_ns, leg.side);
    EXPECT_EQ(rc.status, RiskLaneStatus::Ok) << leg.id;
  }
}

// ── PersistStateStoreReal (licensed, env-gated) ──────────────────────────────

namespace {

struct RealStore {
  SpiderRockStoreSpec spec;
};

[[nodiscard]] std::optional<RealStore> real_store() {
  const auto root = env("ATX_VOL_SPIDERROCK_STORE");
  if (!root) return std::nullopt;
  RealStore r;
  r.spec.store_root = *root;
  r.spec.date = env("ATX_VOL_SPIDERROCK_DATE").value_or("2026-10-14");
  r.spec.max_underliers = 100; // the bench's top-100 universe
  return r;
}

} // namespace

TEST(PersistStateStoreReal, Bucket1000ArchiveEqualsParquetState) {
  const auto real = real_store();
  if (!real) GTEST_SKIP() << "ATX_VOL_SPIDERROCK_STORE not set (licensed store)";
  const fs::path out = fresh_dir("real1000");
  const std::vector<std::string> one{"1000"};
  auto built = build_state_store_from_spiderrock(real->spec, one, options_of(out, 1));
  ASSERT_TRUE(built.has_value()) << to_string(built.error().status);
  auto archive = StateStore::open(out.string()).value().open_bucket(real->spec.date, "1000");
  ASSERT_TRUE(archive.has_value()) << to_string(archive.error().status);
  UnderlierTable table;
  const SpiderRockBucket bucket = load_spiderrock_bucket(real->spec, "1000", table).value();
  BucketStateReport report;
  const MarketStateStorage mem = market_state_from_bucket(bucket, real->spec.time, &report).value();
  expect_state_equals(archive->state(), *archive, mem, table);
  EXPECT_EQ(archive->report().n_marks, report.n_marks);
  EXPECT_GT(archive->n_marks(), 100000u);
  std::printf("[persist-real] bucket 1000: %zu marks, %zu bytes, load %.3f s encode %.3f s "
              "publish %.3f s\n",
              (*built)[0].marks, (*built)[0].bytes, (*built)[0].load_s, (*built)[0].encode_s,
              (*built)[0].publish_s);
}

TEST(PersistStateStoreReal, ElevenBucketExplainTotalsBitEqual) {
  const auto real = real_store();
  if (!real) GTEST_SKIP() << "ATX_VOL_SPIDERROCK_STORE not set (licensed store)";
  const std::vector<std::string> buckets{"1000", "1030", "1100", "1130", "1200", "1230",
                                         "1300", "1330", "1400", "1430", "1500"};
  const fs::path out = fresh_dir("real11");
  auto built = build_state_store_from_spiderrock(real->spec, buckets, options_of(out, 2));
  ASSERT_TRUE(built.has_value()) << to_string(built.error().status);
  auto store = StateStore::open(out.string());
  ASSERT_TRUE(store.has_value());
  // The bench's path: ONE table, buckets loaded in order, the book drawn on bucket 1000.
  UnderlierTable table;
  struct Loaded {
    SpiderRockBucket bucket;
    MarketStateStorage storage;
    MarketState state;
  };
  auto load = [&](const std::string &b) {
    auto l = std::make_unique<Loaded>();
    l->bucket = load_spiderrock_bucket(real->spec, b, table).value();
    l->storage = market_state_from_bucket(l->bucket, real->spec.time).value();
    l->state = make_market_state(l->storage).value();
    return l;
  };
  std::unique_ptr<Loaded> prev = load(buckets[0]);
  riskbench::RiskBenchArgs args;
  args.options = 2000;
  args.shares = 50;
  std::mt19937_64 rng(args.seed);
  const riskbench::BenchBook picked = riskbench::select_bench_book(prev->bucket, args, rng).value();
  const RiskBook book = RiskBook::create(picked.options, picked.shares).value();
  RiskStream from_mem(book, RiskKernel::family());
  RiskStream from_arc(book, RiskKernel::family());
  auto prev_arc = store->open_bucket(real->spec.date, buckets[0]);
  ASSERT_TRUE(prev_arc.has_value());
  expect_state_equals(prev_arc->state(), *prev_arc, prev->storage, table);
  ASSERT_TRUE(from_mem.start(prev->state).has_value());
  ASSERT_TRUE(from_arc.start(prev_arc->state()).has_value());
  for (std::size_t k = 1; k < buckets.size(); ++k) {
    std::unique_ptr<Loaded> cur = load(buckets[k]);
    auto cur_arc = store->open_bucket(real->spec.date, buckets[k]);
    ASSERT_TRUE(cur_arc.has_value()) << buckets[k];
    expect_state_equals(cur_arc->state(), *cur_arc, cur->storage, table);
    auto x = from_mem.step(cur->state);
    auto y = from_arc.step(cur_arc->state());
    ASSERT_TRUE(x.has_value() && y.has_value()) << buckets[k];
    EXPECT_EQ(std::memcmp(&(*x)->total, &(*y)->total, sizeof(ExplainTotals)), 0)
        << "explain totals " << buckets[k - 1] << " -> " << buckets[k];
    EXPECT_TRUE(same(std::span<const double>{(*x)->pnl_total},
                     std::span<const double>{(*y)->pnl_total}));
    std::printf("[persist-real] %s->%s pnl_total %.6f unexplained %.6f n_ok %u (bit-equal)\n",
                buckets[k - 1].c_str(), buckets[k].c_str(), (*y)->total.pnl_total,
                (*y)->total.pnl_unexplained, (*y)->total.n_ok);
    // Two rolling archives / states: k-1 is released only after step(k) returned.
    prev = std::move(cur);
    prev_arc = std::move(cur_arc);
  }
}
