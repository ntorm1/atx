// AMZN-around-earnings CStar surface report (WS-1 + WS-2 emitter).
//
// Reproduces the VolaDynamics "AMZN around earnings" surface fit on the committed
// real OPRA cbbo-1m NBBO fixture (snapshot 2018-04-26 19:45Z, 15 min before the
// after-close earnings print). Proves the shipped CStar calibrator handles the
// front-expiry "W-shape" with EXTREME NEGATIVE ATF curvature (c2_eff ~ -1.1..-1.5,
// VolaDynamics convention) where SSVI/eSSVI break, staying butterfly- AND
// calendar-arbitrage-free near-money, and emits the full CSV set the Python
// renderer consumes (schema: atx-vol/python/amzn_report_schema.md).
//
// The FIT itself (two-pass eSSVI-seed + C8 CStar per slice + calendar projection)
// lives in the shared header amzn_earnings_fit.hpp so this emitter and the
// correctness gate (tests/amzn_earnings_test.cpp) never diverge. This TU adds only
// the CLI, the CSV/JSON emission, and the earnings decomposition.
//
// CLI: amzn_earnings_report [--opra <parquet>] [--snapshot <iso>] [--r <rate>]
//                           [--out <dir>]
//
// NOTE — de-Americanization. AMZN 2018 paid no dividend (q = 0), so calls are
// exactly European; puts carry a small early-exercise premium. The fit feeds RAW
// Black-76-inverted observations (cstar_calibrate_slice prices European Black-76
// internally). The front W is call-dominated / European.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "amzn_earnings_fit.hpp"        // the shared two-pass fit (WS-1)
#include "atx/vol/data.hpp"            // iso_to_ns
#include "atx/vol/earnings_repro.hpp"  // run_earnings_repro, EarningsReproResult
#include "atx/vol/event_vol.hpp"       // EventSchedule
#include "atx/vol/implied_vol.hpp"     // implied_vol (bid/ask IV inversion)
#include "atx/vol/opra_panel.hpp"      // OpraLoadSpec, load_opra_cbbo_parquet
#include "atx/vol/session.hpp"         // VolaSession
#include "atx/vol/sr_tenor_grid.hpp"   // SrTenorGrid::kTradingDays

namespace {

using namespace atx::vol;
using namespace atx::vol::amzn;

// AMZN earnings instants bracketing the snapshot: 2018-04-26 (this print) and
// 2018-07-26, both after the 16:00-ET close (20:00Z EDT).
inline constexpr std::array<std::string_view, 2> kEventInstants = {
    "2018-04-26T20:00:00Z", "2018-07-26T20:00:00Z"};

// ── Small formatting utilities ───────────────────────────────────────────────

[[nodiscard]] std::string ymd_dashed(int ymd) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", ymd / 10000, (ymd / 100) % 100,
                ymd % 100);
  return buf;
}

[[nodiscard]] std::string ns_to_iso_utc(std::int64_t ns) {
  const int ymd = yyyymmdd_from_ns(ns);
  std::int64_t tod = ns % kNsPerDay;
  if (tod < 0) {
    tod += kNsPerDay;
  }
  const int secs = static_cast<int>(tod / 1'000'000'000LL);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%sT%02d:%02d:%02dZ", ymd_dashed(ymd).c_str(),
                secs / 3600, (secs % 3600) / 60, secs % 60);
  return buf;
}

// NaN-safe compact CSV number ("nan" for a non-finite cell so pandas parses it).
[[nodiscard]] std::string num(double v) {
  if (!std::isfinite(v)) {
    return "nan";
  }
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.8g", v);
  return buf;
}

[[nodiscard]] std::string find_existing(
    const std::vector<std::string>& candidates) {
  for (const std::string& c : candidates) {
    std::error_code ec;
    if (!c.empty() && std::filesystem::exists(c, ec)) {
      return c;
    }
  }
  return {};
}

// ── stdout table ─────────────────────────────────────────────────────────────

