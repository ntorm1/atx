// RunArchive (ATXRUN01) — the binary result container for backtest outputs.
//
// Task 1 (schema): the column registry in run_archive_schema.hpp is the single
// source of truth for every result section. The `backtest` column order is
// load-bearing — it matches `append_backtest_series_tsv` (tearsheet.cpp) and
// `BacktestResult` — and every other section's columns are enumerated from the
// existing TSV writer that owns that output (listed_dispersion_reconciliation,
// listed_dispersion_schedule, the example's mark-divergence replay,
// write_diagnostics). `ra_schema_hash()` folds the whole registry into one
// constexpr FNV-1a-64 value so a header can pin the schema at open time.

#include "atx/vol/run_archive.hpp"
#include "atx/vol/run_archive_schema.hpp"

#include "atx/vol/backtest.hpp"            // BacktestResult (Task 5 encoders)
#include "atx/vol/backtest_series_columns.hpp" // backtest_series_columns() (T6 single source)
#include "atx/vol/corpus.hpp"              // CorpusManifest (RunDir::clock via manifest)
#include "atx/vol/detail/archive_util.hpp" // crc32c (independent CRC check)
#include "atx/vol/dispersion_workflow.hpp" // RunSpec (Task 5 meta encoder)
#include "atx/vol/listed_dispersion.hpp"   // ListedDispersionSelection (RunDir schedule fixture)
#include "atx/vol/listed_dispersion_reconciliation.hpp"
#include "atx/vol/listed_dispersion_schedule.hpp"
#include "atx/vol/surface_archive.hpp" // ArchiveContentIdentity (identity())
#include "atx/vol/tearsheet.hpp"       // write_backtest_tsv (T6 TSV/encoder column parity)

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace atx::vol {
namespace {

const RaSection* find_section(std::string_view name) {
  for (const RaSection& section : ra_sections()) {
    if (section.name == name) return &section;
  }
  return nullptr;
}

TEST(RunArchiveSchema, BacktestSectionPinned) {
  // backtest section present, 27 cols (date + ts_ns + 25 doubles), nav at 16.
  const RaSection* bt = find_section("backtest");
  ASSERT_NE(bt, nullptr);
  EXPECT_EQ(bt->kind, RaSectionKind::TimeSeries);
  ASSERT_EQ(bt->columns.size(), 27u);
  EXPECT_EQ(bt->columns[0].name, "date");
  EXPECT_EQ(bt->columns[0].dtype, RaDType::DictStr);
  EXPECT_EQ(bt->columns[1].name, "ts_ns");
  EXPECT_EQ(bt->columns[1].dtype, RaDType::I64);
  EXPECT_EQ(bt->columns[16].name, "nav");
  EXPECT_EQ(bt->columns[16].dtype, RaDType::F64);
  EXPECT_EQ(bt->columns[26].name, "n_unpriced_greeks");
  // Everything after date/ts_ns is a plain double column.
  for (std::size_t i = 2; i < bt->columns.size(); ++i) {
    EXPECT_EQ(bt->columns[i].dtype, RaDType::F64) << "column " << bt->columns[i].name;
  }
}

TEST(RunArchiveSchema, ProjectedSectionsReuseBacktestColumns) {
  // projected_cold / projected_nodiv share the backtest column array (same
  // pointer, not a copy) so the three sections can never drift.
  const RaSection* bt = find_section("backtest");
  const RaSection* cold = find_section("projected_cold");
  const RaSection* nodiv = find_section("projected_nodiv");
  ASSERT_NE(bt, nullptr);
  ASSERT_NE(cold, nullptr);
  ASSERT_NE(nodiv, nullptr);
  EXPECT_EQ(cold->kind, RaSectionKind::TimeSeries);
  EXPECT_EQ(nodiv->kind, RaSectionKind::TimeSeries);
  EXPECT_EQ(cold->columns.data(), bt->columns.data());
  EXPECT_EQ(nodiv->columns.data(), bt->columns.data());
  EXPECT_EQ(cold->columns.size(), bt->columns.size());
  EXPECT_EQ(nodiv->columns.size(), bt->columns.size());
}

TEST(RunArchiveSchema, WriterOwnedSectionsPinned) {
  // Column counts come from the TSV writers that own each output today:
  //   reconciliation  <- serialize_listed_reconciliation (11 cols)
  //   trade_schedule  <- serialize_listed_dispersion_schedule kHeader (29 cols)
  //   contract_marks  <- serialize_listed_contract_marks (19 cols)
  //   mark_divergence <- write_mark_divergence_replay header (10 cols)
  //   diagnostics     <- write_diagnostics header (4 cols)
  const RaSection* meta = find_section("meta");
  ASSERT_NE(meta, nullptr);
  EXPECT_EQ(meta->kind, RaSectionKind::ScalarKV);

  const RaSection* rec = find_section("reconciliation");
  ASSERT_NE(rec, nullptr);
  EXPECT_EQ(rec->kind, RaSectionKind::TimeSeries);
  ASSERT_EQ(rec->columns.size(), 11u);
  EXPECT_EQ(rec->columns[0].name, "date");
  EXPECT_EQ(rec->columns[10].name, "n_quote_mid_lots");

  const RaSection* sched = find_section("trade_schedule");
  const RaSection* proj_sched = find_section("projected_schedule");
  ASSERT_NE(sched, nullptr);
  ASSERT_NE(proj_sched, nullptr);
  EXPECT_EQ(sched->kind, RaSectionKind::SubTable);
  EXPECT_EQ(proj_sched->kind, RaSectionKind::SubTable);
  ASSERT_EQ(sched->columns.size(), 29u);
  EXPECT_EQ(sched->columns[0].name, "roll_date");
  EXPECT_EQ(sched->columns[28].name, "surface_fingerprint");
  EXPECT_EQ(proj_sched->columns.data(), sched->columns.data());
  EXPECT_EQ(proj_sched->columns.size(), sched->columns.size());

  const RaSection* marks = find_section("contract_marks");
  ASSERT_NE(marks, nullptr);
  EXPECT_EQ(marks->kind, RaSectionKind::SubTable);
  ASSERT_EQ(marks->columns.size(), 19u);
  EXPECT_EQ(marks->columns[0].name, "date");
  EXPECT_EQ(marks->columns[18].name, "model_in_spread");

  const RaSection* div = find_section("mark_divergence");
  ASSERT_NE(div, nullptr);
  EXPECT_EQ(div->kind, RaSectionKind::SubTable);
  ASSERT_EQ(div->columns.size(), 10u);
  EXPECT_EQ(div->columns[0].name, "date");
  EXPECT_EQ(div->columns[9].name, "abs_diff_bps_of_mark");

  const RaSection* diag = find_section("diagnostics");
  ASSERT_NE(diag, nullptr);
  EXPECT_EQ(diag->kind, RaSectionKind::SubTable);
  ASSERT_EQ(diag->columns.size(), 4u);
  EXPECT_EQ(diag->columns[0].name, "subcommand");
  EXPECT_EQ(diag->columns[1].name, "phase");
  EXPECT_EQ(diag->columns[2].name, "wall_ms");
  EXPECT_EQ(diag->columns[3].name, "count");

  EXPECT_EQ(ra_sections().size(), 10u);
}

TEST(RunArchiveSchema, SchemaHashStableAndNonzero) {
  // Usable in constant expressions (a header stamps it at compile time).
  static_assert(ra_schema_hash() != 0, "RunArchive schema hash must be nonzero");
  EXPECT_EQ(ra_schema_hash(), ra_schema_hash());
}

TEST(RunArchiveSchema, FormatConstants) {
  EXPECT_EQ(std::string_view(kRaMagic, sizeof kRaMagic), "ATXRUN01");
  EXPECT_EQ(kRaMajor, 1u);
  EXPECT_EQ(kRaMinor, 0u);
}

// ── Task 2: on-disk ABI (run_archive.hpp) ────────────────────────────────────
// These are on-disk ABI structs, so beyond sizeof the load-bearing offsets are
// pinned explicitly: a field reorder that preserved sizeof would still silently
// corrupt readers (mirrors the ArchiveV2Header discipline).

TEST(RunArchiveAbi, FileHeaderLayoutPinned) {
  static_assert(sizeof(RunArchiveHeader) == 256);
  static_assert(std::is_trivially_copyable_v<RunArchiveHeader>);
  static_assert(std::is_standard_layout_v<RunArchiveHeader>);
  static_assert(offsetof(RunArchiveHeader, file_size) == 8);
  static_assert(offsetof(RunArchiveHeader, created_ts_ns) == 16);
  static_assert(offsetof(RunArchiveHeader, schema_hash) == 24);
  static_assert(offsetof(RunArchiveHeader, writer_version_hash) == 32);
  static_assert(offsetof(RunArchiveHeader, run_identity_hash) == 40);
  static_assert(offsetof(RunArchiveHeader, section_dir_offset) == 48);
  static_assert(offsetof(RunArchiveHeader, data_offset) == 56);
  static_assert(offsetof(RunArchiveHeader, section_count) == 64);
  static_assert(offsetof(RunArchiveHeader, header_crc32c) == 68);
  static_assert(offsetof(RunArchiveHeader, metadata_crc32c) == 72);
  static_assert(offsetof(RunArchiveHeader, flags) == 76);
  static_assert(offsetof(RunArchiveHeader, major) == 80);
  SUCCEED();
}

TEST(RunArchiveAbi, SectionStructsPinned) {
  static_assert(sizeof(RaSectionDescriptor) == 80);
  static_assert(std::is_trivially_copyable_v<RaSectionDescriptor>);
  static_assert(std::is_standard_layout_v<RaSectionDescriptor>);
  static_assert(offsetof(RaSectionDescriptor, section_offset) == 0);
  static_assert(offsetof(RaSectionDescriptor, section_size) == 8);
  static_assert(offsetof(RaSectionDescriptor, n_rows) == 16);
  static_assert(offsetof(RaSectionDescriptor, payload_crc32c) == 32);

  static_assert(sizeof(RaSectionHeader) == 64);
  static_assert(std::is_trivially_copyable_v<RaSectionHeader>);
  static_assert(std::is_standard_layout_v<RaSectionHeader>);
  static_assert(offsetof(RaSectionHeader, section_size) == 8);
  static_assert(offsetof(RaSectionHeader, n_rows) == 16);
  static_assert(offsetof(RaSectionHeader, payload_crc32c) == 36);

  static_assert(sizeof(RaColumnDescriptor) == 96);
  static_assert(std::is_trivially_copyable_v<RaColumnDescriptor>);
  static_assert(std::is_standard_layout_v<RaColumnDescriptor>);
  static_assert(offsetof(RaColumnDescriptor, data_offset) == 0);
  static_assert(offsetof(RaColumnDescriptor, data_size) == 8);
  static_assert(offsetof(RaColumnDescriptor, aux_offset) == 16);
  static_assert(offsetof(RaColumnDescriptor, name) == 40);
  SUCCEED();
}

// ── Task 3: writer (write_run_archive + atomic file write) ───────────────────
// The writer must not depend on the Task 5 encoders, so the sections are built
// BY HAND from small literal column vectors (a dict-str date column, an i64
// ts_ns, a couple of registry f64 columns).

// Two-row `backtest` slice (registry subset) + one-row `meta` ScalarKV section.
// The literal vectors live in static storage so the returned RaSectionData's
// non-owning spans stay valid for the whole test.
std::vector<RaSectionData> make_test_sections() {
  static const std::vector<std::string> date_dict = {"2026-07-11", "2026-07-12"};
  static const std::vector<std::uint32_t> date_codes = {0, 1};
  static const std::vector<std::int64_t> ts = {1, 2};
  static const std::vector<double> nav = {100.0, 101.5};
  static const std::vector<double> cash = {10.0, 11.25};
  static const std::vector<std::uint8_t> flag_codes = {0, 1};
  static const std::vector<std::string> flag_labels = {"no", "yes"};

  RaSectionData bt;
  bt.name = "backtest";
  bt.kind = RaSectionKind::TimeSeries;
  bt.n_rows = 2;
  bt.columns.emplace_back("date", RaColumnData::of_dict(date_codes, date_dict));
  bt.columns.emplace_back("ts_ns", RaColumnData::of_i64(ts));
  bt.columns.emplace_back("nav", RaColumnData::of_f64(nav));
  bt.columns.emplace_back("cash", RaColumnData::of_f64(cash));
  // Dynamically-appended column (not in the registry) — the writer must accept
  // it, like the per-signal backtest series appended at write time.
  bt.columns.emplace_back("flag", RaColumnData::of_u8enum(flag_codes, flag_labels));

  static const std::vector<std::string> kv_dict = {"schema", "1"};
  static const std::vector<std::uint32_t> key_codes = {0};
  static const std::vector<std::uint32_t> value_codes = {1};
  RaSectionData meta;
  meta.name = "meta";
  meta.kind = RaSectionKind::ScalarKV;
  meta.n_rows = 1;
  meta.columns.emplace_back("key", RaColumnData::of_dict(key_codes, kv_dict));
  meta.columns.emplace_back("value", RaColumnData::of_dict(value_codes, kv_dict));

  std::vector<RaSectionData> sections;
  sections.push_back(std::move(bt));
  sections.push_back(std::move(meta));
  return sections;
}

[[nodiscard]] std::uint32_t header_crc(RunArchiveHeader h) {
  h.header_crc32c = 0;
  std::array<std::byte, sizeof h> raw{};
  std::memcpy(raw.data(), &h, sizeof h);
  return detail::crc32c(raw.data(), raw.size());
}

TEST(RunArchiveWriter, WritesFramedBuffer) {
  const std::vector<RaSectionData> sections = make_test_sections();
  auto bytes = write_run_archive(sections, /*created_ts_ns=*/123, /*run_identity_hash=*/0xABCull);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
  ASSERT_GT(bytes->size(), sizeof(RunArchiveHeader));

  RunArchiveHeader h{};
  std::memcpy(&h, bytes->data(), sizeof h);
  EXPECT_EQ(std::string_view(h.magic, 8), std::string_view(kRaMagic, 8));
  EXPECT_EQ(h.major, kRaMajor);
  EXPECT_EQ(h.minor, kRaMinor);
  EXPECT_EQ(h.header_size, sizeof(RunArchiveHeader));
  EXPECT_EQ(h.endian, 1u);
  EXPECT_EQ(h.pointer_bits, 64u);
  EXPECT_EQ(h.schema_hash, ra_schema_hash());
  EXPECT_EQ(h.created_ts_ns, 123u);
  EXPECT_EQ(h.run_identity_hash, 0xABCull);
  EXPECT_EQ(h.section_count, 2u);
  EXPECT_EQ(h.file_size, bytes->size());

  // Header CRC verifies over the header bytes with its own field zeroed.
  EXPECT_EQ(header_crc(h), h.header_crc32c);

  // Directory: sorted by name ("backtest" < "meta"), metadata CRC covers it.
  ASSERT_EQ(h.section_dir_offset, sizeof(RunArchiveHeader));
  const std::uint64_t dir_bytes = 2ull * sizeof(RaSectionDescriptor);
  ASSERT_LE(h.section_dir_offset + dir_bytes, bytes->size());
  EXPECT_EQ(detail::crc32c(bytes->data() + h.section_dir_offset,
                           static_cast<std::size_t>(dir_bytes)),
            h.metadata_crc32c);
  RaSectionDescriptor d0{};
  RaSectionDescriptor d1{};
  std::memcpy(&d0, bytes->data() + h.section_dir_offset, sizeof d0);
  std::memcpy(&d1, bytes->data() + h.section_dir_offset + sizeof d0, sizeof d1);
  EXPECT_EQ(std::string_view(d0.name, 8), "backtest");
  EXPECT_EQ(d0.n_rows, 2u);
  EXPECT_EQ(d0.n_cols, 5u);
  EXPECT_EQ(d0.kind, RaSectionKind::TimeSeries);
  EXPECT_EQ(d0.section_offset % kRaSectionAlign, 0u);
  EXPECT_EQ(std::string_view(d1.name, 4), "meta");
  EXPECT_EQ(d1.n_rows, 1u);
  EXPECT_EQ(d1.n_cols, 2u);
  EXPECT_EQ(d1.kind, RaSectionKind::ScalarKV);

  // Section record framing: magic + descriptor agreement + CRC copy.
  ASSERT_LE(d0.section_offset + d0.section_size, bytes->size());
  RaSectionHeader sh{};
  std::memcpy(&sh, bytes->data() + d0.section_offset, sizeof sh);
  EXPECT_EQ(std::string_view(sh.magic, 8), std::string_view(kRaSectionMagic, 8));
  EXPECT_EQ(sh.section_size, d0.section_size);
  EXPECT_EQ(sh.n_rows, d0.n_rows);
  EXPECT_EQ(sh.n_cols, d0.n_cols);
  EXPECT_EQ(sh.payload_crc32c, d0.payload_crc32c);
  EXPECT_NE(sh.payload_crc32c, 0u);
}

TEST(RunArchiveWriter, RejectsMalformedSections) {
  // Empty section list.
  EXPECT_FALSE(write_run_archive({}, 1, 1).has_value());

  // Dict code out of range for its dict table.
  {
    static const std::vector<std::string> dict = {"only"};
    static const std::vector<std::uint32_t> codes = {0, 7};
    RaSectionData bad;
    bad.name = "backtest";
    bad.kind = RaSectionKind::TimeSeries;
    bad.n_rows = 2;
    bad.columns.emplace_back("date", RaColumnData::of_dict(codes, dict));
    const auto out = write_run_archive(std::span(&bad, 1), 1, 1);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), atx::core::ErrorCode::InvalidArgument);
  }

  // Column length disagrees with n_rows.
  {
    static const std::vector<double> nav = {100.0};
    RaSectionData bad;
    bad.name = "backtest";
    bad.kind = RaSectionKind::TimeSeries;
    bad.n_rows = 2;
    bad.columns.emplace_back("nav", RaColumnData::of_f64(nav));
    EXPECT_FALSE(write_run_archive(std::span(&bad, 1), 1, 1).has_value());
  }

  // Registry dtype drift: `nav` is F64 in the registry, not I64.
  {
    static const std::vector<std::int64_t> nav = {100, 101};
    RaSectionData bad;
    bad.name = "backtest";
    bad.kind = RaSectionKind::TimeSeries;
    bad.n_rows = 2;
    bad.columns.emplace_back("nav", RaColumnData::of_i64(nav));
    EXPECT_FALSE(write_run_archive(std::span(&bad, 1), 1, 1).has_value());
  }

  // Duplicate section names.
  {
    static const std::vector<double> nav = {1.0};
    std::vector<RaSectionData> dup(2);
    for (RaSectionData& s : dup) {
      s.name = "backtest";
      s.kind = RaSectionKind::TimeSeries;
      s.n_rows = 1;
      s.columns.emplace_back("nav", RaColumnData::of_f64(nav));
    }
    const auto out = write_run_archive(dup, 1, 1);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), atx::core::ErrorCode::AlreadyExists);
  }
}

