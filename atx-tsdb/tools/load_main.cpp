// atx-tsdb-load — pivot a long-format parquet into a sealed segment file.
//
// Usage:
//   atx-tsdb-load --in IN.parquet --out OUT.atxseg --time TS_COL --symbol SYM_COL
//                 --fields close,volume[,...]
//
// created_at_nanos is stamped 0 here (deterministic output); a wrapper that
// wants a real wall clock can pass it in a future flag.

#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "atx/tsdb/load_parquet.hpp"

namespace {

[[nodiscard]] std::string arg_after(const std::vector<std::string> &args, std::string_view flag) {
  for (std::size_t i = 1; i + 1 < args.size(); ++i) {
    if (flag == args[i]) {
      return args[i + 1];
    }
  }
  return {};
}

[[nodiscard]] std::vector<std::string> split_csv(const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  for (const char c : s) {
    if (c == ',') {
      if (!cur.empty()) {
        out.push_back(cur);
      }
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) {
    out.push_back(cur);
  }
  return out;
}

} // namespace

// The body is fully wrapped in try/catch(...); the only residual throw site is
// the diagnostic stream I/O inside the handlers, which at worst std::terminates
// a CLI on its way out — acceptable for an entry point.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char **argv) {
  try {
    // SAFETY: argv holds argc valid pointers (plus a trailing nullptr). Copy them
    // into owned strings so the rest of the parser indexes a bounds-checked vector.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const std::vector<std::string> args(argv, argv + argc);

    const std::string in = arg_after(args, "--in");
    const std::string out = arg_after(args, "--out");
    const std::string time_col = arg_after(args, "--time");
    const std::string sym_col = arg_after(args, "--symbol");
    const std::vector<std::string> fields = split_csv(arg_after(args, "--fields"));

    if (in.empty() || out.empty() || time_col.empty() || sym_col.empty() || fields.empty()) {
      std::cerr << "usage: atx-tsdb-load --in IN.parquet --out OUT.atxseg "
                   "--time TS_COL --symbol SYM_COL --fields f1,f2,...\n";
      return 2;
    }

    const auto status =
        atx::tsdb::load_parquet(in, out, time_col, sym_col, fields, /*created_at_nanos=*/0);
    if (!status) {
      std::cerr << "load failed: " << status.error().to_string() << "\n";
      return 1;
    }
    std::cout << "wrote " << out << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "fatal: unknown exception\n";
    return 1;
  }
}
