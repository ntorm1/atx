#include "stages.hpp"

#include <algorithm>  // std::max
#include <cmath>      // std::floor
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>      // std::optional (C2.1: scope-lifetime ProcessExecutor)
#include <sstream>
#include <string>
#include <thread>        // std::thread::hardware_concurrency
#include <unordered_map>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/parser.hpp"        // alpha::Library
#include "atx/engine/combine/gate.hpp"        // combine::AlphaGate, GateConfig
#include "atx/engine/eval/lockbox.hpp"        // eval::reserve_lockbox (P2b guard)
#include "atx/engine/eval/cpcv.hpp"           // eval::CpcvConfig (default embargo)
#include "atx/engine/exec/execution_sim.hpp"  // exec::ExecutionSimulator
#include "atx/engine/factory/factory.hpp"     // factory::FactoryConfig
#include "atx/engine/factory/research_driver.hpp" // factory::ResearchDriver, ResearchConfig
#include "atx/engine/library/library.hpp"     // library::Library
#include "atx/engine/loop/weight_policy.hpp"  // engine::WeightPolicy
#include "atx/engine/parallel/executor.hpp"        // parallel::ExecutorConfig (C2.1 substrate)
#include "atx/engine/parallel/process_executor.hpp" // parallel::ProcessExecutor (C2.1 substrate)

#include "artifacts.hpp"
#include "config.hpp"
#include "research_sim.hpp"
#include "serialize_panel.hpp"
#include "stage_discover_detail.hpp"  // T3a: detail::build_robust_holdout_panel