TEST(RunArchiveWriter, AtomicFileWrite) {
  const std::vector<RaSectionData> sections = make_test_sections();
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "atx_run_archive_writer_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::filesystem::path path = dir / "archive.runar";
  const auto temp_count = [&] {
    std::size_t count = 0u;
    const std::string prefix = path.filename().string() + ".tmp.";
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
      count += entry.path().filename().string().starts_with(prefix) ? 1u : 0u;
    }
    return count;
  };

  auto st = write_run_archive_file(path.string(), sections, 5, 7);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  ASSERT_TRUE(std::filesystem::exists(path));
  EXPECT_EQ(temp_count(), 0u);

  // Re-write over the existing destination (Windows: remove-then-rename).
  auto st2 = write_run_archive_file(path.string(), sections, 6, 7);
  ASSERT_TRUE(st2.has_value()) << st2.error().to_string();
  EXPECT_EQ(temp_count(), 0u);

  // The published file leads with the RunArchive magic.
  std::ifstream is(path, std::ios::binary);
  ASSERT_TRUE(is.good());
  char magic[8] = {};
  is.read(magic, 8);
  EXPECT_EQ(std::string_view(magic, 8), std::string_view(kRaMagic, 8));
  is.close();
  std::filesystem::remove_all(dir);
}

// ── Task 4: reader (RunArchive open/section/validate + mmap) ─────────────────

[[nodiscard]] std::vector<std::byte> make_test_bytes() {
  const std::vector<RaSectionData> sections = make_test_sections();
  auto bytes = write_run_archive(sections, /*created_ts_ns=*/123, /*run_identity_hash=*/0xABCull);
  EXPECT_TRUE(bytes.has_value());
  return std::move(*bytes);
}

TEST(RunArchiveReader, RoundTrip) {
  auto ar = RunArchive::open(make_test_bytes());
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  EXPECT_EQ(ar->count(), 2u);
  EXPECT_EQ(ar->header().run_identity_hash, 0xABCull);
  ASSERT_EQ(ar->directory().size(), 2u);

  const ArchiveContentIdentity id = ar->identity();
  EXPECT_EQ(id.file_size, ar->header().file_size);
  EXPECT_EQ(id.created_ts_ns, 123u);
  EXPECT_EQ(id.header_crc32c, ar->header().header_crc32c);
  EXPECT_EQ(id.metadata_crc32c, ar->header().metadata_crc32c);

  auto sec = ar->section("backtest");
  ASSERT_TRUE(sec.has_value()) << sec.error().to_string();
  EXPECT_EQ(sec->name(), "backtest");
  EXPECT_EQ(sec->kind(), RaSectionKind::TimeSeries);
  EXPECT_EQ(sec->n_rows(), 2u);
  EXPECT_EQ(sec->n_cols(), 5u);

  const auto nav = sec->f64_col("nav");
  ASSERT_EQ(nav.size(), 2u);
  EXPECT_EQ(nav[0], 100.0);
  EXPECT_EQ(nav[1], 101.5);
  const auto cash = sec->f64_col("cash");
  ASSERT_EQ(cash.size(), 2u);
  EXPECT_EQ(cash[1], 11.25);
  const auto ts = sec->i64_col("ts_ns");
  ASSERT_EQ(ts.size(), 2u);
  EXPECT_EQ(ts[0], 1);
  EXPECT_EQ(ts[1], 2);

  const RaDictColumn dates = sec->dict_col("date");
  ASSERT_EQ(dates.size(), 2u);
  EXPECT_EQ(dates.at(0), "2026-07-11");
  EXPECT_EQ(dates.at(1), "2026-07-12");
  EXPECT_EQ(dates.table().size(), 2u);
  EXPECT_EQ(dates.codes()[1], 1u);

  const auto flags = sec->u8enum_col("flag");
  ASSERT_EQ(flags.size(), 2u);
  EXPECT_EQ(flags[0], 0u);
  EXPECT_EQ(flags[1], 1u);
  const RaStringTable labels = sec->u8enum_labels("flag");
  ASSERT_EQ(labels.size(), 2u);
  EXPECT_EQ(labels.at(0), "no");
  EXPECT_EQ(labels.at(1), "yes");

  // Name/dtype misses are empty, not errors.
  EXPECT_TRUE(sec->f64_col("no_such_column").empty());
  EXPECT_TRUE(sec->f64_col("ts_ns").empty()); // ts_ns is I64, not F64
  EXPECT_TRUE(sec->dict_col("nav").empty());

  auto meta = ar->section("meta");
  ASSERT_TRUE(meta.has_value());
  EXPECT_EQ(meta->kind(), RaSectionKind::ScalarKV);
  EXPECT_EQ(meta->dict_col("key").at(0), "schema");
  EXPECT_EQ(meta->dict_col("value").at(0), "1");

  const auto missing = ar->section("no_such_section");
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code(), atx::core::ErrorCode::NotFound);

  // Lazy integrity passes on an untampered archive.
  EXPECT_TRUE(ar->validate_section("backtest").has_value());
  EXPECT_TRUE(ar->validate_all().has_value());
}

TEST(RunArchiveReader, RejectsSchemaDrift) {
  // (a) Flip schema_hash without repairing the header CRC: rejected (CRC).
  {
    auto bytes = make_test_bytes();
    RunArchiveHeader h{};
    std::memcpy(&h, bytes.data(), sizeof h);
    h.schema_hash ^= 1;
    std::memcpy(bytes.data(), &h, sizeof h);
    EXPECT_FALSE(RunArchive::open(std::move(bytes)).has_value());
  }
  // (b) Flip schema_hash AND recompute the header CRC — a "valid-but-drifted"
  // file (a column rename in a future writer): rejected on schema_hash.
  {
    auto bytes = make_test_bytes();
    RunArchiveHeader h{};
    std::memcpy(&h, bytes.data(), sizeof h);
    h.schema_hash ^= 1;
    h.header_crc32c = header_crc(h);
    std::memcpy(bytes.data(), &h, sizeof h);
    const auto out = RunArchive::open(std::move(bytes));
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), atx::core::ErrorCode::ParseError);
  }
  // (c) Truncated buffer: file_size disagrees.
  {
    auto bytes = make_test_bytes();
    bytes.pop_back();
    EXPECT_FALSE(RunArchive::open(std::move(bytes)).has_value());
  }
}

TEST(RunArchiveReader, LazyCrcCatchesPayloadTamper) {
  auto bytes = make_test_bytes();
  // Locate the `backtest` section (sorted first in the directory) and flip the
  // last byte of its payload extent.
  RunArchiveHeader h{};
  std::memcpy(&h, bytes.data(), sizeof h);
  RaSectionDescriptor d0{};
  std::memcpy(&d0, bytes.data() + h.section_dir_offset, sizeof d0);
  ASSERT_EQ(std::string_view(d0.name, 8), "backtest");
  bytes[static_cast<std::size_t>(d0.section_offset + d0.section_size - 1)] ^= std::byte{0xFF};

  // open() is lazy: framing still validates.
  auto ar = RunArchive::open(std::move(bytes));
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  // The per-section CRC catches the tamper; the untouched section still passes.
  EXPECT_FALSE(ar->validate_section("backtest").has_value());
  EXPECT_TRUE(ar->validate_section("meta").has_value());
  EXPECT_FALSE(ar->validate_all().has_value());
}

TEST(RunArchiveReader, RejectsForgedRowCountOverflow) {
  // C1 regression: a forged descriptor n_rows that WRAPS the
  // `n_rows * ra_dtype_size(dtype)` byte-extent product. The `backtest`
  // section's `date` column is DictStr (4-byte codes), so n_rows == 2^62 wraps
  // the u64 product to 0 (2^64 mod 2^64). With a matching data_size == 0 that
  // would have passed section()'s size check and then driven its eager dict
  // code-range scan (for r in [0, n_rows)) off the end of the archive. open()
  // must cap n_rows up front instead. The layered CRCs are recomputed so the
  // file is otherwise valid (mirrors RejectsSchemaDrift case (b)) — the cap,
  // not a checksum, is what rejects it.
  auto bytes = make_test_bytes();
  RunArchiveHeader h{};
  std::memcpy(&h, bytes.data(), sizeof h);

  RaSectionDescriptor d0{};
  std::memcpy(&d0, bytes.data() + h.section_dir_offset, sizeof d0);
  ASSERT_EQ(std::string_view(d0.name, 8), "backtest");
  d0.n_rows = std::uint64_t{1} << 62; // > 2^48 cap; * 4 (DictStr) wraps to 0
  std::memcpy(bytes.data() + h.section_dir_offset, &d0, sizeof d0);

  // Repair framing so open reaches the directory validation loop: metadata CRC
  // over the (patched) directory, then the header CRC over the new header.
  const std::uint64_t dir_bytes =
      static_cast<std::uint64_t>(h.section_count) * sizeof(RaSectionDescriptor);
  h.metadata_crc32c = detail::crc32c(bytes.data() + h.section_dir_offset,
                                     static_cast<std::size_t>(dir_bytes));
  h.header_crc32c = header_crc(h);
  std::memcpy(bytes.data(), &h, sizeof h);

  const auto out = RunArchive::open(std::move(bytes));
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().code(), atx::core::ErrorCode::ParseError);
}

