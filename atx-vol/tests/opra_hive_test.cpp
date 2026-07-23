#include "support/synthetic_opra_hive.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/io/parquet.hpp" // read_parquet, ParquetTable
#include "atx/vol/opra_batch.hpp"  // OpraBatchSpec/Result, load_opra_daterange (parity ref)
#include "atx/vol/opra_hive.hpp"   // OpraHiveSpec, load_opra_hive
#include "atx/vol/opra_panel.hpp"  // OpraLoadSpec, load_opra_cbbo_from_table

// Self-tests for the synthetic multi-symbol OPRA hive fixture (Task 2). These
// prove the fixture is loader-consumable BEFORE Tasks 3/5 build the C++ loader
// and build driver on top of it:
//
//   1. V2LayoutFilesExist                      — the hive-v2 date-partition layout
//                                                <root>/date=<d>/data.parquet.
//   2. V2FileLoadsPerSymbolThroughTableSeam    — one date file, read via
//                                                read_parquet, then split per
//                                                `underlying` through the
//                                                load_opra_cbbo_from_table seam
//                                                (the 8-column v2 schema it
//                                                requires), yields a 36-quote
//                                                panel with a PCP-implied spot.

namespace {

namespace fs = std::filesystem;
namespace tsupport = atx::vol::testsupport;

TEST(SyntheticHive, V2LayoutFilesExist) {
  const fs::path tmp = fs::temp_directory_path() / "atx_synth_hive_v2_exist";
  fs::remove_all(tmp);
  tsupport::write_synthetic_hive_v2(tmp, {});
  EXPECT_TRUE(fs::exists(tmp / "date=2026-07-01" / "data.parquet"));
  EXPECT_TRUE(fs::exists(tmp / "date=2026-07-02" / "data.parquet"));
  EXPECT_TRUE(fs::exists(tmp / "date=2026-07-06" / "data.parquet"));
  fs::remove_all(tmp);
}

TEST(SyntheticHive, V2FileLoadsPerSymbolThroughTableSeam) {
  const fs::path tmp = fs::temp_directory_path() / "atx_synth_hive_v2_load";
  fs::remove_all(tmp);
  tsupport::write_synthetic_hive_v2(tmp, {});

  const fs::path date_file = tmp / "date=2026-07-01" / "data.parquet";
  ASSERT_TRUE(fs::exists(date_file));

  // The v2 date file carries all three symbols; read it once, then feed it back
  // per-underlying through the in-memory-table seam (Task 1) that requires the
  // full 8-column OPRA schema.
  const auto table = atx::core::io::read_parquet(date_file.string());
  ASSERT_TRUE(table.has_value()) << table.error().to_string();

  atx::vol::OpraLoadSpec spec;
  spec.path = date_file.string();
  spec.underlying = "AAA";
  spec.snapshot_iso = "2026-07-01";
  spec.r = 0.03;

  const auto panel = atx::vol::load_opra_cbbo_from_table(*table, spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  // 9 strikes x 2 expiries x {C,P} = 36 quotes for underlying "AAA".
  EXPECT_EQ(panel->n_contracts, 36U);
  EXPECT_EQ(panel->frame.rows.size(), 36U);
  // PCP-consistent Black mids imply the planted spot (within 1% of 100.0).
  EXPECT_NEAR(panel->implied_spot, 100.0, 1.0);

  fs::remove_all(tmp);
}

// ── load_opra_hive: the date-partitioned OPRA hive v2 loader (Task 3) ─────────
//
// These exercise the centerpiece loader against the Task 2 synthetic hive-v2
// fixture. Exact counts are for the default SyntheticHiveSpec (36 quotes per
// (symbol, date) panel; symbols AAA/BBB/CCC). Where a pristine gap-free grid is
// wanted, a 3-CONTIGUOUS-date fixture is used so the calendar enumeration has no
// holes; the default (07-01/02/06) fixture is used where a missing date is under
// test.

namespace {
namespace ahv = atx::vol;

// Locate the (symbol, date) cell in a batch/hive result.
[[nodiscard]] const ahv::OpraBatchEntry *find_cell(const ahv::OpraBatchResult &r,
                                                   const std::string &symbol,
                                                   const std::string &date) {
  for (const ahv::OpraBatchEntry &e : r.entries) {
    if (e.symbol == symbol && e.date == date) {
      return &e;
    }
  }
  return nullptr;
}
} // namespace

// 3 symbols x 3 contiguous dates -> a pristine 9-cell grid, all loaded, ordered
// date-major then symbol-major.
TEST(OpraHive, LoadsAllSymbolsAllDates) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_hive_all";
  fs::remove_all(root);
  tsupport::SyntheticHiveSpec fx;
  fx.dates = {"2026-07-01", "2026-07-02", "2026-07-03"};
  tsupport::write_synthetic_hive_v2(root, fx);

  ahv::OpraHiveSpec spec;
  spec.root_dir = root.string();
  spec.date_lo = "2026-07-01";
  spec.date_hi = "2026-07-03";
  spec.symbols = {"AAA", "BBB", "CCC"};
  spec.r = 0.03;

  const auto res = ahv::load_opra_hive(spec);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  EXPECT_EQ(res->n_total, std::size_t{9});
  EXPECT_EQ(res->n_loaded, std::size_t{9});
  EXPECT_EQ(res->n_missing, std::size_t{0});
  EXPECT_EQ(res->n_error, std::size_t{0});
  ASSERT_EQ(res->entries.size(), std::size_t{9});

  const std::vector<std::pair<std::string, std::string>> want = {
      {"2026-07-01", "AAA"}, {"2026-07-01", "BBB"}, {"2026-07-01", "CCC"},
      {"2026-07-02", "AAA"}, {"2026-07-02", "BBB"}, {"2026-07-02", "CCC"},
      {"2026-07-03", "AAA"}, {"2026-07-03", "BBB"}, {"2026-07-03", "CCC"}};
  for (std::size_t i = 0; i < want.size(); ++i) {
    EXPECT_EQ(res->entries[i].date, want[i].first) << "entry " << i;
    EXPECT_EQ(res->entries[i].symbol, want[i].second) << "entry " << i;
    ASSERT_TRUE(res->entries[i].panel.has_value())
        << want[i].first << " " << want[i].second << ": "
        << res->entries[i].panel.error().to_string();
    EXPECT_EQ(res->entries[i].panel->n_contracts, std::size_t{36});
    EXPECT_EQ(res->entries[i].panel->frame.rows.size(), std::size_t{36});
  }
  fs::remove_all(root);
}

// A calendar date with no date=<d>/ directory is NON-fatal: each requested
// symbol yields Err(NotFound) and bumps n_missing; the batch is Ok overall.
TEST(OpraHive, MissingDateNonFatal) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_hive_missing";
  fs::remove_all(root);
  // Default fixture: 2026-07-01, 2026-07-02, 2026-07-06 (so 2026-07-03 is absent).
  tsupport::write_synthetic_hive_v2(root, {});