void print_table(const FitResult& fit) {
  std::printf("\nAMZN around earnings — CStar surface report\n");
  std::printf("  snapshot %s  implied spot %.4f  r %.4f (q=0)  %zu chains  fit %.2f ms\n",
              fit.snapshot_iso.c_str(), fit.implied_spot, fit.r,
              fit.n_expiries_total, fit.fit_wall_ms);
  std::printf("  (c2_eff = f''(0) = wpp(0), VolaDynamics conv; c2_base is atx "
              "f=1+2*s2*z+c2*z^2; tier=%s)\n", tier_name(kReportTier));
  std::printf("%s\n", std::string(140, '-').c_str());
  std::printf("%-9s %6s %8s %9s %7s | %9s %8s %9s %9s %9s | %8s %10s %4s %s\n",
              "expiry", "DTE", "T", "F", "sigma0", "theta", "s2", "c2_base",
              "c2_eff", "C_right", "volRMSE", "minRoperG", "nq", "flag");
  std::printf("%s\n", std::string(140, '-').c_str());
  for (const SliceFit& row : fit.slices) {
    if (!row.ok) {
      std::printf("%-9d %6.3f %8.5f %9.3f %7s | %s\n", row.expiry_ymd, row.dte,
                  row.T, row.F, "-", row.note.c_str());
      continue;
    }
    const char* flag = row.reverted ? "REVERT" : (row.c2_eff < 0.0 ? "c2<0" : "");
    std::printf("%-9d %6.3f %8.5f %9.3f %7.4f | %9.6f %8.4f %+9.4f %+9.4f %9.5f | "
                "%8.5f %+10.3e %4d %s\n",
                row.expiry_ymd, row.dte, row.T, row.F, row.atm_vol, row.theta,
                row.s2, row.c2, row.c2_eff, row.C_right, row.vol_rmse,
                row.min_roper_g, row.n_quotes, flag);
  }
  std::printf("%s\n", std::string(140, '-').c_str());
}

void print_cal(const char* tag, const CalStats& cs) {
  std::printf("calendar %-6s min dw  k=-0.6:%+.2e  k=-0.3:%+.2e  k=0:%+.2e  "
              "k=+0.3:%+.2e  k=+0.6:%+.2e  (violations %d)\n",
              tag, cs.min_dw[0], cs.min_dw[1], cs.min_dw[2], cs.min_dw[3],
              cs.min_dw[4], cs.n_viol);
  if (!cs.detail.empty()) {
    std::printf("%s", cs.detail.c_str());
  }
}

// ── CSV / JSON emitters ──────────────────────────────────────────────────────

[[nodiscard]] bool write_meta(const FitResult& fit, const std::string& out_dir) {
  std::ofstream os(std::filesystem::path(out_dir) / "meta.json");
  if (!os) {
    return false;
  }
  os << "{\n"
     << "  \"underlying\": \"AMZN\",\n"
     << "  \"snapshot_iso\": \"" << fit.snapshot_iso << "\",\n"
     << "  \"spot\": " << num(fit.implied_spot) << ",\n"
     << "  \"r\": " << num(fit.r) << ",\n"
     << "  \"q\": 0.0,\n"
     << "  \"n_expiries\": " << fit.n_expiries_total << ",\n"
     << "  \"fit_ms\": " << num(fit.fit_wall_ms) << ",\n"
     << "  \"event_instants\": [";
  for (std::size_t i = 0; i < kEventInstants.size(); ++i) {
    os << (i ? ", " : "") << "\"" << kEventInstants[i] << "\"";
  }
  os << "],\n  \"curve\": \"CStar-C8\"\n}\n";
  return true;
}

[[nodiscard]] bool write_slices(const FitResult& fit, const std::string& out_dir) {
  std::ofstream os(std::filesystem::path(out_dir) / "slices.csv");
  if (!os) {
    return false;
  }
  os << "expiry_date,expiry_iso,T,dte,F,sigma0,atm_vol,theta,s2,c2_base,c2_eff,"
        "C_left,C_right";
  for (std::size_t j = 0; j < kCStarNModes; ++j) {
    os << ",beta" << j;
  }
  os << ",tier,rmse_px,vol_rmse,min_roper_g,n_quotes,reverted\n";
  for (const SliceFit& row : fit.slices) {
    if (!row.ok) {
      continue;
    }
    os << ymd_dashed(row.expiry_ymd) << ',' << ns_to_iso_utc(row.expiry_ns) << ','
       << num(row.T) << ',' << num(row.dte) << ',' << num(row.F) << ','
       << num(row.atm_vol) << ',' << num(row.atm_vol) << ',' << num(row.theta)
       << ',' << num(row.s2) << ',' << num(row.c2) << ',' << num(row.c2_eff)
       << ',' << num(row.C_left) << ',' << num(row.C_right);
    for (std::size_t j = 0; j < kCStarNModes; ++j) {
      os << ',' << num(row.beta[j]);
    }
    os << ',' << tier_name(row.tier) << ',' << num(row.rmse_price) << ','
       << num(row.vol_rmse) << ',' << num(row.min_roper_g) << ',' << row.n_quotes
       << ',' << (row.reverted ? 1 : 0) << '\n';
  }
  return true;
}

