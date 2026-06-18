#include "stages.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/parser.hpp"          // alpha::Library, alpha::parse_expr
#include "atx/engine/alpha/unparse.hpp"          // alpha::unparse
#include "atx/engine/combine/store.hpp"          // combine::AlphaStore
#include "atx/engine/exec/execution_sim.hpp"     // exec::ExecutionSimulator + configs
#include "atx/engine/factory/genome.hpp"         // factory::Genome
#include "atx/engine/factory/search_driver.hpp"  // factory::SearchDriver, SearchConfig, SearchResult
#include "atx/engine/loop/weight_policy.hpp"     // engine::WeightPolicy

#include "artifacts.hpp"
#include "config.hpp"
#include "serialize_genome.hpp"
#include "serialize_panel.hpp"

namespace atx::impl {

namespace exec = atx::engine::exec;

// ---------------------------------------------------------------------------
// frictionless_sim — verbatim from factory_search_driver_test.cpp (brief §3)
// ---------------------------------------------------------------------------
static exec::ExecutionSimulator frictionless_sim() {
    return exec::ExecutionSimulator{
        exec::FillCfg{},
        exec::SlippageCfg{exec::SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
        exec::ImpactCfg{0.0, 0.5, 0.0},
        exec::CommissionCfg{exec::CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
        exec::LatencyCfg{},
        exec::VolumeCapCfg{1.0}};
}

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

    // 6. Run the evolutionary search.
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
