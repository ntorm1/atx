#include "stages.hpp"

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

        // 7b. Write convenience files (not R8-pinned).
        {
            const fs::path rdir{cfg.report_out};
            // equity_curve.csv
            {
                std::ofstream ec_file{rdir / "equity_curve.csv"};
                if (!ec_file.is_open()) {
                    return atx::core::Err(atx::core::ErrorCode::IoError,
                                          "report: cannot write equity_curve.csv");
                }
                ec_file << "period,equity\n";
                for (atx::usize s = 0; s < S; ++s) {
                    ec_file << std::to_string(s) << ","
                            << std::to_string(rep.equity_curve[s]) << "\n";
                }
            }
            // summary.txt
            {
                std::ofstream sm_file{rdir / "summary.txt"};
                if (!sm_file.is_open()) {
                    return atx::core::Err(atx::core::ErrorCode::IoError,
                                          "report: cannot write summary.txt");
                }
                sm_file << "final_equity=" << std::to_string(final_equity) << "\n";
                sm_file << "total_pnl_gross=" << std::to_string(total_pnl_gross) << "\n";
                sm_file << "total_pnl_net=" << std::to_string(total_pnl_net) << "\n";
                sm_file << "total_pnl_cost=" << std::to_string(total_pnl_cost) << "\n";
                sm_file << "avg_gross_leverage=" << std::to_string(avg_gross_leverage) << "\n";
                sm_file << "avg_turnover=" << std::to_string(avg_turnover) << "\n";
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
            {"periods",       std::to_string(S)},
            {"final_equity",  std::to_string(final_equity)},
            {"pnl_net",       std::to_string(total_pnl_net)},
            {"avg_gross",     std::to_string(avg_gross_leverage)},
        };
        return atx::core::Ok(std::move(sr));
    }
}

} // namespace atx::impl
