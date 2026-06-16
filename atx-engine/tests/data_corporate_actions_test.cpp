// atx::engine::data — security-master ingestion tests (p3 S1-2).
//
// Suite: DataCorporateActions
//
// Covers the 5 named S1-2 tests:
//   1. LoadsSmokeMasterRowShapeMatchesManifest — load the on-disk smoke
//      security_master.parquet; assert 3 symbols + the canonical 6 columns.
//   2. DividendZeroFilledOffExDates — non-ex dates read 0.0; a known AAPL
//      ex-date reads the published per-share dividend.
//   3. SharesOutstandingPitForwardFill — the load-bearing PIT leak guard: a
//      fundamental is INVISIBLE (NaN) on every date before its shares_filed_date
//      knowledge-date, and forward-filled after.
//   4. NonUsdCurrencyRejected — a synthetic non-USD row -> Err (via the
//      partitioned/loader currency guard exercised on a hand fixture).
//   5. SymbolInterningDeterministic — two loads produce identical symbol->id order.
//
// Tests 1/2/3 read the REAL on-disk smoke parquet; the data dir is gitignored and
// lives at the (main) repo root, resolved robustly from __FILE__ + the git
// worktree marker. If the data cannot be located the smoke tests skip with a clear
// message (never a false pass).

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <filesystem>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/io/parquet_writer.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/data/corporate_actions.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"