  ahv::OpraHiveSpec spec;
  spec.root_dir = root.string();
  spec.date_lo = "2026-07-01";
  spec.date_hi = "2026-07-03"; // range includes the absent 2026-07-03
  spec.symbols = {"AAA", "BBB", "CCC"};
  spec.r = 0.03;

  const auto res = ahv::load_opra_hive(spec);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  EXPECT_EQ(res->n_total, std::size_t{9});   // 3 symbols x 3 calendar dates
  EXPECT_EQ(res->n_loaded, std::size_t{6});  // 07-01, 07-02 present
  EXPECT_EQ(res->n_missing, std::size_t{3}); // 07-03 absent -> 3 NotFound cells
  EXPECT_EQ(res->n_error, std::size_t{0});

  for (const std::string &sym : {"AAA", "BBB", "CCC"}) {
    const ahv::OpraBatchEntry *e = find_cell(*res, sym, "2026-07-03");
    ASSERT_NE(e, nullptr) << sym;
    ASSERT_FALSE(e->panel.has_value()) << sym;
    EXPECT_EQ(e->panel.error().code(), ahv::ErrorCode::NotFound) << sym;
  }
  fs::remove_all(root);
}

// Empty spec.symbols => discover the sorted distinct underlyings from each date
// file (all 3 present here), globally deterministic order.
TEST(OpraHive, EmptySymbolsDiscoversUnderlyings) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_hive_discover";
  fs::remove_all(root);
  tsupport::SyntheticHiveSpec fx;
  fx.dates = {"2026-07-01", "2026-07-02", "2026-07-03"};
  tsupport::write_synthetic_hive_v2(root, fx);

  ahv::OpraHiveSpec spec;
  spec.root_dir = root.string();
  spec.date_lo = "2026-07-01";
  spec.date_hi = "2026-07-03";
  spec.symbols = {}; // discover
  spec.r = 0.03;

  const auto res = ahv::load_opra_hive(spec);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  EXPECT_EQ(res->n_total, std::size_t{9});
  EXPECT_EQ(res->n_loaded, std::size_t{9});
  EXPECT_EQ(res->n_missing, std::size_t{0});
  EXPECT_EQ(res->n_error, std::size_t{0});
  ASSERT_EQ(res->entries.size(), std::size_t{9});

  // Discovered symbols appear sorted (AAA, BBB, CCC) within each date.
  const std::vector<std::string> want_syms = {"AAA", "BBB", "CCC"};
  const std::vector<std::string> want_dates = {"2026-07-01", "2026-07-02", "2026-07-03"};
  for (std::size_t d = 0; d < want_dates.size(); ++d) {
    for (std::size_t s = 0; s < want_syms.size(); ++s) {
      const std::size_t i = d * 3 + s;
      EXPECT_EQ(res->entries[i].date, want_dates[d]) << "entry " << i;
      EXPECT_EQ(res->entries[i].symbol, want_syms[s]) << "entry " << i;
      ASSERT_TRUE(res->entries[i].panel.has_value())
          << res->entries[i].panel.error().to_string();
    }
  }
  fs::remove_all(root);
}

