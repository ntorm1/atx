#include "support/synthetic_opra_hive.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/io/parquet.hpp" // read_parquet, ParquetTable
#include "atx/vol/api/core/chain.hpp"             // OptionChain, OptionRef
#include "atx/vol/api/core/vol_time.hpp"          // TimeConvention, vol_time_years
#include "atx/vol/api/marketdata/opra_batch.hpp"  // OpraBatchSpec/Result, load_opra_daterange (parity ref)
#include "atx/vol/api/marketdata/opra_hive.hpp"   // OpraHiveSpec, load_opra_hive
#include "atx/vol/api/marketdata/opra_panel.hpp"  // OpraLoadSpec, load_opra_cbbo_from_table

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

// ── P-01: the per-underlying row index must be a PURE I/O restructure ─────────
//
// `scan_opra_cbbo_table` materializes each column once per DATE and indexes the
// rows by `underlying`, so a per-symbol split visits only its own rows instead of
// re-walking (and re-sizing every buffer for) the whole table. That is a claim
// about COST, and it is only admissible if the panel is bit-for-bit what the
// full-table scan produced -- the loader feeds the fitter, and a single reordered
// or dropped quote row moves every fitted surface downstream.
//
// The reference here is not a re-implementation: setting `indexed = false` on the
// very same scan selects the pre-P-01 code path exactly -- every loop walks
// [0, n_rows) and skips on the filter, every buffer is reserved for the whole
// table. So this compares the two paths over identical decoded bytes.
//
// The comparison is exhaustive on purpose. `source_fingerprint` alone already
// folds in the frame uid, the snapshot stamp and every row's uid / expiry /
// strike / side / bid / ask / sizes / instrument id, so it would catch a
// reordering; the per-field row walk is there so a failure says WHICH field
// moved rather than "a hash differs".
TEST(OpraHive, IndexedScanPanelIsIdenticalToTheFullScanPanel) {
  namespace io = atx::core::io;
  const fs::path root = fs::temp_directory_path() / "atx_opra_hive_scan_parity";
  fs::remove_all(root);
  tsupport::SyntheticHiveSpec fx;
  fx.dates = {"2026-07-01"};
  tsupport::write_synthetic_hive_v2(root, fx); // AAA/BBB/CCC in ONE table
  const std::string path = (root / "date=2026-07-01" / "data.parquet").string();

  const auto table = io::read_parquet(path);
  ASSERT_TRUE(table.has_value()) << table.error().to_string();

  const auto scan = ahv::scan_opra_cbbo_table(*table, path);
  ASSERT_TRUE(scan.has_value()) << scan.error().to_string();
  ASSERT_TRUE(scan->indexed) << "a 3-underlying table must be indexable";

  // The index partitions the table: every row appears in exactly one group, and
  // ASCENDING within it. The ascending property is what makes the two paths visit
  // rows in the same relative order, so it is asserted rather than assumed.
  std::size_t indexed_rows = 0;
  for (const char *sym : {"AAA", "BBB", "CCC"}) {
    const std::span<const std::uint32_t> g = scan->rows_for(sym);
    EXPECT_FALSE(g.empty()) << sym;
    for (std::size_t i = 1; i < g.size(); ++i) {
      ASSERT_LT(g[i - 1], g[i]) << sym << " group is not ascending";
    }
    indexed_rows += g.size();
  }
  EXPECT_EQ(indexed_rows, scan->n_rows);
  EXPECT_TRUE(scan->rows_for("ZZZ").empty());

  // The pre-P-01 path, over the same decoded bytes.
  ahv::OpraTableScan full = *scan;
  full.indexed = false;

  for (const char *sym : {"AAA", "BBB", "CCC"}) {
    ahv::OpraLoadSpec spec;
    spec.path = path;
    spec.underlying = sym;
    spec.snapshot_iso = "2026-07-01T19:55:00Z";
    spec.r = 0.03;

    const auto fast = ahv::load_opra_cbbo_from_scan(*scan, spec);
    const auto reference = ahv::load_opra_cbbo_from_scan(full, spec);
    ASSERT_TRUE(fast.has_value()) << sym << ": " << fast.error().to_string();
    ASSERT_TRUE(reference.has_value()) << sym << ": " << reference.error().to_string();

    EXPECT_EQ(fast->source_fingerprint, reference->source_fingerprint) << sym;
    EXPECT_EQ(fast->n_contracts, reference->n_contracts) << sym;
    EXPECT_EQ(fast->n_expiries, reference->n_expiries) << sym;
    EXPECT_EQ(fast->n_dropped, reference->n_dropped) << sym;
    EXPECT_EQ(fast->n_dropped_uncovered_expiry, reference->n_dropped_uncovered_expiry) << sym;
    EXPECT_EQ(fast->uncovered_expiries, reference->uncovered_expiries) << sym;
    EXPECT_EQ(fast->implied_spot, reference->implied_spot) << sym; // bitwise, not NEAR
    EXPECT_EQ(fast->snapshot_iso, reference->snapshot_iso) << sym;
    EXPECT_EQ(fast->source_schema_version, reference->source_schema_version) << sym;
    EXPECT_EQ(fast->provenance_complete, reference->provenance_complete) << sym;
    EXPECT_EQ(fast->source_instrument_ids, reference->source_instrument_ids) << sym;
    ASSERT_EQ(fast->source_identities.size(), reference->source_identities.size()) << sym;
    for (std::size_t i = 0; i < fast->source_identities.size(); ++i) {
      EXPECT_EQ(fast->source_identities[i].instrument_id,
                reference->source_identities[i].instrument_id)
          << sym << " #" << i;
      EXPECT_EQ(fast->source_identities[i].raw_symbol, reference->source_identities[i].raw_symbol)
          << sym << " #" << i;
    }

    EXPECT_EQ(fast->frame.uid, reference->frame.uid) << sym;
    EXPECT_EQ(fast->frame.spot, reference->frame.spot) << sym;
    EXPECT_EQ(fast->frame.snapshot_ts_ns, reference->frame.snapshot_ts_ns) << sym;
    EXPECT_EQ(fast->frame.uid_strs, reference->frame.uid_strs) << sym;
    ASSERT_EQ(fast->frame.rows.size(), reference->frame.rows.size()) << sym;
    for (std::size_t i = 0; i < fast->frame.rows.size(); ++i) {
      const auto &a = fast->frame.rows[i];
      const auto &b = reference->frame.rows[i];
      EXPECT_EQ(a.uid, b.uid) << sym << " row " << i;
      EXPECT_EQ(a.expiry_iso, b.expiry_iso) << sym << " row " << i;
      EXPECT_EQ(a.strike, b.strike) << sym << " row " << i;
      EXPECT_EQ(a.side, b.side) << sym << " row " << i;
      EXPECT_EQ(a.bid, b.bid) << sym << " row " << i;
      EXPECT_EQ(a.ask, b.ask) << sym << " row " << i;
      EXPECT_EQ(a.bid_size, b.bid_size) << sym << " row " << i;
      EXPECT_EQ(a.ask_size, b.ask_size) << sym << " row " << i;
      EXPECT_EQ(a.expiry_ns, b.expiry_ns) << sym << " row " << i;
      EXPECT_EQ(a.settle, b.settle) << sym << " row " << i;
    }
  }

  // A symbol the table does not carry must produce the seam's exact zero-match
  // Err on BOTH paths -- opra_hive.cpp classifies coverage holes on that text.
  {
    ahv::OpraLoadSpec spec;
    spec.path = path;
    spec.underlying = "ZZZ";
    spec.snapshot_iso = "2026-07-01T19:55:00Z";
    spec.r = 0.03;
    const auto fast = ahv::load_opra_cbbo_from_scan(*scan, spec);
    const auto reference = ahv::load_opra_cbbo_from_scan(full, spec);
    ASSERT_FALSE(fast.has_value());
    ASSERT_FALSE(reference.has_value());
    EXPECT_EQ(fast.error().code(), reference.error().code());
    EXPECT_EQ(fast.error().to_string(), reference.error().to_string());
  }

  fs::remove_all(root);
}