[[nodiscard]] bool write_total_variance(const FitResult& fit,
                                        const std::string& out_dir) {
  std::ofstream os(std::filesystem::path(out_dir) / "total_variance.csv");
  if (!os) {
    return false;
  }
  os << "expiry_date,T,dte,k,z,w,fit_iv\n";
  constexpr int kN = 121;
  for (const SliceFit& row : fit.slices) {
    if (!row.ok || row.obs.empty()) {
      continue;
    }
    double k_lo = row.obs.front().k;
    double k_hi = row.obs.front().k;
    for (const FitObs& o : row.obs) {
      k_lo = std::min(k_lo, o.k);
      k_hi = std::max(k_hi, o.k);
    }
    k_lo = std::max(-1.5, k_lo);
    k_hi = std::min(1.5, k_hi);
    const double sig0_root_T = row.atm_vol * std::sqrt(row.T);
    const std::string date = ymd_dashed(row.expiry_ymd);
    for (int i = 0; i < kN; ++i) {
      const double k = k_lo + (k_hi - k_lo) * static_cast<double>(i) /
                                  static_cast<double>(kN - 1);
      const double fit_iv = cstar_slice_iv(row.params, k);
      const double w = row.T * fit_iv * fit_iv;
      const double z = (sig0_root_T > 0.0)
                           ? k / sig0_root_T
                           : std::numeric_limits<double>::quiet_NaN();
      os << date << ',' << num(row.T) << ',' << num(row.dte) << ',' << num(k)
         << ',' << num(z) << ',' << num(w) << ',' << num(fit_iv) << '\n';
    }
  }
  return true;
}

[[nodiscard]] bool write_smiles(const FitResult& fit, const std::string& out_dir) {
  std::ofstream os(std::filesystem::path(out_dir) / "smiles.csv");
  if (!os) {
    return false;
  }
  os << "expiry_date,T,dte,K,k,z,mkt_iv,iv_err,bid_iv,ask_iv,leg,vega,in_fit\n";
  for (const SliceFit& row : fit.slices) {
    if (!row.ok) {
      continue;
    }
    const double sig0_root_T = row.atm_vol * std::sqrt(row.T);
    const std::string date = ymd_dashed(row.expiry_ymd);
    for (const FitObs& o : row.obs) {
      const double bid_px = o.mid - 0.5 * o.spread;
      const double ask_px = o.mid + 0.5 * o.spread;
      const auto bres = implied_vol(bid_px, o.F, o.K, row.T, o.df, o.side);
      const auto ares = implied_vol(ask_px, o.F, o.K, row.T, o.df, o.side);
      const double bid_iv =
          bres.has_value() ? bres.value() : o.sigma_mkt - 0.5 * o.noise_sigma;
      const double ask_iv =
          ares.has_value() ? ares.value() : o.sigma_mkt + 0.5 * o.noise_sigma;
      const double iv_err = 0.5 * (ask_iv - bid_iv);
      const double z = (sig0_root_T > 0.0)
                           ? o.k / sig0_root_T
                           : std::numeric_limits<double>::quiet_NaN();
      os << date << ',' << num(row.T) << ',' << num(row.dte) << ',' << num(o.K)
         << ',' << num(o.k) << ',' << num(z) << ',' << num(o.sigma_mkt) << ','
         << num(iv_err) << ',' << num(bid_iv) << ',' << num(ask_iv) << ','
         << (o.side == Side::Call ? 'C' : 'P') << ',' << num(o.vega) << ",1\n";
    }
  }
  return true;
}