// A symbol subset loads ONLY the requested underlyings (BBB), one per date.
TEST(OpraHive, SubsetSymbolsLoadsOnlyRequested) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_hive_subset";
  fs::remove_all(root);
  tsupport::SyntheticHiveSpec fx;
  fx.dates = {"2026-07-01", "2026-07-02", "2026-07-03"};
  tsupport::write_synthetic_hive_v2(root, fx);

  ahv::OpraHiveSpec spec;
  spec.root_dir = root.string();
  spec.date_lo = "2026-07-01";
  spec.date_hi = "2026-07-03";
  spec.symbols = {"BBB"};
  spec.r = 0.03;

  const auto res = ahv::load_opra_hive(spec);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  EXPECT_EQ(res->n_total, std::size_t{3});
  EXPECT_EQ(res->n_loaded, std::size_t{3});
  ASSERT_EQ(res->entries.size(), std::size_t{3});
  for (const ahv::OpraBatchEntry &e : res->entries) {
    EXPECT_EQ(e.symbol, "BBB");
    ASSERT_TRUE(e.panel.has_value()) << e.date << ": " << e.panel.error().to_string();
    EXPECT_EQ(e.panel->n_contracts, std::size_t{36});
    EXPECT_EQ(e.panel->frame.uid, "BBB");
  }
  fs::remove_all(root);
}