// The whole-hive statement of the same claim: `load_opra_hive` (which now scans
// once per date and splits through the index) must produce the same panels the
// per-symbol file loader does. `load_opra_daterange` over the v1 layout is the
// independent reference -- a different on-disk layout, a different code path, the
// same rows -- so this is not the indexed path checking its own homework.
TEST(OpraHive, ScanSplitStillMatchesThePerSymbolFileLoader) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_hive_scan_v1_parity";
  fs::remove_all(root);
  tsupport::SyntheticHiveSpec fx;
  fx.dates = {"2026-07-01", "2026-07-02"};
  tsupport::write_synthetic_hive_v2(root / "v2", fx);
  tsupport::write_synthetic_hive_v1(root / "v1", fx);

  ahv::OpraHiveSpec hspec;
  hspec.root_dir = (root / "v2").string();
  hspec.date_lo = "2026-07-01";
  hspec.date_hi = "2026-07-02";
  hspec.symbols = {"AAA", "BBB", "CCC"};
  hspec.r = 0.03;
  const auto hive = ahv::load_opra_hive(hspec);
  ASSERT_TRUE(hive.has_value()) << hive.error().to_string();
  ASSERT_EQ(hive->n_loaded, std::size_t{6});

  for (const std::string &date : fx.dates) {
    for (const std::string &sym : fx.symbols) {
      const ahv::OpraBatchEntry *cell = find_cell(*hive, sym, date);
      ASSERT_NE(cell, nullptr) << sym << "/" << date;
      ASSERT_TRUE(cell->panel.has_value()) << sym << "/" << date;

      ahv::OpraLoadSpec fspec;
      fspec.path = (root / "v1" / sym / (date + ".parquet")).string();
      fspec.underlying = sym;
      fspec.snapshot_iso = date + hspec.snapshot_suffix;
      fspec.r = hspec.r;
      const auto file_panel = ahv::load_opra_cbbo_parquet(fspec);
      ASSERT_TRUE(file_panel.has_value()) << sym << "/" << date;

      EXPECT_EQ(cell->panel->n_contracts, file_panel->n_contracts) << sym << "/" << date;
      EXPECT_EQ(cell->panel->n_dropped, file_panel->n_dropped) << sym << "/" << date;
      EXPECT_EQ(cell->panel->implied_spot, file_panel->implied_spot) << sym << "/" << date;

      // Quote-for-quote, as a SET. The two layouts store the same quotes in
      // different physical order -- v2 sorts each date file by
      // (underlying, symbol), v1 writes generation order -- so row INDEX is not
      // comparable across them and only the contents are. (Within one layout the
      // order IS pinned, and that is what the indexed-vs-full-scan test above
      // asserts exactly.)
      const auto key_sorted = [](const std::vector<atx::vol::QuoteRow> &rows) {
        std::vector<std::tuple<std::string, double, int, double, double>> out;
        out.reserve(rows.size());
        for (const auto &r : rows) {
          out.emplace_back(r.expiry_iso, r.strike, static_cast<int>(r.side), r.bid, r.ask);
        }
        std::sort(out.begin(), out.end());
        return out;
      };
      EXPECT_EQ(key_sorted(cell->panel->frame.rows), key_sorted(file_panel->frame.rows))
          << sym << "/" << date;
    }
  }
  fs::remove_all(root);
}

