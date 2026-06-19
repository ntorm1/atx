#include "stages.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include <thread>                                 // std::thread::hardware_concurrency

#include "atx/engine/alpha/parser.hpp"          // alpha::Library, alpha::parse_expr
#include "atx/engine/alpha/unparse.hpp"          // alpha::unparse
#include "atx/engine/combine/gate.hpp"           // combine::AlphaGate, GateConfig
#include "atx/engine/combine/store.hpp"          // combine::AlphaStore
#include "atx/engine/exec/execution_sim.hpp"     // exec::ExecutionSimulator + configs
#include "atx/engine/factory/factory.hpp"        // factory::Factory, FactoryConfig, FactoryReport
#include "atx/engine/factory/genome.hpp"         // factory::Genome
#include "atx/engine/factory/search_driver.hpp"  // factory::SearchDriver, SearchConfig, SearchResult
#include "atx/engine/library/library.hpp"        // library::Library, AlphaId, AlphaRecordView
#include "atx/engine/loop/weight_policy.hpp"     // engine::WeightPolicy

#include "artifacts.hpp"
#include "config.hpp"
#include "research_sim.hpp"
#include "serialize_genome.hpp"
#include "serialize_panel.hpp"

namespace atx::impl {

namespace exec = atx::engine::exec;

// ---------------------------------------------------------------------------
// run_discover_gated — opt-in quality-gated discovery (--gated).
//
// Routes the evolutionary search through factory::Factory::mine_into: every
// distinct candidate is ranked by deflated Sharpe (DSR — the multiple-testing-
// corrected statistic) and admitted into a persistent library::Library only if
// it clears the AlphaGate floors (min standalone Sharpe, min WorldQuant fitness,
// max turnover, max |corr| to any already-admitted alpha) AND dsr >= min_dsr.
// Setting --target-aum > 0 additionally activates the ADV-aware capacity cost
// objective in the search fitness (favoring high-capacity alphas). The library
// is a durable on-disk alpha database; admitted alphas are ALSO written to
// <alpha_out>/alpha_NNN.dsl (deterministic, best-deflated-first) so the
// downstream `combine` stage consumes them unchanged.
// ---------------------------------------------------------------------------
namespace {

atx::core::Result<StageResult> run_discover_gated(
    const RunConfig& cfg,
    const atx::engine::alpha::Panel& panel,
    const atx::engine::alpha::Library& lib,
    const atx::engine::WeightPolicy& policy,
    const atx::engine::exec::ExecutionSimulator& sim,
    const atx::engine::factory::SearchConfig& sc,
    const std::vector<std::string>& fields)
{
    namespace fs      = std::filesystem;
    namespace combine = atx::engine::combine;
    namespace factory = atx::engine::factory;
    namespace library = atx::engine::library;

    // Fresh library dir each run: deterministic (same seed => same DB) and avoids
    // accumulating stale alphas across re-runs.
    const std::string lib_dir = (fs::path{cfg.alpha_out} / "_library").string();
    std::error_code ec;
    fs::remove_all(lib_dir, ec);
    fs::create_directories(lib_dir);

    // AlphaGate floors from the CLI (defaults: BRAIN gold-standard sharpe/fitness
    // = 1.0, WQ turnover <= 0.70, orthogonality |corr| <= 0.70).
    combine::GateConfig gc;
    gc.min_sharpe    = cfg.min_sharpe;
    gc.min_fitness   = cfg.min_fitness;
    gc.max_turnover  = cfg.max_turnover;
    gc.max_pool_corr = cfg.max_pool_corr;
    const combine::AlphaGate gate{gc};

    // Factory config: search budget + grammar + capacity (target_aum) + the S1
    // deflation admission bar (min_dsr).
    factory::FactoryConfig fcfg;
    fcfg.search                    = sc;
    fcfg.search.fitness.target_aum = cfg.target_aum; // >0 => ADV-aware cost objective (capacity)
    fcfg.seed_exprs                = cfg.seed_exprs;
    fcfg.panel_fields              = fields;
    fcfg.min_dsr                   = cfg.min_dsr;

    // Mine -> deflate -> gate -> admit into the persistent library.
    factory::Factory fac{lib, panel, sim, policy};
    library::Library liblib = library::Library::open(lib_dir, gc, {cfg.seed});
    const factory::FactoryReport rep = fac.mine_into(fcfg, liblib, gate);

    {
        auto fr = liblib.flush_all();
        if (!fr.has_value()) {
            return atx::core::Err(fr.error());
        }
    }

    const atx::u64 n = liblib.n_alphas();
    if (n == 0) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
            "discover (gated): no alphas cleared the gate "
            "(loosen --min-sharpe/--min-fitness/--max-turnover/--max-pool-corr/--min-dsr, "
            "widen the panel, or raise --population/--generations)");
    }

