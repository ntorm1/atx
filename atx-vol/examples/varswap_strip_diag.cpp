// varswap_strip_diag.cpp — variance-strip diagnostic over a SurfaceDb corpus.
//
// For every session carrying the symbol's surface, prices the model-free
// variance strip at a CONSTANT tenor under the engine's default DerivConfig and
// under a ladder of pinned log-strike spans, and dumps the surface reads the
// strip depends on (ATM vol, wing IVs, forward, spot). Constant tenor isolates
// day-to-day surface noise from contract aging: any move in K_var between two
// adjacent rows is a statement about the surfaces, not about the swap.
//
//   atx-vol-varswap-strip-diag --db DIR --out FILE.tsv [--symbol XOM] [--tenor-years 0.2491444216]
//
// Diagnostic instrumentation for the strangle-vs-varswap investigation; not a
// shipped operator CLI. OFF by default (ATX_BUILD_EXAMPLES).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/backtest.hpp"        // Clock, SnapshotRef
#include "atx/vol/derivatives.hpp"     // var_swap_fair_strike, DerivConfig
#include "atx/vol/priced_surface.hpp"  // PricedSurface
#include "atx/vol/surface_archive.hpp" // SurfaceArchiveV2
#include "atx/vol/surface_db.hpp"      // SurfaceDb
#include "atx/vol/types.hpp"

using namespace atx::vol;

namespace {

struct Args {
  std::string db;
  std::string out;
  std::string symbol{"XOM"};
  double tenor_years{0.2491444216};
};

bool parse_args(int argc, char **argv, Args &a) {
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    auto need = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (s == "--db") {
      const char *v = need("--db");
      if (!v) return false;
      a.db = v;
    } else if (s == "--out") {
      const char *v = need("--out");
      if (!v) return false;
      a.out = v;
    } else if (s == "--symbol") {
      const char *v = need("--symbol");
      if (!v) return false;
      a.symbol = v;
    } else if (s == "--tenor-years") {
      const char *v = need("--tenor-years");
      if (!v) return false;
      a.tenor_years = std::atof(v);
    } else {
      std::fprintf(stderr, "unknown arg %s\n", s.c_str());
      return false;
    }
  }
  return !a.db.empty() && !a.out.empty();
}

// K_var under a span pinned to [-half, +half] log-moneyness; NaN when the strip
// refuses (so a hole in the series is visible rather than silently skipped).
double kvar_pinned(const PricedSurface &ps, double T, double half) {
  DerivConfig cfg{};
  cfg.k_min_log = -half;
  cfg.k_max_log = half;
  auto q = var_swap_fair_strike(ps, T, cfg);
  return q ? q->fair_strike_dec : std::numeric_limits<double>::quiet_NaN();
}

} // namespace

int main(int argc, char **argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    std::fprintf(stderr,
                 "usage: --db DIR --out FILE.tsv [--symbol XOM] [--tenor-years Y]\n");
    return 2;
  }

  auto db = SurfaceDb::open(args.db);
  if (!db) {
    std::fprintf(stderr, "SurfaceDb::open: %s\n", db.error().to_string().c_str());
    return 1;
  }
  auto clock = Clock::from_surface_db(*db);
  if (!clock) {
    std::fprintf(stderr, "Clock::from_surface_db: %s\n", clock.error().to_string().c_str());
    return 1;
  }

  std::ofstream out(args.out, std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "cannot open %s\n", args.out.c_str());
    return 1;
  }
  out << "date\tspot\tfwd\tsigma_atm\t"
         "iv_m150\tiv_m100\tiv_m050\tiv_m025\tiv_p025\tiv_p050\tiv_p100\t"
         "kvar_default\tkvar_h025\tkvar_h050\tkvar_h075\tkvar_h100\t"
         "err_est\tk_lo_used\tk_hi_used\tnodes\tflags\n";

  const double T = args.tenor_years;
  std::size_t n_ok = 0, n_dark = 0;

  for (const SnapshotRef &ref : clock->refs()) {
    auto archive = SurfaceArchiveV2::open_mapped(ref.archive_path);
    if (!archive) {
      std::fprintf(stderr, "open %s: %s\n", ref.archive_path.c_str(),
                   archive.error().to_string().c_str());
      return 1;
    }
    auto ps = archive->reconstruct_symbol(args.symbol);
    if (!ps) {
      if (ps.error().code() == ErrorCode::NotFound) {
        ++n_dark;
        continue;
      }
      std::fprintf(stderr, "reconstruct %s on %s: %s\n", args.symbol.c_str(), ref.date.c_str(),
                   ps.error().to_string().c_str());
      return 1;
    }

    const double S = ps->pricing().S;
    const double F = ps->forward_at(T);
    auto iv_at = [&](double x) { return ps->iv(F * std::exp(x), T); };

    auto qd = var_swap_fair_strike(*ps, T, DerivConfig{});
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double kvar_d = qd ? qd->fair_strike_dec : nan;
    const double err = qd ? qd->integration_error_est : nan;
    const double klo = qd ? qd->strip_k_lo_used : nan;
    const double khi = qd ? qd->strip_k_hi_used : nan;
    const unsigned nodes = qd ? qd->strip_nodes_used : 0u;
    const unsigned flags = qd ? static_cast<unsigned>(qd->flags) : 0u;

    char line[640];
    std::snprintf(line, sizeof line,
                  "%s\t%.10g\t%.10g\t%.10g\t"
                  "%.10g\t%.10g\t%.10g\t%.10g\t%.10g\t%.10g\t%.10g\t"
                  "%.10g\t%.10g\t%.10g\t%.10g\t%.10g\t"
                  "%.6g\t%.6g\t%.6g\t%u\t%u\n",
                  ref.date.c_str(), S, F, ps->iv(F, T), iv_at(-1.5), iv_at(-1.0), iv_at(-0.5),
                  iv_at(-0.25), iv_at(0.25), iv_at(0.5), iv_at(1.0), kvar_d,
                  kvar_pinned(*ps, T, 0.25), kvar_pinned(*ps, T, 0.5), kvar_pinned(*ps, T, 0.75),
                  kvar_pinned(*ps, T, 1.0), err, klo, khi, nodes, flags);
    out << line;
    ++n_ok;
  }
  out.close();
  std::fprintf(stderr, "diag: %zu sessions written, %zu dark for %s\n", n_ok, n_dark,
               args.symbol.c_str());
  return 0;
}