TEST(RunArchiveReader, OpenFileAndMapped) {
  const std::vector<RaSectionData> sections = make_test_sections();
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "atx_run_archive_reader_test.runar";
  std::filesystem::remove(path);
  ASSERT_TRUE(write_run_archive_file(path.string(), sections, 123, 0xABCull).has_value());

  {
    auto ar = RunArchive::open_file(path.string());
    ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
    auto sec = ar->section("backtest");
    ASSERT_TRUE(sec.has_value());
    ASSERT_EQ(sec->f64_col("nav").size(), 2u);
    EXPECT_EQ(sec->f64_col("nav")[0], 100.0);
  }
  {
    auto ar = RunArchive::open_mapped(path.string());
    ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
    auto sec = ar->section("backtest");
    ASSERT_TRUE(sec.has_value());
    ASSERT_EQ(sec->f64_col("nav").size(), 2u);
    EXPECT_EQ(sec->f64_col("nav")[1], 101.5);
    EXPECT_EQ(sec->dict_col("date").at(0), "2026-07-11");
    EXPECT_TRUE(ar->validate_all().has_value());
  } // archive (and its mapping) destroyed before the file is removed

  EXPECT_FALSE(RunArchive::open_file((path.string() + ".does_not_exist")).has_value());
  std::filesystem::remove(path);
}

// ── Task 5: section encoders (library type → RaSectionData) ──────────────────

// GOLDEN SCHEMA-HASH PIN — format freeze. ra_schema_hash() folds the whole
// registry (section names/kinds, column names/dtypes/units) into one value, so
// ANY registry edit lands here first. An intentional format change requires
// bumping this golden AND kRaMinor together; anything else is silent drift.
TEST(RunArchiveSchema, GoldenHashPinned) {
  EXPECT_EQ(ra_schema_hash(), 0xdcce47781ac8390dull)
      << "registry drift: actual hash 0x" << std::hex << ra_schema_hash();
}

// The exact 2-row fixture the Wave A brief pins: dates/ts_ns/nav/pnl_vega and
// one "atm_iv" signal populated, every other double column zero. Shared by the
// value-exact round-trip test and the committed Python fixture.
BacktestResult make_encoder_fixture_result() {
  BacktestResult r;
  const std::vector<double> zeros = {0.0, 0.0};
  r.date = {"2026-07-11", "2026-07-12"};
  r.ts_ns = {10, 20};
  r.pnl_total = zeros;
  r.pnl_delta = zeros;
  r.pnl_gamma = zeros;
  r.pnl_vega = {1.25, -0.5};
  r.pnl_vanna = zeros;
  r.pnl_volga = zeros;
  r.pnl_theta = zeros;
  r.pnl_rho = zeros;
  r.pnl_charm = zeros;
  r.pnl_unexplained = zeros;
  r.pnl_settlement = zeros;
  r.pnl_shares = zeros;
  r.financing = zeros;
  r.cost = zeros;
  r.nav = {100.0, 101.5};
  r.cash = zeros;
  r.gross_delta = zeros;
  r.gross_gamma = zeros;
  r.gross_vega = zeros;
  r.gross_theta = zeros;
  r.turnover_notional = zeros;
  r.turnover_vega = zeros;
  r.n_open_lots = zeros;
  r.n_unpriced_lots = zeros;
  r.n_unpriced_greeks = zeros;
  r.signals.emplace_back("atm_iv", std::vector<double>{0.20, 0.21});
  return r;
}

// Registry-name → BacktestResult-member map (the append_backtest_series_tsv
// order, tearsheet.cpp) used to assert EVERY double column bit-exactly.
std::vector<std::pair<std::string_view, const std::vector<double>*>>
backtest_dbl_cols(const BacktestResult& r) {
  return {
      {"pnl_total", &r.pnl_total},
      {"pnl_delta", &r.pnl_delta},
      {"pnl_gamma", &r.pnl_gamma},
      {"pnl_vega", &r.pnl_vega},
      {"pnl_vanna", &r.pnl_vanna},
      {"pnl_volga", &r.pnl_volga},
      {"pnl_theta", &r.pnl_theta},
      {"pnl_rho", &r.pnl_rho},
      {"pnl_charm", &r.pnl_charm},
      {"pnl_unexplained", &r.pnl_unexplained},
      {"pnl_settlement", &r.pnl_settlement},
      {"pnl_shares", &r.pnl_shares},
      {"financing", &r.financing},
      {"cost", &r.cost},
      {"nav", &r.nav},
      {"cash", &r.cash},
      {"gross_delta", &r.gross_delta},
      {"gross_gamma", &r.gross_gamma},
      {"gross_vega", &r.gross_vega},
      {"gross_theta", &r.gross_theta},
      {"turnover_notional", &r.turnover_notional},
      {"turnover_vega", &r.turnover_vega},
      {"n_open_lots", &r.n_open_lots},
      {"n_unpriced_lots", &r.n_unpriced_lots},
      {"n_unpriced_greeks", &r.n_unpriced_greeks},
  };
}

TEST(RunArchiveEncoders, BacktestSectionRoundTripsValueExact) {
  const BacktestResult r = make_encoder_fixture_result();
  const RaSectionData sec = encode_backtest_section("backtest", r);
  EXPECT_EQ(sec.name, "backtest");
  EXPECT_EQ(sec.kind, RaSectionKind::TimeSeries);
  EXPECT_EQ(sec.n_rows, 2u);
  ASSERT_EQ(sec.columns.size(), 28u); // 27 registry columns + 1 signal

  auto bytes = write_run_archive(std::span(&sec, 1), 1, 1);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
  auto ar = RunArchive::open(std::move(*bytes));
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  auto v = ar->section("backtest");
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_EQ(v->n_cols(), 28u);

  // All 25 registry double columns round-trip bit-exactly from the member
  // vectors, in registry order (== the TSV writer's column order).
  const RaSection* reg = find_section("backtest");
  ASSERT_NE(reg, nullptr);
  const auto cols = backtest_dbl_cols(r);
  ASSERT_EQ(cols.size() + 2, reg->columns.size());
  for (std::size_t i = 2; i < reg->columns.size(); ++i) {
    const std::string_view name = reg->columns[i].name;
    ASSERT_EQ(name, cols[i - 2].first); // registry order == member-map order
    const auto got = v->f64_col(name);
    const std::vector<double>& want = *cols[i - 2].second;
    ASSERT_EQ(got.size(), want.size()) << name;
    for (std::size_t row = 0; row < want.size(); ++row) {
      EXPECT_EQ(std::memcmp(&got[row], &want[row], sizeof(double)), 0)
          << name << " row " << row;
    }
  }

  const auto ts = v->i64_col("ts_ns");
  ASSERT_EQ(ts.size(), 2u);
  EXPECT_EQ(ts[0], 10);
  EXPECT_EQ(ts[1], 20);
  EXPECT_EQ(v->f64_col("nav")[1], 101.5);
  EXPECT_EQ(v->f64_col("pnl_vega")[0], 1.25);

  // The dynamically-appended signal column.
  const auto sig = v->f64_col("atm_iv");
  ASSERT_EQ(sig.size(), 2u);
  EXPECT_EQ(sig[0], 0.20);
  EXPECT_EQ(sig[1], 0.21);

  // The dict-str date column.
  const RaDictColumn dates = v->dict_col("date");
  ASSERT_EQ(dates.size(), 2u);
  EXPECT_EQ(dates.at(0), "2026-07-11");
  EXPECT_EQ(dates.at(1), "2026-07-12");
}

// ── 2.9: the backtest encoder SNAPSHOTS its source ───────────────────────────
//
// encode_backtest_section used to span the caller's BacktestResult IN PLACE
// while every sibling encoder parks a copy in the section's arena. A staged
// section was therefore not a value: it silently aliased memory the caller still
// owned and could mutate or free, and `write_run_archive` memcpy'd whatever the
// spans pointed at by then — a dangling read in the common "encode, then let the
// result go out of scope" shape. The encoder now copies into the arena like the
// others, so a section owns every byte it will write.
TEST(RunArchiveEncoders, BacktestSection_SourceMutatedAfterEncode_WritesSnapshot) {
  BacktestResult r = make_encoder_fixture_result();
  const RaSectionData sec = encode_backtest_section("backtest", r);

  // One mutation per class of previously-spanned column: the i64 ts_ns axis, two
  // of the 25 registry F64 members, and a dynamically-appended signal series.
  r.ts_ns[0] = -999;
  r.nav[1] = -1.0;
  r.pnl_vega[0] = -2.0;
  r.signals[0].second[0] = -3.0;

  auto bytes = write_run_archive(std::span(&sec, 1), 1, 1);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
  auto ar = RunArchive::open(std::move(*bytes));
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  auto v = ar->section("backtest");
  ASSERT_TRUE(v.has_value()) << v.error().to_string();

  const auto ts = v->i64_col("ts_ns");
  ASSERT_EQ(ts.size(), 2u);
  EXPECT_EQ(ts[0], 10); // the pre-mutation fixture value
  EXPECT_EQ(v->f64_col("nav")[1], 101.5);
  EXPECT_EQ(v->f64_col("pnl_vega")[0], 1.25);
  EXPECT_EQ(v->f64_col("atm_iv")[0], 0.20);
}

TEST(RunArchiveEncoders, BacktestSection_SourceDestroyedBeforeWrite_WritesSnapshot) {
  // The source dies with the lambda; the section must still own its bytes.
  const RaSectionData sec = [] {
    const BacktestResult r = make_encoder_fixture_result();
    return encode_backtest_section("backtest", r);
  }();

  auto bytes = write_run_archive(std::span(&sec, 1), 1, 1);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
  auto ar = RunArchive::open(std::move(*bytes));
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  auto v = ar->section("backtest");
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_EQ(v->n_cols(), 28u);

  const auto ts = v->i64_col("ts_ns");
  ASSERT_EQ(ts.size(), 2u);
  EXPECT_EQ(ts[0], 10);
  EXPECT_EQ(ts[1], 20);
  EXPECT_EQ(v->f64_col("nav")[1], 101.5);
  EXPECT_EQ(v->f64_col("atm_iv")[1], 0.21);
  const RaDictColumn dates = v->dict_col("date");
  ASSERT_EQ(dates.size(), 2u);
  EXPECT_EQ(dates.at(0), "2026-07-11");
}

// ── T6: 25-double backtest column single-source-of-truth ─────────────────────
// backtest_series_columns() is the ONE ordered {name, member-ptr} table that
// BOTH append_backtest_series_tsv (tearsheet.cpp) and encode_backtest_section
// (run_archive.cpp) iterate. It must never drift from the FROZEN registry
// kBacktestCols[2..26] (whose fold feeds ra_schema_hash() == 0xdcce…): this test
// pins the shared table's names/order to the registry, and the encoder side
// carries a compile-time static_assert of the same invariant.
TEST(RunArchiveEncoders, BacktestSeriesColumnsMatchRegistryOrder) {
  const auto cols = backtest_series_columns();
  ASSERT_EQ(cols.size(), 25u) << "the backtest series has exactly 25 F64 columns";

  const RaSection* bt = find_section("backtest");
  ASSERT_NE(bt, nullptr);
  ASSERT_EQ(bt->columns.size(), 27u); // date + ts_ns + 25 doubles

  // Shared-table names == registry backtest columns index 2..26, in order.
  for (std::size_t i = 0; i < cols.size(); ++i) {
    EXPECT_EQ(cols[i].name, bt->columns[i + 2].name) << "column " << i;
  }
  EXPECT_EQ(cols.front().name, "pnl_total");
  EXPECT_EQ(cols.back().name, "n_unpriced_greeks");
}

// The whole point of the dedup: the TSV writer and the archive encoder emit the
// SAME 25 columns, in the SAME order, with byte-identical values — because both
// now iterate the single source. Prove it column-for-column: encode + TSV-write
// the same BacktestResult, then for every shared column assert the TSV cell, the
// decoded archive value, and the source member vector are all bit-identical.
TEST(RunArchiveEncoders, BacktestSeriesColumnsTsvEncoderParity) {
  const BacktestResult r = make_encoder_fixture_result();

  // Archive side.
  const RaSectionData sec = encode_backtest_section("backtest", r);
  auto bytes = write_run_archive(std::span(&sec, 1), 1, 1);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
  auto ar = RunArchive::open(std::move(*bytes));
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  auto v = ar->section("backtest");
  ASSERT_TRUE(v.has_value()) << v.error().to_string();

  // TSV side.
  const std::filesystem::path tsv_path =
      std::filesystem::temp_directory_path() / "atx_t6_backtest_series.tsv";
  std::error_code ec;
  std::filesystem::remove(tsv_path, ec);
  const Status st = write_backtest_tsv(r, tsv_path.string());
  ASSERT_TRUE(st.has_value()) << st.error().to_string();

  // Parse the TSV into a header + rows of tab-separated cells.
  std::ifstream is(tsv_path, std::ios::binary);
  ASSERT_TRUE(is.good());
  const std::string content((std::istreambuf_iterator<char>(is)),
                            std::istreambuf_iterator<char>());
  std::vector<std::vector<std::string>> table;
  for (std::size_t start = 0; start < content.size();) {
    const std::size_t nl = content.find('\n', start);
    if (nl == std::string::npos) break;
    const std::string line = content.substr(start, nl - start);
    std::vector<std::string> cells;
    for (std::size_t cs = 0;;) {
      const std::size_t tab = line.find('\t', cs);
      if (tab == std::string::npos) {
        cells.push_back(line.substr(cs));
        break;
      }
      cells.push_back(line.substr(cs, tab - cs));
      cs = tab + 1;
    }
    table.push_back(std::move(cells));
    start = nl + 1;
  }
  ASSERT_EQ(table.size(), r.size() + 1); // header + one row per step
  const std::vector<std::string>& header = table.front();

  // Frozen leading header: date, ts_ns, then the 25 shared columns in order.
  ASSERT_GE(header.size(), 27u);
  EXPECT_EQ(header[0], "date");
  EXPECT_EQ(header[1], "ts_ns");
  const auto cols = backtest_series_columns();
  for (std::size_t i = 0; i < cols.size(); ++i) {
    EXPECT_EQ(header[i + 2], cols[i].name) << "header column " << (i + 2);
  }

  const auto col_index = [&](std::string_view name) -> std::size_t {
    for (std::size_t i = 0; i < header.size(); ++i) {
      if (header[i] == name) return i;
    }
    ADD_FAILURE() << "column not found in TSV: " << name;
    return header.size();
  };

  // Column-for-column bit-parity: TSV cell == decoded archive == source member.
  for (const auto& col : cols) {
    const std::size_t ci = col_index(col.name);
    ASSERT_LT(ci, header.size()) << col.name;
    const auto archive = v->f64_col(col.name);
    const std::vector<double>& src = r.*col.member;
    ASSERT_EQ(archive.size(), r.size()) << col.name;
    ASSERT_EQ(src.size(), r.size()) << col.name;
    for (std::size_t row = 0; row < r.size(); ++row) {
      const double from_tsv = std::strtod(table[row + 1][ci].c_str(), nullptr);
      EXPECT_EQ(std::memcmp(&from_tsv, &src[row], sizeof(double)), 0)
          << col.name << " TSV vs source, row " << row;
      EXPECT_EQ(std::memcmp(&archive[row], &src[row], sizeof(double)), 0)
          << col.name << " archive vs source, row " << row;
    }
  }
}