namespace atx::impl {

// ---------------------------------------------------------------------------
// run_sweep — drive ResearchDriver over K seeds into one --library-dir
//
// Mirrors run_discover_gated for object setup, then hands the panel + gate +
// library over to factory::ResearchDriver which runs up to --sweep-runs mine
// iterations, each on a distinct per-run seed (seed_for_run), accumulating
// into the one shared --library-dir library.  R1 (cumulative trial counter),
// R2 (walk-forward window per run), and R3a (OOS-on-by-default) are all wired
// in here; their engine-side implementations are unchanged.
//
// The sweep ALWAYS requires --library-dir (accumulation is its whole purpose).
// ---------------------------------------------------------------------------
atx::core::Result<StageResult> run_sweep(const RunConfig& cfg)
{
    namespace fs      = std::filesystem;
    namespace combine = atx::engine::combine;
    namespace factory = atx::engine::factory;
    namespace library = atx::engine::library;
    namespace alpha   = atx::engine::alpha;
    namespace eval    = atx::engine::eval;

    // ---- Validation --------------------------------------------------------
    if (cfg.panel.empty() || cfg.alpha_out.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
            "sweep: --panel and --alpha-out required");
    }
    if (cfg.seed_exprs.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
            "sweep: at least one --seed-expr required");
    }
    if (cfg.library_dir.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
            "sweep: --library-dir is required (sweep always accumulates into a persistent library)");
    }
    if (cfg.sweep_runs < 1) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
            "sweep: --sweep-runs must be >= 1");
    }

    // ---- Panel + run-wide borrows ------------------------------------------
    ATX_TRY(auto panel, read_panel(cfg.panel));

    alpha::Library dsl{};
    atx::engine::WeightPolicy policy{};
    atx::engine::exec::ExecutionSimulator sim = frictionless_sim();

    // Collect panel field names.
    std::vector<std::string> fields;
    for (atx::usize i = 0; i < panel.num_fields(); ++i) {
        fields.push_back(std::string{panel.field_name(i)});
    }

    // ---- SearchConfig (same as run_discover) --------------------------------
    factory::SearchConfig sc;
    sc.master_seed  = cfg.seed;
    sc.population   = cfg.population  > 0
                        ? static_cast<atx::usize>(cfg.population)  : 200;
    sc.generations  = cfg.generations > 0
                        ? static_cast<atx::usize>(cfg.generations) : 5;
    sc.elites       = 2;
    sc.k_tournament = 3;
    sc.p_cross      = 0.5;
    sc.enable_behavioral_novelty = true;

    {
        const unsigned hw = std::thread::hardware_concurrency();
        const atx::usize autow = hw > 1 ? static_cast<atx::usize>(hw - 1) : 1;
        sc.n_workers = cfg.workers > 0 ? static_cast<atx::usize>(cfg.workers) : autow;
    }
    sc.n_immigrants = std::max<atx::usize>(sc.population / 10, 4);

    // ---- AlphaGate ---------------------------------------------------------
    combine::GateConfig gc;
    gc.min_sharpe    = cfg.min_sharpe;
    gc.min_fitness   = cfg.min_fitness;
    gc.max_turnover  = cfg.max_turnover;
    gc.max_pool_corr = cfg.max_pool_corr;
    const combine::AlphaGate gate{gc};

    // ---- A4: default a research sweep to R2 walk-forward validation ---------
    // Default 3 rotating holdout windows so the accumulated library is mined
    // against multiple OOS slices, not one terminal holdout. 3 windows is chosen
    // so that at the system-standard 0.25 holdout fraction the panel tiles
    // feasibly: 3*0.25 (holdout) + 0.25 (one leading train block) = 1.0*T. This
    // keeps the standard 0.25 OOS fraction (consistent with combine holdout-frac
    // and discover R3a) instead of coupling a second lowered default — the
    // infeasibility cliff sits at an implausibly-high EXPLICIT fraction
    // (>=~0.34) rather than at the common 0.25 value. The roadmap specified "~4"
    // windows, so 3 is within spec. An explicit --oos-windows (incl. an explicit
    // 0 = legacy terminal holdout) still wins, as does an explicit --oos-fraction.
    const long eff_oos_windows =
        (cfg.set_flags.count("oos-windows") == 0) ? 3 : cfg.oos_windows;

    // ---- R3a: OOS-on-by-default for sweep (sweep ALWAYS accumulates) --------
    // Sweep always accumulates, so oos_fraction defaults ON unless the user
    // explicitly set --oos-fraction (same rule as discover's accumulation path).
    const atx::f64 eff_oos_fraction =
        (cfg.set_flags.count("oos-fraction") == 0) ? 0.25 : cfg.oos_fraction;

    // ---- P2b geometry guard (same as run_discover_gated) -------------------
    if (eff_oos_fraction > 0.0) {
        const atx::usize T = panel.dates();
        const atx::usize embargo_len =
            (cfg.oos_embargo > 0.0)
                ? eval::detail::embargo_len_from_cpcv(cfg.oos_embargo, T)
                : eval::detail::embargo_len_from_cpcv(eval::CpcvConfig{}.embargo, T);
        auto sealed_r = eval::reserve_lockbox(panel, eff_oos_fraction, embargo_len);
        if (!sealed_r.has_value()) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                "sweep: --oos-fraction " + std::to_string(eff_oos_fraction) +
                " leaves too little train/holdout for a panel of " +
                std::to_string(panel.dates()) + " dates (" +
                sealed_r.error().message() + ")");
        }

        // ---- A4 up-front walk-forward geometry guard -----------------------
        // For the windowed path the engine tiles eff_oos_windows DISJOINT holdout
        // blocks of width w = floor(oos_fraction * T) over the terminal region and
        // REQUIRES at least one leading training block to precede the earliest
        // window (factory.cpp:888 admits ZERO when
        //   embargo_len + 1 >= T  ||  oos_n_windows > (T - embargo_len - 1)/w,
        // i.e. when  oos_n_windows * w >= T - embargo_len). Mirror that exact
        // inequality here (same overflow-safe division form) so an infeasible
        // windows x fraction surfaces an actionable geometry diagnostic instead of
        // the misleading "no alphas cleared the gate" path.
        if (eff_oos_windows > 0) {
            const atx::usize n_win = static_cast<atx::usize>(eff_oos_windows);
            const atx::usize w =
                static_cast<atx::usize>(std::floor(eff_oos_fraction * static_cast<double>(T)));
            const bool infeasible =
                (w == 0U) ||
                (embargo_len + 1U >= T) ||
                (n_win > (T - embargo_len - 1U) / w);
            if (infeasible) {
                return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                    "sweep: --oos-windows N (=" + std::to_string(eff_oos_windows) +
                    ") x floor(--oos-fraction " + std::to_string(eff_oos_fraction) +
                    " * T=" + std::to_string(T) + ") = " + std::to_string(n_win * w) +
                    " tiles >= the T-date panel minus embargo (" +
                    std::to_string(T - embargo_len) +
                    "), leaving no training block; lower --oos-windows or --oos-fraction");
            }
        }
    }

    // ---- T3a W4a robust factor: build weak/holdout sub-universe when active ----
    // When --robust-holdout-frac > 0, build a DETERMINISTIC seeded weak panel
    // over the same (post-capacity-screen) `panel`, seeded by sc.master_seed
    // (the stable per-sweep seed — the weak sub-universe is the SAME fixed
    // sub-universe for every run of the sweep; ResearchDriver derives per-run
    // search seeds separately). The OWNED optional MUST outlive rd.run(rc)
    // (the pointer is copied into rc.per_run.weak_panel and dereferenced during
    // every mine_into of the sweep) — same lifetime discipline as pexec below.
    // frac == 0 (the default) -> no panel built, weak_panel stays nullptr,
    // robust stays 1.0, and the sweep is byte-identical to the pre-T3a baseline.
    std::optional<alpha::Panel> weak_panel_owned;
    const alpha::Panel* weak_panel = nullptr;
    if (cfg.robust_holdout_frac > 0.0) {
        ATX_TRY(auto wp, detail::build_robust_holdout_panel(panel, cfg.robust_holdout_frac,
                                                            sc.master_seed));
        weak_panel_owned.emplace(std::move(wp));
        weak_panel = &*weak_panel_owned; // borrowed; weak_panel_owned outlives rd.run(rc)
    }

    // ---- R1 typed-fields cardinality scan (opt-in via --typed-fields; default OFF) ----
    // Same logic as stage_discover.cpp: when ON, a single deterministic pass over the
    // (post-capacity-screen) panel classifies low-cardinality fields as numeric-excluded
    // and any raw categorical column as extra-group. Empty when OFF -> byte-identical.
    std::vector<std::string> numeric_excluded_fields;
    std::vector<std::string> extra_group_fields;
    if (cfg.typed_fields) {
        detail::classify_typed_fields(panel, fields, cfg.field_cardinality_max,
                                      numeric_excluded_fields, extra_group_fields);
    }

    // ---- FactoryConfig (per-run template handed to ResearchDriver) ----------
    factory::FactoryConfig per_run;
    per_run.search                    = sc;
    per_run.search.fitness.target_aum = cfg.target_aum;
    per_run.seed_exprs                = cfg.seed_exprs;
    per_run.panel_fields              = fields;
    per_run.min_dsr                   = cfg.min_dsr;
    per_run.oos_fraction              = eff_oos_fraction;
    per_run.oos_embargo               = cfg.oos_embargo;
    // A4 — eff_oos_windows computed above (default 3 at the plain 0.25 fraction).
    per_run.oos_n_windows = static_cast<atx::usize>(std::max<long>(eff_oos_windows, 0));
    per_run.oos_window    = static_cast<atx::usize>(std::max<long>(cfg.oos_window,  0));
    // NOTE: per_run.oos_window is the base value; ResearchDriver::run overrides it
    // per run (run % oos_n_windows) when oos_n_windows > 0 (R2 wiring in research_driver.cpp).
    per_run.weak_panel = weak_panel; // T3a: nullptr (default) keeps robust=1.0
    // R1: empty lists (default) keep SearchDriver partition identical -> byte-identical.
    per_run.numeric_excluded_fields = numeric_excluded_fields;
    per_run.extra_group_fields      = extra_group_fields;
    per_run.max_price_scale_corr    = cfg.max_price_scale_corr; // R2 price-scale gate (off by default = 1.0)
    per_run.dsr_subwindows = static_cast<atx::usize>(std::max<int>(cfg.dsr_subwindows, 0)); // R3 intra-holdout DSR sub-windows (off by default = 0)
    per_run.search.deflate_selection = cfg.deflate_selection; // R4: opt-in deflated-Sharpe search selection

    // ---- Open accumulating library (do NOT wipe) ---------------------------
    fs::create_directories(cfg.library_dir);
    library::Library liblib = library::Library::open(cfg.library_dir, gc, {cfg.seed});

    // ---- Run ResearchDriver ------------------------------------------------
    factory::ResearchDriver rd{liblib, dsl, panel, sim, policy, gate};
    factory::ResearchConfig rc;
    rc.per_run     = per_run;
    rc.max_runs    = static_cast<atx::usize>(cfg.sweep_runs);
    rc.patience    = static_cast<atx::usize>(std::max<long>(cfg.patience, 0));
    rc.master_seed = cfg.seed;
    rc.robustness_gate = false; // out of scope this sprint

    // C2.1 — opt-in parallel substrate. "process" runs each per-run mine on the proven
    // bit-identical ProcessExecutor (mine_into_oos_parallel); "" / "inprocess" keep the
    // serial path (rc.exec stays nullptr — byte-identical to today). The ProcessExecutor
    // (declared in this scope) MUST outlive rd.run(rc). cfg.workers picks the worker count
    // (0 ⇒ substrate default); sc.n_workers (the in-process DetPool) does NOT drive the
    // cross-process eval map on the MultiProcess substrate.
    std::optional<atx::engine::parallel::ProcessExecutor> pexec;
    if (cfg.executor == "process") {
        const atx::usize w = cfg.workers > 0 ? static_cast<atx::usize>(cfg.workers) : 0;
        pexec.emplace(atx::engine::parallel::ExecutorConfig{w, false});
        rc.exec = &*pexec;
    }

    const factory::ResearchReport rep = rd.run(rc);

    // ---- Flush + write durable sidecar (R1 counter) -----------------------
    {
        auto fr = liblib.flush_all();
        if (!fr.has_value()) {
            return atx::core::Err(fr.error());
        }
    }
    // R1: write the durable cumulative-trials sidecar (_manifest.bin) so a
    // follow-on sweep or discover --library-dir restores cumulative_trials_.
    (void)liblib.snapshot();

    const atx::u64 n = liblib.n_alphas();
    if (n == 0) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
            "sweep: no alphas cleared the gate across all runs "
            "(loosen gate floors, raise --sweep-runs / --population / --generations, "
            "or widen the panel)");
    }

    // ---- Write .dsl files (AlphaId order = best-deflated-first) -----------
    fs::create_directories(cfg.alpha_out);
    std::vector<std::string> dsl_strs;
    dsl_strs.reserve(static_cast<atx::usize>(n));

    for (atx::u64 a = 0; a < n; ++a) {
        const auto arec = liblib.get(library::AlphaId{static_cast<atx::u32>(a)});
        const std::string& expr = arec.provenance.expr_source;

        std::ostringstream name_ss;
        name_ss << "alpha_" << std::setw(3) << std::setfill('0') << a << ".dsl";
        const std::string dsl_path = (fs::path{cfg.alpha_out} / name_ss.str()).string();
        std::ofstream of{dsl_path};
        if (!of.is_open()) {
            return atx::core::Err(atx::core::ErrorCode::IoError,
                "sweep: cannot write alpha: " + dsl_path);
        }
        of << expr << '\n';
        dsl_strs.push_back(expr);
    }

    // ---- Write _manifest.txt -----------------------------------------------
    // NOTE: _manifest.txt is human-readable telemetry ONLY — downstream combine/run
    // consume the .dsl files (byte-identical to discover's), NOT this manifest.
    // The sweep-specific header fields (runs/total_admitted/research_digest/
    // manifest_version_id) are intentional additions for observability and do NOT
    // need to match discover's header exactly.  Do NOT change this format to match
    // discover's — the .dsl files are the canonical artifact; the manifest is just
    // a human-readable sidecar for debugging and auditing.
    {
        // Build OOS metrics lookup (canon_hash -> OosReportEntry) from the
        // last run's rep.oos_metrics.  For a multi-run sweep, oos_metrics
        // only covers the LAST mine_into call (ResearchDriver only surfaces the
        // last run's FactoryReport); downstream manifest IS/OOS columns are
        // best-effort for the multi-run case (no API to aggregate).
        std::unordered_map<atx::u64, factory::OosReportEntry> oos_map;
        for (const auto& entry : rep.last_run_oos_metrics) {
            oos_map[entry.canon_hash] = entry;
        }

        const std::string manifest_path = (fs::path{cfg.alpha_out} / "_manifest.txt").string();
        std::ofstream mf{manifest_path};
        if (!mf.is_open()) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                "sweep: cannot write manifest: " + manifest_path);
        }
        mf << "gated=1\n";
        mf << "seed="              << cfg.seed               << '\n';
        mf << "count="             << n                      << '\n';
        mf << "runs="              << rep.runs               << '\n';
        mf << "total_admitted="    << rep.total_admitted      << '\n';
        mf << "total_duplicates="  << rep.total_duplicates    << '\n';
        // C2.2 (measurement-only; REPORT-ONLY) — cross-run redundant-eval telemetry.
        // Additive human-readable manifest lines; not part of any digest.
        mf << "cross_run_total_evals="     << rep.cross_run_total_evals     << '\n';
        mf << "cross_run_distinct_evals="  << rep.cross_run_distinct_evals  << '\n';
        mf << "cross_run_redundant_evals=" << rep.cross_run_redundant_evals << '\n';
        mf << "cross_run_redundant_pct="   << rep.cross_run_redundant_pct   << '\n';
        mf << "research_digest="   << to_hex16(rep.digest)   << '\n';
        mf << "manifest_version_id=" << to_hex16(static_cast<atx::u64>(rep.manifest_version_id)) << '\n';
        mf << "min_dsr="           << cfg.min_dsr            << '\n';
        mf << "min_sharpe="        << gc.min_sharpe          << '\n';
        mf << "min_fitness="       << gc.min_fitness         << '\n';
        mf << "max_turnover="      << gc.max_turnover        << '\n';
        mf << "max_pool_corr="     << gc.max_pool_corr       << '\n';
        mf << "target_aum="        << cfg.target_aum         << '\n';
        if (eff_oos_fraction > 0.0) {
            mf << "oos_fraction="  << eff_oos_fraction       << '\n';
            mf << "oos_embargo="   << cfg.oos_embargo        << '\n';
        }
        mf << "panel="             << cfg.panel              << '\n';
        for (atx::u64 a = 0; a < n; ++a) {
            const auto arec = liblib.get(library::AlphaId{static_cast<atx::u32>(a)});
            mf << "alpha[" << a << "]"
               << " sharpe="   << arec.metrics.sharpe
               << " fitness="  << arec.metrics.fitness
               << " turnover=" << arec.metrics.turnover
               << " returns="  << arec.metrics.returns
               << " drawdown=" << arec.metrics.drawdown;
            if (eff_oos_fraction > 0.0) {
                const auto it = oos_map.find(arec.canon_hash);
                if (it != oos_map.end()) {
                    const auto& e = it->second;
                    mf << " is_sharpe="   << e.is_metrics.sharpe
                       << " is_fitness="  << e.is_metrics.fitness
                       << " is_turnover=" << e.is_metrics.turnover
                       << " oos_sharpe="  << e.oos_metrics.sharpe
                       << " oos_fitness=" << e.oos_metrics.fitness
                       << " oos_turnover="<< e.oos_metrics.turnover;
                }
            }
            mf << " : " << arec.provenance.expr_source << '\n';
        }
    }

    // ---- Stage digest (fnv1a64 over '\n'-joined DSL, same as discover) -----
    std::string joined;
    for (atx::usize i = 0; i < dsl_strs.size(); ++i) {
        if (i > 0) joined += '\n';
        joined += dsl_strs[i];
    }
    const atx::u64 stage_digest = fnv1a64(joined.data(), joined.size());

    // dedup_pct for kvs
    const atx::usize denom = rep.total_admitted + rep.total_duplicates;
    const atx::f64 dedup_pct = (denom == 0U)
        ? 0.0
        : static_cast<atx::f64>(rep.total_duplicates) / static_cast<atx::f64>(denom);

    StageResult sr;
    sr.digest = stage_digest;
    sr.kvs = {
        {"runs",              std::to_string(rep.runs)},
        {"library_size",      std::to_string(n)},
        {"total_admitted",    std::to_string(rep.total_admitted)},
        {"total_duplicates",  std::to_string(rep.total_duplicates)},
        {"dedup_pct",         std::to_string(dedup_pct)},
        // C2.2 (measurement-only; REPORT-ONLY) — cross-run redundant-eval telemetry.
        {"cross_run_total_evals",     std::to_string(rep.cross_run_total_evals)},
        {"cross_run_distinct_evals",  std::to_string(rep.cross_run_distinct_evals)},
        {"cross_run_redundant_evals", std::to_string(rep.cross_run_redundant_evals)},
        {"cross_run_redundant_pct",   std::to_string(rep.cross_run_redundant_pct)},
        {"research_digest",   to_hex16(rep.digest)},
        {"manifest_version_id", to_hex16(static_cast<atx::u64>(rep.manifest_version_id))},
        {"seed",              std::to_string(cfg.seed)},
        {"population",        std::to_string(sc.population)},
        {"generations",       std::to_string(sc.generations)},
        {"sweep_runs",        std::to_string(cfg.sweep_runs)},
        {"oos_windows",       std::to_string(eff_oos_windows)},
        // C2.1 — the EFFECTIVE substrate ("" defaults to the serial inprocess path).
        {"executor",          cfg.executor.empty() ? std::string{"inprocess"} : cfg.executor},
    };
    return atx::core::Ok(std::move(sr));
}

} // namespace atx::impl