// Best-effort earnings decomposition off the fit's fitted eSSVI session.
[[nodiscard]] std::vector<std::pair<std::string, std::size_t>> write_earnings(
    const VolaSession& sess, std::int64_t now_ns, const std::string& out_dir) {
  std::vector<std::pair<std::string, std::size_t>> files;

  std::vector<std::int64_t> event_ns;
  event_ns.reserve(kEventInstants.size());
  for (const std::string_view ev : kEventInstants) {
    event_ns.push_back(iso_to_ns(ev));
  }
  const EventSchedule sched(std::move(event_ns));

  const auto repro = run_earnings_repro(sess, sched, now_ns);
  if (!repro.has_value()) {
    std::printf("  (earnings decomposition skipped: %s)\n",
                repro.error().to_string().c_str());
    return files;
  }
  const EarningsReproResult& er = repro.value();

  {
    std::ofstream os(std::filesystem::path(out_dir) / "earnings_summary.csv");
    if (os) {
      os << "iEMove,st,lt,decay,fit_error\n";
      os << num(er.fit.emove) << ',' << num(er.fit.st) << ',' << num(er.fit.lt)
         << ',' << num(er.fit.decay) << ',' << num(er.fit.fit_error) << '\n';
      files.emplace_back("earnings_summary.csv", 1u);
    }
  }
  {
    std::ofstream os(std::filesystem::path(out_dir) / "earnings_tenors.csv");
    if (os) {
      os << "nd,T,atm_dirty,atm_cen,n_earn,event_var_share\n";
      std::size_t rows = 0;
      for (std::size_t i = 0; i < SrTenorGrid::kTradingDays.size(); ++i) {
        const double T = er.tenor_T[i];
        const double F = sess.forward_at(T);
        const double w_dirty = (F > 0.0)
                                   ? sess.total_variance(F, T)
                                   : std::numeric_limits<double>::quiet_NaN();
        const double atm_dirty = (std::isfinite(w_dirty) && T > 0.0)
                                     ? std::sqrt(w_dirty / T)
                                     : std::numeric_limits<double>::quiet_NaN();
        const double share =
            (std::isfinite(w_dirty) && w_dirty > 0.0)
                ? static_cast<double>(er.n_earn[i]) * er.fit.emove * er.fit.emove /
                      w_dirty
                : std::numeric_limits<double>::quiet_NaN();
        os << SrTenorGrid::kTradingDays[i] << ',' << num(T) << ',' << num(atm_dirty)
           << ',' << num(er.atm_cen_i[i]) << ',' << er.n_earn[i] << ','
           << num(share) << '\n';
        ++rows;
      }
      files.emplace_back("earnings_tenors.csv", rows);
    }
  }
  std::printf("  earnings: iEMove=%.4f st=%.4f lt=%.4f decay=%.4f fit_error=%.4f\n",
              er.fit.emove, er.fit.st, er.fit.lt, er.fit.decay, er.fit.fit_error);
  return files;
}