TEST(RunArchiveEncoders, ReconciliationSectionRoundTrips) {
  ListedDispersionReconciliation rec;
  ListedReconciliationRow a;
  a.date = "2026-07-11";
  a.valuation_ts_ns = 100;
  a.held_cohort = 3;
  a.model_option_pnl = 1.5;
  a.quote_mid_pnl = 1.25;
  a.model_minus_quote_pnl = 0.25;
  a.model_nav = 10.0;
  a.quote_mid_nav = 9.75;
  a.quote_mid_coverage = 0.5;
  a.n_held_lots = 4;
  a.n_quote_mid_lots = 2;
  ListedReconciliationRow b = a;
  b.date = "2026-07-12";
  b.valuation_ts_ns = 200;
  b.model_nav = 11.0;
  b.n_held_lots = 5;
  rec.rows = {a, b};

  const RaSectionData sec = encode_reconciliation_section(rec);
  EXPECT_EQ(sec.name, "reconciliation");
  EXPECT_EQ(sec.kind, RaSectionKind::TimeSeries);
  EXPECT_EQ(sec.n_rows, 2u);
  ASSERT_EQ(sec.columns.size(), 11u);

  auto bytes = write_run_archive(std::span(&sec, 1), 1, 1);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
  auto ar = RunArchive::open(std::move(*bytes));
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  auto v = ar->section("reconciliation");
  ASSERT_TRUE(v.has_value()) << v.error().to_string();

  // One spot value per dtype class: f64, u32, i64, dict-str.
  const auto nav = v->f64_col("model_nav");
  ASSERT_EQ(nav.size(), 2u);
  EXPECT_EQ(nav[0], 10.0);
  EXPECT_EQ(nav[1], 11.0);
  EXPECT_EQ(v->f64_col("quote_mid_coverage")[0], 0.5);
  const auto held = v->u32_col("n_held_lots");
  ASSERT_EQ(held.size(), 2u);
  EXPECT_EQ(held[1], 5u);
  EXPECT_EQ(v->u32_col("held_cohort")[0], 3u);
  EXPECT_EQ(v->i64_col("valuation_ts_ns")[1], 200);
  EXPECT_EQ(v->dict_col("date").at(1), "2026-07-12");
}

TEST(RunArchiveEncoders, ScheduleSectionRoundTrips) {
  ListedScheduleLeg idx;
  idx.roll_date = "2026-07-11";
  idx.cohort = 1;
  idx.is_index = true;
  idx.symbol = "SPY";
  idx.uid = 7;
  idx.instrument_id = 77;
  idx.raw_symbol = "SPY   260731C00500000";
  idx.expiry_ts_ns = 2000;
  idx.strike = 500.0;
  idx.side = Side::Call;
  idx.quantity = 2.0;
  idx.multiplier = 100.0;
  idx.raw_bid = 1.0;
  idx.raw_ask = 1.2;
  idx.raw_mid = 1.1;
  idx.model_mark = 1.05;
  idx.delta_per_share = 0.5;
  idx.vega_per_unit_vol = 20.0;
  idx.vega_per_contract_per_vol_point = 0.2;
  idx.normalized_weight = 1.0;
  idx.target_straddle_vega_per_vol_point = 10.0;
  idx.achieved_leg_vega_per_vol_point = 10.0;
  idx.source_fingerprint = 0xDEADBEEFCAFEBABEull;
  idx.surface_fingerprint = 0x0123456789ABCDEFull;
  ListedScheduleLeg name = idx;
  name.is_index = false;
  name.symbol = "AAPL";
  name.raw_symbol = "AAPL  260731P00200000";
  name.side = Side::Put;
  name.strike = 200.0;

  ListedScheduleRoll roll;
  roll.roll_date = "2026-07-11";
  roll.valuation_ts_ns = 1000;
  roll.cohort = 1;
  roll.expiry_ts_ns = 2000;
  roll.gross_index_vega_target_per_vol_point = 10000.0;
  roll.net_vega_per_vol_point = -1.5;
  roll.gross_vega_per_vol_point = 3.5;
  roll.n_names = 1;
  roll.legs = {idx, name};
  ListedDispersionSchedule sched;
  sched.rolls = {roll};

  const RaSectionData sec = encode_schedule_section("trade_schedule", sched);
  EXPECT_EQ(sec.name, "trade_schedule");
  EXPECT_EQ(sec.kind, RaSectionKind::SubTable);
  EXPECT_EQ(sec.n_rows, 2u); // rolls×legs flattened: one row per leg
  ASSERT_EQ(sec.columns.size(), 29u);

  auto bytes = write_run_archive(std::span(&sec, 1), 1, 1);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
  auto ar = RunArchive::open(std::move(*bytes));
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  auto v = ar->section("trade_schedule");
  ASSERT_TRUE(v.has_value()) << v.error().to_string();

  // Roll fields repeat on every leg row.
  EXPECT_EQ(v->dict_col("roll_date").at(0), "2026-07-11");
  EXPECT_EQ(v->dict_col("roll_date").at(1), "2026-07-11");
  EXPECT_EQ(v->i64_col("valuation_ts_ns")[1], 1000);
  EXPECT_EQ(v->i64_col("expiry_ts_ns")[0], 2000);
  EXPECT_EQ(v->f64_col("gross_index_vega_target")[1], 10000.0);
  EXPECT_EQ(v->u32_col("n_names")[0], 1u);

  // Leg fields: f64, u32, dict-str, and the u8+label enum columns.
  const auto strike = v->f64_col("strike");
  ASSERT_EQ(strike.size(), 2u);
  EXPECT_EQ(strike[0], 500.0);
  EXPECT_EQ(strike[1], 200.0);
  EXPECT_EQ(v->u32_col("uid")[0], 7u);
  EXPECT_EQ(v->dict_col("symbol").at(1), "AAPL");
  EXPECT_EQ(v->dict_col("raw_symbol").at(0), "SPY   260731C00500000");

  const auto side = v->u8enum_col("side");
  ASSERT_EQ(side.size(), 2u);
  EXPECT_EQ(side[0], 0u); // Side::Call
  EXPECT_EQ(side[1], 1u); // Side::Put
  const RaStringTable side_labels = v->u8enum_labels("side");
  ASSERT_EQ(side_labels.size(), 2u);
  EXPECT_EQ(side_labels.at(0), "C"); // the TSV writer's 'C'/'P' convention
  EXPECT_EQ(side_labels.at(1), "P");
  const auto is_index = v->u8enum_col("is_index");
  EXPECT_EQ(is_index[0], 1u);
  EXPECT_EQ(is_index[1], 0u);
  const RaStringTable bool_lab = v->u8enum_labels("is_index");
  EXPECT_EQ(bool_lab.at(0), "0"); // the TSV writer's '0'/'1' bool convention
  EXPECT_EQ(bool_lab.at(1), "1");

  // u64 fingerprints ride as I64 bit patterns (no U64 dtype by design).
  EXPECT_EQ(v->i64_col("source_fingerprint")[0],
            static_cast<std::int64_t>(0xDEADBEEFCAFEBABEull));
  EXPECT_EQ(v->i64_col("surface_fingerprint")[1],
            static_cast<std::int64_t>(0x0123456789ABCDEFull));
}

TEST(RunArchiveEncoders, ContractMarksSectionRoundTrips) {
  ListedContractMark ok;
  ok.date = "2026-07-11";
  ok.valuation_ts_ns = 10;
  ok.role = ListedMarkRole::Entry;
  ok.cohort = 1;
  ok.symbol = "SPY";
  ok.uid = 7;
  ok.instrument_id = 77;
  ok.raw_symbol = "SPY   260731P00500000";
  ok.expiry_ts_ns = 999;
  ok.strike = 500.0;
  ok.side = Side::Put;
  ok.quantity = -2.0;
  ok.multiplier = 100.0;
  ok.status = ListedMarkStatus::Ok;
  ok.raw_bid = 1.0;
  ok.raw_ask = 1.2;
  ok.raw_mid = 1.1;
  ok.model_mark = 1.05;
  ok.model_in_spread = true;
  ListedContractMark bad = ok;
  bad.date = "2026-07-12";
  bad.role = ListedMarkRole::Held;
  bad.status = ListedMarkStatus::NoSurface;
  bad.model_in_spread = false;
  ListedDispersionReconciliation rec;
  rec.marks = {ok, bad};

  const RaSectionData sec = encode_contract_marks_section(rec);
  EXPECT_EQ(sec.name, "contract_marks");
  EXPECT_EQ(sec.kind, RaSectionKind::SubTable);
  EXPECT_EQ(sec.n_rows, 2u);
  ASSERT_EQ(sec.columns.size(), 19u);

  auto bytes = write_run_archive(std::span(&sec, 1), 1, 1);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
  auto ar = RunArchive::open(std::move(*bytes));
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  auto v = ar->section("contract_marks");
  ASSERT_TRUE(v.has_value()) << v.error().to_string();

  // Enum columns carry the to_string() label vocabulary; codes are the C++
  // enum values, so labels[code] == to_string(value).
  const auto role = v->u8enum_col("role");
  EXPECT_EQ(role[0], 0u); // Entry
  EXPECT_EQ(role[1], 1u); // Held
  const RaStringTable role_labels = v->u8enum_labels("role");
  ASSERT_EQ(role_labels.size(), 2u);
  EXPECT_EQ(role_labels.at(0), "Entry");
  EXPECT_EQ(role_labels.at(1), "Held");
  const auto status = v->u8enum_col("status");
  const RaStringTable status_labels = v->u8enum_labels("status");
  ASSERT_EQ(status_labels.size(), 5u);
  EXPECT_EQ(status_labels.at(status[0]), "Ok");
  EXPECT_EQ(status_labels.at(status[1]), "NoSurface");
  const auto in_spread = v->u8enum_col("model_in_spread");
  EXPECT_EQ(in_spread[0], 1u);
  EXPECT_EQ(in_spread[1], 0u);
  EXPECT_EQ(v->u8enum_labels("model_in_spread").at(1), "1");

  // NA convention mirrors serialize_listed_contract_marks: raw_* is NA (NaN
  // in the pinned F64 dtype) unless status == Ok; model_mark is NA under
  // NoSurface / PricingError.
  const auto raw_mid = v->f64_col("raw_mid");
  ASSERT_EQ(raw_mid.size(), 2u);
  EXPECT_EQ(raw_mid[0], 1.1);
  EXPECT_TRUE(std::isnan(raw_mid[1]));
  EXPECT_TRUE(std::isnan(v->f64_col("raw_bid")[1]));
  EXPECT_TRUE(std::isnan(v->f64_col("raw_ask")[1]));
  const auto model_mark = v->f64_col("model_mark");
  EXPECT_EQ(model_mark[0], 1.05);
  EXPECT_TRUE(std::isnan(model_mark[1]));

  EXPECT_EQ(v->dict_col("date").at(1), "2026-07-12");
  EXPECT_EQ(v->u32_col("instrument_id")[0], 77u);
  EXPECT_EQ(v->i64_col("expiry_ts_ns")[0], 999);
  EXPECT_EQ(v->f64_col("quantity")[0], -2.0);
}