// n_threads is a perf-only knob: 1 vs 8 must give byte-identical entry order,
// counts, and per-panel row counts. A gappy grid (default fixture over a range
// with holes) makes any completion-order dependence visible.
TEST(OpraHive, DeterministicAcrossThreadCounts) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_hive_determinism";
  fs::remove_all(root);
  tsupport::write_synthetic_hive_v2(root, {}); // 07-01, 07-02, 07-06 present

  ahv::OpraHiveSpec spec;
  spec.root_dir = root.string();
  spec.date_lo = "2026-07-01";
  spec.date_hi = "2026-07-06"; // 07-03/04/05 absent -> a grid with holes
  spec.symbols = {"AAA", "BBB", "CCC"};
  spec.r = 0.03;

  spec.n_threads = 1;
  const auto serial = ahv::load_opra_hive(spec);
  spec.n_threads = 8;
  const auto parallel = ahv::load_opra_hive(spec);
  ASSERT_TRUE(serial.has_value()) << serial.error().to_string();
  ASSERT_TRUE(parallel.has_value()) << parallel.error().to_string();

  EXPECT_EQ(serial->n_total, parallel->n_total);
  EXPECT_EQ(serial->n_loaded, parallel->n_loaded);
  EXPECT_EQ(serial->n_missing, parallel->n_missing);
  EXPECT_EQ(serial->n_error, parallel->n_error);
  ASSERT_EQ(serial->entries.size(), parallel->entries.size());

  for (std::size_t i = 0; i < serial->entries.size(); ++i) {
    const ahv::OpraBatchEntry &s = serial->entries[i];
    const ahv::OpraBatchEntry &p = parallel->entries[i];
    EXPECT_EQ(s.symbol, p.symbol) << "entry " << i;
    EXPECT_EQ(s.date, p.date) << "entry " << i;
    ASSERT_EQ(s.panel.has_value(), p.panel.has_value()) << s.symbol << " " << s.date;
    if (s.panel.has_value()) {
      EXPECT_EQ(s.panel->n_contracts, p.panel->n_contracts) << s.symbol << " " << s.date;
      EXPECT_EQ(s.panel->frame.rows.size(), p.panel->frame.rows.size())
          << s.symbol << " " << s.date;
      EXPECT_DOUBLE_EQ(s.panel->implied_spot, p.panel->implied_spot)
          << s.symbol << " " << s.date;
    } else {
      EXPECT_EQ(s.panel.error().code(), p.panel.error().code()) << s.symbol << " " << s.date;
    }
  }
  fs::remove_all(root);
}

// Same rows, two layouts: the v1 per-symbol tree via load_opra_daterange is the
// reference for the v2 date-partition hive via load_opra_hive. Per (symbol,
// date) cell the panels must agree economically (contracts, expiries, rows,
// implied spot).
TEST(OpraHive, ParityWithPerSymbolLayout) {
  const fs::path root_v1 = fs::temp_directory_path() / "atx_opra_hive_parity_v1";
  const fs::path root_v2 = fs::temp_directory_path() / "atx_opra_hive_parity_v2";
  fs::remove_all(root_v1);
  fs::remove_all(root_v2);
  tsupport::SyntheticHiveSpec fx;
  fx.dates = {"2026-07-01", "2026-07-02", "2026-07-03"};
  tsupport::write_synthetic_hive_v1(root_v1, fx);
  tsupport::write_synthetic_hive_v2(root_v2, fx);

  const std::vector<std::string> symbols = {"AAA", "BBB", "CCC"};

  ahv::OpraBatchSpec bspec;
  bspec.symbols = symbols;
  bspec.date_lo = "2026-07-01";
  bspec.date_hi = "2026-07-03";
  bspec.root_dir = root_v1.string();
  bspec.r = 0.03; // default path_template "{symbol}/{date}.parquet"
  const auto ref = ahv::load_opra_daterange(bspec);
  ASSERT_TRUE(ref.has_value()) << ref.error().to_string();

  ahv::OpraHiveSpec hspec;
  hspec.symbols = symbols;
  hspec.date_lo = "2026-07-01";
  hspec.date_hi = "2026-07-03";
  hspec.root_dir = root_v2.string();
  hspec.r = 0.03;
  const auto hive = ahv::load_opra_hive(hspec);
  ASSERT_TRUE(hive.has_value()) << hive.error().to_string();

  for (const std::string &sym : symbols) {
    for (const std::string &date : fx.dates) {
      const ahv::OpraBatchEntry *a = find_cell(*ref, sym, date);
      const ahv::OpraBatchEntry *b = find_cell(*hive, sym, date);
      ASSERT_NE(a, nullptr) << sym << " " << date;
      ASSERT_NE(b, nullptr) << sym << " " << date;
      ASSERT_TRUE(a->panel.has_value()) << sym << " " << date << ": " << a->panel.error().to_string();
      ASSERT_TRUE(b->panel.has_value()) << sym << " " << date << ": " << b->panel.error().to_string();
      EXPECT_EQ(a->panel->n_contracts, b->panel->n_contracts) << sym << " " << date;
      EXPECT_EQ(a->panel->n_expiries, b->panel->n_expiries) << sym << " " << date;
      EXPECT_EQ(a->panel->frame.rows.size(), b->panel->frame.rows.size()) << sym << " " << date;
      EXPECT_DOUBLE_EQ(a->panel->implied_spot, b->panel->implied_spot) << sym << " " << date;
    }
  }
  fs::remove_all(root_v1);
  fs::remove_all(root_v2);
}

