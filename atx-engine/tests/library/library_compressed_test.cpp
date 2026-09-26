// W1-A5 postimplementation contracts. Synthetic, bounded; no 10k timing claim.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/library/library.hpp"
#include "atx/engine/library/record.hpp"
#include "atx/engine/library/store.hpp"

namespace atx_test_w1_a5_compressed {
namespace lib = atx::engine::library;
using atx::f64;
using atx::engine::combine::AlphaId;

std::string directory() {
  const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
  const auto path = std::filesystem::temp_directory_path() / "atx_w1_a5" / test->name();
  std::error_code error;
  std::filesystem::remove_all(path, error);
  std::filesystem::create_directories(path, error);
  return path.string();
}
lib::LibraryStorageOptions compact_options(atx::usize instruments = 3) {
  lib::LibraryStorageOptions options;
  options.rule = lib::LibraryStorageRule::CompressedV2;
  options.instruments = instruments;
  return options;
}
lib::AlphaMetadata metadata() {
  return {"reversal", "price", 21, 12.5, "sector-v2", 987, 654, 321, 1.0 / 32767.0};
}
atx::engine::combine::AlphaMetrics metrics() {
  atx::engine::combine::AlphaMetrics m{};
  m.fitness = 2.0; m.sharpe = 2.0; m.turnover = 0.1;
  return m;
}
lib::CompressedAlphaRecord record(atx::usize periods = 4) {
  lib::CompressedAlphaRecord result;
  result.canon_hash = 77; result.metrics = metrics(); result.metadata = metadata();
  result.provenance = {"rank(close)", {12, 34}, 5, 56};
  result.pnl.resize(periods);
  for (atx::usize i = 0; i < periods; ++i)
    result.pnl[i] = static_cast<atx::f32>(static_cast<f64>(i) * 0.001);
  result.signal_sketch = {-32768, -32767, 0, 32767};
  return result;
}
void reseal(std::vector<std::byte>& bytes) {
  lib::SegmentHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  lib::SegmentFooter footer{lib::kLibSealMarker,
      atx::tsdb::crc32(bytes.data(), static_cast<atx::usize>(header.off_footer)), 0U};
  std::memcpy(bytes.data() + header.off_footer, &footer, sizeof(footer));
}

TEST(LibraryCompressed, RecordRoundTripUsesFloatPnlAndBoundedSketchWithoutDensePositions) {
  auto original = record(5000);
  original.pnl[17] = std::numeric_limits<atx::f32>::quiet_NaN();
  const std::array rows{original};
  const auto bytes = lib::write_compressed_segment_bytes(100000, 5000, 0, rows);
  ASSERT_TRUE(bytes.has_value());
  EXPECT_LT(bytes->size(), 1U << 20U); // one T5000 record, independent of instrument count
  auto reader = lib::SegmentReaderLite::attach_bytes(*bytes);
  ASSERT_TRUE(reader.has_value());
  EXPECT_EQ(reader->format_version(), 2U);
  EXPECT_EQ(reader->metadata(0), original.metadata);
  EXPECT_EQ(reader->provenance(0).expr_source, "rank(close)");
  EXPECT_EQ(reader->provenance(0).parent_hashes, original.provenance.parent_hashes);
  EXPECT_EQ(reader->provenance(0).mutation_op, 5U);
  const auto sketch = reader->signal_sketch(0);
  EXPECT_TRUE(std::equal(sketch.begin(), sketch.end(), original.signal_sketch.begin(),
                         original.signal_sketch.end()));
  const auto pnl = reader->pnl_row(0);
  ASSERT_EQ(pnl.size(), 5000U);
  EXPECT_TRUE(std::isnan(pnl[17]));
  EXPECT_DOUBLE_EQ(pnl[234], static_cast<f64>(original.pnl[234]));
  const auto compact = reader->pnl_f32_row(0);
  EXPECT_EQ(std::bit_cast<atx::u32>(compact[234]), std::bit_cast<atx::u32>(original.pnl[234]));
}

TEST(LibraryCompressed, ResealedMalformedGeometryAndProvenanceAreRejectedBeforeViews) {
  const std::array rows{record()};
  auto source = lib::write_compressed_segment_bytes(3, 4, 0, rows);
  ASSERT_TRUE(source.has_value());
  auto corrupt = *source;
  lib::SegmentHeader header{};
  std::memcpy(&header, corrupt.data(), sizeof(header));
  const auto original_header = header;
  header.n_periods = std::numeric_limits<atx::u64>::max();
  std::memcpy(corrupt.data(), &header, sizeof(header)); reseal(corrupt);
  EXPECT_FALSE(lib::SegmentReaderLite::attach_bytes(corrupt).has_value());

  corrupt = *source;
  lib::AlphaDirEntry entry{};
  std::memcpy(&entry, corrupt.data() + original_header.off_dir, sizeof(entry));
  entry.pad_ = 257;
  std::memcpy(corrupt.data() + original_header.off_dir, &entry, sizeof(entry)); reseal(corrupt);
  EXPECT_FALSE(lib::SegmentReaderLite::attach_bytes(corrupt).has_value());

  corrupt = *source;
  const auto impossible_length = std::numeric_limits<atx::u64>::max();
  std::memcpy(corrupt.data() + original_header.off_prov, &impossible_length, sizeof(impossible_length));
  reseal(corrupt);
  EXPECT_FALSE(lib::SegmentReaderLite::attach_bytes(corrupt).has_value());

  corrupt = *source;
  const auto infinity = std::numeric_limits<atx::f32>::infinity();
  std::memcpy(corrupt.data() + original_header.off_pnl, &infinity, sizeof(infinity)); reseal(corrupt);
  EXPECT_FALSE(lib::SegmentReaderLite::attach_bytes(corrupt).has_value());
}

TEST(LibraryCompressed, MissingResolverFailsAndBoundRecipeRecomputesOnlyRequestedRow) {
  const auto dir = directory();
  const std::vector<f64> pnl{0.0, 0.123456789, -0.25, 0.0625};
  const std::array<atx::i16, 3> sketch{-120, 0, 120};
  {
    lib::LibraryStore store(dir, compact_options());
    ASSERT_TRUE(store.stage(nullptr, pnl, {}, metrics(), {"close"}, 7, metadata(), sketch));
    EXPECT_DOUBLE_EQ(store.pnl(AlphaId{0})[1], static_cast<f64>(static_cast<atx::f32>(pnl[1])));
    EXPECT_FALSE(store.positions_checked(AlphaId{0}, 1).has_value());
    ASSERT_TRUE(store.flush());
  }
  lib::LibraryStore reopened(dir); // persisted V2 policy, no implicit downgrade
  EXPECT_EQ(reopened.storage_rule(), lib::LibraryStorageRule::CompressedV2);
  EXPECT_EQ(reopened.get(AlphaId{0}).metadata, metadata());
  EXPECT_EQ(reopened.signal_sketch(AlphaId{0}).size(), 3U);
  int calls = 0;
  reopened.set_position_resolver([&calls](const lib::PositionRequest& request)
      -> atx::core::Result<std::vector<f64>> {
    ++calls;
    if (request.metadata.context_hash != 987 || request.metadata.position_recipe_hash != 654 ||
        request.provenance.expr_source != "close")
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "fixture recipe mismatch");
    return atx::core::Ok(std::vector<f64>(request.instruments, static_cast<f64>(request.period)));
  });
  EXPECT_FALSE(reopened.positions_checked(AlphaId{0}, 4));
  EXPECT_EQ(calls, 0);
  const auto positions = reopened.positions_checked(AlphaId{0}, 2);
  ASSERT_TRUE(positions); EXPECT_EQ(calls, 1);
  EXPECT_EQ(*positions, (std::vector<f64>{2, 2, 2}));
  reopened.set_position_resolver([](const lib::PositionRequest&) {
    return atx::core::Ok(std::vector<f64>{1, 2});
  });
  EXPECT_FALSE(reopened.positions_checked(AlphaId{0}, 1));
}

