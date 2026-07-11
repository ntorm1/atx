// databento_bulk_opra.cpp — bulk OPRA cbbo-1m puller (P2-5): many underlyings over a
// date range into a per-(symbol,date) Parquet hive, cost-gated with a FREE preflight.
//
// For every calendar date in an inclusive [--start, --end] range and every --symbol, it
// pulls the OPRA full-chain top-of-book (cbbo-1m, consolidated BBO subsampled once per
// minute) at the pre-close snapshot minute (19:55Z) into
//   <out>/{symbol}/{date}.parquet
// — the exact layout the atx-vol `load_opra_daterange` default template reads
// ("{symbol}/{date}.parquet"). Each file carries the
// `pull_opra_cbbo_1m_to_parquet` schema (ts, underlying, symbol, bid/ask px+sz).
//
// COST DISCIPLINE (OPRA historical is billed per symbol-record; a full chain is hundreds
// of records per contract-minute):
//   1. A FREE preflight ALWAYS runs first — MetadataGetRecordCount / MetadataGetBillableSize
//      / MetadataGetCost (the free wrapper is atx::external::databento::estimate_cost) over
//      the exact per-(symbol,date) minute windows we would pull. NO billable egress.
//   2. HARD CAP: if the estimated TOTAL exceeds --cap the tool REFUSES and exits non-zero
//      WITHOUT pulling. The same --cap is also passed as the per-call cap into
//      pull_opra_cbbo_1m_to_parquet (split_under_cap), so no single API call can exceed it.
//   3. --dry-run does the preflight + prints the full plan (every (symbol,date) -> path and
//      the total estimate) and EXITS 0 WITHOUT pulling. This is the free go/no-go evidence
//      P2-6 consumes before authorizing any spend.
//
// The paid pull is idempotent: an already-present target file is skipped, and a running
// cost total is accumulated across files — the loop stops before any pull that would carry
// the running total over --cap.
//
// Usage:
//   databento_bulk_opra --symbols SYM[,SYM...] --start YYYY-MM-DD --end YYYY-MM-DD
//                       [--out DIR] [--cap USD] [--layout TMPL] [--dry-run] [-h]
// A real pull (and the network preflight) requires DATABENTO_API_KEY in the environment
// (see .env). With no key set the tool degrades gracefully: it prints that a key is
// required and, for --dry-run, still emits the local (network-free) plan.

// Allow std::getenv on MSVC/clang-cl without the _CRT deprecation warning (read-only use).
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib> // getenv, strtod
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <databento/constants.hpp>  // dataset::kOpraPillar
#include <databento/datetime.hpp>   // DateTimeRange
#include <databento/enums.hpp>      // Schema, SType
#include <databento/historical.hpp> // Historical

#include "atx/external/databento.hpp" // pull_opra_cbbo_1m_to_parquet, estimate_cost, PullStats

namespace {

namespace fs = std::filesystem;
namespace dbx = atx::external::databento;

// OPRA cbbo-1m pre-close snapshot: the single minute [19:55:00, 19:56:00) UTC per date.
// Matches the atx-vol OpraBatchSpec default snapshot (snapshot_suffix "T19:55:00Z"), so the
// files this pulls line up 1:1 with what load_opra_daterange expects to read.
constexpr const char *kSnapStart = "T19:55:00";
constexpr const char *kSnapEnd = "T19:56:00";

// ── Civil-date kernel (Howard-Hinnant days-from-civil) — copied from
// atx-vol/src/opra_batch.cpp so the date walk needs no external date library ──────────────
struct Civil {
  int y = 0;
  unsigned m = 0;
  unsigned d = 0;
};

// Serial day number (1970-01-01 == 0) for a civil date. m in [1,12], d in [1,31].
[[nodiscard]] std::int64_t days_from_civil(int y, unsigned m, unsigned d) noexcept {
  y -= (m <= 2);
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);             // [0, 399]
  const unsigned doy = (153u * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1; // [0, 365]
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            // [0, 146096]
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// Inverse of days_from_civil.
[[nodiscard]] Civil civil_from_days(std::int64_t z) noexcept {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);               // [0, 146096]
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
  const int y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
  const unsigned mp = (5 * doy + 2) / 153;                      // [0, 11]
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;              // [1, 31]
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;                 // [1, 12]
  return Civil{y + static_cast<int>(m <= 2), m, d};
}

[[nodiscard]] bool parse_uint(std::string_view s, int &out) noexcept {
  const char *first = s.data();
  const char *last = s.data() + s.size();
  const std::from_chars_result res = std::from_chars(first, last, out);
  return res.ec == std::errc{} && res.ptr == last;
}

// Parse exactly "YYYY-MM-DD" with an in-range month/day.
[[nodiscard]] bool parse_civil(std::string_view s, Civil &out) noexcept {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') {
    return false;
  }
  int y = 0;
  int m = 0;
  int d = 0;
  if (!parse_uint(s.substr(0, 4), y) || !parse_uint(s.substr(5, 2), m) ||
      !parse_uint(s.substr(8, 2), d)) {
    return false;
  }
  if (m < 1 || m > 12 || d < 1 || d > 31) {
    return false;
  }
  out = Civil{y, static_cast<unsigned>(m), static_cast<unsigned>(d)};
  return true;
}