TEST(RunArchiveEncoders, MetaSectionEchoesResolvedSpec) {
  RunSpec spec;
  spec.label = "meta-test";
  spec.date_lo = "2026-07-11";
  spec.date_hi = "2026-07-12";
  const std::vector<std::pair<std::string, std::string>> extra = {
      {"input_hash", "0xabc"}, {"n_dates", "2"}};

  const RaSectionData sec = encode_meta_section(spec, extra);
  EXPECT_EQ(sec.name, "meta");
  EXPECT_EQ(sec.kind, RaSectionKind::ScalarKV);
  ASSERT_EQ(sec.columns.size(), 2u);

  auto bytes = write_run_archive(std::span(&sec, 1), 1, 1);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
  auto ar = RunArchive::open(std::move(*bytes));
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  auto v = ar->section("meta");
  ASSERT_TRUE(v.has_value()) << v.error().to_string();

  const RaDictColumn keys = v->dict_col("key");
  const RaDictColumn values = v->dict_col("value");
  ASSERT_EQ(keys.size(), values.size());
  const auto value_of = [&](std::string_view key) -> std::string_view {
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (keys.at(i) == key) return values.at(i);
    }
    return {};
  };
  // Resolved-spec echo (write_resolved_spec key vocabulary) + window.
  EXPECT_EQ(value_of("label"), "meta-test");
  EXPECT_EQ(value_of("date_lo"), "2026-07-11");
  EXPECT_EQ(value_of("date_hi"), "2026-07-12");
  EXPECT_EQ(value_of("snapshot_suffix"), "T19:55:00Z");
  EXPECT_EQ(value_of("min_names"), "10");
  EXPECT_EQ(value_of("min_weight_coverage"), "0.80000000000000004"); // %.17g
  EXPECT_EQ(value_of("gross_index_vega"), "10000");
  EXPECT_EQ(value_of("core_mode"), "0");
  // L12: the meta echo must not silently omit a resolved knob. `index_symbol`
  // is appended last, mirroring write_resolved_spec's key order.
  EXPECT_EQ(value_of("index_symbol"), "SPY");
  // Caller-supplied extra pairs ride behind the spec echo.
  EXPECT_EQ(value_of("input_hash"), "0xabc");
  EXPECT_EQ(value_of("n_dates"), "2");
}

// ── Task 5: committed Python fixture (Task 8's reader consumes this) ─────────

// Locate <repo>/atx-vol/python/tests robustly: __FILE__ is not reliably
// absolute under ninja/clang-cl, so probe upward the way earnings tests do.
std::filesystem::path python_fixture_path() {
  const std::filesystem::path tail =
      std::filesystem::path("data") / "runarchive" / "wave_a_fixture.atxrun";
  for (const char* base : {".", "..", "../..", "../../..", "../../../.."}) {
    const std::filesystem::path candidate =
        std::filesystem::path(base) / "atx-vol" / "python" / "tests";
    if (std::filesystem::exists(candidate)) return candidate / tail;
  }
  return std::filesystem::path(__FILE__).parent_path().parent_path() / "python" / "tests" /
         tail;
}

TEST(RunArchiveEncoders, MatchesCommittedPythonFixture) {
  const BacktestResult r = make_encoder_fixture_result();
  std::vector<RaSectionData> sections;
  sections.push_back(encode_backtest_section("backtest", r));

  // meta: EXACTLY the three brief-pinned pairs, staged in the encoder's
  // two-dict shape (key table / value table, codes in row order).
  static const std::vector<std::string> key_table = {"label", "date_lo", "date_hi"};
  static const std::vector<std::string> value_table = {"wave-a-fixture", "2026-07-11",
                                                       "2026-07-12"};
  static const std::vector<std::uint32_t> kv_codes = {0, 1, 2};
  RaSectionData meta;
  meta.name = "meta";
  meta.kind = RaSectionKind::ScalarKV;
  meta.n_rows = 3;
  meta.columns.emplace_back("key", RaColumnData::of_dict(kv_codes, key_table));
  meta.columns.emplace_back("value", RaColumnData::of_dict(kv_codes, value_table));
  sections.push_back(std::move(meta));

  // Determinism / freeze guard. Write to a TEMP path with the SAME pinned
  // created_ts_ns the committed fixture was written with, then assert the bytes
  // are IDENTICAL to the committed golden. This test does NOT regenerate the
  // committed fixture: an encoder or determinism regression must fail RED here,
  // not silently rewrite the frozen anchor the Python reader pins against.
  const std::filesystem::path temp_path =
      std::filesystem::temp_directory_path() / "atx_wave_a_fixture_roundtrip.atxrun";
  std::error_code ec;
  std::filesystem::remove(temp_path, ec);

  // created_ts_ns MUST be nonzero: 0 falls back to the system clock, and the
  // produced bytes must be identical to the committed fixture across reruns.
  const auto st = write_run_archive_file(temp_path.string(), sections,
                                         /*created_ts_ns=*/123456789,
                                         /*run_identity_hash=*/0xABCDEFull);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();

  // Byte-identity vs the committed golden fixture (the file itself is frozen).
  const std::filesystem::path committed = python_fixture_path();
  ASSERT_TRUE(std::filesystem::exists(committed))
      << "committed fixture missing: " << committed.string();
  const auto slurp = [](const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(in.good()) << "cannot open " << p.string();
    const std::streamsize size = in.tellg();
    std::vector<char> bytes(static_cast<std::size_t>(size < 0 ? 0 : size));
    in.seekg(0);
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return bytes;
  };
  const std::vector<char> produced = slurp(temp_path);
  const std::vector<char> golden = slurp(committed);
  ASSERT_EQ(produced.size(), golden.size()) << "fixture byte-length drift vs committed golden";
  EXPECT_TRUE(produced == golden) << "fixture byte drift vs committed golden";

  // The produced archive still round-trips (framing + values).
  auto ar = RunArchive::open_file(temp_path.string());
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  EXPECT_EQ(ar->header().created_ts_ns, 123456789u);
  EXPECT_EQ(ar->header().run_identity_hash, 0xABCDEFull);

  auto v = ar->section("backtest");
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  const auto nav = v->f64_col("nav");
  ASSERT_EQ(nav.size(), 2u);
  EXPECT_EQ(nav[0], 100.0);
  EXPECT_EQ(nav[1], 101.5);
  EXPECT_EQ(v->f64_col("pnl_vega")[0], 1.25);
  EXPECT_EQ(v->f64_col("atm_iv")[1], 0.21);
  EXPECT_EQ(v->dict_col("date").at(0), "2026-07-11");
  EXPECT_EQ(v->i64_col("ts_ns")[1], 20);

  auto mv = ar->section("meta");
  ASSERT_TRUE(mv.has_value()) << mv.error().to_string();
  EXPECT_EQ(mv->n_rows(), 3u);
  EXPECT_EQ(mv->dict_col("key").at(0), "label");
  EXPECT_EQ(mv->dict_col("value").at(0), "wave-a-fixture");
  EXPECT_EQ(mv->dict_col("key").at(2), "date_hi");
  EXPECT_EQ(mv->dict_col("value").at(2), "2026-07-12");
  EXPECT_TRUE(ar->validate_all().has_value());

  std::filesystem::remove(temp_path, ec);
}

// ── Task 7: RunDir (typed run-directory handle + verify) ─────────────────────
//
// verify() lifts the example orchestrator's acceptance gates to library level,
// so the fixtures below reconstruct a well-formed run dir with the LIBRARY
// writers: run_spec via write_resolved_spec, surface_manifest via
// write_manifest_file, trade_schedule via write_listed_dispersion_schedule_file,
// and run.atxrun via RunDir::write_run_archive.

constexpr std::int64_t kRdValuation = 1'700'000'000'000'000'000LL;
constexpr std::int64_t kRdExpiry = kRdValuation + 30 * static_cast<std::int64_t>(kListedNsPerDay);

ListedOptionQuote rd_option(const std::string& symbol, std::uint32_t id, Side side) {
  ListedOptionQuote q;
  q.trade_date = "2026-07-10";
  q.symbol = symbol;
  q.instrument_id = id;
  q.raw_symbol = symbol + std::to_string(id);
  q.expiry_ts_ns = kRdExpiry;
  q.strike = symbol == "SPY" ? 500.0 : 100.0;
  q.side = side;
  q.bid = 2.0 + static_cast<double>(id) * 0.01;
  q.ask = q.bid + 0.2;
  q.quote_ts_ns = kRdValuation;
  q.multiplier = 100.0;
  q.standard_monthly = true;
  q.standard_deliverable = true;
  q.source_fingerprint = 1000u + id;
  return q;
}

ListedStraddle rd_straddle(std::string symbol, std::uint32_t uid, std::uint32_t id,
                           double raw_weight, double normalized_weight) {
  ListedStraddle s;
  s.symbol = std::move(symbol);
  s.uid = uid;
  s.expiry_ts_ns = kRdExpiry;
  s.strike = s.symbol == "SPY" ? 500.0 : 100.0;
  s.call = rd_option(s.symbol, id, Side::Call);
  s.put = rd_option(s.symbol, id + 1, Side::Put);
  s.raw_weight = raw_weight;
  s.normalized_weight = normalized_weight;
  return s;
}

// A schedule with one valid roll (index straddle + one name straddle), built
// through build_listed_dispersion_roll so it satisfies the vega arithmetic that
// validate_listed_dispersion_schedule (called inside verify) re-checks. The
// recipe mirrors listed_dispersion_schedule_test.cpp.
ListedDispersionSchedule make_valid_schedule() {
  ListedDispersionSelection sel;
  sel.trade_date = "2026-07-10";
  sel.valuation_ts_ns = kRdValuation;
  sel.expiry_ts_ns = kRdExpiry;
  sel.dte_days = 30.0;
  sel.index = rd_straddle("SPY", 1u, 1u, 0.0, 0.0);
  sel.names.push_back(rd_straddle("N0", 2u, 3u, 1.0, 1.0));

  const ListedRiskLookup risks = [](std::uint32_t uid,
                                    const ListedOptionQuote& q) -> Result<ListedOptionRisk> {
    double vega = 0.0;
    if (uid == 1u) {
      vega = 10.0;
    } else if (uid == 2u) {
      vega = q.side == Side::Call ? 8.0 : 12.0;
    } else {
      return atx::core::Err(atx::core::ErrorCode::NotFound, "missing risk");
    }
    const double delta = q.side == Side::Call ? 0.55 : -0.45;
    return atx::core::Ok(ListedOptionRisk{0.5 * (q.bid + q.ask), delta, vega});
  };

  ListedScheduleBuildConfig cfg;
  cfg.gross_index_vega_target_per_vol_point = 10000.0;
  cfg.cohort = 1u;
  cfg.surface_fingerprint = 987654321u;
  auto roll = build_listed_dispersion_roll(sel, risks, cfg);
  EXPECT_TRUE(roll.has_value()) << (roll ? "" : roll.error().to_string());
  ListedDispersionSchedule schedule;
  if (roll) {
    schedule.rolls.push_back(std::move(*roll));
  }
  return schedule;
}

// A manifest with `n_dates` Ok dates -> Clock::from_manifest yields n_dates refs.
CorpusManifest make_manifest(std::size_t n_dates) {
  CorpusManifest manifest;
  for (std::size_t i = 0; i < n_dates; ++i) {
    char date[16] = {};
    std::snprintf(date, sizeof date, "2026-07-%02zu", i + 1);
    manifest.dates.emplace_back(date);
    CorpusEntry entry;
    entry.date = date;
    entry.symbol = "SPY";
    entry.status = CorpusFitStatus::Ok;
    entry.n_slices = 1u;
    entry.archive_path = std::string(date) + ".atxvsa";
    manifest.entries.push_back(std::move(entry));
  }
  manifest.n_boards = static_cast<std::uint32_t>(n_dates);
  manifest.n_ok = static_cast<std::uint32_t>(n_dates);
  return manifest;
}

// `n_rows` reconciliation rows + `n_rows` contract marks (encode_* build the
// sections). The default (2) reproduces the original fixture byte-for-byte
// ("2026-07-11", "2026-07-12"); a caller can request a different count to drive
// verify()'s backtest/reconciliation cardinality cross-check off its match.
ListedDispersionReconciliation make_reconciliation(int n_rows = 2) {
  ListedDispersionReconciliation rec;
  for (int i = 0; i < n_rows; ++i) {
    char date[16] = {};
    std::snprintf(date, sizeof date, "2026-07-%02d", i + 11);
    ListedReconciliationRow row;
    row.date = date;
    row.valuation_ts_ns = 100 + i;
    row.held_cohort = 1;
    row.model_option_pnl = 1.0 + i;
    row.quote_mid_pnl = 1.0;
    row.model_minus_quote_pnl = static_cast<double>(i);
    row.model_nav = 10.0 + i;
    row.quote_mid_nav = 9.0;
    row.quote_mid_coverage = 0.5;
    row.n_held_lots = 2;
    row.n_quote_mid_lots = 1;
    rec.rows.push_back(row);

    ListedContractMark mark;
    mark.date = row.date;
    mark.valuation_ts_ns = row.valuation_ts_ns;
    mark.role = ListedMarkRole::Entry;
    mark.cohort = 1;
    mark.symbol = "SPY";
    mark.uid = 7;
    mark.instrument_id = 77;
    mark.raw_symbol = "SPY   260731C00500000";
    mark.expiry_ts_ns = 999;
    mark.strike = 500.0;
    mark.side = Side::Call;
    mark.quantity = 1.0;
    mark.multiplier = 100.0;
    mark.status = ListedMarkStatus::Ok;
    mark.raw_bid = 1.0;
    mark.raw_ask = 1.2;
    mark.raw_mid = 1.1;
    mark.model_mark = 1.05;
    mark.model_in_spread = true;
    rec.marks.push_back(mark);
  }
  return rec;
}

// Write the retained TEXT inputs of a run dir via the library writers.
void write_run_dir_text_inputs(const std::filesystem::path& dir, bool core_mode,
                               std::size_t n_dates) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  ASSERT_FALSE(ec) << ec.message();

  RunSpec spec;
  spec.label = "rundir-test";
  spec.date_lo = "2026-07-10";
  spec.date_hi = "2026-07-10";
  spec.opra_root = "/data/opra";
  spec.universe_path = dir / std::string(kUniverseScheduleFile);
  // read_run_spec rejects a core-mode spec with < 40 names / < 80% coverage, so
  // keep the spec valid in both modes.
  spec.min_names = 40;
  spec.min_weight_coverage = 0.8;
  spec.core_mode = core_mode;
  ASSERT_TRUE(write_resolved_spec(dir / std::string(kRunSpecFile), spec).has_value());

  // universe_schedule.tsv is folded into the run identity (not parsed by RunDir).
  {
    std::ofstream u(dir / std::string(kUniverseScheduleFile), std::ios::binary | std::ios::trunc);
    u << "effective_date\tsymbol\traw_weight\tsource\tas_of\n"
      << "2026-07-10\tSPY\t1.0\tauthored\t2026-07-10\n";
    ASSERT_TRUE(u.good());
  }

  ASSERT_TRUE(write_manifest_file((dir / std::string(kSurfaceManifestFile)).string(),
                                  make_manifest(n_dates))
                  .has_value());
  ASSERT_TRUE(write_listed_dispersion_schedule_file(
                  (dir / std::string(kTradeScheduleFile)).string(), make_valid_schedule())
                  .has_value());
}