TEST(LibraryCompressed, AppendPeriodsAndLaterAdmissionsSurviveReopenWithoutRewritingBase) {
  const auto dir = directory();
  atx::u32 original_crc = 0, extended_crc = 0;
  {
    lib::LibraryStore store(dir, compact_options(0));
    const std::array<f64, 4> first{1, 2, 3, 4}, second{-1, -2, -3, -4};
    ASSERT_TRUE(store.stage(nullptr, first, {}, metrics(), {"first"}, 1));
    ASSERT_TRUE(store.stage(nullptr, second, {}, metrics(), {"second"}, 2));
    ASSERT_TRUE(store.bind_index_recipe(2, 42));
    ASSERT_TRUE(store.flush());
    auto original = lib::SegmentReaderLite::attach(store.segment_path(0));
    ASSERT_TRUE(original); original_crc = original->integrity_crc();
    const auto before = store.record_crc(AlphaId{0});
    const std::array<f64, 2> tail{5, -5};
    ASSERT_TRUE(store.append_periods(tail, 1));
    EXPECT_NE(store.record_crc(AlphaId{0}), before);
    const std::array<f64, 5> later{10, 20, 30, 40, 50};
    ASSERT_TRUE(store.stage(nullptr, later, {}, metrics(), {"later"}, 3));
    const std::array<f64, 6> more{6, 7, -6, -7, 60, 70};
    ASSERT_TRUE(store.append_periods(more, 2));
    extended_crc = store.record_crc(AlphaId{0});
    EXPECT_EQ(store.n_periods(), 7U);
    EXPECT_EQ(store.pnl(AlphaId{2})[6], 70.0);
    auto unchanged = lib::SegmentReaderLite::attach(store.segment_path(0));
    ASSERT_TRUE(unchanged); EXPECT_EQ(unchanged->integrity_crc(), original_crc);
  }
  lib::LibraryStore reopened(dir);
  ASSERT_TRUE(reopened.bind_index_recipe(0, 42));
  EXPECT_EQ(reopened.n_periods(), 7U); EXPECT_EQ(reopened.n_alphas(), 3U);
  EXPECT_EQ(reopened.record_crc(AlphaId{0}), extended_crc);
  for (atx::u32 a = 0; a < 3; ++a) {
    const auto values = reopened.pnl(AlphaId{a});
    ASSERT_EQ(values.size(), 7U);
    const f64 multiplier = a == 0 ? 1.0 : (a == 1 ? -1.0 : 10.0);
    for (atx::usize t = 0; t < values.size(); ++t)
      EXPECT_DOUBLE_EQ(values[t], multiplier * static_cast<f64>(t + 1));
  }
}

