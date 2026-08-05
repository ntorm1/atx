// spy_mark_continuity.cpp — day-over-day mark continuity probe for SurfaceDb fits.
//
// A backtest holding fixed (K, expiry) lots marks them daily off the fitted
// surface. This diag walks a date window in one or more year roots and, for a
// fixed expiry anchor and a small strike set, reports everything a mark
// depends on so day-over-day IV wiggles can be attributed:
//
//   - iv at FIXED absolute strike (what the lot sees; forward + level + shape)
//   - iv at FIXED moneyness of today's forward (level + shape only)
//   - forward_at / q_eff_at / rate_at at the anchor T (carry-fit noise)
//   - the bracketing slice pillars around T and their at-K ivs (pillar noise
//     vs T-interpolation noise)
//
// Output: one TSV row per (date, strike-tag) on stdout.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "atx/vol/priced_surface.hpp"
#include "atx/vol/surface_db.hpp"

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

struct Args {
  std::string db_prefix = "C:/atx-data/surface-db-r2/spy";
  std::string from;
  std::string to;
  double tenor = 2.0;      // anchor T on the first date of the window
  bool constant_tenor = false; // probe T = tenor on every date (census mode)
  double put_m = 0.90;     // fixed-moneyness tags relative to first-date forward
  double call_m = 1.10;
};

int usage() {
  std::fprintf(stderr,
               "usage: spy-mark-continuity --from YYYY-MM-DD --to YYYY-MM-DD "
               "[--db-prefix P] [--tenor Y] [--put-m M] [--call-m M]\n");
  return 2;
}

std::optional<Args> parse(int argc, char **argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    auto next = [&]() -> const char * { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if (k == "--db-prefix") { const char *v = next(); if (!v) return std::nullopt; a.db_prefix = v; }
    else if (k == "--from") { const char *v = next(); if (!v) return std::nullopt; a.from = v; }
    else if (k == "--to") { const char *v = next(); if (!v) return std::nullopt; a.to = v; }
    else if (k == "--tenor") { const char *v = next(); if (!v) return std::nullopt; a.tenor = std::atof(v); }
    else if (k == "--constant-tenor") { a.constant_tenor = true; }
    else if (k == "--put-m") { const char *v = next(); if (!v) return std::nullopt; a.put_m = std::atof(v); }
    else if (k == "--call-m") { const char *v = next(); if (!v) return std::nullopt; a.call_m = std::atof(v); }
    else return std::nullopt;
  }
  if (a.from.empty() || a.to.empty()) return std::nullopt;
  return a;
}

// days since civil epoch (Howard Hinnant's algorithm) for ACT/365.25 T decay
long civil_days(int y, int m, int d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long>(doe) - 719468;
}

long civil_days(const std::string &iso) {
  return civil_days(std::atoi(iso.substr(0, 4).c_str()),
                    std::atoi(iso.substr(5, 2).c_str()),
                    std::atoi(iso.substr(8, 2).c_str()));
}

} // namespace

int main(int argc, char **argv) {
  const auto args = parse(argc, argv);
  if (!args) return usage();

  // collect (date, root) pairs across the year roots covering [from, to]
  std::vector<std::pair<std::string, std::string>> dates; // (key, root)
  const int y_lo = std::atoi(args->from.substr(0, 4).c_str());
  const int y_hi = std::atoi(args->to.substr(0, 4).c_str());
  for (int y = y_lo; y <= y_hi; ++y) {
    const std::string root = args->db_prefix + "-" + std::to_string(y);
    if (!fs::exists(root)) continue;
    auto db = SurfaceDb::open(root);
    if (!db) { std::fprintf(stderr, "open failed: %s\n", root.c_str()); return 1; }
    for (const DbPartitionInfo &p : db->partitions()) {
      if (p.key >= args->from && p.key <= args->to) dates.emplace_back(p.key, root);
    }
  }
  std::sort(dates.begin(), dates.end());
  if (dates.empty()) { std::fprintf(stderr, "no partitions in window\n"); return 1; }

  const long d0 = civil_days(dates.front().first);

  // strikes fixed from the first date's forward at the anchor tenor
  double Kp = 0, Ka = 0, Kc = 0;
  bool first = true;

  std::printf("date\ttag\tK\tT\tiv_fixedK\tiv_fixedM\tforward\tq_eff\trate\t"
              "T_lo\tiv_lo_atK\tT_hi\tiv_hi_atK\tn_slices\n");

  for (const auto &[key, root] : dates) {
    auto db = SurfaceDb::open(root);
    if (!db) { std::fprintf(stderr, "open failed: %s\n", root.c_str()); return 1; }
    auto sur = db->load_surface(key, "SPY");
    if (!sur) { std::fprintf(stderr, "%s: load_surface failed\n", key.c_str()); continue; }
    const PricedSurface &ps = *sur;

    const double T = args->constant_tenor
        ? args->tenor
        : args->tenor - static_cast<double>(civil_days(key) - d0) / 365.25;
    if (T <= 0.05) break;
    const double F = ps.forward_at(T);
    if (first) {
      Kp = args->put_m * F;
      Ka = F;
      Kc = args->call_m * F;
      first = false;
    }

    // bracketing pillars around T
    const auto ctx = ps.context();
    double T_lo = 0, T_hi = 0;
    for (const SliceContext &c : ctx) {
      if (c.T <= T) T_lo = c.T;
      if (c.T >= T) { T_hi = c.T; break; }
    }

    struct Probe { const char *tag; double K; double m; };
    const Probe probes[] = {{"put", Kp, args->put_m}, {"atm", Ka, 1.0}, {"call", Kc, args->call_m}};
    for (const Probe &p : probes) {
      const double iv_fixedK = ps.iv(p.K, T);
      const double iv_fixedM = ps.iv(p.m * F, T);
      const double iv_lo = T_lo > 0 ? ps.iv(p.K, T_lo) : 0.0;
      const double iv_hi = T_hi > 0 ? ps.iv(p.K, T_hi) : 0.0;
      std::printf("%s\t%s\t%.4f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t"
                  "%.6f\t%.6f\t%.6f\t%.6f\t%zu\n",
                  key.c_str(), p.tag, p.K, T, iv_fixedK, iv_fixedM, F,
                  ps.q_eff_at(T), ps.rate_at(T), T_lo, iv_lo, T_hi, iv_hi,
                  ps.n_slices());
    }
  }
  return 0;
}