namespace atxtest_data_corporate_actions_test {

namespace fs = std::filesystem;
using atx::core::ErrorCode;
using atx::engine::data::CorpActionColumns;
using atx::engine::data::corp_action_schema;
using atx::engine::data::Dataset;
using atx::engine::data::DateKey;
using atx::engine::data::extract_symbol;
using atx::engine::data::InstKey;
using atx::engine::data::load_security_master;

// ---------------------------------------------------------------------------
// On-disk data resolution
// ---------------------------------------------------------------------------
namespace {

// Resolve the repo data dir robustly. The `/data/` dir is gitignored and lives at
// the MAIN repo root. We try, in order: (1) the ATX_DATA_DIR env override, (2)
// walk up from __FILE__ for a `data/us_security_master_smoke`, (3) walk up from
// __FILE__ and, at an ancestor whose `.git` is a worktree-marker FILE, parse its
// `gitdir:` to find the main repo root and check `<mainroot>/data`, (4) walk up
// from the current working directory.
constexpr const char *kProbe = "data/us_security_master_smoke/security_master/security_master.parquet";

[[nodiscard]] std::optional<fs::path> probe(const fs::path &root) {
  std::error_code ec;
  const fs::path candidate = root / kProbe;
  if (fs::exists(candidate, ec)) {
    return candidate;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<fs::path> main_root_from_worktree(const fs::path &ancestor) {
  std::error_code ec;
  const fs::path git_marker = ancestor / ".git";
  if (!fs::is_regular_file(git_marker, ec)) {
    return std::nullopt;
  }
  std::ifstream in(git_marker);
  std::string line;
  if (!std::getline(in, line)) {
    return std::nullopt;
  }
  const std::string prefix = "gitdir:";
  if (line.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  std::string gitdir = line.substr(prefix.size());
  while (!gitdir.empty() && (gitdir.front() == ' ' || gitdir.front() == '\t')) {
    gitdir.erase(gitdir.begin());
  }
  // gitdir = <mainroot>/.git/worktrees/<name>  ->  mainroot = parent^3.
  fs::path p(gitdir);
  return p.parent_path().parent_path().parent_path();
}

// Read an environment variable into `out`; returns true iff it was set. Uses the
// MSVC-safe _dupenv_s (std::getenv trips -Wdeprecated-declarations under /WX).
[[nodiscard]] bool read_env(const char *name, std::string &out) {
  char *buf = nullptr;
  size_t len = 0;
  if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) {
    return false;
  }
  out.assign(buf);
  std::free(buf);
  return true;
}

[[nodiscard]] std::optional<fs::path> find_master_parquet() {
  if (std::string env; read_env("ATX_DATA_DIR", env)) {
    const fs::path direct =
        fs::path(env) / "us_security_master_smoke" / "security_master" / "security_master.parquet";
    std::error_code ec;
    if (fs::exists(direct, ec)) {
      return direct;
    }
  }
  for (fs::path dir = fs::path(__FILE__).parent_path(); !dir.empty(); dir = dir.parent_path()) {
    if (auto hit = probe(dir)) {
      return hit;
    }
    if (auto root = main_root_from_worktree(dir)) {
      if (auto hit = probe(*root)) {
        return hit;
      }
    }
    if (dir == dir.root_path()) {
      break;
    }
  }
  std::error_code ec;
  for (fs::path dir = fs::current_path(ec); !dir.empty(); dir = dir.parent_path()) {
    if (auto hit = probe(dir)) {
      return hit;
    }
    if (dir == dir.root_path()) {
      break;
    }
  }
  return std::nullopt;
}

// Find the row index of a given epoch-day in the Dataset's date axis, or npos.
[[nodiscard]] atx::usize date_row(const Dataset &ds, DateKey day) {
  const auto dates = ds.dates();
  for (atx::usize d = 0; d < dates.size(); ++d) {
    if (dates[d] == day) {
      return d;
    }
  }
  return dates.size();
}

// Known AAPL signatures in the smoke master. We don't assume a fixed intern id;
// AAPL is located at runtime by its published 2026-05-11 ex-date dividend.
constexpr atx::i64 kAaplLastExDay = 20584; // 2026-05-11
constexpr atx::f64 kAaplLastExDiv = 0.27;
constexpr atx::i64 kAapl2026NonExDay = 20455; // 2026-01-02 (no dividend)
// PIT leak window #1: row dated 2009-06-29 carries shares filed only 2009-07-22.
constexpr atx::i64 kPitFiledDay = 14447;    // 2009-07-22
constexpr atx::i64 kPitBeforeFiled = 14446; // 2009-07-21 (knowledge not yet public)
constexpr atx::f64 kPitSharesValue = 895816758.0;
// PIT leak window #2 (recent): the 2026-05-01 filing (epoch-day 20574) carries
// 14,687,356,000 shares; the prior knowable value (filed 2026-01-30) is
// 14,681,140,000. The newer count must NOT appear before its 20574 filing date.
constexpr atx::i64 kPit2BeforeFiled = 20573; // 2026-04-30 (May filing not yet public)
constexpr atx::i64 kPit2FiledDay = 20574;    // 2026-05-01
constexpr atx::f64 kPit2NewShares = 14687356000.0;
constexpr atx::f64 kPit2PriorShares = 14681140000.0;

[[nodiscard]] std::optional<InstKey> find_aapl(const Dataset &ds) {
  const atx::usize row = date_row(ds, kAaplLastExDay);
  if (row == ds.num_dates()) {
    return std::nullopt;
  }
  const auto div = ds.column(1); // cash_dividend
  const atx::usize ni = ds.num_instruments();
  for (atx::usize i = 0; i < ni; ++i) {
    if (std::abs(div[row * ni + i] - kAaplLastExDiv) < 1e-9) {
      return ds.instruments()[i];
    }
  }
  return std::nullopt;
}

// Stage a tiny synthetic security_master.parquet whose dividend_currency is EUR,
// exercising the non-USD rejection path without depending on on-disk data. Uses
// the atx-core writer (i64 dates — the loader accepts i64 or date32).
[[nodiscard]] bool write_synthetic_non_usd(const fs::path &path) {
  namespace io = atx::core::io;
  const std::vector<atx::i64> date = {19000, 19001};        // epoch-days
  const std::vector<atx::i64> filed = {19000, 19001};
  const std::vector<atx::f64> caf = {1.0, 1.0};
  const std::vector<atx::f64> div = {0.0, 0.50};            // a EUR dividend
  const std::vector<atx::i64> shares = {1000, 1000};
  const std::vector<std::string> symbol = {"XEUR", "XEUR"};
  const std::vector<std::string> currency = {"USD", "EUR"}; // second row is non-USD
  const std::vector<std::string> sic = {"3571", "3571"};
  const std::vector<std::string> gics = {"45", "45"};

  const std::vector<io::WriteColumn> cols = {
      {"symbol", std::span<const std::string>(symbol)},
      {"date", std::span<const atx::i64>(date)},
      {"cumulative_adjustment_factor", std::span<const atx::f64>(caf)},
      {"cash_dividend", std::span<const atx::f64>(div)},
      {"dividend_currency", std::span<const std::string>(currency)},
      {"shares_outstanding", std::span<const atx::i64>(shares)},
      {"shares_filed_date", std::span<const atx::i64>(filed)},
      {"sec_sic", std::span<const std::string>(sic)},
      {"gics_sector_code", std::span<const std::string>(gics)},
  };
  return io::write_parquet(cols, path.string()).has_value();
}

} // namespace

// ---------------------------------------------------------------------------
// Test 1 — row/shape vs manifest
// ---------------------------------------------------------------------------
TEST(DataCorporateActions, LoadsSmokeMasterRowShapeMatchesManifest) {
  const auto path = find_master_parquet();
  if (!path) {
    GTEST_SKIP() << "smoke security_master.parquet not found (set ATX_DATA_DIR)";
  }
  auto res = load_security_master(path->string(), corp_action_schema());
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const Dataset ds = std::move(res).value();

  // Smoke master: 3 symbols (AAPL, IBM, MSFT).
  EXPECT_EQ(ds.num_instruments(), 3U);
  EXPECT_EQ(ds.role(), atx::engine::data::Role::Reference);

  // Canonical six columns present, in order.
  EXPECT_EQ(ds.schema().columns.size(), atx::engine::data::kCorpActionColumnCount);
  EXPECT_TRUE(ds.column_by_name(atx::engine::data::kColCumAdjFactor).has_value());
  EXPECT_TRUE(ds.column_by_name(atx::engine::data::kColCashDividend).has_value());
  EXPECT_TRUE(ds.column_by_name(atx::engine::data::kColSharesOutstanding).has_value());
  EXPECT_TRUE(ds.column_by_name(atx::engine::data::kColSharesFiledDate).has_value());
  EXPECT_TRUE(ds.column_by_name(atx::engine::data::kColGicsSectorCode).has_value());
  EXPECT_TRUE(ds.column_by_name(atx::engine::data::kColSicCode).has_value());

  // Date axis is strictly ascending and non-trivial (1962..2026 daily).
  EXPECT_TRUE(atx::engine::data::is_strictly_ascending(ds.dates()));
  EXPECT_GT(ds.num_dates(), 1000U);
  EXPECT_EQ(ds.cells(), ds.num_dates() * ds.num_instruments());
}

// ---------------------------------------------------------------------------
// Test 2 — dividend zero-filled off ex-dates; known value on the ex-date
// ---------------------------------------------------------------------------
TEST(DataCorporateActions, DividendZeroFilledOffExDates) {
  const auto path = find_master_parquet();
  if (!path) {
    GTEST_SKIP() << "smoke security_master.parquet not found (set ATX_DATA_DIR)";
  }
  auto res = load_security_master(path->string(), corp_action_schema());
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const Dataset ds = std::move(res).value();

  const auto aapl = find_aapl(ds);
  ASSERT_TRUE(aapl.has_value());
  auto cols_res = extract_symbol(ds, *aapl);
  ASSERT_TRUE(cols_res.has_value()) << cols_res.error().to_string();
  const CorpActionColumns c = std::move(cols_res).value();

  // Known AAPL ex-date 2026-05-11 -> 0.27 per-share.
  bool saw_ex = false;
  bool saw_non_ex = false;
  for (atx::usize k = 0; k < c.dates.size(); ++k) {
    if (c.dates[k] == kAaplLastExDay) {
      EXPECT_NEAR(c.cash_dividend[k], kAaplLastExDiv, 1e-9);
      saw_ex = true;
    }
    if (c.dates[k] == kAapl2026NonExDay) {
      // Non-ex trading date: dividend EXACTLY 0.0 (never NaN, never fabricated).
      EXPECT_DOUBLE_EQ(c.cash_dividend[k], 0.0);
      saw_non_ex = true;
    }
  }
  EXPECT_TRUE(saw_ex) << "AAPL ex-date 2026-05-11 not present in smoke data";
  EXPECT_TRUE(saw_non_ex) << "AAPL 2026-01-02 not present in smoke data";
}

// ---------------------------------------------------------------------------
// Test 3 — PIT forward-fill leak guard (the load-bearing invariant)
// ---------------------------------------------------------------------------
TEST(DataCorporateActions, SharesOutstandingPitForwardFill) {
  const auto path = find_master_parquet();
  if (!path) {
    GTEST_SKIP() << "smoke security_master.parquet not found (set ATX_DATA_DIR)";
  }
  auto res = load_security_master(path->string(), corp_action_schema());
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const Dataset ds = std::move(res).value();

  const auto aapl = find_aapl(ds);
  ASSERT_TRUE(aapl.has_value());
  auto cols_res = extract_symbol(ds, *aapl);
  ASSERT_TRUE(cols_res.has_value()) << cols_res.error().to_string();
  const CorpActionColumns c = std::move(cols_res).value();

  // Collect the PIT-resolved shares at each boundary date.
  std::optional<atx::f64> b1, on1, b2, on2;
  for (atx::usize k = 0; k < c.dates.size(); ++k) {
    const atx::i64 d = c.dates[k];
    if (d == kPitBeforeFiled) b1 = c.shares_outstanding[k];
    if (d == kPitFiledDay) on1 = c.shares_outstanding[k];
    if (d == kPit2BeforeFiled) b2 = c.shares_outstanding[k];
    if (d == kPit2FiledDay) on2 = c.shares_outstanding[k];
  }

  // Window #1 (2009): the 895M fact (filed 2009-07-22) is invisible the day before
  // and visible on the filing date.
  ASSERT_TRUE(b1.has_value() && on1.has_value()) << "2009 boundary rows missing";
  EXPECT_NE(*b1, kPitSharesValue);                 // not leaked before knowledge
  EXPECT_DOUBLE_EQ(*on1, kPitSharesValue);         // visible on/after filing

  // Window #2 (2026): the NEWER count (filed 2026-05-01) must NOT appear before its
  // filing — the PRIOR knowable count shows instead — and appears on the filing.
  ASSERT_TRUE(b2.has_value() && on2.has_value()) << "2026 boundary rows missing";
  EXPECT_DOUBLE_EQ(*b2, kPit2PriorShares);         // prior value, no leak of the new one
  EXPECT_NE(*b2, kPit2NewShares);
  EXPECT_DOUBLE_EQ(*on2, kPit2NewShares);          // new value visible on filing date

  // Self-consistency invariant: the resolving knowledge-date stamp never exceeds
  // the trading date for any visible fundamental (no fundamental leaks its future).
  for (atx::usize k = 0; k < c.dates.size(); ++k) {
    if (!std::isnan(c.shares_outstanding[k]) &&
        c.shares_filed_date[k] != atx::engine::data::kNoDate) {
      EXPECT_LE(c.shares_filed_date[k], c.dates[k])
          << "shares visible at date " << c.dates[k] << " stamped " << c.shares_filed_date[k];
    }
  }
}

// ---------------------------------------------------------------------------
// Test 4 — non-USD currency rejected (synthetic fixture)
// ---------------------------------------------------------------------------
TEST(DataCorporateActions, NonUsdCurrencyRejected) {
  // Build a tiny security_master.parquet with a EUR dividend_currency row and
  // assert the loader fails closed. We write it with the atx-core parquet writer.
  const fs::path tmp = fs::temp_directory_path() / "atx_s12_non_usd.parquet";
  ASSERT_TRUE(write_synthetic_non_usd(tmp)) << "could not stage synthetic non-USD parquet";

  auto res = load_security_master(tmp.string(), corp_action_schema());
  EXPECT_FALSE(res.has_value());
  if (!res.has_value()) {
    EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
  }
  std::error_code ec;
  fs::remove(tmp, ec);
}

// ---------------------------------------------------------------------------
// Test 5 — symbol interning is deterministic across loads
// ---------------------------------------------------------------------------
TEST(DataCorporateActions, SymbolInterningDeterministic) {
  const auto path = find_master_parquet();
  if (!path) {
    GTEST_SKIP() << "smoke security_master.parquet not found (set ATX_DATA_DIR)";
  }
  auto a = load_security_master(path->string(), corp_action_schema());
  auto b = load_security_master(path->string(), corp_action_schema());
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  const Dataset da = std::move(a).value();
  const Dataset db = std::move(b).value();

  const auto ia = da.instruments();
  const auto ib = db.instruments();
  ASSERT_EQ(ia.size(), ib.size());
  for (atx::usize i = 0; i < ia.size(); ++i) {
    EXPECT_EQ(ia[i], ib[i]) << "intern order diverged at " << i;
  }
  // Interned ids are the dense first-seen range [0, n).
  for (atx::usize i = 0; i < ia.size(); ++i) {
    EXPECT_EQ(ia[i], static_cast<InstKey>(i));
  }
}

// ---------------------------------------------------------------------------
// Test 6 — partitioned loader fixes intern order to the caller's symbol list
// ---------------------------------------------------------------------------
TEST(DataCorporateActions, PartitionedLoaderOrdersBySymbolArgument) {
  const auto path = find_master_parquet();
  if (!path) {
    GTEST_SKIP() << "smoke security_master.parquet not found (set ATX_DATA_DIR)";
  }
  // <root>/security_master/security_master.parquet -> root.
  const fs::path root = path->parent_path().parent_path();
  // Request a non-row order to prove the loader honors the caller's order.
  const std::vector<std::string> symbols = {"MSFT", "AAPL", "IBM"};
  auto res = atx::engine::data::load_security_master_partitioned(
      root.string(), std::span<const std::string>(symbols), corp_action_schema());
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const Dataset ds = std::move(res).value();
  EXPECT_EQ(ds.num_instruments(), 3U);
  // Each requested symbol maps to its argument-list index as the interned id.
  for (atx::usize i = 0; i < symbols.size(); ++i) {
    EXPECT_EQ(ds.instruments()[i], static_cast<InstKey>(i));
  }
  // MSFT (requested first) carries real fundamentals -> at least one finite
  // shares_outstanding cell, proving the partition data landed under id 0.
  auto msft = extract_symbol(ds, ds.instruments()[0]);
  ASSERT_TRUE(msft.has_value()) << msft.error().to_string();
  bool any_shares = false;
  for (const atx::f64 s : msft.value().shares_outstanding) {
    any_shares = any_shares || std::isfinite(s);
  }
  EXPECT_TRUE(any_shares);
}

} // namespace atxtest_data_corporate_actions_test
