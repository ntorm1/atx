// Cross-sectional vega panel CLI: walk one or more yearly SurfaceDb roots in
// merged date order, stream every (symbol, date) surface through one
// VegaPanelBuilder per symbol, and emit the feature + h-day daily-rehedged
// ATMF-strangle label panel as TSV (shared header, `symbol` column).
//
//   atx-vol-vega-panel --out panel.tsv --db-root ROOT [--db-root ROOT ...]
//       (--symbols A,B,C | --universe FILE)
//       [--tenor-years 1.0] [--abs-delta 0.30] [--vega-target 1000]
//       [--horizon 21]
//
// --universe FILE is one symbol per line, `#` comments and blank lines
// skipped, CR tolerated. Roots are chained by merging every root's partitions
// ascending by key (a duplicate key across roots keeps the first root's and
// warns). A symbol whose load_surface fails on some dates skips those dates
// fail-soft (counted); per-symbol summary on stderr: `SYM rows=N skipped=K`
// with K = load failures + builder-skipped labels.
//
// The panel schema is owned by the library (vega_panel_tsv_header /
// to_tsv_line, vega_panel.hpp); this binary is IO + argument plumbing only.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/surface_db.hpp"
#include "atx/vol/vega_panel.hpp"

namespace {

using namespace atx::vol;

struct Args {
  std::vector<std::string> db_roots;
  std::vector<std::string> symbols;
  std::string universe;
  std::string out;
  double tenor_years{1.0};
  double abs_delta{0.30};
  double vega_target{1000.0};
  int horizon{21};
};

void usage(const char *argv0) {
  std::fprintf(stderr,
               "usage: %s --out PANEL.tsv --db-root ROOT [--db-root ROOT ...]\n"
               "          (--symbols A,B,C | --universe FILE)\n"
               "          [--tenor-years Y] [--abs-delta D] [--vega-target V] [--horizon H]\n",
               argv0);
}

void split_csv(std::string_view csv, std::vector<std::string> &out) {
  // Bounded by csv.size() — each pass consumes at least one character.
  while (!csv.empty()) {
    const std::size_t comma = csv.find(',');
    const std::string_view tok = csv.substr(0, comma);
    if (!tok.empty()) {
      out.emplace_back(tok);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    csv.remove_prefix(comma + 1);
  }
}

[[nodiscard]] bool load_universe(const std::string &path, std::vector<std::string> &out) {
  std::ifstream in{path, std::ios::binary};
  if (!in) {
    std::fprintf(stderr, "cannot open --universe %s\n", path.c_str());
    return false;
  }
  std::string line;
  // Bounded by the file's own line count — std::getline terminates at EOF.
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    out.push_back(line);
  }
  return true;
}

[[nodiscard]] bool parse_args(int argc, char **argv, Args &args) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view a{argv[i]};
    const auto next_s = [&](std::string &dst) {
      if (i + 1 >= argc) {
        return false;
      }
      dst = argv[++i];
      return true;
    };
    const auto next_d = [&](double &dst) {
      if (i + 1 >= argc) {
        return false;
      }
      dst = std::atof(argv[++i]);
      return true;
    };
    bool ok = true;
    std::string v;
    if (a == "--out") {
      ok = next_s(args.out);
    } else if (a == "--db-root") {
      ok = next_s(v);
      if (ok) {
        args.db_roots.push_back(v);
      }
    } else if (a == "--symbols") {
      ok = next_s(v);
      if (ok) {
        split_csv(v, args.symbols);
      }
    } else if (a == "--universe") {
      ok = next_s(args.universe);
    } else if (a == "--tenor-years") {
      ok = next_d(args.tenor_years);
    } else if (a == "--abs-delta") {
      ok = next_d(args.abs_delta);
    } else if (a == "--vega-target") {
      ok = next_d(args.vega_target);
    } else if (a == "--horizon") {
      ok = (i + 1 < argc);
      if (ok) {
        args.horizon = std::atoi(argv[++i]);
      }
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return false;
    }
    if (!ok) {
      std::fprintf(stderr, "missing value for %s\n", argv[i]);
      return false;
    }
  }
  if (args.out.empty() || args.db_roots.empty()) {
    std::fprintf(stderr, "--out and at least one --db-root are required\n");
    return false;
  }
  if (args.symbols.empty() == args.universe.empty()) {
    std::fprintf(stderr, "exactly one of --symbols / --universe is required\n");
    return false;
  }
  if (!(args.tenor_years > 0.0) || !(args.abs_delta > 0.0 && args.abs_delta < 1.0) ||
      !(args.vega_target > 0.0) || args.horizon < 1) {
    std::fprintf(stderr, "invalid tenor / abs-delta / vega-target / horizon\n");
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
  std::vector<std::string> symbols = args.symbols;
  if (symbols.empty() && !load_universe(args.universe, symbols)) {
    return 1;
  }
  if (symbols.empty()) {
    std::fprintf(stderr, "no symbols to process\n");
    return 2;
  }

  // Open every root once and merge their partitions ascending by key. A
  // duplicate key across roots keeps the FIRST root's partition (warned) so
  // the builders' strictly-ascending key contract holds.
  std::vector<SurfaceDb> dbs;
  std::vector<std::pair<std::string, std::size_t>> dates; // (key, dbs index)
  for (const std::string &root : args.db_roots) {
    auto db = SurfaceDb::open(root);
    if (!db.has_value()) {
      std::fprintf(stderr, "SurfaceDb::open(%s): %s\n", root.c_str(),
                   db.error().to_string().c_str());
      return 1;
    }
    for (const DbPartitionInfo &p : db->partitions()) {
      dates.emplace_back(p.key, dbs.size());
    }
    dbs.push_back(std::move(*db));
  }
  std::sort(dates.begin(), dates.end());
  std::vector<std::pair<std::string, std::size_t>> merged;
  merged.reserve(dates.size());
  for (auto &d : dates) {
    if (!merged.empty() && merged.back().first == d.first) {
      std::fprintf(stderr, "duplicate partition key %s across roots: keeping the first\n",
                   d.first.c_str());
      continue;
    }
    merged.push_back(std::move(d));
  }

  std::ofstream out(args.out);
  if (!out.is_open()) {
    std::fprintf(stderr, "cannot open --out %s\n", args.out.c_str());
    return 1;
  }
  out << vega_panel_tsv_header() << '\n';

  VegaPanelConfig cfg;
  cfg.tenor_T = args.tenor_years;
  cfg.target_abs_delta = args.abs_delta;
  cfg.vega_target = args.vega_target;
  cfg.horizon_sessions = args.horizon;

  for (const std::string &symbol : symbols) {
    VegaPanelBuilder builder(cfg, symbol);
    std::size_t n_rows = 0;
    std::size_t n_load_failures = 0;
    for (const auto &[key, db_idx] : merged) {
      auto surf = dbs[db_idx].load_surface(key, symbol);
      if (!surf.has_value()) {
        // A partition without this symbol (or an unreadable one) is skipped
        // loudly; the builder never sees a fabricated day.
        std::fprintf(stderr, "load_surface(%s, %s): %s\n", key.c_str(), symbol.c_str(),
                     surf.error().to_string().c_str());
        ++n_load_failures;
        continue;
      }
      auto row = builder.push(key, *surf);
      if (!row.has_value()) {
        std::fprintf(stderr, "push(%s, %s): %s\n", key.c_str(), symbol.c_str(),
                     row.error().to_string().c_str());
        return 1;
      }
      if (row->has_value()) {
        out << to_tsv_line(**row) << '\n';
        ++n_rows;
      }
    }
    // Still-pending entries: features only (labels NaN, label_valid 0) — the
    // live decision rows for the operational predict path.
    for (const VegaPanelRow &row : builder.finish()) {
      out << to_tsv_line(row) << '\n';
      ++n_rows;
    }
    std::fprintf(stderr, "%s rows=%zu skipped=%zu\n", symbol.c_str(), n_rows,
                 n_load_failures + static_cast<std::size_t>(builder.skipped()));
  }
  return 0;
}
