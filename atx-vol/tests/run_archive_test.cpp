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
#include "atx/vol/detail/archive_util.hpp" // crc32c (independent CRC check)
#include "atx/vol/dispersion_workflow.hpp" // RunSpec (Task 5 meta encoder)
#include "atx/vol/listed_dispersion_reconciliation.hpp"
#include "atx/vol/listed_dispersion_schedule.hpp"
#include "atx/vol/surface_archive.hpp" // ArchiveContentIdentity (identity())

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
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
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "atx_run_archive_writer_test.runar";
  std::filesystem::remove(path);

  auto st = write_run_archive_file(path.string(), sections, 5, 7);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  ASSERT_TRUE(std::filesystem::exists(path));
  EXPECT_FALSE(std::filesystem::exists(path.string() + ".tmp"));

  // Re-write over the existing destination (Windows: remove-then-rename).
  auto st2 = write_run_archive_file(path.string(), sections, 6, 7);
  ASSERT_TRUE(st2.has_value()) << st2.error().to_string();
  EXPECT_FALSE(std::filesystem::exists(path.string() + ".tmp"));

  // The published file leads with the RunArchive magic.
  std::ifstream is(path, std::ios::binary);
  ASSERT_TRUE(is.good());
  char magic[8] = {};
  is.read(magic, 8);
  EXPECT_EQ(std::string_view(magic, 8), std::string_view(kRaMagic, 8));
  is.close();
  std::filesystem::remove(path);
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

TEST(RunArchiveEncoders, WritesPythonFixture) {
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

  const std::filesystem::path path = python_fixture_path();
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  ASSERT_FALSE(ec) << ec.message();

  // created_ts_ns MUST be nonzero: 0 falls back to the system clock, and the
  // committed fixture bytes must be identical across reruns.
  const auto st = write_run_archive_file(path.string(), sections,
                                         /*created_ts_ns=*/123456789,
                                         /*run_identity_hash=*/0xABCDEFull);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();

  auto ar = RunArchive::open_file(path.string());
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
}

}  // namespace
}  // namespace atx::vol
