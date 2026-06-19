#pragma once

// atx::impl — sector_group_map: build a dense per-instrument sector group id
// vector from the research panel's "sector" field, for WeightPolicy industry
// (sector) neutralization. Deterministic: distinct non-NaN codes are assigned
// ascending dense ids by code value; all NaN/missing-sector names share one
// dedicated trailing group. Returns an empty vector when there is no "sector"
// field (caller treats empty as "neutralization off").

#include <cmath>
#include <limits>
#include <map>
#include <vector>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"

namespace atx::impl {

[[nodiscard]] inline std::vector<atx::u32>
sector_group_map(const atx::engine::alpha::Panel& panel) {
    const auto fid_r = panel.field_id("sector");
    if (!fid_r.has_value()) {
        return {};  // no sector field -> no neutralization
    }
    const atx::usize N = panel.instruments();
    const atx::usize D = panel.dates();

    // Representative sector code per instrument: first non-NaN cell scanning dates
    // ascending (sector membership is ~static; first observation is deterministic).
    std::vector<atx::f64> code(N, std::numeric_limits<atx::f64>::quiet_NaN());
    for (atx::usize i = 0; i < N; ++i) {
        for (atx::usize t = 0; t < D; ++t) {
            const atx::f64 v = panel.field_cross_section(*fid_r, t)[i];
            if (!std::isnan(v)) { code[i] = v; break; }
        }
    }

    // Dense id by ascending code value (std::map keeps keys sorted -> deterministic).
    std::map<atx::f64, atx::u32> code_to_id;
    for (atx::usize i = 0; i < N; ++i) {
        if (!std::isnan(code[i])) code_to_id.emplace(code[i], 0u);
    }
    atx::u32 next = 0;
    for (auto& kv : code_to_id) kv.second = next++;
    const atx::u32 nan_group = next;  // dedicated trailing group for NaN names

    std::vector<atx::u32> gm(N, 0u);
    for (atx::usize i = 0; i < N; ++i) {
        gm[i] = std::isnan(code[i]) ? nan_group : code_to_id[code[i]];
    }
    return gm;
}

} // namespace atx::impl