// ── load_opra_hive: the date-partitioned OPRA hive v2 loader (Task 3) ─────────
//
// These exercise the centerpiece loader against the Task 2 synthetic hive-v2
// fixture. Exact counts are for the default SyntheticHiveSpec (36 quotes per
// (symbol, date) panel; symbols AAA/BBB/CCC). Where a pristine gap-free grid is
// wanted, a 3-CONTIGUOUS-date fixture is used so the calendar enumeration has no
// holes; the default (07-01/02/06) fixture is used where a missing date is under
// test.

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

  // A genuinely broken file is a DEFECT, never a coverage hole: the split must
  // not let corruption hide in hole noise (a hole and a wrong-schema file both
  // surface InvalidArgument, so this is exactly what an error-code test would
  // have gotten wrong).
  EXPECT_EQ(res->n_coverage_holes, std::size_t{0});
  EXPECT_EQ(res->n_loaded + res->n_missing + res->n_error, res->n_total);

  for (const std::string &sym : {"AAA", "BBB", "CCC"}) {
    const ahv::OpraBatchEntry *e = find_cell(*res, sym, "2026-07-02");
    ASSERT_NE(e, nullptr) << sym;
    ASSERT_FALSE(e->panel.has_value()) << sym;
    EXPECT_NE(e->panel.error().code(), ahv::ErrorCode::NotFound) << sym; // present, not missing
    EXPECT_FALSE(e->coverage_hole) << sym;
  }
  fs::remove_all(root);
}

// Explicit-symbol mode: a requested symbol absent from a present, readable date
// file is a COVERAGE HOLE, not a defect. This path is classified by the loader's
// own per-date underlying scan (phase A never reads the file in explicit mode),
// so it needs its own coverage separate from the discovery case below.
TEST(OpraHive, ExplicitSymbolsCoverageHoleIsNotALoadError) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_hive_explicit_hole";
  const fs::path tmp2 = fs::temp_directory_path() / "atx_opra_hive_explicit_hole_d2";
  fs::remove_all(root);
  fs::remove_all(tmp2);

  // Uniform base (3 dates x AAA/BBB/CCC), then rewrite 2026-07-02 with ONLY
  // {AAA, CCC}: BBB is present on the other two dates and a hole on that one.
  tsupport::SyntheticHiveSpec full;
  full.dates = {"2026-07-01", "2026-07-02", "2026-07-03"};
  tsupport::write_synthetic_hive_v2(root, full);

  tsupport::SyntheticHiveSpec d2only;
  d2only.dates = {"2026-07-02"};
  d2only.symbols = {"AAA", "CCC"};
  tsupport::write_synthetic_hive_v2(tmp2, d2only);

  std::error_code fec;
  fs::remove(root / "date=2026-07-02" / "data.parquet", fec);
  fs::copy_file(tmp2 / "date=2026-07-02" / "data.parquet",
                root / "date=2026-07-02" / "data.parquet", fec);
  ASSERT_FALSE(fec) << fec.message();

  ahv::OpraHiveSpec spec;
  spec.root_dir = root.string();
  spec.date_lo = "2026-07-01";
  spec.date_hi = "2026-07-03";
  spec.symbols = {"AAA", "BBB", "CCC"}; // EXPLICIT, not discovery
  spec.r = 0.03;

  spec.n_threads = 1;
  const auto serial = ahv::load_opra_hive(spec);
  spec.n_threads = 8;
  const auto parallel = ahv::load_opra_hive(spec);
  ASSERT_TRUE(serial.has_value()) << serial.error().to_string();
  ASSERT_TRUE(parallel.has_value()) << parallel.error().to_string();

  EXPECT_EQ(serial->n_total, std::size_t{9});
  EXPECT_EQ(serial->n_loaded, std::size_t{8});
  EXPECT_EQ(serial->n_missing, std::size_t{0});
  EXPECT_EQ(serial->n_error, std::size_t{1});
  EXPECT_EQ(serial->n_coverage_holes, std::size_t{1}); // the ONE hole, not a defect
  EXPECT_EQ(serial->n_loaded + serial->n_missing + serial->n_error, serial->n_total);

  const ahv::OpraBatchEntry *hole = find_cell(*serial, "BBB", "2026-07-02");
  ASSERT_NE(hole, nullptr);
  ASSERT_FALSE(hole->panel.has_value());
  EXPECT_TRUE(hole->coverage_hole);
  // The Err shape is unchanged by the structural classification.
  EXPECT_NE(hole->panel.error().code(), ahv::ErrorCode::NotFound);

  // Every loaded cell stays a non-hole, and the classification is worker-count
  // independent (it is derived per date from that date's own table).
  EXPECT_EQ(serial->n_coverage_holes, parallel->n_coverage_holes);
  ASSERT_EQ(serial->entries.size(), parallel->entries.size());
  for (std::size_t i = 0; i < serial->entries.size(); ++i) {
    EXPECT_EQ(serial->entries[i].coverage_hole, parallel->entries[i].coverage_hole)
        << "entry " << i;
    EXPECT_EQ(serial->entries[i].panel.has_value(), parallel->entries[i].panel.has_value())
        << "entry " << i;
    if (serial->entries[i].panel.has_value()) {
      EXPECT_FALSE(serial->entries[i].coverage_hole) << "entry " << i;
    }
  }

  fs::remove_all(root);
  fs::remove_all(tmp2);
}

