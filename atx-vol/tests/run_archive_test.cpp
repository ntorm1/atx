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

#include "atx/vol/detail/archive_util.hpp" // crc32c (independent CRC check)
#include "atx/vol/surface_archive.hpp"     // ArchiveContentIdentity (identity())

#include <gtest/gtest.h>

#include <array>
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

}  // namespace
}  // namespace atx::vol