    // Build the reject-histogram string verbatim (per library::AdmitKind, 0..5).
    std::string rej;
    for (atx::usize i = 0; i < rep.reject_histogram.size(); ++i) {
        if (i > 0) rej += ',';
        rej += std::to_string(rep.reject_histogram[i]);
    }

    // Write admitted alphas (AlphaId order == best-deflated-first) as .dsl.
    fs::create_directories(cfg.alpha_out);
    std::vector<std::string> dsl;
    dsl.reserve(static_cast<atx::usize>(n));
    for (atx::u64 a = 0; a < n; ++a) {
        const auto rec = liblib.get(library::AlphaId{static_cast<atx::u32>(a)});
        const std::string& expr = rec.provenance.expr_source;

        std::ostringstream name_ss;
        name_ss << "alpha_" << std::setw(3) << std::setfill('0') << a << ".dsl";
        const std::string dsl_path = (fs::path{cfg.alpha_out} / name_ss.str()).string();
        std::ofstream of{dsl_path};
        if (!of.is_open()) {
            return atx::core::Err(atx::core::ErrorCode::IoError,
                "discover (gated): cannot write alpha: " + dsl_path);
        }
        of << expr << '\n';
        dsl.push_back(expr);
    }

    // _manifest.txt — admitted alphas + per-alpha metrics + gate parameters.
    {
        const std::string manifest_path = (fs::path{cfg.alpha_out} / "_manifest.txt").string();
        std::ofstream mf{manifest_path};
        if (!mf.is_open()) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                "discover (gated): cannot write manifest: " + manifest_path);
        }
        mf << "gated=1\n";
        mf << "seed="            << cfg.seed             << '\n';
        mf << "count="           << n                    << '\n';
        mf << "evaluated="       << rep.evaluated        << '\n';
        mf << "duplicates="      << rep.duplicates       << '\n';
        mf << "reject_histogram="<< rej                  << '\n';
        mf << "factory_digest="  << to_hex16(rep.digest) << '\n';
        mf << "min_dsr="         << cfg.min_dsr          << '\n';
        mf << "min_sharpe="      << gc.min_sharpe        << '\n';
        mf << "min_fitness="     << gc.min_fitness       << '\n';
        mf << "max_turnover="    << gc.max_turnover      << '\n';
        mf << "max_pool_corr="   << gc.max_pool_corr     << '\n';
        mf << "target_aum="      << cfg.target_aum       << '\n';
        mf << "panel="           << cfg.panel            << '\n';
        for (atx::u64 a = 0; a < n; ++a) {
            const auto rec = liblib.get(library::AlphaId{static_cast<atx::u32>(a)});
            mf << "alpha[" << a << "]"
               << " sharpe="   << rec.metrics.sharpe
               << " fitness="  << rec.metrics.fitness
               << " turnover=" << rec.metrics.turnover
               << " returns="  << rec.metrics.returns
               << " drawdown=" << rec.metrics.drawdown
               << " : " << rec.provenance.expr_source << '\n';
        }
    }

    // Stage digest: fnv1a64 over '\n'-joined DSL (process-stable; same scheme as
    // the ungated path).
    std::string joined;
    for (atx::usize i = 0; i < dsl.size(); ++i) {
        if (i > 0) joined += '\n';
        joined += dsl[i];
    }
    const atx::u64 stage_digest = fnv1a64(joined.data(), joined.size());

    StageResult sr;
    sr.digest = stage_digest;
    sr.kvs = {
        {"gated",           "1"},
        {"admitted",        std::to_string(n)},
        {"evaluated",       std::to_string(rep.evaluated)},
        {"duplicates",      std::to_string(rep.duplicates)},
        {"reject_hist",     rej},
        {"factory_digest",  to_hex16(rep.digest)},
        {"population",      std::to_string(sc.population)},
        {"generations",     std::to_string(sc.generations)},
    };
    return atx::core::Ok(std::move(sr));
}

} // namespace