// A file that is BOTH schema-broken AND missing a requested symbol. The hole
// check precedes the table seam's required-column validation, so the absent
// symbol reads as a coverage hole while the symbols the file DOES carry surface
// the schema defect — which is what keeps a broken date from ever reporting as
// all-holes. (CorruptDateFileCountsError cannot cover this: it truncates the file
// so read_parquet fails outright and no cell can be classified at all.)
TEST(OpraHive, SchemaBrokenFileStillReportsHoleAndDefectSeparately) {
  namespace io = atx::core::io;
  const fs::path root = fs::temp_directory_path() / "atx_opra_hive_broken_schema";
  fs::remove_all(root);
  const fs::path dir = root / "date=2026-07-01";
  fs::create_directories(dir);

  // Valid, readable parquet with a good `underlying` column carrying {AAA, CCC},
  // but MISSING the required `ask_sz` column — so `underlying` discovery works
  // while the seam rejects every cell it is asked to build.
  const std::vector<std::string> und = {"AAA", "AAA", "CCC", "CCC"};
  const std::vector<std::string> sym = {
      "AAA   260729C00100000", "AAA   260729P00100000",
      "CCC   260729C00100000", "CCC   260729P00100000"};
  const std::vector<atx::i64> ts(4, atx::i64{1'782'244'500'000'000'000});
  const std::vector<atx::i64> inst = {1, 2, 3, 4};
  const std::vector<atx::i64> px(4, atx::i64{1'000'000'000});
  const std::vector<atx::i64> sz(4, atx::i64{10});
  const std::vector<io::WriteColumn> cols = {
      {"ts", std::span<const atx::i64>(ts)},
      {"underlying", std::span<const std::string>(und)},
      {"symbol", std::span<const std::string>(sym)},
      {"instrument_id", std::span<const atx::i64>(inst)},
      {"bid_px", std::span<const atx::i64>(px)},
      {"ask_px", std::span<const atx::i64>(px)},
      {"bid_sz", std::span<const atx::i64>(sz)},
      // ask_sz DELIBERATELY omitted — a required column.
  };
  ASSERT_TRUE(io::write_parquet(cols, (dir / "data.parquet").string()).has_value());

  ahv::OpraHiveSpec spec;
  spec.root_dir = root.string();
  spec.date_lo = "2026-07-01";
  spec.date_hi = "2026-07-01";
  spec.symbols = {"AAA", "BBB", "CCC"};
  spec.r = 0.03;

  const auto res = ahv::load_opra_hive(spec);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  EXPECT_EQ(res->n_total, std::size_t{3});
  EXPECT_EQ(res->n_loaded, std::size_t{0});  // the schema defect fails every build
  EXPECT_EQ(res->n_missing, std::size_t{0}); // the file is present
  EXPECT_EQ(res->n_error, std::size_t{3});
  EXPECT_EQ(res->n_coverage_holes, std::size_t{1}); // only BBB, the absent symbol

  const ahv::OpraBatchEntry *bbb = find_cell(*res, "BBB", "2026-07-01");
  ASSERT_NE(bbb, nullptr);
  ASSERT_FALSE(bbb->panel.has_value());
  EXPECT_TRUE(bbb->coverage_hole);

  // The symbols the file DOES carry surface the real defect, so the date can
  // never be mistaken for "all holes".
  for (const std::string &s : {"AAA", "CCC"}) {
    const ahv::OpraBatchEntry *e = find_cell(*res, s, "2026-07-01");
    ASSERT_NE(e, nullptr) << s;
    ASSERT_FALSE(e->panel.has_value()) << s;
    EXPECT_FALSE(e->coverage_hole) << s;
  }
  fs::remove_all(root);
}

// Non-uniform hive: one date's file is missing a symbol that OTHER dates carry.
// Discovery must resolve the 3-symbol UNION, lay out a RECTANGULAR grid over it,
// and make the absent (date, symbol) cell a VISIBLE Err (n_error) — not a silent
// hole. Determinism must still hold on the non-uniform case (1 vs 8 threads).
TEST(OpraHive, DiscoversUnionAcrossNonUniformDates) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_hive_nonuniform";
  const fs::path tmp2 = fs::temp_directory_path() / "atx_opra_hive_nonuniform_d2";
  fs::remove_all(root);
  fs::remove_all(tmp2);

  // Uniform base (3 dates × AAA/BBB/CCC), then rewrite 2026-07-02 with ONLY
  // {AAA, CCC} so BBB is a coverage hole on that date (present on the others).
  tsupport::SyntheticHiveSpec full;
  full.dates = {"2026-07-01", "2026-07-02", "2026-07-03"};
  tsupport::write_synthetic_hive_v2(root, full);

  tsupport::SyntheticHiveSpec d2only;
  d2only.dates = {"2026-07-02"};
  d2only.symbols = {"AAA", "CCC"};
  tsupport::write_synthetic_hive_v2(tmp2, d2only);

  const fs::path d2_dst = root / "date=2026-07-02" / "data.parquet";
  const fs::path d2_src = tmp2 / "date=2026-07-02" / "data.parquet";
  ASSERT_TRUE(fs::exists(d2_src));
  std::error_code fec;
  fs::remove(d2_dst, fec);
  fs::copy_file(d2_src, d2_dst, fec);
  ASSERT_FALSE(fec) << fec.message();

  ahv::OpraHiveSpec spec;
  spec.root_dir = root.string();
  spec.date_lo = "2026-07-01";
  spec.date_hi = "2026-07-03";
  spec.symbols = {}; // discover -> union across dates
  spec.r = 0.03;

  spec.n_threads = 1;
  const auto serial = ahv::load_opra_hive(spec);
  spec.n_threads = 8;
  const auto parallel = ahv::load_opra_hive(spec);
  ASSERT_TRUE(serial.has_value()) << serial.error().to_string();
  ASSERT_TRUE(parallel.has_value()) << parallel.error().to_string();

  // Rectangular grid over the 3-symbol union: 3 dates × 3 = 9 cells; the one
  // coverage hole (2026-07-02, BBB) is a visible error.
  EXPECT_EQ(serial->n_total, std::size_t{9});
  EXPECT_EQ(serial->n_loaded, std::size_t{8});
  EXPECT_EQ(serial->n_missing, std::size_t{0});
  EXPECT_EQ(serial->n_error, std::size_t{1});
  ASSERT_EQ(serial->entries.size(), std::size_t{9});

  const std::vector<std::string> u = {"AAA", "BBB", "CCC"};
  const std::vector<std::string> dts = {"2026-07-01", "2026-07-02", "2026-07-03"};
  for (std::size_t d = 0; d < dts.size(); ++d) {
    for (std::size_t s = 0; s < u.size(); ++s) {
      const std::size_t i = d * 3 + s; // rectangular indexing must hold
      EXPECT_EQ(serial->entries[i].date, dts[d]) << "entry " << i;
      EXPECT_EQ(serial->entries[i].symbol, u[s]) << "entry " << i;
    }
  }

  // ...and it is classified as a COVERAGE HOLE, not a defect: n_coverage_holes is
  // a SUB-COUNT of n_error (the partition invariant is untouched), so the build
  // driver can report "the universe is sparse" separately from "the data is bad".
  EXPECT_EQ(serial->n_coverage_holes, std::size_t{1});
  EXPECT_EQ(serial->n_loaded + serial->n_missing + serial->n_error, serial->n_total);

  // The coverage hole is an Err entry, NOT NotFound (the file is present).
  const ahv::OpraBatchEntry *hole = find_cell(*serial, "BBB", "2026-07-02");
  ASSERT_NE(hole, nullptr);
  ASSERT_FALSE(hole->panel.has_value());
  EXPECT_NE(hole->panel.error().code(), ahv::ErrorCode::NotFound);
  EXPECT_TRUE(hole->coverage_hole);
  // The two symbols that ARE on that date loaded fine.
  for (const std::string &sym : {"AAA", "CCC"}) {
    const ahv::OpraBatchEntry *e = find_cell(*serial, sym, "2026-07-02");
    ASSERT_NE(e, nullptr) << sym;
    EXPECT_TRUE(e->panel.has_value())
        << sym << ": " << (e->panel.has_value() ? "" : e->panel.error().to_string());
  }

  // Determinism holds on the non-uniform hive.
  EXPECT_EQ(serial->n_loaded, parallel->n_loaded);
  EXPECT_EQ(serial->n_error, parallel->n_error);
  EXPECT_EQ(serial->n_coverage_holes, parallel->n_coverage_holes);
  ASSERT_EQ(serial->entries.size(), parallel->entries.size());
  for (std::size_t i = 0; i < serial->entries.size(); ++i) {
    EXPECT_EQ(serial->entries[i].symbol, parallel->entries[i].symbol) << "entry " << i;
    EXPECT_EQ(serial->entries[i].date, parallel->entries[i].date) << "entry " << i;
    EXPECT_EQ(serial->entries[i].panel.has_value(), parallel->entries[i].panel.has_value())
        << "entry " << i;
  }

  fs::remove_all(root);
  fs::remove_all(tmp2);
}

// ── OpraHiveSpec::time — which clock the board's T is built on ───────────────
//
// The hive is the only entry point production uses to build a board, and until
// `OpraHiveSpec::time` existed it had no way to express anything but
// Calendar365: `OpraLoadSpec::time` was reachable from the single-file seam and
// nowhere else, so `atx-vol-chain-export` structurally could not fit on
// SpiderRock's hybrid vol-time clock.
//
// Two things have to hold, and they pull in opposite directions:
//   * the flag must actually MOVE `Chain::T` (otherwise it is decoration), and
//   * the DEFAULT must not move at all (otherwise every existing chain parquet,
//     surface archive and backtest repin silently).
// So the calendar branch is pinned to an independently-written literal while the
// vol-time branch is checked against the kernel itself.

namespace {

// One expiry's baked identity: the instants it was built from and the
// year-fraction `data_install` assigned it. Enough to interrogate a clock
// without re-deriving one.
struct FrontExpiry {
  std::int64_t snapshot_ns{0};
  std::int64_t expiry_ns{0};
  double T{0.0};
};

// Load ONE (date, symbol) cell of a synthetic hive under `convention` and decode
// its FRONT listed expiry (`ids()` is ascending expiry, then strike, then side).
// Every other field of the spec is held fixed, so a difference between two calls
// is attributable to the clock and nothing else.
//
// Void + ASSERT_* so a loader failure aborts with its own message; call it under
// ASSERT_NO_FATAL_FAILURE.
void load_front_expiry(const fs::path &root, const std::string &date,
                       ahv::TimeConvention convention, FrontExpiry &out) {
  ahv::OpraHiveSpec spec;
  spec.root_dir = root.string();
  spec.date_lo = date;
  spec.date_hi = date;
  spec.symbols = {"AAA"};
  spec.r = 0.03;
  spec.n_threads = 1;
  spec.time.convention = convention; // the ONLY field that varies

  const auto batch = ahv::load_opra_hive(spec);
  ASSERT_TRUE(batch.has_value()) << batch.error().to_string();
  ASSERT_EQ(batch->entries.size(), std::size_t{1});
  const ahv::OpraBatchEntry &cell = batch->entries.front();
  ASSERT_TRUE(cell.panel.has_value()) << cell.panel.error().to_string();

  // The hive must forward its convention to the panel, which stamps it on the
  // frame; that stamp is the ONLY thing `data_install` reads for Chain::T, so
  // asserting it here localizes a wiring break to the loader.
  ASSERT_EQ(cell.panel->frame.time.convention, convention);
  out.snapshot_ns = cell.panel->frame.snapshot_ts_ns;

  const ahv::CorpusBoard board =
      ahv::corpus_board_from_opra(cell.date, cell.symbol, *cell.panel);
  const auto chain = ahv::OptionChain::from_frame(board.frame, board.env);
  ASSERT_TRUE(chain.has_value()) << chain.error().to_string();
  // ...and the installed chain must be able to SAY which clock it is on.
  ASSERT_EQ(chain->time().convention, convention);

  const std::vector<ahv::OptionId> ids = chain->ids();
  ASSERT_FALSE(ids.empty());
  const auto front = chain->at(ids.front());
  ASSERT_TRUE(front.has_value()) << front.error().to_string();
  out.expiry_ns = front->expiry_ns;
  out.T = front->T;
}

} // namespace

TEST(OpraHive, VolTimeConventionMovesChainTWhileCalendar365StaysPinned) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_hive_time_convention";
  fs::remove_all(root);
  tsupport::SyntheticHiveSpec fx;
  fx.symbols = {"AAA"};
  fx.dates = {"2026-07-01"};
  tsupport::write_synthetic_hive_v2(root, fx);

  FrontExpiry cal;
  ASSERT_NO_FATAL_FAILURE(
      load_front_expiry(root, "2026-07-01", ahv::TimeConvention::Calendar365, cal));
  FrontExpiry vt;
  ASSERT_NO_FATAL_FAILURE(
      load_front_expiry(root, "2026-07-01", ahv::TimeConvention::VolTime, vt));

  // Identical instants on both runs: the clock is the only moving part, so the
  // T difference below cannot be an expiry-stamping or snapshot artifact.
  EXPECT_EQ(cal.snapshot_ns, vt.snapshot_ns);
  EXPECT_EQ(cal.expiry_ns, vt.expiry_ns);

  // REGRESSION PIN on the untouched default. The front expiry is trade date +
  // 28d settling 16:00 ET (20:00Z in July), read from the fixture's 19:55Z
  // snapshot: 28 days and 5 minutes. Written as an independent literal instead
  // of by calling the conversion under test, so it fails if EITHER the expiry
  // instant or the 365.25-day year moves.
  constexpr double kCal365FrontT = (28.0 * 86400.0 + 300.0) / (365.25 * 86400.0);
  EXPECT_NEAR(cal.T, kCal365FrontT, 1e-15);

  // ...and the vol-time clock genuinely produces a different number. Only the
  // FACT of the difference is asserted: its size is a property of the
  // VolTimeParams constants, which are owned by a different lane.
  EXPECT_GT(std::abs(vt.T - cal.T), 1e-6)
      << "calendar365=" << cal.T << " voltime=" << vt.T;

  fs::remove_all(root);
}

