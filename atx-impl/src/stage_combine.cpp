#include "stages.hpp"

#include <algorithm>   // std::sort
#include <filesystem>
#include <fstream>
#include <limits>      // std::numeric_limits
#include <span>        // std::span (per-alpha position rows)
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"       // alpha::compile_batch, alpha::Program
#include "atx/engine/alpha/panel.hpp"          // alpha::Panel, alpha::SignalSet
#include "atx/engine/alpha/parser.hpp"         // alpha::Library
#include "atx/engine/alpha/streams.hpp"        // alpha::extract_streams, alpha::AlphaStreams
#include "atx/engine/alpha/vm.hpp"             // alpha::Engine
#include "atx/engine/combine/combiner.hpp"     // combine::AlphaCombiner, CombinerConfig, CombineMethod, Combination
#include "atx/engine/combine/crowding.hpp"     // combine::decorrelate_weights, CrowdingConfig (9.2)
#include "atx/engine/combine/gate.hpp"         // combine::GateConfig (library open, 8.B)
#include "atx/engine/combine/metrics.hpp"      // combine::compute_metrics
#include "atx/engine/combine/store.hpp"        // combine::AlphaStore
#include "atx/engine/library/library.hpp"      // library::Library, AlphaId, AlphaRecordView (8.B)
#include "atx/engine/loop/weight_policy.hpp"   // engine::WeightPolicy

#include "artifacts.hpp"
#include "config.hpp"
#include "research_sim.hpp"
#include "sector_groups.hpp"
#include "serialize_panel.hpp"

namespace atx::impl {

namespace alpha   = atx::engine::alpha;
namespace combine = atx::engine::combine;
using atx::engine::WeightPolicy;

// ---------------------------------------------------------------------------
// method_from_string — map --method string to CombineMethod.
// ---------------------------------------------------------------------------
static atx::core::Result<combine::CombineMethod>
method_from_string(const std::string& s) {
    if (s.empty() || s == "shrinkage-mv") {
        return atx::core::Ok(combine::CombineMethod::ShrinkageMv);
    }
    if (s == "equal") {
        return atx::core::Ok(combine::CombineMethod::EqualWeight);
    }
    if (s == "rank") {
        return atx::core::Ok(combine::CombineMethod::RankAverage);
    }
    if (s == "ic") {
        return atx::core::Ok(combine::CombineMethod::IcWeighted);
    }
    if (s == "bounded") {
        return atx::core::Ok(combine::CombineMethod::BoundedRegression);
    }
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "combine: unknown --method '" + s + "'");
}