// Append one byte to an existing run-dir file — a minimal byte mutation. None of
// these files is re-parsed by run_identity_hash (it hashes raw bytes), so the
// appended byte cannot fail a parser instead of moving the hash.
void append_identity_probe_byte(const std::filesystem::path& path) {
  std::ofstream out(path, std::ios::binary | std::ios::app);
  out.put('#');
  ASSERT_TRUE(out.good()) << "cannot append to " << path.string();
}

// All five folded inputs plus one deliberately-unfolded run-dir file.
// write_run_dir_text_inputs supplies run_spec / universe_schedule /
// surface_manifest / trade_schedule through the library writers;
// input_inventory.tsv has no library writer (write_input_inventory lives in the
// example orchestrator), so a representative header + row is written literally.
void write_run_dir_folded_inputs(const std::filesystem::path& dir) {
  write_run_dir_text_inputs(dir, /*core_mode=*/false, /*n_dates=*/1);
  {
    std::ofstream inv(dir / std::string(kInputInventoryFile), std::ios::binary | std::ios::trunc);
    inv << "date\tsymbol\tn_rows\tsource_fingerprint\tmarket_input_fingerprint\n"
        << "2026-07-10\tSPY\t128\t1111111111\t2222222222\n";
    ASSERT_TRUE(inv.good());
  }
  // Not a folded input — the negative control below mutates this one.
  {
    std::ofstream q(dir / "quality.tsv", std::ios::binary | std::ios::trunc);
    q << "date\tstatus\n2026-07-10\tOk\n";
    ASSERT_TRUE(q.good());
  }
}

// Write run.atxrun via RunDir.
void write_run_dir_archive(const std::filesystem::path& dir) {
  const BacktestResult r = make_encoder_fixture_result();  // 2 rows
  const ListedDispersionReconciliation rec = make_reconciliation(); // 2 rows + 2 marks
  RunSpec spec;
  spec.label = "rundir-test";
  spec.date_lo = "2026-07-10";
  spec.date_hi = "2026-07-10";

  std::vector<RaSectionData> sections;
  sections.push_back(encode_backtest_section("backtest", r));  // arena-owned
  sections.push_back(encode_reconciliation_section(rec));      // arena-owned
  sections.push_back(encode_contract_marks_section(rec));      // arena-owned
  sections.push_back(encode_meta_section(spec));               // arena-owned

  const RunDir run_dir(dir);
  const auto st = run_dir.write_run_archive(sections);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
}

std::filesystem::path fresh_run_dir(std::string_view name) {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  return dir;
}

// Portable text inputs for the byte-determinism test: run_spec.tsv carries a
// dir-INDEPENDENT universe path (relative, not `dir / ...`), so run_spec.tsv is
// byte-identical across two different run-dir paths. run_identity_hash folds the
// run_spec + universe bytes here (the other three folded inputs are absent and so
// skipped), so identical bytes -> identical identity -> (with the T7 deterministic
// created_ts_ns) byte-identical run.atxrun. Only these two of the five folded
// inputs are written — which also exercises the skip-if-absent rule.
void write_run_dir_text_inputs_portable(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  ASSERT_FALSE(ec) << ec.message();

  RunSpec spec;
  spec.label = "rundir-determinism";
  spec.date_lo = "2026-07-10";
  spec.date_hi = "2026-07-10";
  spec.opra_root = "/data/opra";
  spec.universe_path = std::filesystem::path(std::string(kUniverseScheduleFile)); // dir-independent
  spec.min_names = 40;
  spec.min_weight_coverage = 0.8;
  spec.core_mode = false;
  ASSERT_TRUE(write_resolved_spec(dir / std::string(kRunSpecFile), spec).has_value());

  std::ofstream u(dir / std::string(kUniverseScheduleFile), std::ios::binary | std::ios::trunc);
  u << "effective_date\tsymbol\traw_weight\tsource\tas_of\n"
    << "2026-07-10\tSPY\t1.0\tauthored\t2026-07-10\n";
  ASSERT_TRUE(u.good());
}

// Like write_run_dir_archive, but the reconciliation (and contract_marks)
// section carries 3 rows while the backtest section carries 2 — so the archive
// trips verify()'s count gate (backtest.n_rows() != reconciliation.n_rows()).
void write_run_dir_archive_row_mismatch(const std::filesystem::path& dir) {
  const BacktestResult r = make_encoder_fixture_result();             // 2 rows
  const ListedDispersionReconciliation rec = make_reconciliation(3);  // 3 rows (!= 2)
  RunSpec spec;
  spec.label = "rundir-test";
  spec.date_lo = "2026-07-10";
  spec.date_hi = "2026-07-10";

  std::vector<RaSectionData> sections;
  sections.push_back(encode_backtest_section("backtest", r));  // arena-owned
  sections.push_back(encode_reconciliation_section(rec));      // arena-owned
  sections.push_back(encode_contract_marks_section(rec));      // arena-owned
  sections.push_back(encode_meta_section(spec));               // arena-owned

  const RunDir run_dir(dir);
  const auto st = run_dir.write_run_archive(sections);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
}

