#pragma once

// W0-I0a shared test fixtures (synthetic data only): research panels with an optional
// "future mutation" from a given date on, loose-DSL alpha directories, a permissive
// discover/combine RunConfig, and small readers for the stage sidecars. Header-only;
// every helper lives in namespace atx_test_w0_i0a_fixtures.

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

namespace atx_test_w0_i0a_fixtures {

namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;
using atx::f64;
using atx::usize;

inline constexpr usize kNever = std::numeric_limits<usize>::max();

// Deterministic LCG in [-1, 1).
struct Lcg {
    std::uint64_t s;
    [[nodiscard]] f64 next() noexcept {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint64_t hi = s >> 11U;
        return 2.0 * (static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U)) - 1.0;
    }
};

struct PanelSpec {
    usize dates = 160;
    usize insts = 12;
    std::uint64_t seed = 0x5EED1234ULL;
    // From this date on, prices follow a DIFFERENT random walk and volumes are scaled
    // (kNever = no mutation). Rows < mutate_from are bit-identical to the base panel.
    usize mutate_from = kNever;
    // Instrument `delist_inst` has NaN close/volume and is out of universe for dates
    // >= delist_from (kNever = none).
    usize delist_inst = kNever;
    usize delist_from = kNever;
    bool with_volume = true;
};

struct PanelColumns {
    std::vector<f64> close;
    std::vector<f64> volume;
    std::vector<f64> size; // constant per name (i + 1): a zero-turnover signal source
    std::vector<std::uint8_t> uni;
};

// close: a common shock + a per-name drift rising with the name index (so a book long
// high-index names -- rank(size) -- has a positive edge) + a predictable per-name sine +
// heteroskedastic noise; volume: a 30x log-spaced liquidity ladder with daily noise;
// size: i + 1.
[[nodiscard]] inline PanelColumns make_columns(const PanelSpec& spec) {
    PanelColumns c;
    c.close.assign(spec.dates * spec.insts, 0.0);
    c.volume.assign(spec.dates * spec.insts, 0.0);
    c.size.assign(spec.dates * spec.insts, 0.0);
    c.uni.assign(spec.dates * spec.insts, 1U);
    Lcg base{spec.seed};
    Lcg alt{spec.seed ^ 0x9E3779B97F4A7C15ULL};
    std::vector<f64> px(spec.insts, 100.0);
    for (usize t = 0; t < spec.dates; ++t) {
        const bool mutated = t >= spec.mutate_from;
        Lcg& rng = mutated ? alt : base;
        const f64 common = 0.004 * std::sin(0.23 * static_cast<f64>(t)) + 0.01 * rng.next();
        for (usize i = 0; i < spec.insts; ++i) {
            const f64 drift =
                0.008 * (static_cast<f64>(i) / static_cast<f64>(spec.insts - 1) - 0.5);
            const f64 edge = 0.003 * std::sin(0.41 * static_cast<f64>(t) + static_cast<f64>(i));
            // Heteroskedastic idiosyncratic noise (daily variance 3e-4 .. 1.2e-3, above the
            // diagonal risk model's 1e-4 floor) so the per-name risk lens is informative.
            const f64 idio = 0.03 * (1.0 + static_cast<f64>(i) / static_cast<f64>(spec.insts - 1));
            const f64 ret = common + drift + edge + idio * rng.next() + (mutated ? 0.004 : 0.0);
            if (t > 0) {
                px[i] *= (1.0 + ret);
            }
            const f64 lvl =
                5.0e4 * std::pow(30.0, static_cast<f64>(i) / static_cast<f64>(spec.insts - 1));
            const f64 vol = lvl * (1.0 + 0.3 * rng.next()) * (mutated ? 7.0 : 1.0);
            const bool dead = i == spec.delist_inst && t >= spec.delist_from;
            c.close[t * spec.insts + i] = dead ? std::numeric_limits<f64>::quiet_NaN() : px[i];
            c.volume[t * spec.insts + i] = dead ? std::numeric_limits<f64>::quiet_NaN() : vol;
            c.size[t * spec.insts + i] = static_cast<f64>(i + 1U);
            c.uni[t * spec.insts + i] = dead ? 0U : 1U;
        }
    }
    return c;
}