TEST(OpraHive, VolTimeChainTIsExactlyVolTimeYearsForTheSameSpan) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_hive_time_kernel";
  fs::remove_all(root);
  tsupport::SyntheticHiveSpec fx;
  fx.symbols = {"AAA"};
  fx.dates = {"2026-07-01"};
  tsupport::write_synthetic_hive_v2(root, fx);

  FrontExpiry vt;
  ASSERT_NO_FATAL_FAILURE(
      load_front_expiry(root, "2026-07-01", ahv::TimeConvention::VolTime, vt));

  // The wiring must PASS the value through, not rescale, clamp or re-round it.
  // Checked against the kernel rather than a literal so this stays true when the
  // VolTimeParams constants are recalibrated.
  const ahv::TimeSpec defaults; // the VolTimeParams a convention-only spec carries
  const auto direct = ahv::vol_time_years(vt.snapshot_ns, vt.expiry_ns, defaults.vol_time,
                                          ahv::VolTimeCalendar::us_default());
  ASSERT_TRUE(direct.has_value()) << direct.error().to_string();
  EXPECT_DOUBLE_EQ(vt.T, *direct);

  fs::remove_all(root);
}

// An expiry PAST the vol-time calendar's covered window must cost THAT EXPIRY,
// not the whole symbol -- and must not vanish quietly either.
//
// This is the operationally dangerous case, and it is not hypothetical: a real
// board carries expiries beyond any horizon a closure table can honestly claim,
// including the production store's 2099-01-01 sentinel, which is not a contract
// at all. Four outcomes were possible and only one is right:
//   * silently treat the uncovered days as OPEN -- a wrong T with no symptom;
//   * silently DROP the uncovered rows -- a board short its long end that says
//     so nowhere, i.e. a chain carrying two clocks' worth of coverage;
//   * fail the CELL -- which is what this used to do, and it made one junk row
//     cost every quote on the name, so vol-time was unusable on a real board;
//   * drop that expiry, count it, and name it.
// The loader takes the fourth. `n_uncovered_expiry_rows` /
// `n_cells_with_uncovered_expiries` (batch) and `OpraPanel::uncovered_expiries`
// (cell) are what separate it from the silent-drop option above; the chain-export
// census prints both.
//
// The same fixture under Calendar365 keeps every row, which is what makes this a
// statement about the CLOCK rather than about the fixture.
TEST(OpraHive, VolTimeDropsTheUncoverableExpiryAndKeepsTheRestOfTheCell) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_hive_time_window";
  fs::remove_all(root);
  tsupport::SyntheticHiveSpec fx;
  fx.symbols = {"AAA"};
  // Trade date inside the window; the +28d expiry (2032-12-29) is covered and the
  // +56d one (2033-01-26) is not, so this board straddles the boundary exactly
  // like a real name carrying an expiry past the calendar's end.
  fx.dates = {"2032-12-01"};
  tsupport::write_synthetic_hive_v2(root, fx);

  const auto load = [&root](ahv::TimeConvention convention) {
    ahv::OpraHiveSpec spec;
    spec.root_dir = root.string();
    spec.date_lo = "2032-12-01";
    spec.date_hi = "2032-12-01";
    spec.symbols = {"AAA"};
    spec.r = 0.03;
    spec.n_threads = 1;
    spec.time.convention = convention;
    return ahv::load_opra_hive(spec);
  };

  // Calendar365 reads the straddling board without complaint: both expiries,
  // 36 rows (9 strikes x 2 expiries x {C,P}), nothing dropped for coverage.
  const auto cal = load(ahv::TimeConvention::Calendar365);
  ASSERT_TRUE(cal.has_value()) << cal.error().to_string();
  ASSERT_EQ(cal->entries.size(), std::size_t{1});
  ASSERT_TRUE(cal->entries.front().panel.has_value())
      << cal->entries.front().panel.error().to_string();
  EXPECT_EQ(cal->n_loaded, std::size_t{1});
  EXPECT_EQ(cal->n_error, std::size_t{0});
  EXPECT_EQ(cal->entries.front().panel->n_expiries, std::size_t{2});
  EXPECT_EQ(cal->entries.front().panel->n_contracts, std::size_t{36});
  EXPECT_EQ(cal->n_uncovered_expiry_rows, std::size_t{0});
  EXPECT_EQ(cal->n_cells_with_uncovered_expiries, std::size_t{0});

  // VolTime keeps the cell and drops the one expiry it cannot resolve.
  const auto vt = load(ahv::TimeConvention::VolTime);
  ASSERT_TRUE(vt.has_value()) << vt.error().to_string();
  ASSERT_EQ(vt->entries.size(), std::size_t{1});
  const ahv::OpraBatchEntry &cell = vt->entries.front();
  ASSERT_TRUE(cell.panel.has_value())
      << "one uncoverable expiry must not cost the symbol: " << cell.panel.error().to_string();
  EXPECT_EQ(vt->n_loaded, std::size_t{1});
  EXPECT_EQ(vt->n_error, std::size_t{0});
  EXPECT_FALSE(cell.coverage_hole);
  // Half the board survives: the covered near expiry, all 18 of its rows.
  EXPECT_EQ(cell.panel->n_expiries, std::size_t{1});
  EXPECT_EQ(cell.panel->n_contracts, std::size_t{18});
  // ...and the loss is COUNTED and NAMED rather than inferred from the gap.
  EXPECT_EQ(cell.panel->n_dropped_uncovered_expiry, std::size_t{18});
  ASSERT_EQ(cell.panel->uncovered_expiries.size(), std::size_t{1});
  EXPECT_EQ(cell.panel->uncovered_expiries.front(), "2033-01-26");
  EXPECT_FALSE(cell.panel->uncovered_expiry_reason.empty());
  EXPECT_EQ(vt->n_uncovered_expiry_rows, std::size_t{18});
  EXPECT_EQ(vt->n_cells_with_uncovered_expiries, std::size_t{1});
}

