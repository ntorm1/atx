#include "atx/vol/backtest_db.hpp"
#include "atx/vol/research_db.hpp"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {

[[nodiscard]] std::filesystem::path test_root(std::string_view stem) {
  static std::atomic<std::uint64_t> sequence{0};
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("atx_research_db_" + std::string(stem) + "_" + std::to_string(sequence.fetch_add(1)));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  return root;
}

struct CandidatePayload {
  std::vector<std::uint32_t> metric_codes{0u, 1u};
  std::vector<std::string> metric_names{"sharpe", "max_drawdown"};
  std::vector<double> values{1.25, -0.18};
  RaSectionData section{};

  CandidatePayload() {
    section.name = "candidate_scores";
    section.kind = RaSectionKind::SubTable;
    section.n_rows = values.size();
    section.columns.emplace_back("metric",
                                 RaColumnData::of_dict(std::span<const std::uint32_t>(metric_codes),
                                                       std::span<const std::string>(metric_names)));
    section.columns.emplace_back("value", RaColumnData::of_f64(std::span<const double>(values)));
  }
};

struct SignalPayload {
  std::vector<std::int64_t> event_ts{100, 200};
  std::vector<std::int64_t> available_ts{110, 210};
  std::vector<std::uint32_t> uids{101u, 101u};
  std::vector<std::uint32_t> symbol_codes{0u, 0u};
  std::vector<std::string> symbols{"AAPL"};
  std::vector<double> values{0.25, -0.50};
  std::vector<std::uint32_t> status{0u, 0u};
  RaSectionData section{};

  SignalPayload() {
    section.name = "signal_values";
    section.kind = RaSectionKind::TimeSeries;
    section.n_rows = values.size();
    section.columns.emplace_back("event_ts_ns",
                                 RaColumnData::of_i64(std::span<const std::int64_t>(event_ts)));
    section.columns.emplace_back("available_ts_ns",
                                 RaColumnData::of_i64(std::span<const std::int64_t>(available_ts)));
    section.columns.emplace_back("uid", RaColumnData::of_u32(std::span<const std::uint32_t>(uids)));
    section.columns.emplace_back("symbol",
                                 RaColumnData::of_dict(std::span<const std::uint32_t>(symbol_codes),
                                                       std::span<const std::string>(symbols)));
    section.columns.emplace_back("value", RaColumnData::of_f64(std::span<const double>(values)));
    section.columns.emplace_back("status",
                                 RaColumnData::of_u32(std::span<const std::uint32_t>(status)));
  }
};

