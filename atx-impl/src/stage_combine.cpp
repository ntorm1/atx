#include "stages.hpp"

#include <algorithm>   // std::sort
#include <cmath>       // std::isnan
#include <filesystem>
#include <fstream>
#include <limits>      // std::numeric_limits
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
#include "atx/engine/combine/metrics.hpp"      // combine::compute_metrics
#include "atx/engine/combine/store.hpp"        // combine::AlphaStore
#include "atx/engine/loop/weight_policy.hpp"   // engine::WeightPolicy

#include "artifacts.hpp"
#include "config.hpp"
#include "research_sim.hpp"
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
    // 1. Validate required flags.
    if (cfg.panel.empty() || cfg.alphas.empty() || cfg.combo_out.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "combine: --panel, --alphas, and --combo-out required");
    }

    // 2. Load the research panel.
    ATX_TRY(auto panel, read_panel(cfg.panel));

    // 3. Enumerate and sort alpha .dsl files (sort for determinism).
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

    // Read DSL file contents, trim trailing whitespace/newlines.
    std::vector<std::string> dsl;
    dsl.reserve(dsl_paths.size());
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
    }

    // 4. Compile all DSL sources as a multi-root batch Program.
    alpha::Library lib{};
    std::vector<std::string_view> views(dsl.begin(), dsl.end());
    ATX_TRY(auto program,
            alpha::compile_batch(std::span<const std::string_view>{views}, lib));

    // 5. Evaluate the batch program on the research panel.
    alpha::Engine engine{panel};
    ATX_TRY(auto signals, engine.evaluate(program));

    // 6. Extract per-alpha PnL + position streams.
    WeightPolicy policy{};
    auto sim = frictionless_sim();
    ATX_TRY(auto streams,
            atx::engine::alpha::extract_streams(signals, policy, panel, sim));

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

    // 9. Build the combined mega-alpha matrix [dates * insts], NaN-aware.
    const atx::usize D = panel.dates();
    const atx::usize N = panel.instruments();
    std::vector<atx::f64> combined(D * N, std::numeric_limits<atx::f64>::quiet_NaN());
    for (atx::usize c = 0; c < D * N; ++c) {
        atx::f64 acc = 0.0;
        bool any = false;
        for (atx::usize a = 0; a < combo.weights.size(); ++a) {
            const atx::f64 s = signals.alphas[a].values[c];
            if (!std::isnan(s)) {
                acc += combo.weights[a] * s;
                any = true;
            }
        }
        combined[c] = any ? acc : std::numeric_limits<atx::f64>::quiet_NaN();
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
               << ' ' << dsl_paths[a].string() << '\n';
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