[[nodiscard]] std::string format_civil(const Civil &c) {
  char buf[11];
  std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", c.y, c.m, c.d);
  return std::string(buf);
}

// Substitute "{symbol}" and "{date}" in a path template (copied from opra_batch.cpp).
[[nodiscard]] std::string apply_template(std::string_view tmpl, std::string_view symbol,
                                         std::string_view date) {
  std::string out;
  out.reserve(tmpl.size() + symbol.size() + date.size());
  std::size_t i = 0;
  while (i < tmpl.size()) {
    if (tmpl.compare(i, 8, "{symbol}") == 0) {
      out.append(symbol);
      i += 8;
    } else if (tmpl.compare(i, 6, "{date}") == 0) {
      out.append(date);
      i += 6;
    } else {
      out.push_back(tmpl[i]);
      ++i;
    }
  }
  return out;
}

// Underlying -> OPRA parent symbol, matching pull_opra_cbbo_1m_to_parquet's derivation:
// strip '.' then append ".OPT" (e.g. "XOM" -> "XOM.OPT", "BRK.B" -> "BRKB.OPT").
[[nodiscard]] std::string to_parent(std::string_view u) {
  std::string root;
  root.reserve(u.size());
  for (const char ch : u) {
    if (ch != '.') {
      root.push_back(ch);
    }
  }
  return root + ".OPT";
}

