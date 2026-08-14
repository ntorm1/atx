// Build or incrementally extend the standard projection-backed BacktestDb.
//
// Usage:
//   atx-vol-backtest-db-build --surface-db <root> --db <root>
//       [--symbols SPY,AAPL,... | --symbols-file sp500.txt]
//       [--from YYYY-MM-DD] [--to YYYY-MM-DD]
//       [--position long|short] [--entry-every N] [--threads N]
//   atx-vol-backtest-db-build --db <root> --vacuum
//
// With no symbol flags, every symbol registered in the SurfaceDb manifest is
// built. Re-running the same command scans source identities but performs no
// pricing for unchanged cells; appended source dates resume from checkpoints.
// Vacuum is a separate offline maintenance mode: no writer or reader retaining
// an older manifest snapshot may be using the database.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "storage/backtest_db.hpp"
#include "storage/backtest_db_build.hpp"
#include "backtest/backtest_template.hpp"

using namespace atx::vol;

namespace {

void print_usage(std::FILE *out) {
  std::fprintf(out, "usage: atx-vol-backtest-db-build --surface-db <root> --db <root>\n"
                    "       [--symbols SPY,AAPL,... | --symbols-file sp500.txt]\n"
                    "       [--from YYYY-MM-DD] [--to YYYY-MM-DD]\n"
                    "       [--position long|short] [--entry-every N] [--threads N]\n"
                    "       atx-vol-backtest-db-build --db <root> --vacuum\n"
                    "\n"
                    "  --vacuum  Offline-only: remove unindexed generation partitions.\n"
                    "            Stop the writer and all readers holding older manifests first.\n");
}

[[nodiscard]] bool parse_unsigned(std::string_view text, unsigned &out) {
  if (text.empty() || text.front() == '-') {
    return false;
  }
  const std::string owned(text);
  char *end = nullptr;
  errno = 0;
  const unsigned long value = std::strtoul(owned.c_str(), &end, 10);
  if (errno == ERANGE || end != owned.c_str() + owned.size() ||
      value > (std::numeric_limits<unsigned>::max)()) {
    return false;
  }
  out = static_cast<unsigned>(value);
  return true;
}

[[nodiscard]] std::string_view trim_ascii_whitespace(std::string_view text) noexcept {
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const std::size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1u);
}

[[nodiscard]] bool append_csv(std::string_view text, std::vector<std::string> &symbols) {
  std::size_t first = 0u;
  while (first <= text.size()) {
    const std::size_t comma = text.find(',', first);
    const std::size_t last = comma == std::string_view::npos ? text.size() : comma;
    const std::string_view token = trim_ascii_whitespace(text.substr(first, last - first));
    if (token.empty()) {
      return false;
    }
    symbols.emplace_back(token);
    if (comma == std::string_view::npos) {
      return true;
    }
    first = comma + 1u;
  }
  return false;
}

[[nodiscard]] bool load_symbols_file(std::string_view path, std::vector<std::string> &symbols) {
  std::ifstream input{std::string(path)};
  if (!input) {
    return false;
  }
  std::string line;
  while (std::getline(input, line)) {
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) {
      line.erase(comment);
    }
    const std::size_t first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      continue;
    }
    const std::size_t last = line.find_last_not_of(" \t\r\n");
    if (!append_csv(std::string_view(line).substr(first, last - first + 1u), symbols)) {
      return false;
    }
  }
  return input.eof() && !symbols.empty();
}

[[nodiscard]] const char *mode_name(BacktestDbCellBuildMode mode) noexcept {
  switch (mode) {
  case BacktestDbCellBuildMode::Full:
    return "full";
  case BacktestDbCellBuildMode::Extended:
    return "extended";
  case BacktestDbCellBuildMode::Rebuilt:
    return "rebuilt";
  case BacktestDbCellBuildMode::Unchanged:
    return "unchanged";
  case BacktestDbCellBuildMode::Failed:
    return "failed";
  }
  return "invalid";
}

[[nodiscard]] std::string sanitize_report_field(std::string_view text) {
  std::string sanitized{text};
  for (char &value : sanitized) {
    if (value == ',') {
      value = ';';
    } else if (value == '\r' || value == '\n') {
      value = ' ';
    }
  }
  return sanitized;
}