// ---------------------------------------------------------------------------
// run_combine
// ---------------------------------------------------------------------------
atx::core::Result<StageResult> run_combine(const RunConfig& cfg)
{
    // 1. Validate required flags. The alpha SOURCE is either the loose .dsl directory
    //    (--alphas) or, when --library-dir is set (8.B), the accumulated persistent
    //    library::Library. Exactly one source feeds the combine inputs; everything
    //    downstream (compile_batch + evaluate + the combine math) is identical.
    const bool from_library = !cfg.library_dir.empty();
    if (cfg.panel.empty() || cfg.combo_out.empty() ||
        (!from_library && cfg.alphas.empty())) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "combine: --panel, --combo-out, and one of "
                              "--alphas / --library-dir required");
    }

    // 2. Load the research panel.
    ATX_TRY(auto panel, read_panel(cfg.panel));

    // 3. Collect the alpha DSL sources + a per-alpha label (sidecar provenance).
    //    Two interchangeable sources; both yield `dsl` (expression strings, in a
    //    deterministic order) and `labels` (the weights-sidecar provenance string).
    std::vector<std::string> dsl;
    std::vector<std::string> labels;
    if (from_library) {
        // 3a (8.B). Library-backed input: enumerate ALL admitted records in AlphaId
        //     order (the same deterministic order discover writes alpha_NNN.dsl in)
        //     and use each record's stored expression source — the unparse'd DSL the
        //     library persisted on admit. Re-opening is read-only here (no admit), so
        //     the gate floors are irrelevant; a default GateConfig suffices. No seeds
        //     are needed for a pure enumeration (the corr index is not consulted).
        namespace library = atx::engine::library;
        std::filesystem::path lib_path{cfg.library_dir};
        if (!std::filesystem::exists(lib_path) ||
            !std::filesystem::is_directory(lib_path)) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                  "combine: --library-dir not found: " + cfg.library_dir);
        }
        library::Library liblib =
            library::Library::open(cfg.library_dir, combine::GateConfig{}, {});
        const atx::u64 n = liblib.n_alphas();
        if (n == 0) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                  "combine: --library-dir has no admitted alphas: " +
                                  cfg.library_dir);
        }
        dsl.reserve(static_cast<atx::usize>(n));
        labels.reserve(static_cast<atx::usize>(n));
        for (atx::u64 a = 0; a < n; ++a) {
            const auto rec = liblib.get(library::AlphaId{static_cast<atx::u32>(a)});
            dsl.push_back(rec.provenance.expr_source);
            labels.push_back("lib:" + cfg.library_dir + "#alpha_" + std::to_string(a));
        }
    } else {
        // 3b. Loose-.dsl input (backward compat): enumerate + sort .dsl files (sort for
        //     determinism), then read each, trimming trailing whitespace/newlines.
        std::vector<std::filesystem::path> dsl_paths;
        {
            std::filesystem::path alphas_dir{cfg.alphas};
            if (!std::filesystem::exists(alphas_dir) ||
                !std::filesystem::is_directory(alphas_dir)) {
                return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                      "combine: --alphas directory not found: " + cfg.alphas);
            }
            for (const auto& entry : std::filesystem::directory_iterator(alphas_dir)) {
                if (entry.path().extension() == ".dsl") {
                    dsl_paths.push_back(entry.path());
                }
            }
        }
        std::sort(dsl_paths.begin(), dsl_paths.end());

        if (dsl_paths.empty()) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                  "combine: no .dsl alphas found in --alphas dir");
        }

        dsl.reserve(dsl_paths.size());
        labels.reserve(dsl_paths.size());
        for (const auto& p : dsl_paths) {
            std::ifstream f{p};
            if (!f.is_open()) {
                return atx::core::Err(atx::core::ErrorCode::IoError,
                                      "combine: cannot open DSL file: " + p.string());
            }
            std::string contents{std::istreambuf_iterator<char>(f),
                                 std::istreambuf_iterator<char>()};
            // Trim trailing whitespace/newlines.
            while (!contents.empty() &&
                   (contents.back() == '\n' || contents.back() == '\r' ||
                    contents.back() == ' '  || contents.back() == '\t')) {
                contents.pop_back();
            }
            dsl.push_back(std::move(contents));
            labels.push_back(p.string());
        }
    }

    // 4. Compile all DSL sources as a multi-root batch Program.
    alpha::Library lib{};
    std::vector<std::string_view> views(dsl.begin(), dsl.end());
    ATX_TRY(auto program,
            alpha::compile_batch(std::span<const std::string_view>{views}, lib));

    // 5. Evaluate the batch program on the research panel.
    alpha::Engine engine{panel};
    ATX_TRY(auto signals, engine.evaluate(program));

    // 5b. Sector (industry) neutralization: per-alpha books are sector-demeaned so
    //     the mega-alpha expresses idiosyncratic views, not sector bets (WQ
    //     indneutralize). group_map empty (no "sector" field) -> neutralization off.
    std::vector<atx::u32> group_map;
    if (cfg.sector_neutral) {
        group_map = sector_group_map(panel);
    }

    // 6. Extract per-alpha PnL + position streams (sector-neutral when group_map set).
    WeightPolicy policy{};
    policy.industry_neutral = !group_map.empty();
    auto sim = frictionless_sim();
    ATX_TRY(auto streams,
            atx::engine::alpha::extract_streams(
                signals, policy, panel, sim,
                std::span<const atx::u32>{group_map}));

    // 7. Build the AlphaStore pool from real constituent streams.
    combine::AlphaStore pool;
    const atx::usize np = streams.n_periods();
    const atx::usize ni = streams.n_instruments();
    for (atx::usize a = 0; a < streams.n_alphas(); ++a) {
        std::vector<atx::f64> pos_flat;
        pos_flat.reserve(np * ni);
        for (atx::usize t = 0; t < np; ++t) {
            auto cs = streams.positions(a, t);
            pos_flat.insert(pos_flat.end(), cs.begin(), cs.end());
        }
        const auto m = combine::compute_metrics(
            streams.pnl(a), pos_flat, ni, /*book_size*/1.0);
        ATX_TRY(auto id, pool.insert(/*source*/nullptr, streams.pnl(a), pos_flat, m));
        (void)id;
    }

    // 8. Resolve method and fit window.
    ATX_TRY(auto cm, method_from_string(cfg.method));
    combine::AlphaCombiner combiner;
    combiner.cfg.method = cm;

    const atx::usize fit_begin =
        cfg.fit_begin > 0 ? static_cast<atx::usize>(cfg.fit_begin) : 0;
    const atx::usize fit_end =
        (cfg.fit_end > 0 && static_cast<atx::usize>(cfg.fit_end) <= np)
            ? static_cast<atx::usize>(cfg.fit_end) : np;

    ATX_TRY(auto combo, combiner.fit(pool, fit_begin, fit_end));

    // 8b. (9.2) Opt-in crowding de-correlation: a post-fit transform that shrinks
    //     each fitted weight by how mutually correlated its alpha is with the rest
    //     of the pool (corr_penalty) and, optionally, by how capacity-limited the
    //     name is (capacity_floor). Both knobs default 0.0 -> the engine's EXACT
    //     passthrough rail (corr_penalty==0 AND capacity_floor<=0 => weights returned
    //     bit-for-bit), so the no-flag combine output is byte-identical to today. We
    //     only call into decorrelate_weights when at least one knob is active; the
    //     default path never touches combo.weights. The transform reuses the SAME
    //     [fit_begin, fit_end) window the weights were fit on (the engine takes
    //     correlations over that PnL sub-span).
    if (cfg.corr_penalty > 0.0 || cfg.capacity_floor > 0.0) {
        // Capacity is caller-supplied per name; capacity scaling is OFF by default
        // (capacity_floor <= 0), in which case these values are UNUSED — fill with a
        // stable constant 1.0 so the vector is well-formed and deterministic. A
        // positive --capacity-floor would fade names in over [0, floor]; this stage
        // does not yet compute remaining capacity from a cost model, so a positive
        // floor with the constant 1.0 simply means every name is at/above the floor.
        std::vector<atx::f64> capacity(pool.size(), 1.0);
        combine::CrowdingConfig ccfg{};
        ccfg.corr_penalty   = cfg.corr_penalty;
        ccfg.capacity_floor = cfg.capacity_floor;
        combo.weights = combine::decorrelate_weights(
            combo.weights, pool, fit_begin, fit_end,
            std::span<const atx::f64>{capacity}, ccfg);
    }

    // 9. Build the combined mega-alpha matrix [dates * insts] from the per-alpha
    //    TARGET-WEIGHT (position) streams — the representation each alpha's
    //    metrics were validated on, and the engine's documented combiner input
    //    (streams.hpp: "the Phase-4 combiner ... consumes, per alpha, the
    //    position (target-weight) stream"). Each position row is winsorized,
    //    rank/zscore-transformed, dollar-neutralized and gross-normalized, so
    //    the alphas live on a COMPARABLE scale and enter in their validated
    //    profitable orientation. Averaging the RAW signals here (the prior bug)
    //    mixed incomparable scales (e.g. market_cap/close ~1e7 vs rank ~0..1) and
    //    applied the combiner weights — fit against the position/PnL streams — to
    //    a different, un-normalized book, inverting the realized portfolio sign
    //    relative to the per-alpha Sharpes. Combining positions makes the combo's
    //    PnL exactly Σ_a w_a·pnl_a, the stream the combiner optimized.
    //
    //    Dead cells: WeightPolicy emits 0.0 (not NaN) for out-of-universe / warmup
    //    names, so an alpha simply does not participate there. A cell is left NaN
    //    (no name) only when it is out of the panel universe for that date.
    const atx::usize D = panel.dates();
    const atx::usize N = panel.instruments();
    // 9.1 INVARIANT (positional AlphaId keying): the fitted weight vector is keyed
    // by AlphaId — combo.weights[a] is the weight of the alpha whose AlphaId is `a`.
    // By construction AlphaId `a` == streams row `a` == the step-3 dsl/labels index
    // `a`: step-7 inserts the pool in ascending `a` over streams.n_alphas(), and the
    // combiner returns one weight per pool alpha in that same AlphaId order. So the
    // blend below MUST apply combo.weights[a] to the stream whose AlphaId is `a`
    // (NOT to a directory-sort position or any other ordering). Assert the one fact
    // that makes the positional index sound: one weight per stream alpha. This is a
    // debug-only programmer-error guard; it changes NO numeric behavior on the
    // static path (the loop bounds and arithmetic are unchanged).
    ATX_ASSERT(combo.weights.size() == streams.n_alphas());
    std::vector<atx::f64> combined(D * N, std::numeric_limits<atx::f64>::quiet_NaN());
    std::vector<std::span<const atx::f64>> rows(combo.weights.size());
    for (atx::usize t = 0; t < D; ++t) {
        for (atx::usize a = 0; a < combo.weights.size(); ++a) {
            rows[a] = streams.positions(a, t);
        }
        for (atx::usize i = 0; i < N; ++i) {
            if (!panel.in_universe(t, i)) {
                continue; // leave NaN — not a tradable name on this date
            }
            atx::f64 acc = 0.0;
            for (atx::usize a = 0; a < combo.weights.size(); ++a) {
                acc += combo.weights[a] * rows[a][i];
            }
            combined[t * N + i] = acc;
        }
    }

    // 10. Serialize as a 1-field panel with the research panel's universe mask.
    std::vector<std::uint8_t> uni(D * N);
    for (atx::usize d = 0; d < D; ++d) {
        for (atx::usize i = 0; i < N; ++i) {
            uni[d * N + i] = panel.in_universe(d, i) ? 1 : 0;
        }
    }
    ATX_TRY(auto cpanel,
            alpha::Panel::create(D, N, {"alpha"}, {combined}, uni));
    ATX_TRY(auto digest, write_panel(cpanel, cfg.combo_out));

    // 11. Write weights sidecar.
    {
        const std::string sidecar_path = cfg.combo_out + ".weights.txt";
        std::ofstream wf{sidecar_path};
        if (!wf.is_open()) {
            return atx::core::Err(atx::core::ErrorCode::IoError,
                                  "combine: cannot write weights sidecar: " + sidecar_path);
        }
        const std::string method_str = cfg.method.empty() ? "shrinkage-mv" : cfg.method;
        wf << "method="     << method_str        << '\n';
        wf << "fit_begin="  << fit_begin         << '\n';
        wf << "fit_end="    << fit_end            << '\n';
        for (atx::usize a = 0; a < combo.weights.size(); ++a) {
            wf << "w[" << a << "]=" << combo.weights[a]
               << ' ' << labels[a] << '\n';
        }
    }

    // 12. Return StageResult.
    const std::string method_label = cfg.method.empty() ? "shrinkage-mv" : cfg.method;
    StageResult sr;
    sr.digest = digest;
    sr.kvs = {
        {"alphas",    std::to_string(dsl.size())},
        {"method",    method_label},
        {"fit_begin", std::to_string(fit_begin)},
        {"fit_end",   std::to_string(fit_end)},
        {"combo",     to_hex16(digest)},
    };
    return atx::core::Ok(std::move(sr));
}

} // namespace atx::impl