// ── SR-DIVS: the discrete cash-dividend schedule reaches the frame ───────────
//
// `OpraHiveSpec::cash_divs` exists because folding every dividend into ONE
// solved continuous borrow cannot reproduce an American early-exercise
// boundary: that boundary depends on WHEN each cash dividend lands, not only on
// the forward the dividends integrate to. The two cases below pin the only
// things the loader itself owns — that a supplied schedule lands on the right
// symbol's frame, and that the default is still EMPTY, which is the behaviour
// every existing caller relies on.

TEST(OpraHive, DefaultSpecLeavesEveryFrameCashDividendScheduleEmpty) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_hive_divs_default";
  fs::remove_all(root);
  tsupport::SyntheticHiveSpec fx;
  fx.symbols = {"AAA", "BBB"};
  fx.dates = {"2026-07-01"};
  tsupport::write_synthetic_hive_v2(root, fx);

  ahv::OpraHiveSpec spec;
  spec.root_dir = root.string();
  spec.date_lo = "2026-07-01";
  spec.date_hi = "2026-07-01";
  spec.symbols = {"AAA", "BBB"};
  spec.r = 0.03;
  spec.n_threads = 1;

  const auto batch = ahv::load_opra_hive(spec);
  ASSERT_TRUE(batch.has_value()) << batch.error().to_string();
  ASSERT_EQ(batch->n_loaded, std::size_t{2});
  for (const ahv::OpraBatchEntry &e : batch->entries) {
    ASSERT_TRUE(e.panel.has_value()) << e.symbol << ": " << e.panel.error().to_string();
    EXPECT_TRUE(e.panel->frame.divs.empty()) << e.symbol;
  }

  fs::remove_all(root);
}