int run(const std::string& opra_path, const std::string& snapshot_iso, double r,
        const std::string& out_dir) {
  OpraLoadSpec load;
  load.path = opra_path;
  load.underlying = "AMZN";
  load.snapshot_iso = snapshot_iso;
  load.r = r;
  // PM-settlement close (16:00 ET, DST-aware): the decisive fix that gives the
  // 1-DTE earnings expiry its true ~1.0-day time-to-expiry (not ~4 h).
  load.expiry_close = ExpiryCloseConvention::UsEquityPmClose;
  auto panel = load_opra_cbbo_parquet(load);
  if (!panel.has_value()) {
    std::printf("ERROR: load_opra_cbbo_parquet failed: %s\n",
                panel.error().to_string().c_str());
    return 1;
  }

  const FitResult fit = amzn_earnings_fit(panel.value(), r);
  if (fit.slices.empty()) {
    std::printf("ERROR: fit produced no slices (install/session failure)\n");
    return 1;
  }

  print_table(fit);

  // ── Acceptance summary ──────────────────────────────────────────────────────
  int n_ok = 0;
  int n_c2_neg = 0;
  int n_reverted = 0;
  int n_arb_viol = 0;
  const SliceFit* front = nullptr;
  for (const SliceFit& row : fit.slices) {
    if (!row.ok) {
      continue;
    }
    ++n_ok;
    if (front == nullptr) {
      front = &row;
    }
    n_c2_neg += (row.c2_eff < 0.0) ? 1 : 0;
    n_reverted += row.reverted ? 1 : 0;
    n_arb_viol += (row.min_roper_g >= 0.0) ? 0 : 1;
  }
  std::printf("summary: %d/%zu slices fit | c2_eff<0 on %d | reverted-to-seed %d "
              "| butterfly-arb violations %d\n",
              n_ok, fit.slices.size(), n_c2_neg, n_reverted, n_arb_viol);
  print_cal("BEFORE", fit.cal_before);
  std::printf("calendar projection: %s\n",
              fit.calendar_projected_ok ? "Ok" : "residual persists");
  print_cal("AFTER", fit.cal_after);
  if (front != nullptr) {
    std::printf("front expiry %d: DTE=%.3f  c2_base=%+.4f  c2_eff=%+.4f (VD conv; "
                "seed base %+.4f)  minRoperG=%+.3e  %s\n",
                front->expiry_ymd, front->dte, front->c2, front->c2_eff,
                front->c2_seed, front->min_roper_g,
                front->reverted ? "[REVERTED]" : "");
  }

  // ── Emit the CSV set ────────────────────────────────────────────────────────
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (!write_meta(fit, out_dir) || !write_slices(fit, out_dir) ||
      !write_total_variance(fit, out_dir) || !write_smiles(fit, out_dir)) {
    std::printf("ERROR: failed to write one or more CSV/JSON files to %s\n",
                out_dir.c_str());
    return 1;
  }

  std::size_t tv_rows = 0;
  std::size_t sm_rows = 0;
  for (const SliceFit& row : fit.slices) {
    if (row.ok) {
      tv_rows += 121u;
      sm_rows += row.obs.size();
    }
  }

  std::vector<std::pair<std::string, std::size_t>> earn_files;
  if (fit.session_ok) {
    earn_files = write_earnings(*fit.session, fit.snapshot_ns, out_dir);
  }

  std::printf("wrote to %s :\n", out_dir.c_str());
  std::printf("  meta.json\n");
  std::printf("  slices.csv (%d rows)\n", n_ok);
  std::printf("  total_variance.csv (%zu rows)\n", tv_rows);
  std::printf("  smiles.csv (%zu rows)\n", sm_rows);
  for (const auto& [name, rows] : earn_files) {
    std::printf("  %s (%zu rows)\n", name.c_str(), rows);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string opra = find_existing({
      "atx-vol/tests/data/amzn_earnings_2018/amzn_opra_cbbo1m_2018-04-26T1945Z.parquet",
      "../atx-vol/tests/data/amzn_earnings_2018/amzn_opra_cbbo1m_2018-04-26T1945Z.parquet",
      "C:/atx-wt/wt-amzn-earn/atx-vol/tests/data/amzn_earnings_2018/"
      "amzn_opra_cbbo1m_2018-04-26T1945Z.parquet",
  });
  std::string snapshot = "2018-04-26T19:45:00Z";
  double r = 0.019;
  std::string out = "amzn_earnings_out";

  for (int i = 1; i < argc; ++i) {
    const std::string_view a{argv[i]};
    if (a == "--opra" && i + 1 < argc) {
      opra = argv[++i];
    } else if (a == "--snapshot" && i + 1 < argc) {
      snapshot = argv[++i];
    } else if (a == "--r" && i + 1 < argc) {
      r = std::atof(argv[++i]);
    } else if (a == "--out" && i + 1 < argc) {
      out = argv[++i];
    } else if (a == "--help" || a == "-h") {
      std::printf("usage: amzn_earnings_report [--opra <parquet>] [--snapshot "
                  "<iso>] [--r <rate>] [--out <dir>]\n");
      return 0;
    }
  }

  if (opra.empty()) {
    std::printf("ERROR: no --opra path and committed fixture not found; pass "
                "--opra <parquet>.\n");
    return 1;
  }
  return run(opra, snapshot, r, out);
}
