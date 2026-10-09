// Structure-selector panel CLI: walk yearly SurfaceDb roots in date order,
// stream every daily surface through StructurePanelBuilder, and emit the
// feature + one-day-hold label panel as TSV.
//
//   atx-vol-structure-panel --out panel.tsv
//       [--db-prefix C:/atx-data/surface-db-r2/spy] [--year-lo 2019]
//       [--year-hi 2026] [--symbol SPY] [--front-days 30] [--back-days 365.25]
//       [--vega-target 1000]
//
// The panel schema is owned by the library (panel_tsv_header / to_tsv_line,
// structure_panel.hpp); this binary is IO + argument plumbing only.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "backtest/structure_panel.hpp"
#include "atx/vol/api/storage/surface_db.hpp"

namespace {

using namespace atx::vol;

struct Args {
  std::string db_prefix{"C:/atx-data/surface-db-r2/spy"};
  std::string symbol{"SPY"};
  std::string out;
  int year_lo{2019};
  int year_hi{2026};
  double front_days{30.0};
  double back_days{365.25};
  double vega_target{1000.0};
};

void usage(const char *argv0) {
  std::fprintf(stderr,
               "usage: %s --out PANEL.tsv [--db-prefix P] [--year-lo Y] [--year-hi Y]\n"
               "          [--symbol S] [--front-days D] [--back-days D] [--vega-target V]\n",
               argv0);
}

[[nodiscard]] bool parse_args(int argc, char **argv, Args &args) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view a{argv[i]};
    const auto next = [&](double &dst) {
      if (i + 1 >= argc) {
        return false;
      }
      dst = std::atof(argv[++i]);
      return true;
    };
    const auto next_i = [&](int &dst) {
      if (i + 1 >= argc) {
        return false;
      }
      dst = std::atoi(argv[++i]);
      return true;
    };
    const auto next_s = [&](std::string &dst) {
      if (i + 1 >= argc) {
        return false;
      }
      dst = argv[++i];
      return true;
    };
    bool ok = true;
    if (a == "--out") {
      ok = next_s(args.out);
    } else if (a == "--db-prefix") {
      ok = next_s(args.db_prefix);
    } else if (a == "--symbol") {
      ok = next_s(args.symbol);
    } else if (a == "--year-lo") {
      ok = next_i(args.year_lo);
    } else if (a == "--year-hi") {
      ok = next_i(args.year_hi);
    } else if (a == "--front-days") {
      ok = next(args.front_days);
    } else if (a == "--back-days") {
      ok = next(args.back_days);
    } else if (a == "--vega-target") {
      ok = next(args.vega_target);
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return false;
    }
    if (!ok) {
      std::fprintf(stderr, "missing value for %s\n", argv[i]);
      return false;
    }
  }
  if (args.out.empty()) {
    std::fprintf(stderr, "--out is required\n");
    return false;
  }
  if (args.year_lo > args.year_hi || !(args.front_days > 0.0) ||
      !(args.back_days > args.front_days) || !(args.vega_target > 0.0)) {
    std::fprintf(stderr, "invalid year range / tenors / vega target\n");
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    usage(argv[0]);
    return 2;
  }

  StructurePanelConfig cfg;
  cfg.front_T = args.front_days / 365.25;
  cfg.back_T = args.back_days / 365.25;
  cfg.vega_target = args.vega_target;
  StructurePanelBuilder builder(cfg);

  std::ofstream out(args.out);
  if (!out.is_open()) {
    std::fprintf(stderr, "cannot open --out %s\n", args.out.c_str());
    return 1;
  }
  out << panel_tsv_header() << '\n';

  std::size_t n_pushed = 0;
  std::size_t n_rows = 0;
  std::size_t n_invalid = 0;
  std::size_t n_load_failures = 0;

  for (int year = args.year_lo; year <= args.year_hi; ++year) {
    const std::string root = args.db_prefix + "-" + std::to_string(year);
    auto db = SurfaceDb::open(root);
    if (!db.has_value()) {
      std::fprintf(stderr, "SurfaceDb::open(%s): %s\n", root.c_str(),
                   db.error().to_string().c_str());
      return 1;
    }
    auto parts = db->partitions();
    std::sort(parts.begin(), parts.end(),
              [](const DbPartitionInfo &a, const DbPartitionInfo &b) { return a.key < b.key; });
    for (const DbPartitionInfo &p : parts) {
      auto surf = db->load_surface(p.key, args.symbol);
      if (!surf.has_value()) {
        // A partition without this symbol (or an unreadable one) is skipped
        // loudly; the builder never sees a fabricated day.
        std::fprintf(stderr, "load_surface(%s, %s): %s\n", p.key.c_str(), args.symbol.c_str(),
                     surf.error().to_string().c_str());
        ++n_load_failures;
        continue;
      }
      auto row = builder.push(p.key, *surf);
      if (!row.has_value()) {
        std::fprintf(stderr, "push(%s): %s\n", p.key.c_str(), row.error().to_string().c_str());
        return 1;
      }
      ++n_pushed;
      if (row->has_value()) {
        out << to_tsv_line(**row) << '\n';
        ++n_rows;
        n_invalid += (*row)->pnl_valid ? 0u : 1u;
      }
      if (n_pushed % 250 == 0) {
        std::fprintf(stderr, "... %zu sessions pushed (%zu rows)\n", n_pushed, n_rows);
      }
    }
    std::fprintf(stderr, "%s: done (%zu sessions cumulative)\n", root.c_str(), n_pushed);
  }

  // Final pending session: features only (labels NaN, pnl_valid 0) — the live
  // decision row for the operational predict path.
  if (auto last = builder.finish(); last.has_value()) {
    out << to_tsv_line(*last) << '\n';
    ++n_rows;
  }

  std::fprintf(stderr,
               "panel complete: %zu sessions pushed, %zu rows written, %zu invalid labels, "
               "%zu load failures, builder skipped %llu\n",
               n_pushed, n_rows, n_invalid, n_load_failures,
               static_cast<unsigned long long>(builder.skipped()));
  return 0;
}
