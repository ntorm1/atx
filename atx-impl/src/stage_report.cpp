#include "stages.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/book/report.hpp"
#include "atx/engine/library/library.hpp"
#include "atx/engine/risk/multi_period.hpp"

#include "artifacts.hpp"
#include "config.hpp"
#include "diag_risk.hpp"
#include "serialize_panel.hpp"

namespace atx::impl {

namespace fs    = std::filesystem;
namespace alpha = atx::engine::alpha;
namespace book  = atx::engine::book;
namespace risk  = atx::engine::risk;
namespace lib_ns = atx::engine::library;

atx::core::Result<StageResult> run_report(const RunConfig& cfg)
{
    // 1. Validate required flags.
    if (cfg.panel.empty() || cfg.books.empty() || cfg.report_out.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "report: --panel, --books, and --out required");
    }

    // 2. Load research and books panels.
    ATX_TRY(auto research,   read_panel(cfg.panel));
    ATX_TRY(auto bookspanel, read_panel(cfg.books));

    // 3. Parse the S5 meta sidecar to reconstruct schedule and MultiPeriodResult.
    const std::string sidecar_path = cfg.books + ".meta.txt";
    std::ifstream sidecar_file{sidecar_path};
    if (!sidecar_file.is_open()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "report: cannot open sidecar: " + sidecar_path);
    }

    atx::usize S_meta = 0;
    atx::usize M_meta = 0;

    struct PeriodEntry {
        atx::usize s      = 0;
        atx::usize period = 0;
        atx::f64   turnover = 0.0;
        atx::f64   cost_bps = 0.0;
    };
    std::vector<PeriodEntry> entries;

    {
        std::string line;
        while (std::getline(sidecar_file, line)) {
            if (line.rfind("periods=", 0) == 0) {
                S_meta = static_cast<atx::usize>(std::stoull(line.substr(8)));
            } else if (line.rfind("instruments=", 0) == 0) {
                M_meta = static_cast<atx::usize>(std::stoull(line.substr(12)));
            } else if (line.rfind("s=", 0) == 0) {
                // Format: s=<s> period=<p> turnover=<t> cost_bps=<c>
                PeriodEntry e;
                std::istringstream iss(line);
                std::string tok;
                while (iss >> tok) {
                    if (tok.rfind("s=", 0) == 0) {
                        e.s = static_cast<atx::usize>(std::stoull(tok.substr(2)));
                    } else if (tok.rfind("period=", 0) == 0) {
                        e.period = static_cast<atx::usize>(std::stoull(tok.substr(7)));
                    } else if (tok.rfind("turnover=", 0) == 0) {
                        e.turnover = std::stod(tok.substr(9));
                    } else if (tok.rfind("cost_bps=", 0) == 0) {
                        e.cost_bps = std::stod(tok.substr(9));
                    }
                }
                entries.push_back(e);
            }
        }
    }

    // Sort entries by s-index (should already be in order, but be safe).
    std::sort(entries.begin(), entries.end(),
              [](const PeriodEntry& a, const PeriodEntry& b) { return a.s < b.s; });

    const atx::usize S = bookspanel.dates();
    const atx::usize M = bookspanel.instruments();

    if (S_meta != S || M_meta != M) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "report: sidecar shape mismatch with books panel");
    }
    if (entries.size() != S) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "report: sidecar period count mismatch");
    }

    // Build RebalanceSchedule and MultiPeriodResult from sidecar.
    risk::RebalanceSchedule sched;
    risk::MultiPeriodResult mpr;
    sched.periods.reserve(S);
    mpr.books.reserve(S);
    mpr.turnover.reserve(S);
    mpr.cost_bps.reserve(S);

    ATX_TRY(const auto weight_fid, bookspanel.field_id("weight"));

    for (atx::usize s = 0; s < S; ++s) {
        sched.periods.push_back(entries[s].period);
        mpr.turnover.push_back(entries[s].turnover);
        mpr.cost_bps.push_back(entries[s].cost_bps);

        // Copy the weight row for period s into a std::vector<f64>.
        const auto row_span = bookspanel.field_cross_section(weight_fid, s);
        mpr.books.push_back(std::vector<atx::f64>(row_span.begin(), row_span.end()));
    }

    // 4. Build forward-returns panel (1 field "ret") from research TRI close.
    //
    // NOTE: the report charges each rebalance's book against the 1-PERIOD-FORWARD
    // TRI return at its as-of date — a simplification; full holding-interval
    // compounding is a future enhancement (matches accumulate_report's
    // one-cross-section-per-period design).
    {
        const atx::usize D = research.dates();
        ATX_TRY(const auto close_id, research.field_id("close"));
        const auto close = research.field_all(close_id);  // date-major D*M

        // Resolve raw_close (as-traded price for true notional); fall back to
        // "close" if "raw_close" is absent (e.g. in synthetic test panels where
        // TRI-adjusted close is all that exists).
        alpha::FieldId raw_close_id = close_id;
        {
            auto r_rc = research.field_id("raw_close");
            if (r_rc.has_value()) { raw_close_id = *r_rc; }
            // else: use close as fallback — comment documents the choice
        }
        const auto raw_close = research.field_all(raw_close_id);  // date-major D*M

        // Resolve volume field (must be present for capacity computation).
        // If absent, capacity metrics will all be 0 (guarded in the loop below).
        bool has_volume = false;
        alpha::FieldId volume_id{};
        {
            auto r_vol = research.field_id("volume");
            if (r_vol.has_value()) {
                volume_id  = *r_vol;
                has_volume = true;
            }
        }
        const auto volume_span = has_volume
            ? research.field_all(volume_id)
            : std::span<const atx::f64>{};  // empty span; never dereferenced when !has_volume

        std::vector<atx::f64> ret(D * M, std::numeric_limits<atx::f64>::quiet_NaN());
        for (atx::usize d = 0; d + 1 < D; ++d) {
            for (atx::usize i = 0; i < M; ++i) {
                const atx::f64 p0 = close[d * M + i];
                const atx::f64 p1 = close[(d + 1) * M + i];
                if (!std::isnan(p0) && !std::isnan(p1) && p0 != 0.0) {
                    ret[d * M + i] = p1 / p0 - 1.0;
                }
            }
        }
        std::vector<std::uint8_t> uni(D * M);
        for (atx::usize d = 0; d < D; ++d) {
            for (atx::usize i = 0; i < M; ++i) {
                uni[d * M + i] = research.in_universe(d, i) ? 1 : 0;
            }
        }
        ATX_TRY(auto retpanel,
                alpha::Panel::create(D, M, {"ret"}, {ret}, uni));
        ATX_TRY(const auto ret_fid, retpanel.field_id("ret"));

        // 5. Rebuild the same diagonal FactorModel S5 used.
        ATX_TRY(auto V, diagonal_risk_model(research));

        // 6. Construct an empty Library (nothing admitted; census all zeros).
        //    master_seeds must be non-empty — open() reads front().
        //    NOTE: An empty library (n_alphas()==0) produces an all-zero census,
        //    which is all accumulate_report needs from it.
        //    The Library ctor opens SQLite files inside the dir; create both
        //    report_out and _lib first (SQLite cannot create parent directories,
        //    and write_report hasn't run yet at this point).
        {
            std::error_code ec;
            fs::create_directories(fs::path{cfg.report_out} / "_lib", ec);
            if (ec) {
                return atx::core::Err(atx::core::ErrorCode::IoError,
                                      "report: cannot create report/_lib dir");
            }
        }
        // NOTE: Library::open returns a plain Library (NOT a Result) — it directly
        // constructs the object; the ctor opens SQLite via open_or_abort (an
        // always-on ATX_CHECK on a genuine environment fault). There is no Result
        // to ATX_TRY here. The fs::create_directories above is what makes the open
        // succeed on the happy path (SQLite cannot create parent dirs).
        auto libr = lib_ns::Library::open(
            (fs::path{cfg.report_out} / "_lib").string(),
            lib_ns::GateConfig{},
            std::vector<atx::u64>{0});

        const atx::f64 capacity_gross = 1e9;
        ATX_TRY(auto rep,
                book::accumulate_report(mpr, retpanel, ret_fid, sched, V,
                                        capacity_gross, libr, 0));

        // 6b. (A2b) Locate + parse combo.meta to recover the IS/OOS boundary.
        //
        // combo.meta (written by A2a) is key=value lines: n_periods, fit_begin,
        // fit_end, holdout_begin, holdout_frac. holdout_begin is on the SAME
        // panel-date axis as sched.periods[] / research.dates() (all read the
        // same panel.bin). Rebalance period s is OUT-OF-SAMPLE iff
        // sched.periods[s] >= holdout_begin.
        //
        // Default: holdout_begin = research.dates() => NO OOS window (whole
        // series is in-sample). A standalone `report` without --combo must work
        // exactly as before (plus the additive full-series portfolio_sharpe).
        // holdout_begin == research.dates() is the canonical "no split" sentinel:
        // every sched.periods[s] < research.dates(), so oos_idx stays empty.
        atx::usize holdout_begin = research.dates();
        if (!cfg.combo.empty()) {
            const std::string meta_path = cfg.combo + ".meta";
            std::ifstream meta_file{meta_path};
            if (meta_file.is_open()) {
                atx::usize meta_holdout   = 0;
                atx::usize meta_nperiods  = 0;
                bool got_holdout  = false;
                bool got_nperiods = false;
                std::string mline;
                while (std::getline(meta_file, mline)) {
                    if (mline.rfind("holdout_begin=", 0) == 0) {
                        meta_holdout =
                            static_cast<atx::usize>(std::stoull(mline.substr(14)));
                        got_holdout = true;
                    } else if (mline.rfind("n_periods=", 0) == 0) {
                        meta_nperiods =
                            static_cast<atx::usize>(std::stoull(mline.substr(10)));
                        got_nperiods = true;
                    }
                }
                // Only honor the split when the meta is internally consistent
                // with THIS report's panel (same date axis) and carves a real
                // non-empty IS/OOS partition.
                if (got_holdout && got_nperiods &&
                    meta_nperiods == research.dates() &&
                    meta_holdout > 0 && meta_holdout < research.dates()) {
                    holdout_begin = meta_holdout;
                }
            }
            // Missing/unparsable/inconsistent meta => holdout_begin stays at
            // research.dates() (no OOS split); do NOT error (standalone report
            // must still succeed).
        }

        // 6c. (A2b) Split rebalance periods s in [0,S) into IS / OOS index sets
        //     using the panel-date mapping sched.periods[s] >= holdout_begin.
        std::vector<atx::usize> is_idx, oos_idx;
        is_idx.reserve(S);
        oos_idx.reserve(S);
        for (atx::usize s = 0; s < S; ++s) {
            (sched.periods[s] < holdout_begin ? is_idx : oos_idx).push_back(s);
        }

        // 6d. (A2b) Annualized, scale-invariant Sharpe over a chosen subset of
        //     rep.pnl_net. Periods-per-year is derived from the actual schedule
        //     spacing (panel-date indices ARE trading days). Sharpe = mean/std,
        //     so the capacity_gross dollar scale of pnl_net cancels.
        double ann = std::sqrt(252.0);
        if (S > 1 && sched.periods.back() > sched.periods.front()) {
            const double span =
                static_cast<double>(sched.periods.back() - sched.periods.front());
            ann = std::sqrt(252.0 * static_cast<double>(S - 1) / span);
        }
        auto sharpe_of = [&](const std::vector<atx::usize>& idx) -> double {
            if (idx.size() < 2) return std::numeric_limits<double>::quiet_NaN();
            double mean = 0.0;
            for (auto s : idx) mean += rep.pnl_net[s];
            mean /= static_cast<double>(idx.size());
            double var = 0.0;
            for (auto s : idx) { const double d = rep.pnl_net[s] - mean; var += d * d; }
            var /= static_cast<double>(idx.size() - 1);
            const double sd = std::sqrt(var);
            return sd > 0.0 ? mean / sd * ann : 0.0;
        };

        std::vector<atx::usize> all_idx(S);
        std::iota(all_idx.begin(), all_idx.end(), atx::usize{0});
        const double portfolio_sharpe     = sharpe_of(all_idx);
        const double portfolio_is_sharpe  = sharpe_of(is_idx);
        const double portfolio_oos_sharpe = sharpe_of(oos_idx);

        // 6e. (A2b) OOS turnover (mean of rep.turnover over oos_idx; NaN if
        //     empty) and OOS max drawdown (peak-to-trough decline of the OOS
        //     cumulative pnl_net sub-curve, in ABSOLUTE pnl_net units).
        double oos_turnover = std::numeric_limits<double>::quiet_NaN();
        if (!oos_idx.empty()) {
            double t = 0.0;
            for (auto s : oos_idx) t += rep.turnover[s];
            oos_turnover = t / static_cast<double>(oos_idx.size());
        }
        double oos_max_drawdown = 0.0;  // absolute pnl_net units; 0 if no OOS
        double oos_pnl_net      = 0.0;  // sum of rep.pnl_net over oos_idx
        {
            double E    = 0.0;
            double peak = 0.0;
            for (auto s : oos_idx) {
                E += rep.pnl_net[s];
                oos_pnl_net = E;
                if (E > peak) peak = E;
                const double dd = peak - E;
                if (dd > oos_max_drawdown) oos_max_drawdown = dd;
            }
        }

        // 7. Write canonical TSVs via write_report.
        ATX_TRY_VOID(book::write_report(rep, cfg.report_out));

        // Summary stats — computed ONCE here and reused for both summary.txt and
        // the StageResult kvs (no double std::accumulate; bytes/digest unchanged).
        const atx::f64 final_equity =
            rep.equity_curve.empty() ? 0.0 : rep.equity_curve.back();
        const atx::f64 total_pnl_gross =
            std::accumulate(rep.pnl_gross.begin(), rep.pnl_gross.end(), 0.0);
        const atx::f64 total_pnl_net =
            std::accumulate(rep.pnl_net.begin(), rep.pnl_net.end(), 0.0);
        const atx::f64 total_pnl_cost =
            std::accumulate(rep.pnl_cost.begin(), rep.pnl_cost.end(), 0.0);
        const atx::f64 avg_gross_leverage =
            S > 0
            ? std::accumulate(rep.gross_leverage.begin(),
                              rep.gross_leverage.end(), 0.0)
                  / static_cast<atx::f64>(S)
            : 0.0;
        const atx::f64 avg_turnover =
            S > 0
            ? std::accumulate(rep.turnover.begin(),
                              rep.turnover.end(), 0.0)
                  / static_cast<atx::f64>(S)
            : 0.0;

        // 7b. Compute capacity-footprint metrics (%ADV participation at report_aum).
        //
        // For each rebalance period s and each in-universe held name i (|w|>0):
        //   dvol    = raw_close[d,i] * volume[d,i]   (d = sched.periods[s])
        //   notional= |w| * report_aum
        //   part    = notional / dvol  (skip if dvol <= 0 or NaN)
        // Aggregates over all finite (s,i) participations:
        //   avg_names_held   — mean over periods of count(|w|>0)
        //   max_participation_pct, p95_participation_pct, median_participation_pct
        //   pct_gross_over_5pct_adv — mean over periods of Σ_{part_i>0.05}|w_i|
        atx::f64 avg_names_held  = 0.0;
        atx::f64 max_part_pct    = 0.0;
        atx::f64 p95_part_pct    = 0.0;
        atx::f64 med_part_pct    = 0.0;
        atx::f64 pct_gross_over5 = 0.0;

        if (has_volume && S > 0) {
            std::vector<atx::f64> parts;       // all finite participations
            parts.reserve(S * M);
            atx::f64 sum_names_held   = 0.0;
            atx::f64 sum_gross_over5  = 0.0;
            const atx::f64 report_aum = cfg.report_aum;

            for (atx::usize s = 0; s < S; ++s) {
                const atx::usize d = sched.periods[s]; // research panel date index
                const auto& book_row = mpr.books[s];   // weights for period s

                atx::usize names_held    = 0;
                atx::f64   gross_over5_s = 0.0;

                for (atx::usize i = 0; i < M; ++i) {
                    const atx::f64 w = book_row[i];
                    const atx::f64 abs_w = w < 0.0 ? -w : w;
                    if (abs_w == 0.0) continue;
                    if (!research.in_universe(d, i)) continue;

                    ++names_held;

                    const atx::f64 rc   = raw_close[d * M + i];
                    const atx::f64 vol  = volume_span[d * M + i];
                    const atx::f64 dvol = rc * vol;
                    if (!std::isfinite(dvol) || dvol <= 0.0) continue;

                    const atx::f64 notional = abs_w * report_aum;
                    const atx::f64 part     = notional / dvol;
                    if (!std::isfinite(part)) continue;

                    parts.push_back(part);
                    if (part > 0.05) {
                        gross_over5_s += abs_w;
                    }
                }

                sum_names_held  += static_cast<atx::f64>(names_held);
                sum_gross_over5 += gross_over5_s;
            }

            avg_names_held  = sum_names_held  / static_cast<atx::f64>(S);
            pct_gross_over5 = sum_gross_over5 / static_cast<atx::f64>(S);

            if (!parts.empty()) {
                std::vector<atx::f64> sorted_parts = parts;
                std::sort(sorted_parts.begin(), sorted_parts.end());
                const atx::usize n = sorted_parts.size();

                max_part_pct = sorted_parts.back() * 100.0;

                // p95: index = floor(0.95 * (n-1))
                const atx::usize p95_idx =
                    static_cast<atx::usize>(0.95 * static_cast<atx::f64>(n - 1));
                p95_part_pct = sorted_parts[p95_idx] * 100.0;

                // median: middle element for odd n; average of two middles for even n
                if (n % 2 == 1) {
                    med_part_pct = sorted_parts[n / 2] * 100.0;
                } else {
                    med_part_pct = (sorted_parts[n / 2 - 1] + sorted_parts[n / 2]) * 0.5 * 100.0;
                }
            }
        }

        // 7c. Write convenience files (not R8-pinned).
        {
            const fs::path rdir{cfg.report_out};
            // equity_curve.csv
            {
                std::ofstream ec_file{rdir / "equity_curve.csv"};
                if (!ec_file.is_open()) {
                    return atx::core::Err(atx::core::ErrorCode::IoError,
                                          "report: cannot write equity_curve.csv");
                }
                // (A2b) Additive `segment` column marks each period IS/OOS.
                // This file is convenience-only (not R8-pinned; no test pins its
                // header), so widening it is safe.
                ec_file << "period,equity,segment\n";
                for (atx::usize s = 0; s < S; ++s) {
                    const char* seg =
                        (sched.periods[s] >= holdout_begin) ? "oos" : "is";
                    ec_file << std::to_string(s) << ","
                            << std::to_string(rep.equity_curve[s]) << ","
                            << seg << "\n";
                }
            }
            // summary.txt — first 6 lines are the existing prefix (byte-identical
            // to previous behaviour); capacity metrics are appended after.
            {
                std::ofstream sm_file{rdir / "summary.txt"};
                if (!sm_file.is_open()) {
                    return atx::core::Err(atx::core::ErrorCode::IoError,
                                          "report: cannot write summary.txt");
                }
                // Existing 6 lines (MUST remain byte-identical — do NOT reorder).
                sm_file << "final_equity=" << std::to_string(final_equity) << "\n";
                sm_file << "total_pnl_gross=" << std::to_string(total_pnl_gross) << "\n";
                sm_file << "total_pnl_net=" << std::to_string(total_pnl_net) << "\n";
                sm_file << "total_pnl_cost=" << std::to_string(total_pnl_cost) << "\n";
                sm_file << "avg_gross_leverage=" << std::to_string(avg_gross_leverage) << "\n";
                sm_file << "avg_turnover=" << std::to_string(avg_turnover) << "\n";
                // Capacity-footprint metrics (additive; after existing prefix).
                sm_file << "report_aum=" << std::to_string(cfg.report_aum) << "\n";
                sm_file << "avg_names_held=" << std::to_string(avg_names_held) << "\n";
                sm_file << "max_participation_pct=" << std::to_string(max_part_pct) << "\n";
                sm_file << "p95_participation_pct=" << std::to_string(p95_part_pct) << "\n";
                sm_file << "median_participation_pct=" << std::to_string(med_part_pct) << "\n";
                sm_file << "pct_gross_over_5pct_adv=" << std::to_string(pct_gross_over5) << "\n";
                // (A2b) Portfolio Sharpe + IS/OOS split (additive; after the
                // capacity prefix; existing lines stay byte-identical). With no
                // split (holdout_begin == research.dates()), oos_idx is empty =>
                // OOS fields are NaN/0 and n_oos_periods=0, but portfolio_sharpe
                // (full series) is still meaningful. oos_max_drawdown /
                // oos_pnl_net are absolute pnl_net units (capacity_gross-scaled
                // dollars).
                sm_file << "holdout_begin=" << std::to_string(holdout_begin) << "\n";
                sm_file << "n_is_periods=" << std::to_string(is_idx.size()) << "\n";
                sm_file << "n_oos_periods=" << std::to_string(oos_idx.size()) << "\n";
                sm_file << "portfolio_sharpe=" << std::to_string(portfolio_sharpe) << "\n";
                sm_file << "portfolio_is_sharpe=" << std::to_string(portfolio_is_sharpe) << "\n";
                sm_file << "portfolio_oos_sharpe=" << std::to_string(portfolio_oos_sharpe) << "\n";
                sm_file << "oos_turnover=" << std::to_string(oos_turnover) << "\n";
                sm_file << "oos_max_drawdown=" << std::to_string(oos_max_drawdown) << "\n";
                sm_file << "oos_pnl_net=" << std::to_string(oos_pnl_net) << "\n";
            }
        }

        // 8. Stage digest = fnv1a64 over report numeric series in fixed order.
        std::vector<atx::f64> digest_buf;
        digest_buf.reserve(7 * S);
        for (atx::f64 v : rep.equity_curve)    { digest_buf.push_back(v); }
        for (atx::f64 v : rep.pnl_gross)       { digest_buf.push_back(v); }
        for (atx::f64 v : rep.pnl_net)         { digest_buf.push_back(v); }
        for (atx::f64 v : rep.pnl_cost)        { digest_buf.push_back(v); }
        for (atx::f64 v : rep.gross_leverage)  { digest_buf.push_back(v); }
        for (atx::f64 v : rep.net_exposure)    { digest_buf.push_back(v); }
        for (atx::f64 v : rep.turnover)        { digest_buf.push_back(v); }

        const atx::u64 digest = fnv1a64(
            digest_buf.data(),
            digest_buf.size() * sizeof(atx::f64));

        StageResult sr;
        sr.digest = digest;
        sr.kvs = {
            {"periods",               std::to_string(S)},
            {"final_equity",          std::to_string(final_equity)},
            {"pnl_net",               std::to_string(total_pnl_net)},
            {"avg_gross",             std::to_string(avg_gross_leverage)},
            {"max_participation_pct", std::to_string(max_part_pct)},
            // (A2b) Surface portfolio Sharpe to programmatic callers (e.g. the A4
            // acceptance test) without re-parsing summary.txt. Digest is over the
            // rep.* numeric series only, so these kvs do NOT affect it.
            {"portfolio_sharpe",      std::to_string(portfolio_sharpe)},
            {"portfolio_oos_sharpe",  std::to_string(portfolio_oos_sharpe)},
        };
        return atx::core::Ok(std::move(sr));
    }
}

} // namespace atx::impl