[[nodiscard]] ResearchPublishRequest candidate_request(std::string expected_head_id = {}) {
  ResearchPublishRequest request;
  request.kind = ResearchArtifactKind::Candidate;
  request.logical_id = "dispersion/candidate/001";
  request.expected_head_id = std::move(expected_head_id);
  request.payload_schema_salt = 0x43414E4449440001ULL;
  request.parameters = {
      ResearchParameter::f64("sizing", "target_vega", 10'000.0),
      ResearchParameter::text("universe", "index", "SPX"),
      ResearchParameter::u32("execution", "entry_every_n", 5u),
  };
  return request;
}

TEST(ResearchDbIdentity, ParameterOrderDoesNotAffectCanonicalSha256) {
  ResearchPublishRequest first = candidate_request();
  ResearchPublishRequest reordered = first;
  std::swap(reordered.parameters[0], reordered.parameters[2]);

  auto first_id = research_artifact_identity(first);
  auto reordered_id = research_artifact_identity(reordered);
  ASSERT_TRUE(first_id.has_value()) << (first_id ? "" : first_id.error().to_string());
  ASSERT_TRUE(reordered_id.has_value()) << (reordered_id ? "" : reordered_id.error().to_string());
  EXPECT_EQ(*first_id, *reordered_id);
  EXPECT_EQ(first_id->size(), 64u);

  reordered.parameters[0] = ResearchParameter::u32("execution", "entry_every_n", 6u);
  auto changed_id = research_artifact_identity(reordered);
  ASSERT_TRUE(changed_id.has_value());
  EXPECT_NE(*changed_id, *first_id);
}

TEST(ResearchDbIdentity, SignedZeroIsCanonicalForParametersAndPayload) {
  CandidatePayload positive_payload;
  positive_payload.values[0] = 0.0;
  ResearchPublishRequest positive = candidate_request();
  positive.parameters[0] = ResearchParameter::f64("sizing", "target_vega", 0.0);
  positive.sections.push_back(positive_payload.section);

  CandidatePayload negative_payload;
  negative_payload.values[0] = -0.0;
  ResearchPublishRequest negative = candidate_request();
  negative.parameters[0] = ResearchParameter::f64("sizing", "target_vega", -0.0);
  negative.sections.push_back(negative_payload.section);

  auto positive_id = research_artifact_identity(positive);
  auto negative_id = research_artifact_identity(negative);
  ASSERT_TRUE(positive_id.has_value());
  ASSERT_TRUE(negative_id.has_value());
  EXPECT_EQ(*positive_id, *negative_id);

  const std::filesystem::path root = test_root("signed_zero");
  auto db = ResearchDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  auto published = db->publish(negative);
  ASSERT_TRUE(published.has_value());
  auto scores = db->map_section(published->artifact_id, "candidate_scores");
  ASSERT_TRUE(scores.has_value());
  EXPECT_FALSE(std::signbit(scores->view.f64_col("value")[0]));
  auto parameters = db->map_section(published->artifact_id, "parameters");
  ASSERT_TRUE(parameters.has_value());
  EXPECT_FALSE(std::signbit(parameters->view.f64_col("f64_value")[1]));
  scores->view = {};
  scores->archive.reset();
  parameters->view = {};
  parameters->archive.reset();
  std::filesystem::remove_all(root);
}

TEST(ResearchDbIdentity, NonFinitePayloadValuesAreRejected) {
  for (const double invalid :
       {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN()}) {
    CandidatePayload payload;
    payload.values[0] = invalid;
    ResearchPublishRequest request = candidate_request();
    request.sections.push_back(payload.section);
    const auto identity = research_artifact_identity(request);
    ASSERT_FALSE(identity.has_value());
    EXPECT_EQ(identity.error().code(), ErrorCode::InvalidArgument);
  }

  const std::filesystem::path root = test_root("non_finite");
  auto db = ResearchDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  CandidatePayload payload;
  payload.values[0] = std::numeric_limits<double>::infinity();
  ResearchPublishRequest request = candidate_request();
  request.sections.push_back(payload.section);
  const auto rejected = db->publish(request);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(db->generation(), 1u);
  EXPECT_TRUE(db->artifacts().empty());
  EXPECT_TRUE(db->heads().empty());
  std::filesystem::remove_all(root);
}

TEST(ResearchDbStore, PublishRoundTripAndMappedSectionOwnsArchiveLifetime) {
  const std::filesystem::path root = test_root("roundtrip");
  CandidatePayload payload;
  ResearchPublishRequest request = candidate_request();
  request.sections.push_back(payload.section);

  MappedResearchSection mapped;
  std::string artifact_id;
  {
    auto db = ResearchDb::create(root.string());
    ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
    EXPECT_EQ(db->generation(), 1u);

    auto published = db->publish(request);
    ASSERT_TRUE(published.has_value()) << (published ? "" : published.error().to_string());
    artifact_id = published->artifact_id;
    EXPECT_EQ(published->kind, ResearchArtifactKind::Candidate);
    EXPECT_EQ(published->logical_id, request.logical_id);
    EXPECT_EQ(published->revision, 1u);

    auto head = db->find_head(ResearchArtifactKind::Candidate, request.logical_id);
    ASSERT_TRUE(head.has_value());
    EXPECT_EQ(head->artifact_id, artifact_id);
    auto dependencies = db->load_dependencies(artifact_id);
    ASSERT_TRUE(dependencies.has_value());
    EXPECT_TRUE(dependencies->empty());

    auto opened = db->map_section(artifact_id, "candidate_scores");
    ASSERT_TRUE(opened.has_value()) << (opened ? "" : opened.error().to_string());
    mapped = std::move(*opened);
  }

  EXPECT_EQ(mapped->n_rows(), 2u);
  const auto values = mapped->f64_col("value");
  ASSERT_EQ(values.size(), 2u);
  EXPECT_DOUBLE_EQ(values[0], 1.25);
  EXPECT_DOUBLE_EQ(values[1], -0.18);

  auto reopened = ResearchDb::open(root.string());
  ASSERT_TRUE(reopened.has_value()) << (reopened ? "" : reopened.error().to_string());
  auto info = reopened->find_artifact(artifact_id);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->artifact_id, artifact_id);
  EXPECT_EQ(reopened->heads().size(), 1u);
  mapped.view = {};
  mapped.archive.reset();
  std::filesystem::remove_all(root);
}

TEST(ResearchDbStore, IdenticalReplayIsATrueNoOp) {
  const std::filesystem::path root = test_root("idempotent_replay");
  CandidatePayload payload;
  ResearchPublishRequest request = candidate_request();
  request.sections.push_back(payload.section);
  auto db = ResearchDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  auto first = db->publish(request);
  ASSERT_TRUE(first.has_value());
  const std::uint64_t generation = db->generation();
  auto head_before = db->find_head(ResearchArtifactKind::Candidate, request.logical_id);
  ASSERT_TRUE(head_before.has_value());

  auto replay = db->publish(request);
  ASSERT_TRUE(replay.has_value());
  EXPECT_EQ(*replay, *first);
  EXPECT_EQ(db->generation(), generation);
  EXPECT_EQ(db->artifacts().size(), 1u);
  auto head_after = db->find_head(ResearchArtifactKind::Candidate, request.logical_id);
  ASSERT_TRUE(head_after.has_value());
  EXPECT_EQ(*head_after, *head_before);
  std::filesystem::remove_all(root);
}