TEST(LibraryCompressed, InvalidPayloadAndShapeLeaveStoreUnchanged) {
  lib::LibraryStore store(directory(), compact_options(3));
  const std::array<f64, 2> valid{0.0, 0.25};
  const std::array<f64, 2> invalid{0.0, std::numeric_limits<f64>::max()};
  EXPECT_FALSE(store.stage(nullptr, invalid, {}, metrics(), {}));
  EXPECT_EQ(store.n_alphas(), 0U); EXPECT_EQ(store.n_periods(), 0U);
  const std::array<f64, 3> ragged{0, 0, 0};
  EXPECT_FALSE(store.stage(nullptr, valid, ragged, metrics(), {}));
  ASSERT_TRUE(store.stage(nullptr, valid, {}, metrics(), {}));
  const std::array<f64, 1> short_history{0.0};
  EXPECT_FALSE(store.stage(nullptr, short_history, {}, metrics(), {}));
  EXPECT_FALSE(store.append_periods(ragged, 2));
  EXPECT_FALSE(store.append_periods(invalid, 2));
  EXPECT_EQ(store.n_alphas(), 1U); EXPECT_EQ(store.n_periods(), 2U);
  EXPECT_FALSE(store.positions_checked(AlphaId{0}, 0)); // missing bound metadata, no invented weights
}

TEST(LibraryCompressed, IndexRecipeIsDurableAndMigrationChangesIdentity) {
  const auto dir = directory();
  {
    lib::LibraryStore legacy(dir);
    const std::array<f64, 4> pnl{1, -1, 2, -2};
    ASSERT_TRUE(legacy.stage(nullptr, pnl, {}, metrics(), {"legacy"}));
    ASSERT_TRUE(legacy.flush());
    auto adopted = legacy.bind_index_recipe(0, 91);
    ASSERT_TRUE(adopted); EXPECT_EQ(*adopted, 1U); // metadata-less populated V1 keeps its rule
    const auto old_identity = legacy.record_crc(AlphaId{0});
    EXPECT_FALSE(legacy.bind_index_recipe(2, 91));
    EXPECT_FALSE(legacy.bind_index_recipe(1, 92));
    ASSERT_TRUE(legacy.bind_index_recipe(2, 91, true));
    EXPECT_NE(legacy.record_crc(AlphaId{0}), old_identity);
  }
  lib::LibraryStore reopened(dir);
  auto adopted = reopened.bind_index_recipe(0, 91);
  ASSERT_TRUE(adopted); EXPECT_EQ(*adopted, 2U);
  EXPECT_FALSE(reopened.bind_index_recipe(0, 92));
}