void split_csv_into(std::string_view csv, std::vector<std::string> &out) {
  std::size_t start = 0;
  while (start <= csv.size()) {
    const std::size_t comma = csv.find(',', start);
    const std::size_t end = comma == std::string_view::npos ? csv.size() : comma;
    if (end > start) {
      out.emplace_back(csv.substr(start, end - start));
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
}

struct Config {
  std::vector<std::string> symbols;
  std::string start;
  std::string end;
  std::string dates_file;
  std::string out = "data/opra";
  std::string layout = "{symbol}/{date}.parquet";
  double cap = 5.0;
  unsigned workers = 1u;
  bool dry_run = false;
  bool help = false;
};

// A single (symbol, date) work item; preflight numbers are filled in later.
struct Cell {
  std::string symbol;
  std::string date;
  std::string parent; // e.g. "XOM.OPT"
  std::string path;   // resolved <out>/{symbol}/{date}.parquet
  std::uint64_t records = 0;
  std::uint64_t billable = 0; // bytes
  double cost = 0.0;
};

void print_usage() {
  std::printf(
      "Usage: databento_bulk_opra --symbols SYM[,SYM...] "
      "(--start YYYY-MM-DD --end YYYY-MM-DD | --dates-file FILE)\n"
      "                           [--out DIR] [--cap USD] [--workers N] "
      "[--layout TMPL] [--dry-run] "
      "[-h]\n\n"
      "Bulk OPRA cbbo-1m puller: for every date in [--start,--end] and every --symbol,\n"
      "pull the 19:55Z pre-close full-chain NBBO snapshot into <out>/{symbol}/{date}.parquet.\n\n"
      "Options:\n"
      "  --symbols SYM[,SYM...]  Underlyings; comma-separated and/or repeatable (required).\n"
      "  --start YYYY-MM-DD      Inclusive first date (required).\n"
      "  --end   YYYY-MM-DD      Inclusive last date (required).\n"
      "  --dates-file FILE       Exact ordered YYYY-MM-DD sessions, one per line.\n"
      "  --out DIR               Output root (default: data/opra).\n"
      "  --cap USD               Hard cost cap; also the per-API-call cap (default: 5.00).\n"
      "  --workers N             Concurrent zero-cost pulls (default: 1).\n"
      "  --layout TMPL           Path template under --out (default: "
      "\"{symbol}/{date}.parquet\").\n"
      "  --dry-run               Preflight + print the plan, then exit 0 without pulling.\n"
      "  -h, --help              Show this help.\n\n"
      "A real pull and the network preflight require DATABENTO_API_KEY in the environment.\n"
      "Exit codes: 0 ok/dry-run, 2 bad args, 3 cap refusal, 4 no API key, 1 runtime error.\n");
}

// argv -> Config. Returns false (caller prints usage + exits 2) on any malformed input.
[[nodiscard]] bool parse_args(int argc, char **argv, Config &cfg) {
  const auto need_val = [&](int &i) -> const char * {
    if (i + 1 >= argc) {
      std::fprintf(stderr, "error: %s requires a value\n", argv[i]);
      return nullptr;
    }
    return argv[++i];
  };
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "-h" || a == "--help") {
      cfg.help = true;
      return true;
    } else if (a == "--dry-run") {
      cfg.dry_run = true;
    } else if (a == "--symbols") {
      const char *v = need_val(i);
      if (!v)
        return false;
      split_csv_into(v, cfg.symbols);
    } else if (a == "--start") {
      const char *v = need_val(i);
      if (!v)
        return false;
      cfg.start = v;
    } else if (a == "--end") {
      const char *v = need_val(i);
      if (!v)
        return false;
      cfg.end = v;
    } else if (a == "--dates-file") {
      const char *v = need_val(i);
      if (!v)
        return false;
      cfg.dates_file = v;
    } else if (a == "--out") {
      const char *v = need_val(i);
      if (!v)
        return false;
      cfg.out = v;
    } else if (a == "--layout") {
      const char *v = need_val(i);
      if (!v)
        return false;
      cfg.layout = v;
    } else if (a == "--cap") {
      const char *v = need_val(i);
      if (!v)
        return false;
      char *endp = nullptr;
      const double parsed = std::strtod(v, &endp);
      if (endp == v || *endp != '\0' || !(parsed > 0.0)) {
        std::fprintf(stderr, "error: --cap must be a positive number, got '%s'\n", v);
        return false;
      }
      cfg.cap = parsed;
    } else if (a == "--workers") {
      const char *v = need_val(i);
      if (!v)
        return false;
      int parsed = 0;
      if (!parse_uint(v, parsed) || parsed < 1 || parsed > 64) {
        std::fprintf(stderr, "error: --workers must be in [1,64], got '%s'\n", v);
        return false;
      }
      cfg.workers = static_cast<unsigned>(parsed);
    } else {
      std::fprintf(stderr, "error: unknown argument '%s'\n", argv[i]);
      return false;
    }
  }
  if (cfg.symbols.empty()) {
    std::fprintf(stderr, "error: --symbols is required\n");
    return false;
  }
  const bool has_range = !cfg.start.empty() || !cfg.end.empty();
  if ((has_range && (cfg.start.empty() || cfg.end.empty())) ||
      (has_range == !cfg.dates_file.empty())) {
    std::fprintf(stderr, "error: provide exactly one of --start/--end or --dates-file\n");
    return false;
  }
  return true;
}

