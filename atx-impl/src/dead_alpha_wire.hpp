#pragma once

// Shared dead-alpha crowding wire (p9 S1, extracted in S2-0 for metabook reuse).
// resolve/open/collect the accumulating library::Library that build_risk_model
// augments into Kakushadze-Yu dead-alpha crowding factors. Consumed by BOTH
// stage_optimize.cpp (S1, the run_optimize path) AND stage_metabook.cpp (S2, the
// run_metabook / mega-book path) -- the two build_risk_model sites the p9 ROADMAP
// R3/R4 names. Header-only inline: both TUs already pull library.hpp; the fail-open
// contract (nullptr/{} => byte-identical no-op) lives entirely in maybe_open_dead_lib.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "config.hpp" // atx::impl::RunConfig

#include "atx/engine/combine/gate.hpp"          // combine::GateConfig, AlphaId
#include "atx/engine/library/library.hpp"       // library::Library
#include "atx/engine/library/lifecycle.hpp"     // library::LifecycleState
#include "atx/engine/risk/factor_model.hpp"     // risk::RiskModelConfig

namespace atx::impl {

namespace combine = atx::engine::combine;
namespace library = atx::engine::library;
namespace risk    = atx::engine::risk;

// S1 (p9): resolve the on-disk library directory the dead-alpha crowding wire
// reads from. --dead-alpha-lib-dir wins when set; otherwise fall back to the
// discover stage's own accumulating --library-dir (the "library dir already in
// the pipeline" the p9 ROADMAP names as S1's source). Neither set -> "" -> the
// caller's fail-open no-op (maybe_open_dead_lib below).
[[nodiscard]] inline std::string resolve_dead_alpha_lib_dir(const RunConfig& cfg) {
    return !cfg.dead_alpha_lib_dir.empty() ? cfg.dead_alpha_lib_dir : cfg.library_dir;
}

// S1 (p9): open the accumulating library for the dead-alpha-factor wire, or
// return nullopt on any of three fail-open conditions: (1) the augmentation
// gate itself is off; (2) no directory resolves anywhere; (3) the resolved
// directory does not exist yet (an operator ran --dead-alpha-factors before any
// --library-dir discover run created it -- a MISSING library is a documented
// no-op, not a hard failure: crowding defense is a risk-reduction enhancement,
// never a run-blocking dependency -- mirrors build_risk_model's own dead_lib==
// nullptr contract, stage_riskmodel.hpp:120-126). GateConfig{} is inert here:
// this handle only ever calls the READ methods (n_alphas/n_periods/
// state_as_of); the gate floors matter only to admit()/try_admit(), never
// invoked on this path.
[[nodiscard]] inline std::optional<library::Library>
maybe_open_dead_lib(const RunConfig& cfg, const risk::RiskModelConfig& risk_cfg) {
    if (!risk_cfg.dead_alpha_factors) {
        return std::nullopt;
    }
    const std::string dir = resolve_dead_alpha_lib_dir(cfg);
    if (dir.empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        return std::nullopt;
    }
    return library::Library::open(dir, combine::GateConfig{}, {cfg.seed});
}

// S1 (p9): the "admitted dead-alpha pool" -- every AlphaId the library already
// admitted as of `dead_as_of`. The codebase today has NO driver that walks an
// alpha through Live->Decaying->Dead (grep of atx-impl/src for
// LifecycleState::Dead / .mark( is empty), so gating on LifecycleState::Dead
// literally would make this wire a permanent no-op against every real
// accumulating library. Until a future sprint adds that lifecycle-aging
// driver, "dead" here means "already admitted, not yet recycled":
// state_as_of(id, dead_as_of) NOT IN {Candidate, Recycled} -- Candidate
// excludes an id not yet admitted as of this PIT query (the state_as_of PIT
// contract, lifecycle.hpp:150-164); Recycled excludes a GC'd/reclaimed slot.
// Ascending AlphaId order by construction; NOT load-bearing for
// extract_dead_factors' own bit-reproducibility (it re-sorts internally,
// dead_factor.hpp:194-198) -- see the DeadIdOrderInvariant proof (S1-2).
[[nodiscard]] inline std::vector<combine::AlphaId>
collect_dead_alpha_ids(const library::Library& lib, atx::usize dead_as_of) {
    std::vector<combine::AlphaId> ids;
    const atx::u64 n = lib.n_alphas();
    ids.reserve(static_cast<atx::usize>(n));
    for (atx::u64 a = 0; a < n; ++a) {
        const combine::AlphaId id{static_cast<atx::u32>(a)};
        const auto st = lib.state_as_of(id, dead_as_of);
        if (st.has_value() && *st != library::LifecycleState::Candidate &&
            *st != library::LifecycleState::Recycled) {
            ids.push_back(id);
        }
    }
    return ids;
}

} // namespace atx::impl