// ---------------------------------------------------------------------------
// run_discover
// ---------------------------------------------------------------------------
atx::core::Result<StageResult> run_discover(const RunConfig& cfg)
{
    namespace alpha   = atx::engine::alpha;
    namespace combine = atx::engine::combine;
    namespace factory = atx::engine::factory;
    using atx::engine::WeightPolicy;

    // 1. Validate required flags.
    if (cfg.panel.empty() || cfg.alpha_out.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "discover: --panel and --out (alpha_out) required");
    }
    if (cfg.seed_exprs.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "discover: at least one --seed-expr required");
    }

    // 2. Load the research panel.
    ATX_TRY(auto panel, read_panel(cfg.panel));

    // 3. Build run-wide objects.
    alpha::Library lib{};
    WeightPolicy   policy{};
    exec::ExecutionSimulator sim = frictionless_sim();
    combine::AlphaStore pool{};   // empty pool: first-generation discovery

    // 4. Collect panel field names.
    std::vector<std::string> fields;
    for (atx::usize i = 0; i < panel.num_fields(); ++i) {
        fields.push_back(std::string{panel.field_name(i)});
    }

    // 5. Build SearchConfig.
    factory::SearchConfig sc;
    sc.master_seed  = cfg.seed;
    sc.population   = cfg.population  > 0
                        ? static_cast<atx::usize>(cfg.population)  : 16;
    sc.generations  = cfg.generations > 0
                        ? static_cast<atx::usize>(cfg.generations) : 5;
    sc.elites       = 2;
    sc.k_tournament = 3;
    sc.p_cross      = 0.5;
    sc.novelty_w    = 0.1;

    // Parallelize the search (digest-invariant: SearchConfig::n_workers affects
    // speed/memory, never bits — F1). --workers overrides; 0 = auto (cores-1).
    // Each worker holds a full per-genome signal buffer, so on a wide panel with
    // limited RAM, cap --workers to bound peak memory.
    {
        const unsigned hw = std::thread::hardware_concurrency();
        const atx::usize autow = hw > 1 ? static_cast<atx::usize>(hw - 1) : 1;
        sc.n_workers = cfg.workers > 0 ? static_cast<atx::usize>(cfg.workers) : autow;
    }

    // 6'. Gated discovery (opt-in via --gated): route every distinct candidate
    //     through the factory's deflated-Sharpe ranking + AlphaGate floors so the
    //     emitted alphas are robust (DSR bar), low-turnover, low-correlation, and
    //     high-fitness. Admitted alphas persist in an on-disk library::Library
    //     (a durable alpha database) and are also written as .dsl for `combine`.
    if (cfg.gated) {
        return run_discover_gated(cfg, panel, lib, policy, sim, sc, fields);
    }

    // 6. Run the evolutionary search (default ungated path: top-N by raw fitness).
    factory::SearchDriver driver{lib, panel, policy, sim, cfg.seed_exprs, fields};
    factory::SearchResult res = driver.run(sc, pool);

    // 7. Check admission.
    const auto& admitted = res.admitted_candidates;
    if (admitted.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "discover: search admitted no alphas "
                              "(check seed exprs / panel)");
    }

    // NOTE on --min-dsr: SearchResult exposes no per-genome deflated-Sharpe,
    // so DSR gating is NOT applied here.  The flag is parsed and stored in
    // RunConfig but remains unused in S3 (needs Factory::mine + AlphaGate).

    // 8. Serialize admitted genomes.
    std::filesystem::create_directories(cfg.alpha_out);

    const atx::usize n = admitted.size();
    std::vector<std::string> dsl;
    dsl.reserve(n);

    for (atx::usize i = 0; i < n; ++i) {
        // Zero-padded 3-digit index: alpha_000.dsl, alpha_001.dsl, ...
        std::ostringstream name_ss;
        name_ss << "alpha_" << std::setw(3) << std::setfill('0') << i << ".dsl";
        const std::string dsl_path =
            (std::filesystem::path{cfg.alpha_out} / name_ss.str()).string();

        auto ws = write_genome(admitted[i], dsl_path);
        if (!ws.has_value()) {
            return atx::core::Err(ws.error());
        }

        dsl.push_back(alpha::unparse(admitted[i].ast));
    }

    // Write _manifest.txt (plain text, fixed order per brief §8).
    {
        const std::string manifest_path =
            (std::filesystem::path{cfg.alpha_out} / "_manifest.txt").string();
        std::ofstream mf{manifest_path};
        if (!mf.is_open()) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                  "discover: cannot write manifest: " + manifest_path);
        }
        mf << "seed="          << cfg.seed             << '\n';
        mf << "count="         << n                    << '\n';
        mf << "search_digest=" << to_hex16(res.digest) << '\n';
        mf << "panel="         << cfg.panel            << '\n';
    }

    // 9. Stage digest = fnv1a64 over ordered concatenation of DSL strings
    //    (joined with '\n').  Process-stable (unlike res.digest which is wyhash).
    std::string joined;
    for (atx::usize i = 0; i < dsl.size(); ++i) {
        if (i > 0) {
            joined += '\n';
        }
        joined += dsl[i];
    }
    const atx::u64 stage_digest = fnv1a64(joined.data(), joined.size());

    StageResult sr;
    sr.digest = stage_digest;
    sr.kvs = {
        {"admitted",      std::to_string(n)},
        {"trial_count",   std::to_string(res.trial_count)},
        {"candidates",    std::to_string(res.candidates_generated)},
        {"search_digest", to_hex16(res.digest)},
        {"population",    std::to_string(sc.population)},
        {"generations",   std::to_string(sc.generations)},
    };
    return atx::core::Ok(std::move(sr));
}

} // namespace atx::impl