// Malformed spec is the ONLY top-level Err: empty root, reversed dates, and a
// pillar length mismatch each return Err(InvalidArgument).
TEST(OpraHive, MalformedSpecIsTopLevelErr) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_hive_malformed";
  fs::remove_all(root);
  tsupport::write_synthetic_hive_v2(root, {});

  {
    ahv::OpraHiveSpec spec; // empty root_dir
    spec.date_lo = "2026-07-01";
    spec.date_hi = "2026-07-06";
    const auto res = ahv::load_opra_hive(spec);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code(), ahv::ErrorCode::InvalidArgument);
  }
  {
    ahv::OpraHiveSpec spec; // reversed dates
    spec.root_dir = root.string();
    spec.date_lo = "2026-07-06";
    spec.date_hi = "2026-07-01";
    const auto res = ahv::load_opra_hive(spec);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code(), ahv::ErrorCode::InvalidArgument);
  }
  {
    ahv::OpraHiveSpec spec; // mismatched pillar arrays
    spec.root_dir = root.string();
    spec.date_lo = "2026-07-01";
    spec.date_hi = "2026-07-06";
    spec.yc_pillar_t = {0.25, 0.5};
    spec.yc_pillar_r = {0.03}; // length mismatch
    const auto res = ahv::load_opra_hive(spec);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code(), ahv::ErrorCode::InvalidArgument);
  }
  fs::remove_all(root);
}

// A present-but-corrupt date file is NOT a missing file: every requested symbol
// for that date gets an Err entry (n_error), while the rest of the batch loads
// and stays Ok overall.
TEST(OpraHive, CorruptDateFileCountsError) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_hive_corrupt";
  fs::remove_all(root);
  tsupport::SyntheticHiveSpec fx;
  fx.dates = {"2026-07-01", "2026-07-02", "2026-07-03"};
  tsupport::write_synthetic_hive_v2(root, fx);

  // Truncate 2026-07-02's data.parquet to a stub so read_parquet fails (present
  // but unreadable -> an error, not a NotFound).
  const fs::path corrupt = root / "date=2026-07-02" / "data.parquet";
  ASSERT_TRUE(fs::exists(corrupt));
  std::error_code ec;
  fs::resize_file(corrupt, 8, ec);
  ASSERT_FALSE(ec) << ec.message();

  ahv::OpraHiveSpec spec;
  spec.root_dir = root.string();
  spec.date_lo = "2026-07-01";
  spec.date_hi = "2026-07-03";
  spec.symbols = {"AAA", "BBB", "CCC"};
  spec.r = 0.03;

  const auto res = ahv::load_opra_hive(spec);
  ASSERT_TRUE(res.has_value()) << res.error().to_string(); // batch still Ok
  EXPECT_EQ(res->n_total, std::size_t{9});
  EXPECT_EQ(res->n_loaded, std::size_t{6}); // 07-01, 07-03 load
  EXPECT_EQ(res->n_missing, std::size_t{0});
  EXPECT_EQ(res->n_error, std::size_t{3}); // 07-02's 3 symbols error

  for (const std::string &sym : {"AAA", "BBB", "CCC"}) {
    const ahv::OpraBatchEntry *e = find_cell(*res, sym, "2026-07-02");
    ASSERT_NE(e, nullptr) << sym;
    ASSERT_FALSE(e->panel.has_value()) << sym;
    EXPECT_NE(e->panel.error().code(), ahv::ErrorCode::NotFound) << sym; // present, not missing
  }
  fs::remove_all(root);
}

} // namespace