TEST(ResearchDbStore, CompanionStoreLeavesBacktestDbV1ManifestUntouched) {
  const std::filesystem::path root = test_root("backtest_coexistence");
  auto backtests = BacktestDb::create(root.string());
  ASSERT_TRUE(backtests.has_value()) << backtests.error().to_string();
  const std::uint64_t generation = backtests->generation();
  const std::filesystem::path manifest = root / std::string(kBacktestDbManifestName);
  std::ifstream before_stream(manifest, std::ios::binary);
  ASSERT_TRUE(before_stream);
  const std::vector<char> before{std::istreambuf_iterator<char>(before_stream),
                                 std::istreambuf_iterator<char>()};
  before_stream.close();

  auto research = ResearchDb::create(root.string());
  ASSERT_TRUE(research.has_value()) << research.error().to_string();
  std::ifstream after_stream(manifest, std::ios::binary);
  ASSERT_TRUE(after_stream);
  const std::vector<char> after{std::istreambuf_iterator<char>(after_stream),
                                std::istreambuf_iterator<char>()};
  after_stream.close();
  EXPECT_EQ(after, before);

  auto reopened = BacktestDb::open(root.string());
  ASSERT_TRUE(reopened.has_value()) << reopened.error().to_string();
  EXPECT_EQ(reopened->generation(), generation);
  EXPECT_TRUE(reopened->templates().empty());
  EXPECT_TRUE(reopened->series().empty());
  std::filesystem::remove_all(root);
}

TEST(ResearchDbStore, StaleExpectedHeadIsRejectedWithoutMovingHead) {
  const std::filesystem::path root = test_root("cas");
  auto db = ResearchDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  ResearchPublishRequest first_request = candidate_request();
  auto first = db->publish(first_request);
  ASSERT_TRUE(first.has_value());

  ResearchPublishRequest second_request = candidate_request(first->artifact_id);
  second_request.parameters[0] = ResearchParameter::f64("sizing", "target_vega", 20'000.0);
  auto second = db->publish(second_request);
  ASSERT_TRUE(second.has_value());
  ASSERT_NE(second->artifact_id, first->artifact_id);

  ResearchPublishRequest stale_request = candidate_request(first->artifact_id);
  stale_request.parameters[0] = ResearchParameter::f64("sizing", "target_vega", 30'000.0);
  const auto stale = db->publish(stale_request);
  ASSERT_FALSE(stale.has_value());
  EXPECT_EQ(stale.error().code(), ErrorCode::Unavailable);

  auto head = db->find_head(ResearchArtifactKind::Candidate, first_request.logical_id);
  ASSERT_TRUE(head.has_value());
  EXPECT_EQ(head->artifact_id, second->artifact_id);
  std::filesystem::remove_all(root);
}

TEST(ResearchDbStore, CorruptedObjectIsRejectedByMappedRead) {
  const std::filesystem::path root = test_root("corrupt");
  CandidatePayload payload;
  ResearchPublishRequest request = candidate_request();
  request.sections.push_back(payload.section);
  auto db = ResearchDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  auto published = db->publish(request);
  ASSERT_TRUE(published.has_value());

  const std::filesystem::path object_path =
      root / kResearchDbDirectory / kResearchDbObjectDirectory / published->filename;
  {
    std::fstream file(object_path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file.is_open());
    file.seekg(-1, std::ios::end);
    char value = 0;
    file.read(&value, 1);
    value ^= static_cast<char>(0x5a);
    file.seekp(-1, std::ios::end);
    file.write(&value, 1);
  }

  const auto mapped = db->map_section(published->artifact_id, "candidate_scores");
  ASSERT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().code(), ErrorCode::ParseError);
  std::filesystem::remove_all(root);
}

TEST(ResearchDbSignals, AvailabilityIsValidatedAndQueriedPointInTime) {
  const std::filesystem::path root = test_root("signal_availability");
  auto db = ResearchDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  SignalPayload invalid_payload;
  invalid_payload.available_ts[1] = 199;
  ResearchPublishRequest invalid;
  invalid.kind = ResearchArtifactKind::SignalSegment;
  invalid.logical_id = "signals/implied-correlation/sp500";
  invalid.payload_schema_salt = 0x5349474E414C0001ULL;
  invalid.first_ts_ns = 100;
  invalid.last_ts_ns = 200;
  invalid.row_count = 2;
  invalid.sections.push_back(invalid_payload.section);
  const auto rejected = db->publish(invalid);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);

  SignalPayload payload;
  ResearchPublishRequest request = invalid;
  request.sections.clear();
  request.sections.push_back(payload.section);
  auto published = db->publish(request);
  ASSERT_TRUE(published.has_value()) << (published ? "" : published.error().to_string());
  auto mapped = db->map_section(published->artifact_id, "signal_values");
  ASSERT_TRUE(mapped.has_value());

  auto available = research_signal_rows_available_as_of(mapped->view, 150);
  ASSERT_TRUE(available.has_value()) << (available ? "" : available.error().to_string());
  ASSERT_EQ(available->size(), 1u);
  EXPECT_EQ((*available)[0], 0u);
  mapped->view = {};
  mapped->archive.reset();
  std::filesystem::remove_all(root);
}

} // namespace
} // namespace atx::vol