void print_report(const BacktestDbBuildReport &report) {
  std::printf("full=%zu\nextended=%zu\nrebuilt=%zu\nunchanged=%zu\nfailed=%zu\n"
              "rows_computed=%zu\nrows_added=%zu\n",
              report.n_full, report.n_extended, report.n_rebuilt, report.n_unchanged,
              report.n_failed, report.rows_computed, report.rows_added);
  for (const BacktestDbCellBuildReport &cell : report.cells) {
    const std::string template_id = sanitize_report_field(cell.template_id);
    const std::string symbol = sanitize_report_field(cell.symbol);
    const std::string detail = sanitize_report_field(cell.detail);
    std::printf("cell=%s,%s,%s,sources=%zu,rows_before=%zu,rows_after=%zu,"
                "rows_computed=%zu,rows_added=%zu,detail=%s\n",
                template_id.c_str(), symbol.c_str(), mode_name(cell.mode), cell.source_dates,
                cell.rows_before, cell.rows_after, cell.rows_computed, cell.rows_added,
                detail.c_str());
  }
}

} // namespace

int main(int argc, char **argv) {
  BacktestDbBuildSpec spec;
  std::string position{"long"};
  unsigned entry_every = 1u;
  bool have_symbols = false;
  bool have_symbols_file = false;
  bool vacuum = false;
  bool have_build_option = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    const auto value = [&](std::string_view flag) -> const char * {
      if (arg != flag || i + 1 >= argc) {
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--help" || arg == "-h") {
      print_usage(stdout);
      return 0;
    }
    if (const char *v = value("--surface-db")) {
      spec.surface_db_root = v;
      have_build_option = true;
    } else if (const char *v = value("--db")) {
      spec.backtest_db_root = v;
    } else if (const char *v = value("--from")) {
      spec.date_lo = v;
      have_build_option = true;
    } else if (const char *v = value("--to")) {
      spec.date_hi = v;
      have_build_option = true;
    } else if (const char *v = value("--position")) {
      position = v;
      have_build_option = true;
    } else if (const char *v = value("--symbols")) {
      if (have_symbols_file || !append_csv(v, spec.symbols)) {
        print_usage(stderr);
        return 2;
      }
      have_symbols = true;
      have_build_option = true;
    } else if (const char *v = value("--symbols-file")) {
      if (have_symbols || !load_symbols_file(v, spec.symbols)) {
        std::fprintf(stderr, "unable to load --symbols-file '%s'\n", v);
        return 2;
      }
      have_symbols_file = true;
      have_build_option = true;
    } else if (const char *v = value("--entry-every")) {
      if (!parse_unsigned(v, entry_every) || entry_every == 0u) {
        print_usage(stderr);
        return 2;
      }
      have_build_option = true;
    } else if (const char *v = value("--threads")) {
      if (!parse_unsigned(v, spec.price_threads)) {
        print_usage(stderr);
        return 2;
      }
      have_build_option = true;
    } else if (arg == "--vacuum") {
      vacuum = true;
    } else {
      std::fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]);
      print_usage(stderr);
      return 2;
    }
  }

  if (vacuum) {
    if (spec.backtest_db_root.empty() || have_build_option) {
      std::fprintf(stderr, "--vacuum is a standalone offline mode and accepts only --db\n");
      print_usage(stderr);
      return 2;
    }
    auto db = BacktestDb::open(spec.backtest_db_root);
    if (!db) {
      std::fprintf(stderr, "%s\n", db.error().to_string().c_str());
      return 1;
    }
    auto removed = db->vacuum_unindexed_partitions();
    if (!removed) {
      std::fprintf(stderr, "%s\n", removed.error().to_string().c_str());
      return 1;
    }
    std::printf("vacuumed=%zu\n", *removed);
    return 0;
  }

  if (spec.surface_db_root.empty() || spec.backtest_db_root.empty() ||
      (position != "long" && position != "short")) {
    print_usage(stderr);
    return 2;
  }

  const double sign = position == "long" ? 1.0 : -1.0;
  auto strategy = make_40_delta_3_calendar_month_strangle_template(sign, entry_every);
  if (!strategy) {
    std::fprintf(stderr, "%s\n", strategy.error().to_string().c_str());
    return 1;
  }
  strategy->id = position + "-" + strategy->id + "-entry-" + std::to_string(entry_every);
  strategy->name =
      (position == "long" ? "Long" : "Short") + std::string(" 40 Delta 3 Calendar Month Strangle");
  spec.templates.push_back(std::move(*strategy));

  auto report = build_backtest_db(spec);
  if (!report) {
    std::fprintf(stderr, "%s\n", report.error().to_string().c_str());
    return 1;
  }
  print_report(*report);
  return report->n_failed == 0u ? 0 : 3;
}