[[nodiscard]] inline atx::core::Result<std::string> write_research_panel(const fs::path& path,
                                                                         const PanelSpec& spec) {
    PanelColumns c = make_columns(spec);
    std::vector<std::string> names{"close", "size"};
    std::vector<std::vector<f64>> cols{c.close, c.size};
    if (spec.with_volume) {
        names.emplace_back("volume");
        cols.push_back(c.volume);
    }
    ATX_TRY(auto panel, alpha::Panel::create(spec.dates, spec.insts, std::move(names),
                                             std::move(cols), std::move(c.uni)));
    ATX_TRY(auto digest, atx::impl::write_panel(panel, path.string()));
    (void)digest;
    return atx::core::Ok(path.string());
}

inline void write_dsl_dir(const fs::path& dir, const std::vector<std::string>& exprs) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    for (usize i = 0; i < exprs.size(); ++i) {
        std::ostringstream name;
        name << "alpha_" << i << ".dsl";
        std::ofstream f{(dir / name.str()).string()};
        f << exprs[i] << '\n';
    }
}

[[nodiscard]] inline std::vector<std::string> default_exprs() {
    return {"rank(close)", "ts_mean(close,10)", "delta(close,2)", "rank(volume)"};
}

// A permissive discover/combine config: every admission floor open, small search.
[[nodiscard]] inline atx::impl::RunConfig permissive_cfg() {
    atx::impl::RunConfig cfg;
    cfg.allow_unidentified_panels = true; // synthetic legacy fixture: explicit diagnostic mode
    cfg.seed = 20260925ULL;
    cfg.population = 12;
    cfg.generations = 2;
    cfg.workers = 1;
    cfg.seed_exprs = {"rank(close)", "ts_mean(close, 5)", "delta(close, 2)"};
    cfg.min_sharpe = -1.0e9;
    cfg.min_fitness = -1.0e9;
    cfg.max_turnover = 1.0e9;
    cfg.max_pool_corr = 1.0;
    cfg.min_dsr = -1.0e9;
    cfg.gross = 1.0;
    cfg.name_cap = 0.5;
    cfg.rebalance = "weekly";
    return cfg;
}

[[nodiscard]] inline std::string find_kv(const atx::impl::StageResult& sr, const std::string& k) {
    for (const auto& [key, value] : sr.kvs) {
        if (key == k) {
            return value;
        }
    }
    return {};
}

// The "w[a]=..." lines of a combo weights sidecar, verbatim.
[[nodiscard]] inline std::vector<std::string> weight_lines(const std::string& combo) {
    std::vector<std::string> out;
    std::ifstream in{combo + ".weights.txt"};
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("w[", 0) == 0) {
            out.push_back(line);
        }
    }
    return out;
}

[[nodiscard]] inline std::vector<f64> weight_values(const std::string& combo) {
    std::vector<f64> out;
    for (const std::string& line : weight_lines(combo)) {
        const auto eq = line.find('=');
        const auto sp = line.find(' ', eq);
        out.push_back(std::stod(line.substr(eq + 1, sp - eq - 1)));
    }
    return out;
}

// Book rows (one per rebalance step) of a books panel's "weight" field.
[[nodiscard]] inline std::vector<std::vector<f64>> book_rows(const std::string& books) {
    std::vector<std::vector<f64>> rows;
    auto p = atx::impl::read_panel(books);
    if (!p.has_value()) {
        ADD_FAILURE() << "books panel must read: " << p.error().message();
        return rows;
    }
    auto fid = p->field_id("weight");
    if (!fid.has_value()) {
        ADD_FAILURE() << "books panel has no weight field";
        return rows;
    }
    for (usize s = 0; s < p->dates(); ++s) {
        const auto cs = p->field_cross_section(*fid, s);
        rows.emplace_back(cs.begin(), cs.end());
    }
    return rows;
}

// A fresh, empty scratch directory under the (per-process) temp root.
[[nodiscard]] inline fs::path fresh_dir(const std::string& tag) {
    const fs::path d = fs::temp_directory_path() / ("atx_w0i0a_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

} // namespace atx_test_w0_i0a_fixtures