// Build the ordered [start,end] date list (inclusive) via the civil-date kernel. Returns
// false on unparseable dates or an end-before-start range (caller exits 2).
[[nodiscard]] bool build_dates(const Config &cfg, std::vector<std::string> &dates) {
  if (!cfg.dates_file.empty()) {
    std::ifstream input(cfg.dates_file);
    if (!input) {
      std::fprintf(stderr, "error: cannot open --dates-file '%s'\n", cfg.dates_file.c_str());
      return false;
    }
    std::string line;
    std::int64_t previous = std::numeric_limits<std::int64_t>::min();
    while (std::getline(input, line)) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line.empty())
        continue;
      Civil date;
      if (!parse_civil(line, date)) {
        std::fprintf(stderr, "error: invalid date '%s' in --dates-file\n", line.c_str());
        return false;
      }
      const std::int64_t serial = days_from_civil(date.y, date.m, date.d);
      if (serial <= previous) {
        std::fprintf(stderr, "error: --dates-file must be strictly increasing and unique\n");
        return false;
      }
      previous = serial;
      dates.push_back(line);
    }
    if (dates.empty()) {
      std::fprintf(stderr, "error: --dates-file is empty\n");
      return false;
    }
    return true;
  }
  Civil lo;
  Civil hi;
  if (!parse_civil(cfg.start, lo)) {
    std::fprintf(stderr, "error: unparseable --start '%s' (expected YYYY-MM-DD)\n",
                 cfg.start.c_str());
    return false;
  }
  if (!parse_civil(cfg.end, hi)) {
    std::fprintf(stderr, "error: unparseable --end '%s' (expected YYYY-MM-DD)\n", cfg.end.c_str());
    return false;
  }
  const std::int64_t serial_lo = days_from_civil(lo.y, lo.m, lo.d);
  const std::int64_t serial_hi = days_from_civil(hi.y, hi.m, hi.d);
  if (serial_hi < serial_lo) {
    std::fprintf(stderr, "error: --end '%s' precedes --start '%s'\n", cfg.end.c_str(),
                 cfg.start.c_str());
    return false;
  }
  dates.reserve(static_cast<std::size_t>(serial_hi - serial_lo + 1));
  for (std::int64_t s = serial_lo; s <= serial_hi; ++s) {
    dates.push_back(format_civil(civil_from_days(s)));
  }
  return true;
}

template <class Fn> void parallel_for(std::size_t count, unsigned workers, Fn &&fn) {
  std::atomic<std::size_t> next{0u};
  std::vector<std::thread> threads;
  threads.reserve(workers);
  for (unsigned worker = 0; worker < workers; ++worker) {
    threads.emplace_back([&] {
      for (;;) {
        const std::size_t index = next.fetch_add(1u, std::memory_order_relaxed);
        if (index >= count)
          break;
        fn(index);
      }
    });
  }
  for (std::thread &thread : threads)
    thread.join();
}

} // namespace

