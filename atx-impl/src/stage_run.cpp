#include "stages.hpp"

#include <charconv>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "artifacts.hpp"
#include "config.hpp"
#include "stage_combine.hpp"  // W0-I0a: resolve_nested_split, run_discover_window, CombinePitConfig
#include "stage_metabook.hpp" // S5-4: MetaBookStageConfig, run_metabook(cfg, scfg) (S2-owned body)

namespace atx::impl {

namespace fs = std::filesystem;
namespace fund = atx::engine::fund;

// S5-4: map the validated --sleeve-method string (config.cpp already restricts
// it to {erc, hrp, invvol}) to S2's fund::RiskBudgetMethod enum. "invvol" (the
// RunConfig default) maps to InverseVol -- NOT MetaAllocatorConfig{}'s own
// default (EqualRiskContribution) -- the simplest, most conservative choice
// among the three when --metabook is freshly enabled with no further tuning.
[[nodiscard]] static fund::RiskBudgetMethod sleeve_method_from_string(const std::string &s) {
    if (s == "erc") return fund::RiskBudgetMethod::EqualRiskContribution;
    if (s == "hrp") return fund::RiskBudgetMethod::HierarchicalRiskParity;
    return fund::RiskBudgetMethod::InverseVol; // "invvol" and any defensive fallback
}

// S5-4: the standalone "metabook" subcommand entry point (dispatch.cpp routes
// here). Builds a MetaBookStageConfig from cfg.sleeve_method and delegates to
// stage_metabook.hpp's 2-arg run_metabook (S2-owned body, untouched); the
// caller supplies cfg.panel / cfg.combo / cfg.books_out / (optionally)
// cfg.library_dir exactly as the standalone "optimize" subcommand does.
atx::core::Result<StageResult> run_metabook(const RunConfig &cfg) {
    MetaBookStageConfig scfg;
    scfg.meta.alloc.method = sleeve_method_from_string(cfg.sleeve_method);
    return run_metabook(cfg, scfg);
}

atx::core::Result<StageResult> run_all(const RunConfig& cfg)
{
    // Validate: work dir and zip are mandatory for run mode.
    if (cfg.out.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "run: --out (work dir) required");
    }
    if (cfg.zip.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "run: --zip required");
    }

    // A planning charge must not disappear at report handoff, including meta
    // books whose old sidecars round or omit that telemetry. Require a deliberate
    // actual fee choice; zero is valid only when explicitly selected.
    if (!std::isfinite(cfg.cost_bps) || cfg.cost_bps < 0.0 ||
        !std::isfinite(cfg.replay_trade_bps) || cfg.replay_trade_bps < 0.0) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "run: planning and replay trade rates must be finite/nonnegative");
    }
    if (cfg.cost_bps != 0.0 && cfg.replay_trade_bps == 0.0 &&
        cfg.set_flags.count("replay-trade-bps") == 0) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "run: --cost-bps requires an explicit --replay-trade-bps choice");
    }

    const fs::path work = cfg.out;
    {
        std::error_code ec;
        fs::create_directories(work, ec);
        if (ec) {
            return atx::core::Err(atx::core::ErrorCode::IoError,
                                  "run: cannot create work dir: " + cfg.out);
        }
    }

    // 1. load
    RunConfig c_load = cfg;
    c_load.out = (work / "segs").string();
    ATX_TRY(auto d_load, run_load(c_load));

    // 2. panel
    RunConfig c_panel = cfg;
    c_panel.segs      = (work / "segs").string();
    c_panel.panel_out = (work / "panel.bin").string();
    ATX_TRY(auto d_panel, run_panel(c_panel));

    // 3. discover
    RunConfig c_disc = cfg;
    c_disc.panel     = (work / "panel.bin").string();
    c_disc.alpha_out = (work / "alphas").string();
    // A1 — route the default pipeline through library accumulation. The
    // accumulation/library machinery lives entirely in run_discover_gated, which
    // run_discover only enters when cfg.gated is true (stage_discover.cpp:880);
    // the ungated top-N path never touches library_dir. So we must turn ON the
    // quality gate AND point it at a stable library dir:
    //   * gated => route through factory::Factory::mine_into + AlphaGate floors
    //     + the persistent library::Library (the mega-alpha database).
    //   * library_dir non-empty => accumulate (accumulate ==
    //     !cfg.library_dir.empty(), stage_discover.cpp:385) instead of a per-run
    //     wipe, AND auto-enable OOS-default admission of 0.25 (eff_oos_fraction,
    //     stage_discover.cpp:397-400) because run_all does not set --oos-fraction.
    // accumulate keys off library_dir (NOT set_flags), so no set_flags insert is
    // needed; an explicit user --oos-fraction still overrides the 0.25 default.
    c_disc.gated       = true;
    c_disc.library_dir = (work / "_library").string();

    // W0-I0a (I-01) — NESTED SPLITS: discover < combine fit < final test. Before W0,
    // discover admitted on the last 25% of the panel and combine then reported that
    // SAME 25% as "OOS". Now the panel is carved into three consecutive windows with an
    // h+delay embargo between them (resolve_nested_split): discover sees only
    // [0, discover_end) (its own admission lockbox lies inside it), the combiner fits on
    // the fresh [fit_begin, fit_end), and the final test [test_begin, n) is read by no
    // selection or fitting step. The test fraction is --holdout-frac when given (default
    // 0.25). An explicit --holdout-frac 0 or --fit-end keeps the caller's own windows
    // (no final test is reported then; combine's ledger guard still applies).
    const bool nested = !(cfg.set_flags.count("holdout-frac") != 0 &&
                          cfg.combine_holdout_frac <= 0.0) &&
                        cfg.set_flags.count("fit-end") == 0;
    NestedSplit split{};
    if (nested) {
        atx::usize n_dates = 0;
        bool have_dates = false;
        for (const auto& [k, v] : d_panel.kvs) {
            if (k == "dates") {
                const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), n_dates);
                have_dates = ec == std::errc{} && ptr == v.data() + v.size();
            }
        }
        if (!have_dates) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                  "run: panel stage reported no date count");
        }
        NestedSplitConfig ns;
        if (cfg.set_flags.count("holdout-frac") != 0) ns.test_frac = cfg.combine_holdout_frac;
        ns.embargo = 1U + cfg.replay_execution_delay; // h (1-day labels) + execution delay
        ATX_TRY(split, resolve_nested_split(n_dates, ns));
    }
    ATX_TRY(auto d_disc, run_discover_window(c_disc, nested ? split.discover_end : 0U));

    // 4. combine
    RunConfig c_comb = cfg;
    c_comb.panel     = (work / "panel.bin").string();
    c_comb.alphas    = (work / "alphas").string();
    c_comb.combo_out = (work / "combo.bin").string();
    // A1 — feed combine from the SAME accumulated library. With library_dir set,
    // run_combine takes the from_library branch (stage_combine.cpp:73),
    // enumerating admitted records by AlphaId; the loose c_comb.alphas above
    // becomes an ignored harmless fallback (left as-is intentionally).
    c_comb.library_dir = c_disc.library_dir;
    CombinePitConfig comb_pit;
    comb_pit.execution_delay = cfg.replay_execution_delay;
    if (nested) {
        // Fit on the nested fit window; the final test starts after the embargo.
        c_comb.fit_begin = static_cast<long>(split.fit_begin);
        c_comb.fit_end = static_cast<long>(split.fit_end);
        c_comb.set_flags.insert("fit-end");
        comb_pit.test_begin = split.test_begin;
    }
    ATX_TRY(auto d_comb, run_combine(c_comb, comb_pit));

    // 5. optimize (or, S5-4, --metabook: the sleeve-aware fund book substitutes
    // for the single-blend optimize stage — both write books.bin in the SAME
    // shape stage_report.cpp reads, so stage 6 is unchanged either way).
    RunConfig c_opt = cfg;
    c_opt.panel     = (work / "panel.bin").string();
    c_opt.combo     = (work / "combo.bin").string();
    c_opt.books_out = (work / "books.bin").string();
    // S7-4: engage the S6 sign-correct deploy path by default. Signal-as-position deploy
    // (position_mode) skips mean-variance optimize — the S6 work proved that path yields a
    // sign-correct, non-empty book with a sane participation footprint, whereas the MVO path
    // could invert the book sign. Default it ON only when the user has not explicitly chosen a
    // deploy mode (mirrors the holdout-frac guard at line 82). An explicit --position-mode or
    // --risk-aversion (either in set_flags) overrides.
    if (cfg.set_flags.count("position-mode") == 0 &&
        cfg.set_flags.count("risk-aversion") == 0) {
        c_opt.position_mode = true;
    }
    StageResult d_opt;
    if (cfg.metabook) {
        // S5-4: metabook stage (S2), skipped entirely when --metabook is absent
        // (the default) — run_all is byte-identical to the pre-S5 six-stage
        // graph on that path. Reuses the SAME accumulated library run_discover/
        // run_combine already populated (c_disc.library_dir), so the sleeve
        // partition sees every alpha this run admitted.
        MetaBookStageConfig scfg;
        scfg.meta.alloc.method = sleeve_method_from_string(cfg.sleeve_method);
        RunConfig c_meta = c_opt;
        c_meta.library_dir = c_disc.library_dir;
        ATX_TRY(d_opt, run_metabook(c_meta, scfg));
    } else {
        ATX_TRY(d_opt, run_optimize(c_opt));
    }

    // 6. report
    RunConfig c_rep = cfg;
    // --cost-bps belongs to the optimizer's planning model. The identified
    // replay uses its own explicit per-traded-dollar fee, never this scalar.
    c_rep.cost_bps = 0.0;
    c_rep.panel      = (work / "panel.bin").string();
    c_rep.books      = (work / "books.bin").string();
    // A2b — point report at combo.bin so it finds combo.bin.meta and can split
    // its per-period series into in-sample / out-of-sample for portfolio Sharpe.
    c_rep.combo      = (work / "combo.bin").string();
    c_rep.report_out = cfg.report_out.empty()
                           ? (work / "report").string()
                           : cfg.report_out;
    ATX_TRY(auto d_rep, run_report(c_rep));

    // Fold the 6 stage digests in fixed order into the run digest.
    atx::u64 ds[6] = {
        d_load.digest,
        d_panel.digest,
        d_disc.digest,
        d_comb.digest,
        d_opt.digest,
        d_rep.digest,
    };
    const atx::u64 run_digest = fnv1a64(ds, sizeof(ds));

    StageResult sr;
    sr.digest = run_digest;
    sr.kvs = {
        {"load",     to_hex16(d_load.digest)},
        {"panel",    to_hex16(d_panel.digest)},
        {"discover", to_hex16(d_disc.digest)},
        {"combine",  to_hex16(d_comb.digest)},
        // S5-4: the 5th kv names WHICH stage produced books.bin — "metabook" when
        // --metabook substituted the sleeve-aware fund book, else the pre-S5
        // "optimize" key (byte-identical key text at the default, off-path).
        {cfg.metabook ? "metabook" : "optimize", to_hex16(d_opt.digest)},
        {"report",   to_hex16(d_rep.digest)},
    };
    return atx::core::Ok(std::move(sr));
}

} // namespace atx::impl