TEST(OpraHive, SuppliedCashDividendScheduleReachesOnlyItsOwnSymbolsFrame) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_hive_divs_supplied";
  fs::remove_all(root);
  tsupport::SyntheticHiveSpec fx;
  fx.symbols = {"AAA", "BBB"};
  fx.dates = {"2026-07-01"};
  tsupport::write_synthetic_hive_v2(root, fx);

  // Two events, deliberately handed in DESCENDING ex-date order: the loader
  // copies the schedule verbatim (forward_div_corrected scans linearly and
  // requires no order), so this also pins that nothing silently re-sorts it.
  const std::int64_t ex_late = ahv::iso_to_ns("2026-08-15");
  const std::int64_t ex_early = ahv::iso_to_ns("2026-07-20");
  ASSERT_GT(ex_early, 0);
  ASSERT_GT(ex_late, ex_early);

  ahv::OpraHiveSpec spec;
  spec.root_dir = root.string();
  spec.date_lo = "2026-07-01";
  spec.date_hi = "2026-07-01";
  spec.symbols = {"AAA", "BBB"};
  spec.r = 0.03;
  spec.n_threads = 1;
  spec.cash_divs["AAA"] = {ahv::DividendEvent{ex_late, 0.75},
                           ahv::DividendEvent{ex_early, 0.25}};
  // A schedule for a symbol this run never asks for must not leak onto any cell.
  spec.cash_divs["ZZZ"] = {ahv::DividendEvent{ex_early, 9.99}};

  const auto batch = ahv::load_opra_hive(spec);
  ASSERT_TRUE(batch.has_value()) << batch.error().to_string();
  ASSERT_EQ(batch->n_loaded, std::size_t{2});

  const ahv::OpraBatchEntry *aaa = find_cell(*batch, "AAA", "2026-07-01");
  ASSERT_NE(aaa, nullptr);
  ASSERT_TRUE(aaa->panel.has_value()) << aaa->panel.error().to_string();
  ASSERT_EQ(aaa->panel->frame.divs.size(), std::size_t{2});
  EXPECT_EQ(aaa->panel->frame.divs[0].ex_date_ns, ex_late);
  EXPECT_DOUBLE_EQ(aaa->panel->frame.divs[0].amount, 0.75);
  EXPECT_EQ(aaa->panel->frame.divs[1].ex_date_ns, ex_early);
  EXPECT_DOUBLE_EQ(aaa->panel->frame.divs[1].amount, 0.25);

  // BBB is in the SAME hive load and has no entry in the map: an unmatched
  // symbol keeps the historical empty schedule rather than inheriting a
  // neighbour's.
  const ahv::OpraBatchEntry *bbb = find_cell(*batch, "BBB", "2026-07-01");
  ASSERT_NE(bbb, nullptr);
  ASSERT_TRUE(bbb->panel.has_value()) << bbb->panel.error().to_string();
  EXPECT_TRUE(bbb->panel->frame.divs.empty());

  fs::remove_all(root);
}

} // namespace