int main(int argc, char **argv) {
  Config cfg;
  if (!parse_args(argc, argv, cfg)) {
    print_usage();
    return 2;
  }
  if (cfg.help) {
    print_usage();
    return 0;
  }

  std::vector<std::string> dates;
  if (!build_dates(cfg, dates)) {
    return 2;
  }

  // ── Build the (symbol, date) work list — purely local (no network) ────────────────────
  // Date-major then symbol-major, matching atx-vol load_opra_daterange ordering.
  std::vector<Cell> cells;
  cells.reserve(dates.size() * cfg.symbols.size());
  for (const std::string &date : dates) {
    for (const std::string &sym : cfg.symbols) {
      Cell c;
      c.symbol = sym;
      c.date = date;
      c.parent = to_parent(sym);
      fs::path p = fs::path(cfg.out) / apply_template(cfg.layout, sym, date);
      p.make_preferred();
      c.path = p.string();
      cells.push_back(std::move(c));
    }
  }

  std::printf("Bulk OPRA cbbo-1m  dataset=%s  window=%s..%s per day\n",
              databento::dataset::kOpraPillar, kSnapStart, kSnapEnd);
  std::printf("symbols=%zu  dates=%s..%s (%zu)  files=%zu  out=%s  cap=$%.2f workers=%u%s\n\n",
              cfg.symbols.size(), dates.front().c_str(), dates.back().c_str(), dates.size(),
              cells.size(), cfg.out.c_str(), cfg.cap, cfg.workers,
              cfg.dry_run ? "  [DRY RUN]" : "");

  // ── No API key: degrade gracefully (do NOT crash) ─────────────────────────────────────
  const char *env_key = std::getenv("DATABENTO_API_KEY");
  if (env_key == nullptr || env_key[0] == '\0') {
    std::fprintf(stderr, "DATABENTO_API_KEY is not set. The FREE preflight queries Databento "
                         "Metadata over the network, so an API key is required even for --dry-run "
                         "cost estimation.\n");
    if (cfg.dry_run) {
      std::printf(
          "\nLocal plan (network-free; per-file cost estimate UNAVAILABLE without a key):\n");
      for (const Cell &c : cells) {
        std::printf("  %-8s %s -> %s\n", c.symbol.c_str(), c.date.c_str(), c.path.c_str());
      }
      std::printf("\n%zu file(s) planned. Set DATABENTO_API_KEY and re-run for cost figures.\n",
                  cells.size());
    }
    return 4;
  }
  const std::string api_key = env_key;

  // ── FREE preflight: MetadataGetRecordCount / MetadataGetBillableSize / MetadataGetCost ──
  // (estimate_cost() is the library wrapper for the MetadataGetCost call; here one client
  //  serves all three columns the wrapper doesn't surface.) No billable egress.
  double total_cost = 0.0;
  std::uint64_t total_records = 0;
  std::uint64_t total_billable = 0;
  try {
    std::vector<std::string> parents;
    parents.reserve(cfg.symbols.size());
    for (const std::string &symbol : cfg.symbols)
      parents.push_back(to_parent(symbol));
    std::vector<double> date_costs(dates.size(), 0.0);
    std::mutex preflight_mutex;
    bool preflight_failed = false;
    std::string preflight_error;
    parallel_for(dates.size(), cfg.workers, [&](std::size_t date_index) {
      const std::string &date = dates[date_index];
      const databento::DateTimeRange<std::string> range{date + kSnapStart, date + kSnapEnd};
      try {
        auto client = databento::Historical::Builder().SetKey(api_key).Build();
        for (unsigned attempt = 1; attempt <= 3u; ++attempt) {
          try {
            date_costs[date_index] = client.MetadataGetCost(
                databento::dataset::kOpraPillar, range, parents, databento::Schema::Cbbo1M,
                databento::SType::Parent, 0);
            break;
          } catch (const std::exception &error) {
            if (attempt == 3u)
              throw;
            std::this_thread::sleep_for(std::chrono::seconds(attempt * 2u));
          }
        }
        std::lock_guard<std::mutex> lock(preflight_mutex);
        std::printf("preflight %s (%zu parents) cost=$%.6f\n", date.c_str(), parents.size(),
                    date_costs[date_index]);
      } catch (const std::exception &error) {
        std::lock_guard<std::mutex> lock(preflight_mutex);
        preflight_failed = true;
        if (preflight_error.empty())
          preflight_error = date + ": " + error.what();
      }
    });
    if (preflight_failed)
      throw std::runtime_error(preflight_error);
    for (std::size_t date_index = 0; date_index < dates.size(); ++date_index) {
      total_cost += date_costs[date_index];
      cells[date_index * cfg.symbols.size()].cost = date_costs[date_index];
    }
    if (total_cost > 0.0) {
      auto client = databento::Historical::Builder().SetKey(api_key).Build();
      total_cost = 0.0;
      for (Cell &c : cells) {
        const databento::DateTimeRange<std::string> range{c.date + kSnapStart,
                                                          c.date + kSnapEnd};
        const std::vector<std::string> parent{c.parent};
        c.cost = client.MetadataGetCost(databento::dataset::kOpraPillar, range, parent,
                                        databento::Schema::Cbbo1M,
                                        databento::SType::Parent, 0);
        total_cost += c.cost;
      }
    }
  } catch (const std::exception &e) {
    std::fprintf(stderr, "preflight failed (Metadata query): %s\n", e.what());
    return 1;
  }

  // ── Per-symbol + total preflight table ────────────────────────────────────────────────
  std::printf("Preflight (FREE — Metadata endpoints, no data egress):\n");
  std::printf("  %-8s %-10s %12s %14s %14s\n", "symbol", "parent", "records", "billable(MB)",
              "est($)");
  for (const std::string &sym : cfg.symbols) {
    std::uint64_t rec = 0;
    std::uint64_t bytes = 0;
    double cost = 0.0;
    for (const Cell &c : cells) {
      if (c.symbol == sym) {
        rec += c.records;
        bytes += c.billable;
        cost += c.cost;
      }
    }
    std::printf("  %-8s %-10s %12llu %14.3f %14.6f\n", sym.c_str(), to_parent(sym).c_str(),
                static_cast<unsigned long long>(rec), static_cast<double>(bytes) / 1e6, cost);
  }
  std::printf("  %-8s %-10s %12llu %14.3f %14.6f\n", "TOTAL", "",
              static_cast<unsigned long long>(total_records),
              static_cast<double>(total_billable) / 1e6, total_cost);

  // ── Hard cap: refuse the whole request if the estimated total exceeds --cap ────────────
  if (total_cost > cfg.cap) {
    std::fprintf(stderr,
                 "\nREFUSED: estimated total $%.6f exceeds cap $%.2f. No data pulled.\n"
                 "Re-run with --cap >= %.2f to authorize this spend.\n",
                 total_cost, cfg.cap, total_cost);
    return 3;
  }
  std::printf("\nEstimated total $%.6f is within cap $%.2f.\n", total_cost, cfg.cap);

  // ── --dry-run: print the plan and exit WITHOUT pulling (the free go/no-go path) ────────
  if (cfg.dry_run) {
    std::printf("\n--dry-run PLAN (nothing pulled):\n");
    for (const Cell &c : cells) {
      std::printf("  %-8s %s -> %-40s est $%.6f\n", c.symbol.c_str(), c.date.c_str(),
                  c.path.c_str(), c.cost);
    }
    std::printf("\nPlan: %zu file(s), estimated $%.6f (cap $%.2f). DRY RUN — no spend.\n",
                cells.size(), total_cost, cfg.cap);
    return 0;
  }

  // ── Paid pull: per (symbol, date), skip existing files, stop before overrunning cap ────
  std::printf("\nPulling (paid). Idempotent: existing files are skipped.\n");
  double running = 0.0;
  std::size_t pulled = 0;
  std::size_t skipped = 0;
  bool failed = false;
  std::mutex result_mutex;
  const unsigned pull_workers = total_cost == 0.0 ? cfg.workers : 1u;
  parallel_for(cells.size(), pull_workers, [&](std::size_t cell_index) {
    const Cell &c = cells[cell_index];
    const fs::path target{c.path};
    std::error_code ec;
    if (fs::exists(target, ec) && !ec) {
      std::lock_guard<std::mutex> lock(result_mutex);
      ++skipped;
      return;
    }
    {
      std::lock_guard<std::mutex> lock(result_mutex);
      if (failed || running + c.cost > cfg.cap) {
        failed = true;
        return;
      }
      running += c.cost;
    }
    if (target.has_parent_path()) {
      fs::create_directories(target.parent_path(), ec);
    }
    const std::vector<std::string> one{c.symbol};
    // cfg.cap is passed as the PER-CALL cap: pull_opra_cbbo_1m_to_parquet -> split_under_cap
    // keeps every API call's preflight cost under it.
    auto res = dbx::pull_opra_cbbo_1m_to_parquet(
        api_key, one, {c.date + kSnapStart, c.date + kSnapEnd}, c.path, cfg.cap);
    if (!res.has_value()) {
      std::lock_guard<std::mutex> lock(result_mutex);
      std::fprintf(stderr, "  pull failed for %s %s: %s\n", c.symbol.c_str(), c.date.c_str(),
                   res.error().to_string().c_str());
      failed = true;
      return;
    }
    const dbx::PullStats &st = res.value();
    std::lock_guard<std::mutex> lock(result_mutex);
    running += st.cost_usd - c.cost;
    ++pulled;
    std::printf("  PULLED %-8s %s records=%lld cost=$%.6f\n", c.symbol.c_str(), c.date.c_str(),
                static_cast<long long>(st.records), st.cost_usd);
  });
  std::printf("\nDone. pulled=%zu  skipped=%zu  running_cost=$%.6f (cap $%.2f)\n", pulled, skipped,
              running, cfg.cap);
  return failed ? 1 : 0;
}