TEST(RunDir, ReadsInputsAndArchive) {
  const std::filesystem::path dir = fresh_run_dir("atx_rundir_inputs_test");
  write_run_dir_text_inputs(dir, /*core_mode=*/false, /*n_dates=*/1);
  write_run_dir_archive(dir);

  const RunDir run_dir(dir);
  EXPECT_EQ(run_dir.path(), dir);
  EXPECT_EQ(run_dir.archive_path(), dir / "run.atxrun");

  // Retained text inputs read back through the library parsers.
  auto spec = run_dir.spec();
  ASSERT_TRUE(spec.has_value()) << spec.error().to_string();
  EXPECT_EQ(spec->date_lo, "2026-07-10");
  auto clock = run_dir.clock();
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  EXPECT_EQ(clock->size(), 1u);
  auto schedule = run_dir.schedule();
  ASSERT_TRUE(schedule.has_value()) << schedule.error().to_string();
  EXPECT_EQ(schedule->rolls.size(), 1u);

  // The binary result container opens (mapped) with the stamped identity.
  auto id = run_dir.run_identity_hash();
  ASSERT_TRUE(id.has_value()) << id.error().to_string();
  EXPECT_NE(*id, 0u);
  EXPECT_EQ(run_dir.run_identity_hash().value(), *id); // deterministic
  auto archive = run_dir.archive();
  ASSERT_TRUE(archive.has_value()) << archive.error().to_string();
  EXPECT_EQ(archive->header().run_identity_hash, *id);
  EXPECT_EQ(archive->section("backtest").value().f64_col("nav")[1], 101.5);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(RunDir, VerifyPassesOnWellFormedRun) {
  const std::filesystem::path dir = fresh_run_dir("atx_rundir_verify_ok_test");
  write_run_dir_text_inputs(dir, /*core_mode=*/false, /*n_dates=*/1);
  write_run_dir_archive(dir);

  const RunDir run_dir(dir);
  const auto st = run_dir.verify();
  EXPECT_TRUE(st.has_value()) << st.error().to_string();

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(RunDir, VerifyFailsOnCorruptedSectionCrc) {
  const std::filesystem::path dir = fresh_run_dir("atx_rundir_verify_crc_test");
  write_run_dir_text_inputs(dir, /*core_mode=*/false, /*n_dates=*/1);
  write_run_dir_archive(dir);
  const std::filesystem::path archive_path = dir / "run.atxrun";

  // Flip the last byte of the first section's payload extent (sorted first ->
  // "backtest"), exactly the tamper LazyCrcCatchesPayloadTamper exercises. The
  // framing / header / metadata CRCs are untouched, so open_mapped still
  // succeeds; verify() must fail at the lazy per-section validate_all gate.
  std::vector<char> bytes;
  {
    std::ifstream in(archive_path, std::ios::binary | std::ios::ate);
    ASSERT_TRUE(in.good());
    const std::streamsize size = in.tellg();
    bytes.resize(static_cast<std::size_t>(size));
    in.seekg(0);
    in.read(bytes.data(), size);
    ASSERT_EQ(in.gcount(), size);
  }
  RunArchiveHeader h{};
  std::memcpy(&h, bytes.data(), sizeof h);
  RaSectionDescriptor d0{};
  std::memcpy(&d0, bytes.data() + h.section_dir_offset, sizeof d0);
  ASSERT_EQ(std::string_view(d0.name, 8), "backtest");
  bytes.at(static_cast<std::size_t>(d0.section_offset + d0.section_size - 1)) ^= static_cast<char>(0xFF);
  {
    std::ofstream out(archive_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(out.good());
  }

  const RunDir run_dir(dir);
  const auto st = run_dir.verify();
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), atx::core::ErrorCode::ParseError);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(RunDir, VerifyFailsOnMissingArchive) {
  const std::filesystem::path dir = fresh_run_dir("atx_rundir_verify_missing_test");
  write_run_dir_text_inputs(dir, /*core_mode=*/false, /*n_dates=*/1);
  // Deliberately DO NOT write run.atxrun.
  const RunDir run_dir(dir);
  EXPECT_FALSE(run_dir.verify().has_value());
  EXPECT_FALSE(run_dir.archive().has_value());

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(RunDir, VerifyEnforcesCoreModeGate) {
  // core_mode with only 1 admitted date fails the >= 60-date acceptance floor.
  const std::filesystem::path dir = fresh_run_dir("atx_rundir_verify_core_test");
  write_run_dir_text_inputs(dir, /*core_mode=*/true, /*n_dates=*/1);
  write_run_dir_archive(dir);

  const RunDir run_dir(dir);
  const auto st = run_dir.verify();
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), atx::core::ErrorCode::Unavailable);

  // The same run passes when the core-mode gate is disabled by policy.
  RunVerifyOptions relaxed;
  relaxed.enforce_core_mode = false;
  EXPECT_TRUE(run_dir.verify(relaxed).has_value());

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// ── Task 7 (Minor #9): verify count-gate negative path ───────────────────────
//
// verify() cross-checks that the backtest and reconciliation sections carry the
// SAME row count (the per-session cardinality). An archive whose two sections
// disagree must be REJECTED with InvalidArgument — not crash, not pass. This
// locks the existing gate (it had no mismatch-path coverage before).
TEST(RunDir, VerifyRejectsCountGateMismatch) {
  const std::filesystem::path dir = fresh_run_dir("atx_rundir_verify_countgate_test");
  write_run_dir_text_inputs(dir, /*core_mode=*/false, /*n_dates=*/1);
  write_run_dir_archive_row_mismatch(dir);  // backtest=2 rows, reconciliation=3 rows

  const RunDir run_dir(dir);
  const auto st = run_dir.verify();
  ASSERT_FALSE(st.has_value()) << "count-gate mismatch should fail verify()";
  EXPECT_EQ(st.error().code(), atx::core::ErrorCode::InvalidArgument);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// ── Task 7 (Minor #7): deterministic created_ts_ns => byte-identical archive ──
//
// RunDir::write_run_archive derives created_ts_ns deterministically from the run
// identity (a content-derived pseudo-timestamp), NOT the wall clock. Two writes
// of the SAME run-dir inputs therefore produce a byte-identical run.atxrun. This
// was impossible before T7 (the writer stamped the system clock, so the header's
// created_ts_ns + header_crc32c varied run-to-run). Written to two DIFFERENT dirs
// so merge-write does not carry the first write's sections into the second.
TEST(RunDir, WriteIsByteDeterministic) {
  const auto slurp = [](const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(in.good()) << "cannot open " << p.string();
    const std::streamsize size = in.tellg();
    std::vector<char> bytes(static_cast<std::size_t>(size < 0 ? 0 : size));
    in.seekg(0);
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return bytes;
  };

  const std::filesystem::path dir_a = fresh_run_dir("atx_rundir_determinism_a");
  write_run_dir_text_inputs_portable(dir_a);
  write_run_dir_archive(dir_a);

  const std::filesystem::path dir_b = fresh_run_dir("atx_rundir_determinism_b");
  write_run_dir_text_inputs_portable(dir_b);
  write_run_dir_archive(dir_b);

  const std::vector<char> a = slurp(dir_a / "run.atxrun");
  const std::vector<char> b = slurp(dir_b / "run.atxrun");
  ASSERT_FALSE(a.empty());
  ASSERT_EQ(a.size(), b.size()) << "run.atxrun byte-length differs across identical-input writes";
  EXPECT_TRUE(a == b)
      << "run.atxrun bytes differ across identical-input writes (created_ts_ns not deterministic?)";

  // The stamped created_ts_ns IS the run identity (not 0 / wall-clock), and the
  // identity round-trips through the header.
  auto id = RunDir(dir_a).run_identity_hash();
  ASSERT_TRUE(id.has_value()) << id.error().to_string();
  RunArchiveHeader h{};
  ASSERT_GE(a.size(), sizeof h);
  std::memcpy(&h, a.data(), sizeof h);
  EXPECT_EQ(h.run_identity_hash, *id);
  EXPECT_EQ(h.created_ts_ns, *id);  // created_ts_ns := static_cast<int64>(identity), bits preserved

  std::error_code ec;
  std::filesystem::remove_all(dir_a, ec);
  std::filesystem::remove_all(dir_b, ec);
}

// ── Task 7 / I1: merge-write publish (accumulate the UNION across routes) ─────

// Helper: read one meta value by key out of an opened archive's meta section.
std::string meta_value_of(RunArchive& archive, std::string_view key) {
  auto meta = archive.section("meta");
  if (!meta) return {};
  const RaDictColumn keys = meta->dict_col("key");
  const RaDictColumn values = meta->dict_col("value");
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (keys.at(i) == key) return std::string(values.at(i));
  }
  return {};
}

void write_projected_only(const RunDir& run_dir) {
  const BacktestResult projected = make_encoder_fixture_result();
  RunSpec spec;
  spec.label = "projected-run";
  spec.date_lo = "2026-07-10";
  spec.date_hi = "2026-07-10";
  std::vector<RaSectionData> sections;
  sections.push_back(encode_backtest_section("projected_cold", projected));
  sections.push_back(encode_meta_section(spec));
  const Status status = run_dir.write_run_archive(sections);
  ASSERT_TRUE(status.has_value()) << status.error().to_string();
}

void expect_projected_write_started_fresh(const std::filesystem::path& dir) {
  auto archive = RunDir(dir).archive();
  ASSERT_TRUE(archive.has_value()) << archive.error().to_string();
  EXPECT_EQ(archive->count(), 2u);
  EXPECT_TRUE(archive->section("projected_cold").has_value());
  EXPECT_TRUE(archive->section("meta").has_value());
  EXPECT_FALSE(archive->section("backtest").has_value());
  EXPECT_FALSE(archive->section("reconciliation").has_value());
  EXPECT_FALSE(archive->section("contract_marks").has_value());
}

// (a) A projected-style write into a run dir that already holds a listed-style
// archive (SAME inputs) yields the UNION: the listed sections are carried
// forward with their values intact, the projected section is added, and the
// colliding meta is the NEW one (new-wins).
//
// Wave E T6 fix 1: the run dir is now populated with ALL FIVE folded inputs (via
// write_run_dir_folded_inputs) rather than four, so this test also covers the
// widened cache key's stability — it fails if the widening ever turns a
// same-inputs merge into a fresh write. It is a POSITIVE CONTROL for T6 (green
// before and after the widening) and is not counted as a gate for it; the T6
// gates are the two RED tests further down.
TEST(RunDir, MergeWriteCarriesUnsupersededSectionsOnSameInputs) {
  const std::filesystem::path dir = fresh_run_dir("atx_rundir_mergewrite_union_test");
  write_run_dir_folded_inputs(dir);
  write_run_dir_archive(dir);  // listed set: backtest, reconciliation, contract_marks, meta

  const RunDir run_dir(dir);

  // Projected-style write, unchanged inputs: a new section name (projected_cold)
  // + a colliding meta carrying a projected-only label/key.
  const BacktestResult pr = make_encoder_fixture_result();  // outlives the write
  RunSpec proj_spec;
  proj_spec.label = "projected-run";
  proj_spec.date_lo = "2026-07-10";
  proj_spec.date_hi = "2026-07-10";
  const std::vector<std::pair<std::string, std::string>> proj_meta_extra = {
      {"projected_execution", "cold"}};
  std::vector<RaSectionData> proj_sections;
  proj_sections.push_back(encode_backtest_section("projected_cold", pr));
  proj_sections.push_back(encode_meta_section(proj_spec, proj_meta_extra));
  ASSERT_TRUE(run_dir.write_run_archive(proj_sections).has_value());

  auto archive = run_dir.archive();
  ASSERT_TRUE(archive.has_value()) << archive.error().to_string();

  // The UNION is present: listed sections carried + projected section added.
  EXPECT_TRUE(archive->section("backtest").has_value());        // carried
  EXPECT_TRUE(archive->section("reconciliation").has_value());  // carried
  EXPECT_TRUE(archive->section("contract_marks").has_value());  // carried
  EXPECT_TRUE(archive->section("projected_cold").has_value());  // new
  EXPECT_TRUE(archive->section("meta").has_value());            // new-wins
  EXPECT_EQ(archive->count(), 5u);

  // Carried backtest values are intact (bit-exact through the deep copy).
  auto bt = archive->section("backtest");
  ASSERT_TRUE(bt.has_value()) << bt.error().to_string();
  EXPECT_EQ(bt->f64_col("nav")[1], 101.5);
  EXPECT_EQ(bt->f64_col("pnl_vega")[0], 1.25);
  EXPECT_EQ(bt->f64_col("atm_iv")[1], 0.21);  // dynamically-appended signal survives
  EXPECT_EQ(bt->dict_col("date").at(0), "2026-07-11");
  EXPECT_EQ(bt->i64_col("ts_ns")[1], 20);

  // Carried reconciliation intact.
  auto rec = archive->section("reconciliation");
  ASSERT_TRUE(rec.has_value()) << rec.error().to_string();
  EXPECT_EQ(rec->n_rows(), 2u);

  // meta is the NEW one (new-wins on the name collision).
  EXPECT_EQ(meta_value_of(*archive, "label"), "projected-run");
  EXPECT_EQ(meta_value_of(*archive, "projected_execution"), "cold");

  // Every section (carried + new) survives the layered CRC.
  EXPECT_TRUE(archive->validate_all().has_value());

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// (b) Mutating run_spec.tsv between writes changes the run_identity_hash, so the
// stale listed sections are DROPPED — only the new set survives.
TEST(RunDir, MergeWriteDropsCarriedSectionsWhenInputsChange) {
  const std::filesystem::path dir = fresh_run_dir("atx_rundir_mergewrite_stale_test");
  write_run_dir_text_inputs(dir, /*core_mode=*/false, /*n_dates=*/1);
  write_run_dir_archive(dir);  // listed set

  // Mutate run_spec.tsv -> its bytes change -> run_identity_hash changes.
  {
    std::ofstream s(dir / std::string(kRunSpecFile), std::ios::binary | std::ios::app);
    s << "# mutated inputs\n";
    ASSERT_TRUE(s.good());
  }

  const RunDir run_dir(dir);
  const BacktestResult pr = make_encoder_fixture_result();  // outlives the write
  std::vector<RaSectionData> proj_sections;
  proj_sections.push_back(encode_backtest_section("projected_cold", pr));
  {
    RunSpec proj_spec;
    proj_spec.label = "projected-run";
    proj_spec.date_lo = "2026-07-10";
    proj_spec.date_hi = "2026-07-10";
    proj_sections.push_back(encode_meta_section(proj_spec));
  }
  ASSERT_TRUE(run_dir.write_run_archive(proj_sections).has_value());

  auto archive = run_dir.archive();
  ASSERT_TRUE(archive.has_value()) << archive.error().to_string();

  // Only the NEW set survives; stale listed sections are gone.
  EXPECT_EQ(archive->count(), 2u);
  EXPECT_TRUE(archive->section("projected_cold").has_value());
  EXPECT_TRUE(archive->section("meta").has_value());
  EXPECT_FALSE(archive->section("backtest").has_value());
  EXPECT_FALSE(archive->section("reconciliation").has_value());
  EXPECT_FALSE(archive->section("contract_marks").has_value());

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(RunDir, MergeWriteDropsCarriedSectionsWhenManifestChanges) {
  const std::filesystem::path dir = fresh_run_dir("atx_rundir_manifest_dependency_test");
  write_run_dir_text_inputs(dir, /*core_mode=*/false, /*n_dates=*/1);
  write_run_dir_archive(dir);

  ASSERT_TRUE(write_manifest_file((dir / std::string(kSurfaceManifestFile)).string(),
                                  make_manifest(2))
                  .has_value());
  write_projected_only(RunDir(dir));
  expect_projected_write_started_fresh(dir);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(RunDir, MergeWriteDropsCarriedSectionsWhenTradeScheduleChanges) {
  const std::filesystem::path dir = fresh_run_dir("atx_rundir_schedule_dependency_test");
  write_run_dir_text_inputs(dir, /*core_mode=*/false, /*n_dates=*/1);
  write_run_dir_archive(dir);

  {
    std::ofstream schedule(dir / std::string(kTradeScheduleFile),
                           std::ios::binary | std::ios::app);
    schedule << '\n';
    ASSERT_TRUE(schedule.good());
  }
  write_projected_only(RunDir(dir));
  expect_projected_write_started_fresh(dir);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(RunDir, MergeWriteDropsCarriedSectionsWhenShareDividendsChange) {
  const std::filesystem::path dir = fresh_run_dir("atx_rundir_dividend_dependency_test");
  write_run_dir_text_inputs(dir, /*core_mode=*/false, /*n_dates=*/1);
  {
    std::ofstream dividends(dir / std::string(kShareDividendsFile),
                            std::ios::binary | std::ios::trunc);
    dividends << "ATX_SHARE_DIVIDENDS\t1\nfirst-generation\n";
    ASSERT_TRUE(dividends.good());
  }
  write_run_dir_archive(dir);

  {
    std::ofstream dividends(dir / std::string(kShareDividendsFile),
                            std::ios::binary | std::ios::trunc);
    dividends << "ATX_SHARE_DIVIDENDS\t1\nsecond-generation\n";
    ASSERT_TRUE(dividends.good());
  }
  write_projected_only(RunDir(dir));
  expect_projected_write_started_fresh(dir);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(RunDir, MergeWriteDropsCarriedSectionsWhenSurfaceArchiveIdentityChanges) {
  const std::filesystem::path dir = fresh_run_dir("atx_rundir_archive_dependency_test");
  write_run_dir_text_inputs(dir, /*core_mode=*/false, /*n_dates=*/1);

  const std::filesystem::path surface = dir / "2026-07-01.atxvsa";
  ArchiveV2Header header{};
  std::memcpy(header.magic, "ATXVSA20", 8);
  header.file_size = sizeof header;
  header.header_size = sizeof header;
  {
    std::ofstream out(surface, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(&header), sizeof header);
    ASSERT_TRUE(out.good());
  }
  write_run_dir_archive(dir);

  ++header.metadata_crc32c;
  {
    std::ofstream out(surface, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(&header), sizeof header);
    ASSERT_TRUE(out.good());
  }
  write_projected_only(RunDir(dir));
  expect_projected_write_started_fresh(dir);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// (c) A pre-existing corrupt run.atxrun is not readable, so the write succeeds
// FRESH (no throw), replacing the corrupt file with only the new sections.
TEST(RunDir, MergeWriteStartsFreshOnCorruptExistingArchive) {
  const std::filesystem::path dir = fresh_run_dir("atx_rundir_mergewrite_corrupt_test");
  write_run_dir_text_inputs(dir, /*core_mode=*/false, /*n_dates=*/1);

  // Plant a corrupt run.atxrun (not a valid archive: bad magic + short).
  const std::filesystem::path archive_path = dir / std::string(kRunArchiveFile);
  {
    std::ofstream out(archive_path, std::ios::binary | std::ios::trunc);
    const char junk[] = "NOTATXRUN garbage bytes -- not a valid archive at all ....";
    out.write(junk, static_cast<std::streamsize>(sizeof junk));
    ASSERT_TRUE(out.good());
  }

  const RunDir run_dir(dir);
  const BacktestResult r = make_encoder_fixture_result();
  const ListedDispersionReconciliation rec = make_reconciliation();
  RunSpec spec;
  spec.label = "rundir-test";
  spec.date_lo = "2026-07-10";
  spec.date_hi = "2026-07-10";
  std::vector<RaSectionData> sections;
  sections.push_back(encode_backtest_section("backtest", r));
  sections.push_back(encode_reconciliation_section(rec));
  sections.push_back(encode_contract_marks_section(rec));
  sections.push_back(encode_meta_section(spec));

  // Must succeed (fresh container), not error or throw on the corrupt file.
  const auto st = run_dir.write_run_archive(sections);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();

  auto archive = run_dir.archive();
  ASSERT_TRUE(archive.has_value()) << archive.error().to_string();
  EXPECT_EQ(archive->count(), 4u);
  EXPECT_TRUE(archive->section("backtest").has_value());
  EXPECT_TRUE(archive->section("meta").has_value());
  EXPECT_TRUE(archive->validate_all().has_value());

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// ── Wave E T6: the merge-write cache key folds EVERY input fingerprint file ───
//
// RunDir::write_run_archive's identity guard IS a cache: on a match it carries
// another route's sections forward across a process boundary. Before Wave E T6
// the key folded only run_spec.tsv + universe_schedule.tsv, so a rebuilt corpus
// (surface_manifest.tsv), a re-fingerprinted OPRA input set (input_inventory.tsv)
// or a rebuilt schedule (trade_schedule.tsv) did NOT invalidate the merge — the
// archive could union a `backtest` computed from one input set with a
// `projected_cold` computed from another. The tests below are the gate on that:
//   * RunIdentityIsSensitiveToEachFoldedInput — per-file sensitivity of the key;
//   * MergeWriteRejectsCarryForwardAfterManifestChange — the end-to-end
//     consequence (no stale carry-forward after a corpus change);
//   * MergeWriteDropsCarriedSectionsWhenAFoldedInputAppearsLate — the same, for a
//     folded input going ABSENT -> PRESENT between two writes, which is the
//     cross-file ordering invariant the pipeline now depends on;
//   * RunIdentityIsDeliberatelyBlindToDefinitionsContent — NOT a gate: it records
//     the one input that is deliberately NOT folded, as a known limitation.
// The union-survival positive control lives in
// MergeWriteCarriesUnsupersededSectionsOnSameInputs above (all five folded inputs
// present and unchanged => the merge still unions).

// Per-file sensitivity of the cache key. run_spec.tsv and universe_schedule.tsv
// were already folded, so those two legs are POSITIVE CONTROLS (they pass before
// the T6 change); the surface_manifest / input_inventory / trade_schedule legs
// are the RED. The quality.tsv leg is the NEGATIVE CONTROL — it proves the
// comparison can report "unchanged", so an EXPECT_NE that fires is meaningful.
TEST(RunDir, RunIdentityIsSensitiveToEachFoldedInput) {
  const std::filesystem::path dir = fresh_run_dir("atx_rundir_identity_fold_test");
  write_run_dir_folded_inputs(dir);
  const RunDir run_dir(dir);

  constexpr std::string_view kFolded[] = {kRunSpecFile, kUniverseScheduleFile, kSurfaceManifestFile,
                                          kInputInventoryFile, kTradeScheduleFile};

  // Anti-vacuity: every folded input actually exists and is NON-EMPTY, so no leg
  // can "pass" by hashing an absent or zero-byte file.
  for (const std::string_view name : kFolded) {
    const std::filesystem::path p = dir / std::string(name);
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::is_regular_file(p, ec)) << p.string();
    ASSERT_GT(std::filesystem::file_size(p, ec), 0u) << p.string();
  }

  auto base = run_dir.run_identity_hash();
  ASSERT_TRUE(base.has_value()) << base.error().to_string();
  ASSERT_NE(*base, 0u);
  ASSERT_EQ(run_dir.run_identity_hash().value(), *base) << "identity is not deterministic";

  // NEGATIVE CONTROL: quality.tsv is not part of the run identity.
  append_identity_probe_byte(dir / "quality.tsv");
  EXPECT_EQ(run_dir.run_identity_hash().value(), *base)
      << "identity moved on a file that is not one of the folded inputs";

  // Every folded input must move the key. Mutations accumulate, so each leg is
  // compared against the value produced by the previous leg.
  std::uint64_t prev = *base;
  for (const std::string_view name : kFolded) {
    append_identity_probe_byte(dir / std::string(name));
    auto next = run_dir.run_identity_hash();
    ASSERT_TRUE(next.has_value()) << next.error().to_string();
    EXPECT_NE(*next, prev) << "run identity is blind to a change in " << name;
    EXPECT_NE(*next, 0u) << "identity must stay nonzero (0 == unset in the header)";
    prev = *next;
  }

  // Skip-if-absent: a partially-populated run dir (only the required run_spec)
  // still yields a nonzero identity rather than an error.
  const std::filesystem::path bare = fresh_run_dir("atx_rundir_identity_bare_test");
  {
    std::error_code ec;
    std::filesystem::create_directories(bare, ec);
    ASSERT_FALSE(ec) << ec.message();
    RunSpec spec;
    spec.label = "bare";
    spec.date_lo = "2026-07-10";
    spec.date_hi = "2026-07-10";
    spec.opra_root = "/data/opra";
    spec.universe_path = std::filesystem::path(std::string(kUniverseScheduleFile));
    spec.min_names = 40;
    spec.min_weight_coverage = 0.8;
    ASSERT_TRUE(write_resolved_spec(bare / std::string(kRunSpecFile), spec).has_value());
  }
  auto bare_id = RunDir(bare).run_identity_hash();
  ASSERT_TRUE(bare_id.has_value()) << bare_id.error().to_string();
  EXPECT_NE(*bare_id, 0u);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::remove_all(bare, ec);
}

// End-to-end consequence: a rebuilt corpus (surface_manifest.tsv changed) must
// invalidate the merge, so a later route's write does NOT carry the earlier
// route's economically-stale sections forward. run_spec.tsv and
// universe_schedule.tsv are untouched — exactly the case the pre-T6 key missed.
TEST(RunDir, MergeWriteRejectsCarryForwardAfterManifestChange) {
  const std::filesystem::path dir = fresh_run_dir("atx_rundir_mergewrite_manifest_test");
  write_run_dir_folded_inputs(dir);
  write_run_dir_archive(dir);  // set A: backtest, reconciliation, contract_marks, meta

  // Anti-vacuity: set A really is on disk, so "only B survives" below is a real
  // observation and not a comparison over an empty archive. Scoped so the mapping
  // is released before the second write's tmp+rename.
  {
    auto before = RunDir(dir).archive();
    ASSERT_TRUE(before.has_value()) << before.error().to_string();
    ASSERT_EQ(before->count(), 4u);
    ASSERT_TRUE(before->section("backtest").has_value());
    ASSERT_TRUE(before->section("reconciliation").has_value());
    ASSERT_TRUE(before->section("contract_marks").has_value());
  }

  append_identity_probe_byte(dir / std::string(kSurfaceManifestFile));

  const RunDir run_dir(dir);
  const BacktestResult pr = make_encoder_fixture_result();  // outlives the write
  std::vector<RaSectionData> proj_sections;
  proj_sections.push_back(encode_backtest_section("projected_cold", pr));
  {
    RunSpec proj_spec;
    proj_spec.label = "projected-run";
    proj_spec.date_lo = "2026-07-10";
    proj_spec.date_hi = "2026-07-10";
    proj_sections.push_back(encode_meta_section(proj_spec));
  }
  ASSERT_TRUE(run_dir.write_run_archive(proj_sections).has_value());

  auto archive = run_dir.archive();
  ASSERT_TRUE(archive.has_value()) << archive.error().to_string();
  EXPECT_EQ(archive->count(), 2u) << "stale sections were carried across a corpus change";
  EXPECT_TRUE(archive->section("projected_cold").has_value());
  EXPECT_TRUE(archive->section("meta").has_value());
  EXPECT_FALSE(archive->section("backtest").has_value());
  EXPECT_FALSE(archive->section("reconciliation").has_value());
  EXPECT_FALSE(archive->section("contract_marks").has_value());

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// The ORDERING INVARIANT documented on RunDir::run_identity_hash, pinned as a
// consequence (Wave E T6 fix 1, review M-1/M-2). A folded input that was ABSENT
// when archive A was stamped and APPEARS before archive B is written moves the
// cache key, so B must start FRESH — A's sections must NOT be carried forward.
//
// That is exactly the shape of the pipeline hazard: build_schedule_command writes
// trade_schedule.tsv (a folded input) five lines BEFORE its own write_run_archive.
// If those two statements were ever swapped, this is what the pipeline would do —
// silently drop the trade_schedule section on run-backtest's write, with no error.
// The union-survival counterpart (all five inputs present and unchanged => the
// merge still unions) is the POSITIVE CONTROL in
// MergeWriteCarriesUnsupersededSectionsOnSameInputs above.
//
// Distinct from MergeWriteDropsCarriedSectionsWhenInputsChange (mutates an
// already-folded run_spec.tsv) and from MergeWriteRejectsCarryForwardAfterManifest
// Change (mutates an existing surface_manifest.tsv): here the file's TRANSITION IS
// ABSENT -> PRESENT, which is the case skip-if-absent makes possible and which no
// other test covers.
TEST(RunDir, MergeWriteDropsCarriedSectionsWhenAFoldedInputAppearsLate) {
  const std::filesystem::path dir = fresh_run_dir("atx_rundir_mergewrite_late_input_test");
  write_run_dir_folded_inputs(dir);

  // Park trade_schedule.tsv outside the run dir so write A sees it ABSENT (the
  // skip-if-absent leg of the fold). Its real bytes are restored below.
  const std::filesystem::path schedule_path = dir / std::string(kTradeScheduleFile);
  const std::filesystem::path parked = dir.parent_path() / "atx_rundir_late_input_parked.tsv";
  {
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::is_regular_file(schedule_path, ec)) << schedule_path.string();
    ASSERT_GT(std::filesystem::file_size(schedule_path, ec), 0u);
    std::filesystem::remove(parked, ec);
    std::filesystem::rename(schedule_path, parked, ec);
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_FALSE(std::filesystem::exists(schedule_path, ec)) << "folded input must be absent for A";
  }

  const auto id_absent = RunDir(dir).run_identity_hash();
  ASSERT_TRUE(id_absent.has_value()) << id_absent.error().to_string();
  ASSERT_NE(*id_absent, 0u);

  write_run_dir_archive(dir);  // set A: backtest, reconciliation, contract_marks, meta

  // Anti-vacuity: set A really is on disk, so "only B survives" below is a real
  // observation and not a comparison over an empty archive. Scoped so the mapping
  // is released before the second write's tmp+rename.
  {
    auto before = RunDir(dir).archive();
    ASSERT_TRUE(before.has_value()) << before.error().to_string();
    ASSERT_EQ(before->count(), 4u);
    ASSERT_TRUE(before->section("backtest").has_value());
    ASSERT_TRUE(before->section("reconciliation").has_value());
    ASSERT_TRUE(before->section("contract_marks").has_value());
  }

  // The folded input APPEARS between the two writes.
  {
    std::error_code ec;
    std::filesystem::rename(parked, schedule_path, ec);
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_TRUE(std::filesystem::is_regular_file(schedule_path, ec));
    ASSERT_GT(std::filesystem::file_size(schedule_path, ec), 0u);
  }

  // The mechanism, asserted directly: the key moved because the file appeared.
  // EXPECT (not ASSERT) so a regression reports BOTH the mechanism and its
  // economic consequence (the stale carry-forward asserted at the end) in one run.
  const auto id_present = RunDir(dir).run_identity_hash();
  ASSERT_TRUE(id_present.has_value()) << id_present.error().to_string();
  EXPECT_NE(*id_present, *id_absent)
      << "a folded input appearing between two writes did not move the cache key";

  const RunDir run_dir(dir);
  const BacktestResult pr = make_encoder_fixture_result();  // outlives the write
  std::vector<RaSectionData> proj_sections;
  proj_sections.push_back(encode_backtest_section("projected_cold", pr));
  {
    RunSpec proj_spec;
    proj_spec.label = "projected-run";
    proj_spec.date_lo = "2026-07-10";
    proj_spec.date_hi = "2026-07-10";
    proj_sections.push_back(encode_meta_section(proj_spec));
  }
  ASSERT_TRUE(run_dir.write_run_archive(proj_sections).has_value());

  auto archive = run_dir.archive();
  ASSERT_TRUE(archive.has_value()) << archive.error().to_string();
  EXPECT_EQ(archive->count(), 2u)
      << "stale sections were carried across a folded input appearing between writes";
  EXPECT_TRUE(archive->section("projected_cold").has_value());
  EXPECT_TRUE(archive->section("meta").has_value());
  EXPECT_FALSE(archive->section("backtest").has_value());
  EXPECT_FALSE(archive->section("reconciliation").has_value());
  EXPECT_FALSE(archive->section("contract_marks").has_value());

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::remove(parked, ec);
}

// ── THIS TEST RECORDS A KNOWN GAP. IT IS NOT A PROPERTY ANYONE WANTS. ─────────
//
// run_identity_hash does not fold definitions.tsv, on COST GROUNDS ALONE (~700 MB
// on a production run). surface_manifest.tsv and input_inventory.tsv are NOT
// derived from the definitions bytes at all: build-corpus COPIES definitions.tsv
// without reading it, and write_input_inventory derives source_fingerprint /
// market_input_fingerprint solely from the OPRA batch. trade_schedule.tsv IS
// derived from definitions.tsv (build-schedule reads it and feeds it into the
// selection loop) — but that does not close the gap this test pins, because the
// scenario below changes ONLY definitions.tsv without rerunning build-schedule,
// so trade_schedule.tsv's bytes never move either. The Wave E T6 review
// falsified the opposite claim on the real driver — a changed definitions.tsv,
// all five folded inputs byte-identical, the identity unmoved, and a
// six-section run-backtest write producing a nine-section archive.
//
// The assertion below pins that blindness so the gap is VISIBLE in the suite rather
// than latent in a comment. It is a documented limitation, NOT an endorsement:
// callers that swap definitions in place must delete run.atxrun. Wave E's
// definitions-cache task already computes hash_bytes over the whole of
// definitions.tsv on the read path, where the bytes are resident — folding that
// value in here would close the gap at no extra I/O. WHEN THAT HAPPENS THIS TEST
// SHOULD FAIL, and the correct response is to DELETE IT, never to restore the
// blindness it records.
TEST(RunDir, RunIdentityIsDeliberatelyBlindToDefinitionsContent) {
  const std::filesystem::path dir = fresh_run_dir("atx_rundir_identity_definitions_gap_test");
  write_run_dir_folded_inputs(dir);

  // A definitions.tsv the way build-corpus leaves one: a real, non-empty run-dir
  // file that the economics DO read (run_backtest_command parses it and feeds
  // listed_quotes_for_date), and that the identity nevertheless ignores.
  const std::filesystem::path definitions = dir / "definitions.tsv";
  {
    std::ofstream d(definitions, std::ios::binary | std::ios::trunc);
    d << "trade_date\traw_symbol\tunderlying\texpiration\tstrike\tinstrument_class\n"
      << "2026-07-10\tSPY   260717C00500000\tSPY\t2026-07-17\t500.0\tC\n";
    ASSERT_TRUE(d.good());
  }

  std::error_code ec;
  ASSERT_TRUE(std::filesystem::is_regular_file(definitions, ec)) << definitions.string();
  const auto size_before = std::filesystem::file_size(definitions, ec);
  ASSERT_GT(size_before, 0u) << "anti-vacuity: an absent/empty definitions.tsv would pass trivially";

  const RunDir run_dir(dir);
  auto base = run_dir.run_identity_hash();
  ASSERT_TRUE(base.has_value()) << base.error().to_string();
  ASSERT_NE(*base, 0u);

  append_identity_probe_byte(definitions);
  ASSERT_EQ(std::filesystem::file_size(definitions, ec), size_before + 1)
      << "anti-vacuity: the definitions bytes must really have changed";

  // THE GAP: the cache key does not see it, so a merge-write would carry stale
  // sections across a definitions change. Recorded, not endorsed.
  EXPECT_EQ(run_dir.run_identity_hash().value(), *base)
      << "definitions.tsv is now folded into the run identity — the gap this test "
         "records is CLOSED. Delete this test rather than reverting the fold.";

  // Non-vacuity of the comparison itself: on this very run dir the identity CAN
  // move — a folded input proves the EXPECT_EQ above is not comparing constants.
  append_identity_probe_byte(dir / std::string(kSurfaceManifestFile));
  EXPECT_NE(run_dir.run_identity_hash().value(), *base)
      << "control: a folded input must still move the key";

  std::filesystem::remove_all(dir, ec);
}

}  // namespace
}  // namespace atx::vol