TEST(LibraryCompressed, FacadeRebuildsIndexAndBindsManifestAfterPeriodAppend) {
  const auto dir = directory();
  atx::engine::combine::GateConfig config;
  atx::engine::combine::AlphaGate gate{config};
  atx::u64 version = 0;
  {
    auto library = lib::Library::open(dir, config, {42}, lib::CorrIndexRule::ExistingOrSignedV2,
                                      compact_options(0));
    const std::array<f64, 4> pnl{0.125, -0.25, 0.375, -0.5};
    lib::AlphaCandidate candidate{11, pnl, {}, metrics(), {"first"}, 3};
    candidate.metadata = metadata();
    auto admitted = library.try_admit(candidate, gate);
    ASSERT_TRUE(admitted); EXPECT_EQ(admitted->kind, lib::AdmitKind::Accept);
    const auto before = library.snapshot();
    const std::array<f64, 2> tail{0.625, -0.75};
    ASSERT_TRUE(library.append_periods(tail, 2));
    const std::array<f64, 6> inverse{-0.125, 0.25, -0.375, 0.5, -0.625, 0.75};
    EXPECT_NEAR(library.worst_corr_to_pool(inverse, 0.7), 1.0, 1e-12);
    candidate.canon_hash = 12; candidate.pnl = inverse;
    const auto rejected = library.try_admit(candidate, gate);
    ASSERT_TRUE(rejected); EXPECT_EQ(rejected->kind, lib::AdmitKind::RejectCorrelated);
    const auto after = library.snapshot();
    EXPECT_NE(after.version_id, before.version_id); version = after.version_id;
  }
  auto reopened = lib::Library::open(dir, config, {42});
  EXPECT_EQ(reopened.storage_rule(), lib::LibraryStorageRule::CompressedV2);
  EXPECT_EQ(reopened.corr_rule(), lib::CorrIndexRule::SignedHammingV2);
  EXPECT_EQ(reopened.n_periods(), 6U);
  EXPECT_EQ(reopened.snapshot().version_id, version);
}

TEST(LibraryCompressed, FailedFirstAdmissionDoesNotFixPeriodGeometry) {
  atx::engine::combine::GateConfig config;
  const atx::engine::combine::AlphaGate gate{config};
  auto library = lib::Library::open(directory(), config, {29},
      lib::CorrIndexRule::ExistingOrSignedV2, compact_options(3));
  const std::array<f64, 4> bad_pnl{1, 2, 3, std::numeric_limits<f64>::max()};
  lib::AlphaCandidate candidate{12, bad_pnl, {}, metrics(), {"first"}, 3};
  EXPECT_FALSE(library.try_admit(candidate, gate));
  EXPECT_EQ(library.n_periods(), 0U);
  const std::array<f64, 3> good_pnl{0.125, -0.25, 0.375};
  candidate.pnl = good_pnl;
  const auto admitted = library.try_admit(candidate, gate);
  ASSERT_TRUE(admitted);
  EXPECT_EQ(admitted->kind, lib::AdmitKind::Accept);
  EXPECT_EQ(library.n_periods(), 3U);
}

TEST(LibraryCompressed, ContinuousRefinementScoresModerateCorrelationExactlyForSmallPool) {
  lib::LibraryStore store(directory());
  const std::array<f64, 4> base{1, -1, 1, -1};
  ASSERT_TRUE(store.stage(nullptr, base, {}, metrics(), {}));
  lib::CorrNeighborIndex index(33, 4, 64);
  index.add(AlphaId{0}, base);
  const f64 a = 0.3, b = std::sqrt(1.0 - a * a);
  const std::array<f64, 4> query{a + b, -a + b, a - b, -a - b};
  EXPECT_NEAR(lib::online_corr_to_pool(query, store, index, 0.7, true), 0.3, 1e-12);
}
} // namespace atx_test_w1_a5_compressed
