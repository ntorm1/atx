#include "stages.hpp"

#include <algorithm> // std::max (immigrant scaling)
#include <array>
#include <cmath>     // std::isfinite (W2 capacity screen; W4b PBO manifest gating); std::isnan (R3b oos_pbo)
#include <cstdio>    // std::fprintf (W2 screen log; W4b PBO gate warning)
#include <filesystem>
#include <limits>    // std::numeric_limits (W2 NaN vwap stub)
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include <thread>                                 // std::thread::hardware_concurrency

#include "atx/engine/alpha/datafields.hpp"        // alpha::datafields::with_datafields (W2)
#include "atx/engine/alpha/parser.hpp"          // alpha::Library, alpha::parse_expr
#include "atx/engine/alpha/unparse.hpp"          // alpha::unparse
#include "atx/engine/combine/gate.hpp"           // combine::AlphaGate, GateConfig
#include "atx/engine/combine/store.hpp"          // combine::AlphaStore
#include "atx/engine/eval/lockbox.hpp"           // eval::reserve_lockbox, eval::detail::embargo_len_from_cpcv (P2b guard)
#include "atx/engine/eval/cpcv.hpp"             // eval::CpcvConfig (default embargo)
#include "atx/engine/exec/execution_sim.hpp"     // exec::ExecutionSimulator + configs
#include "atx/engine/factory/factory.hpp"        // factory::Factory, FactoryConfig, FactoryReport
#include "atx/engine/factory/genome.hpp"         // factory::Genome
#include "atx/engine/factory/search_driver.hpp"  // factory::SearchDriver, SearchConfig, SearchResult
#include "atx/engine/library/library.hpp"        // library::Library, AlphaId, AlphaRecordView
#include "atx/engine/loop/weight_policy.hpp"     // engine::WeightPolicy

#include "atx/engine/store/db.hpp"               // store::StoreDb (resumable-discover, Task 7)
#include "atx/engine/store/pipeline_progress.hpp" // store::PipelineRecorder, PipelineRunRow, ResumableRun, split_population

#include "artifacts.hpp"
#include "config.hpp"
#include "research_sim.hpp"
#include "serialize_genome.hpp"
#include "serialize_panel.hpp"
#include "stage_discover_detail.hpp"             // atx::impl::detail::apply_capacity_screen (Fix 1: testable)
#include "store_progress_sink.hpp"               // StoreProgressSink, compute_discover_fingerprint, fp_hex, now_unix

namespace atx::impl {

namespace exec = atx::engine::exec;

// ---------------------------------------------------------------------------
// parse_weight_transform (W1a) — map the --weight-transform string to the
// engine's closed Transform taxonomy. The CLI parser already lowercased and
// validated the value against {rank,zscore,raw}, so the final `else` (raw) is
// the only remaining case; an unexpected value here is a programming error and
// is mapped to the Rank default rather than aborting. "rank" -> Rank reproduces
// engine::WeightPolicy{} exactly (the byte-identical default path).
// ---------------------------------------------------------------------------
namespace {
[[nodiscard]] atx::engine::Transform parse_weight_transform(const std::string& s) {
    using T = atx::engine::Transform;
    if (s == "zscore") {
        return T::ZScore;
    }
    if (s == "raw") {
        return T::Raw;
    }
    return T::Rank; // "rank" (and the validated default) -> Rank
}

} // namespace

// ---------------------------------------------------------------------------
// apply_capacity_screen (W2) — build a derived Panel whose universe_ mask is
// original_universe ∧ (close > min_price) ∧ (adv{W} >= min_adv).
//
// Design notes (see W2 brief):
//   * NaN propagation then filters illiquid names across the ENTIRE eval chain
//     with ZERO changes to streams/fitness/factory/search_driver.
//   * ADV is computed over the ORIGINAL universe (not the screened one) so the
//     rolling mean reflects the same liquid set used for capacity validation.
//   * Fail-closed: missing 'close' or 'volume' field => Err(InvalidArgument).
//   * Fix 3: out-of-range adv_window (< 1 or > 65535) => Err(InvalidArgument).
//   * Fix 2: post-screen kept-cell count == 0 => Err(InvalidArgument).
//   * Predicate mirrors single_alpha_capacity_test.cpp:capacity_universe exactly.
//
// Declared in stage_discover_detail.hpp so discover_test.cpp can call the real
// implementation directly (Fix 1: testable helper in named namespace).
// ---------------------------------------------------------------------------
atx::core::Result<atx::engine::alpha::Panel>
atx::impl::detail::apply_capacity_screen(const atx::engine::alpha::Panel& panel,
                                         atx::f64 min_price, atx::f64 min_adv,
                                         long adv_window) {
    namespace alpha = atx::engine::alpha;
    namespace df    = atx::engine::alpha::datafields;
    using EC        = atx::core::ErrorCode;

    const atx::usize D = panel.dates();
    const atx::usize I = panel.instruments();

    // Step 1: extract field names + data + universe from the original panel.
    std::vector<std::string>             field_names;
    std::vector<std::vector<atx::f64>>   field_data;
    field_names.reserve(panel.num_fields());
    field_data.reserve(panel.num_fields());
    for (atx::usize fi = 0; fi < panel.num_fields(); ++fi) {
        field_names.push_back(std::string{panel.field_name(fi)});
        const auto col = panel.field_all(static_cast<alpha::FieldId>(fi));
        field_data.emplace_back(col.begin(), col.end());
    }

    // Collect original universe (date-major, 1 == in-universe).
    std::vector<std::uint8_t> orig_univ(D * I);
    for (atx::usize d = 0; d < D; ++d) {
        for (atx::usize i = 0; i < I; ++i) {
            orig_univ[d * I + i] = panel.in_universe(d, i) ? 1u : 0u;
        }
    }

    // Fail-closed: 'close' and 'volume' must be present before calling
    // with_datafields (which also checks, but we want a clear message here).
    bool has_close  = false;
    bool has_volume = false;
    for (const std::string& n : field_names) {
        if (n == "close")  has_close  = true;
        if (n == "volume") has_volume = true;
    }
    if (!has_close) {
        return atx::core::Err(EC::InvalidArgument,
            "discover capacity screen: panel is missing required field 'close'");
    }
    if (!has_volume) {
        return atx::core::Err(EC::InvalidArgument,
            "discover capacity screen: panel is missing required field 'volume'");
    }

    // Fix 3: out-of-range adv_window => Err when screen is active (called only
    // when capacity_on). Silent clamp-to-20 is replaced by a hard failure so
    // a misconfigured window does not produce silently wrong ADV averages.
    if (adv_window < 1 || adv_window > 0xFFFF) {
        return atx::core::Err(EC::InvalidArgument,
            "discover capacity screen: adv_window=" + std::to_string(adv_window) +
            " is out of valid range [1, 65535]");
    }
    const atx::u16 win = static_cast<atx::u16>(adv_window);

    // with_datafields derives vwap when absent, requiring high/low. We only need
    // adv{W} for the screen, so if vwap/high/low are ALL absent, pre-supply a NaN
    // vwap column to short-circuit the derivation (NaN vwap has no effect on adv).
    {
        bool has_vwap = false, has_high = false, has_low = false;
        for (const std::string& n : field_names) {
            if (n == "vwap")  has_vwap = true;
            if (n == "high")  has_high = true;
            if (n == "low")   has_low  = true;
        }
        if (!has_vwap && (!has_high || !has_low)) {
            field_names.emplace_back("vwap");
            field_data.emplace_back(D * I,
                                    std::numeric_limits<atx::f64>::quiet_NaN());
        }
    }

    const std::array<atx::u16, 1> adv_wins = {win};
    ATX_TRY(auto aug,
            df::with_datafields(D, I, field_names, field_data, orig_univ,
                                std::span<const atx::u16>{adv_wins}));

    // Resolve field ids in the augmented panel.
    ATX_TRY(const auto close_id, aug.field_id("close"));
    const std::string adv_name = std::string{df::kAdvPrefix} + std::to_string(win);
    ATX_TRY(const auto adv_id,   aug.field_id(adv_name));

    const auto close_col = aug.field_all(close_id);
    const auto adv_col   = aug.field_all(adv_id);

    // Step 3: compute the capacity mask (same predicate as capacity_universe in
    // single_alpha_capacity_test.cpp:127-155).
    std::vector<std::uint8_t> cap_univ(D * I, 0);
    atx::usize kept_cells = 0;
    for (atx::usize d = 0; d < D; ++d) {
        for (atx::usize i = 0; i < I; ++i) {
            const atx::usize idx = d * I + i;
            if (!aug.in_universe(d, i)) {
                continue; // out of original universe -> stays out
            }
            const atx::f64 px = close_col[idx];
            const atx::f64 dv = adv_col[idx];
            if (std::isfinite(px) && px > min_price &&
                std::isfinite(dv) && dv >= min_adv) {
                cap_univ[idx] = 1;
                ++kept_cells;
            }
        }
    }

    // Fix 2: fail-closed zero-universe guard — when the screen is active and
    // every cell is excluded, the run would produce zero signal (e.g. adv_window
    // >= dates means all ADV cells are NaN, or thresholds are too strict). Return
    // an actionable error rather than silently producing an all-NaN signal set.
    if (kept_cells == 0) {
        return atx::core::Err(EC::InvalidArgument,
            "discover capacity screen: retained zero in-universe cells "
            "(adv_window too large for panel, or thresholds too strict). "
            "Reduce --adv-window, lower --min-adv / --min-price, or use a longer panel.");
    }

    // Approximate names/day for logging (kept_cells / D, rounded).
    const atx::f64 names_per_day =
        (D > 0) ? (static_cast<atx::f64>(kept_cells) / static_cast<atx::f64>(D)) : 0.0;

    // Emit a brief log line so callers can verify the acceptance criterion
    // (~1,000–1,200 names/day with min_price=$1 / min_adv=$50M on the liquid panel).
    // Use stderr so it doesn't pollute stdout digests.
    (void)std::fprintf(stderr,
        "[W2 capacity screen] min_price=%.2f min_adv=%.0f adv_window=%d  "
        "kept_cells=%zu  ~%.0f names/day\n",
        min_price, min_adv, static_cast<int>(win),
        kept_cells, names_per_day);

    // Step 4: rebuild a Panel with the augmented columns + new universe mask.
    std::vector<std::string>           aug_names;
    std::vector<std::vector<atx::f64>> aug_data;
    aug_names.reserve(aug.num_fields());
    aug_data.reserve(aug.num_fields());
    for (atx::usize fi = 0; fi < aug.num_fields(); ++fi) {
        aug_names.push_back(std::string{aug.field_name(fi)});
        const auto c = aug.field_all(static_cast<alpha::FieldId>(fi));
        aug_data.emplace_back(c.begin(), c.end());
    }
    return alpha::Panel::create(D, I,
                                std::move(aug_names),
                                std::move(aug_data),
                                std::move(cap_univ));
}

// ---------------------------------------------------------------------------
// mean_names_per_day (W5) — mean in-universe instrument count per date over `panel`.
//
// = (sum over all (date, inst) of in_universe) / dates(); 0.0 when dates() == 0. PURE:
// reads only the universe mask. When `panel` is the post-capacity-screen panel this is
// the SAME quantity apply_capacity_screen logs as `names_per_day` (kept_cells / D) by
// construction (identical mask) — recorded here as a durable admission metric (W5) since
// the screen only emits it to stderr. Declared in stage_discover_detail.hpp so the test
// can call it directly on a hand-built mask.
// ---------------------------------------------------------------------------
double atx::impl::detail::mean_names_per_day(const atx::engine::alpha::Panel& panel) {
    const atx::usize D = panel.dates();
    const atx::usize I = panel.instruments();
    if (D == 0) {
        return 0.0;
    }
    atx::usize in_univ = 0;
    for (atx::usize d = 0; d < D; ++d) {
        for (atx::usize i = 0; i < I; ++i) {
            if (panel.in_universe(d, i)) {
                ++in_univ;
            }
        }
    }
    return static_cast<double>(in_univ) / static_cast<double>(D);
}

// ---------------------------------------------------------------------------
// build_robust_holdout_panel (W4a) — the §0.8 weak/holdout sub-universe Panel.
//
// A derived Panel with the SAME field columns but its universe restricted to a
// DETERMINISTIC seeded instrument sub-sample: instrument i stays in-universe iff it
// was in the original universe AND a SplitMix64 mix of (master_seed, i) maps into the
// leading `frac` of [0, 1). Seeded by master_seed ONLY (never thread/time/address),
// so the same seed + frac + panel always yields the same weak universe (F1, seed-
// stable). Fail-closed on an out-of-range frac or a zero-retained sub-sample.
// ---------------------------------------------------------------------------
atx::core::Result<atx::engine::alpha::Panel>
atx::impl::detail::build_robust_holdout_panel(const atx::engine::alpha::Panel& panel,
                                              atx::f64 frac, atx::u64 master_seed) {
    namespace alpha = atx::engine::alpha;
    using EC        = atx::core::ErrorCode;

    if (!(frac > 0.0) || !(frac < 1.0)) {
        return atx::core::Err(EC::InvalidArgument,
            "discover robust holdout: --robust-holdout-frac=" + std::to_string(frac) +
            " must be in the open interval (0, 1)");
    }

    const atx::usize D = panel.dates();
    const atx::usize I = panel.instruments();

    // Deterministic per-instrument selection: SplitMix64 mix of (master_seed, i) ->
    // a u64; SELECTED iff (u64 / 2^64) < frac. Pure / portable / seed-stable.
    auto mix = [](atx::u64 x) noexcept -> atx::u64 {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31U);
    };
    const atx::f64 kU64Scale = 1.0 / 18446744073709551616.0; // 1 / 2^64
    std::vector<std::uint8_t> selected(I, 0u);
    for (atx::usize i = 0; i < I; ++i) {
        const atx::u64 h = mix(master_seed ^ mix(static_cast<atx::u64>(i) + 0x632BE59BD9B4E019ULL));
        const atx::f64 u = static_cast<atx::f64>(h) * kU64Scale; // [0, 1)
        selected[i] = (u < frac) ? 1u : 0u;
    }

    // Extract field names + columns (verbatim) from the source panel.
    std::vector<std::string>           field_names;
    std::vector<std::vector<atx::f64>> field_data;
    field_names.reserve(panel.num_fields());
    field_data.reserve(panel.num_fields());
    for (atx::usize fi = 0; fi < panel.num_fields(); ++fi) {
        field_names.push_back(std::string{panel.field_name(fi)});
        const auto col = panel.field_all(static_cast<alpha::FieldId>(fi));
        field_data.emplace_back(col.begin(), col.end());
    }

    // New universe mask: original membership AND the instrument is selected.
    std::vector<std::uint8_t> weak_univ(D * I, 0u);
    atx::usize kept_cells = 0;
    for (atx::usize d = 0; d < D; ++d) {
        for (atx::usize i = 0; i < I; ++i) {
            if (selected[i] != 0u && panel.in_universe(d, i)) {
                weak_univ[d * I + i] = 1u;
                ++kept_cells;
            }
        }
    }

    if (kept_cells == 0) {
        return atx::core::Err(EC::InvalidArgument,
            "discover robust holdout: the seeded sub-sample retained zero in-universe "
            "cells (frac too small for this universe). Increase --robust-holdout-frac.");
    }

    // (Fix 2) No unconditional stderr diagnostic here: build_robust_holdout_panel has
    // no verbosity/log seam threaded in (it takes only panel/frac/master_seed), so an
    // always-on "[W4a robust holdout] ..." line was unguarded noise. Removed rather
    // than widen the signature for a single trace line; kept_cells is still validated
    // (fail-closed above) and surfaced via the error path when it would be zero.

    return alpha::Panel::create(D, I, std::move(field_names), std::move(field_data),
                                std::move(weak_univ));
}

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
    const std::vector<std::string>& fields,
    const atx::engine::alpha::Panel* weak_panel, // W4a: §0.8 weak sub-universe (nullptr = off)
    const std::vector<std::string>& numeric_excluded_fields, // R1: typed-fields exclusion list
    const std::vector<std::string>& extra_group_fields)      // R1: typed-fields extra group list
{
    namespace fs      = std::filesystem;
    namespace combine = atx::engine::combine;
    namespace factory = atx::engine::factory;
    namespace library = atx::engine::library;

    // Library directory selection (8.A):
    //  * --library-dir SET  -> a STABLE library that ACCUMULATES across runs/seeds.
    //    The dir is re-opened (NOT wiped), so admitted alphas persist; library::admit
    //    dedups by canonical hash + journals, so re-admitting an identical alpha is a
    //    Duplicate (not double-counted). This is the mega-alpha unlock.
    //  * --library-dir UNSET -> TODAY's behavior EXACTLY: a fresh per-run library under
    //    <alpha_out>/_library, WIPED each run (deterministic same-seed => same DB, no
    //    stale carry-over). This keeps single-run determinism/resume goldens byte-
    //    identical — accumulation is strictly opt-in.
    const bool accumulate = !cfg.library_dir.empty();

    // R3a — OOS-on-by-default for accumulation runs. When --library-dir is set
    // (accumulate == true) AND the user did NOT explicitly supply --oos-fraction
    // (set_flags.count("oos-fraction") == 0), default oos_fraction to 0.25 so
    // every accumulation run validates on a real holdout by default. An explicit
    // --oos-fraction (any value, including 0) overrides this default verbatim.
    // The non-accumulation path is UNTOUCHED: without --library-dir, accumulate ==
    // false and we never enter this branch, so oos_fraction stays 0.0 and the
    // legacy path is byte-identical.
    // NOTE: cfg is const& from the caller, so we thread the effective fraction as
    // a local variable (eff_oos_fraction) into fcfg.oos_fraction below.
    const atx::f64 eff_oos_fraction =
        (accumulate && cfg.set_flags.count("oos-fraction") == 0)
            ? 0.25
            : cfg.oos_fraction;

    const std::string lib_dir =
        accumulate ? cfg.library_dir : (fs::path{cfg.alpha_out} / "_library").string();
    std::error_code ec;
    if (!accumulate) {
        fs::remove_all(lib_dir, ec); // per-run wipe ONLY for the default (non-accumulating) dir
    }
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
    fcfg.min_split_sharpe          = cfg.min_split_sharpe;       // W4a split-sample stability floor (off by default)
    fcfg.max_pbo                   = cfg.max_pbo;                 // W4b run-level CSCV-PBO batch gate (off by default = 1.0)
    fcfg.max_price_scale_corr      = cfg.max_price_scale_corr;   // R2 price-scale gate (off by default = 1.0)
    fcfg.dsr_subwindows = static_cast<atx::usize>(std::max<int>(cfg.dsr_subwindows, 0)); // R3 intra-holdout DSR sub-windows (off by default = 0)
    fcfg.search.deflate_selection  = cfg.deflate_selection; // R4: opt-in deflated-Sharpe search selection
    fcfg.oos_fraction              = eff_oos_fraction; // R3a: use the effective fraction (auto-default 0.25 for accumulation)
    fcfg.oos_embargo               = cfg.oos_embargo;
    fcfg.oos_n_windows = static_cast<atx::usize>(std::max<long>(cfg.oos_windows, 0));
    fcfg.oos_window    = static_cast<atx::usize>(std::max<long>(cfg.oos_window,  0));

    // W4a robust factor (Deliverable 2): thread the caller-owned weak/holdout sub-
    // universe Panel (built once in run_discover from sc.master_seed) into the Factory
    // so robust = clamp(wq_on(weak)/wq, 0, 1) ACTIVATES. nullptr (default) -> robust
    // stays the constant 1.0 and the discover digest is byte-identical to today.
    fcfg.weak_panel = weak_panel; // borrowed; the pointee outlives every mine_into below
    // R1 typed-fields: empty lists (the default) keep SearchDriver's partition identical
    // to pre-R1 -> byte-identical digest. Non-empty lists tighten the numeric leaf pool.
    fcfg.numeric_excluded_fields = numeric_excluded_fields;
    fcfg.extra_group_fields      = extra_group_fields;

    // P2b: pre-validation guard — when OOS is on, check the panel geometry NOW
    // (before mine_into) so a too-short panel or too-large fraction fails LOUDLY
    // rather than silently admitting zero alphas.  Reuses the ENGINE's own geometry
    // helper (eval::detail::embargo_len_from_cpcv + eval::reserve_lockbox) so the
    // guard accepts exactly what the engine accepts and rejects exactly what it
    // would silently no-op on.
    // Uses eff_oos_fraction (the R3a effective value) so the guard validates the
    // auto-default 0.25 fraction on accumulation runs.
    if (eff_oos_fraction > 0.0) {
        namespace eval = atx::engine::eval;
        const atx::usize T = panel.dates();
        const atx::usize embargo_len =
            (cfg.oos_embargo > 0.0)
                ? eval::detail::embargo_len_from_cpcv(cfg.oos_embargo, T)
                : eval::detail::embargo_len_from_cpcv(eval::CpcvConfig{}.embargo, T);
        auto sealed_r = eval::reserve_lockbox(panel, eff_oos_fraction, embargo_len);
        if (!sealed_r.has_value()) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                "discover: --oos-fraction " + std::to_string(eff_oos_fraction) +
                " leaves too little train/holdout for a panel of " +
                std::to_string(panel.dates()) + " dates (" +
                sealed_r.error().message() + ")");
        }
    }

    // Mine -> deflate -> gate -> admit into the persistent library.
    factory::Factory fac{lib, panel, sim, policy};
    library::Library liblib = library::Library::open(lib_dir, gc, {cfg.seed});

    // ---- Resumable-discover wiring (Task 7) -------------------------------
    // When --run-db is set, open the progress DB, resume-or-begin a pipeline_run,
    // and drive a store-backed sink through mine_into so each completed generation
    // is checkpointed. When --run-db is EMPTY, NONE of this runs: no DB is opened,
    // no sink is built, and mine_into is called with (nullptr, nullptr) — the SAME
    // overload as the legacy 3-arg call (defaulted params), proven byte-identical at
    // the factory level. The downstream manifest/.dsl writing below is SHARED and
    // unchanged across both paths.
    namespace store = atx::engine::store;
    atx::engine::factory::SearchProgressSink*       sink_ptr   = nullptr;
    const atx::engine::factory::SearchResumeState*  resume_ptr = nullptr;
    std::optional<store::StoreDb>                      sdb;
    std::optional<store::PipelineRecorder>             rec;
    std::optional<StoreProgressSink>                   sink;
    std::optional<atx::engine::factory::SearchResumeState> rs;

    if (!cfg.run_db.empty()) {
        ATX_TRY(auto opened, store::StoreDb::open(cfg.run_db));
        sdb.emplace(std::move(opened));
        const atx::u64 fp = compute_discover_fingerprint(cfg);
        bool resumed = false;
        if (cfg.resume) {
            // find_resumable returns Err(NotFound) when no open run exists — that
            // is the "begin fresh" case, NOT a hard failure, so it is inspected
            // (not ATX_TRY'd).
            auto found = store::PipelineRecorder::find_resumable(sdb->db(), fp);
            if (found.has_value() && found->last_generation >= 0) {
                ATX_TRY(auto r, store::PipelineRecorder::resume(
                                    sdb->db(), found->pipeline_run_id, now_unix()));
                rec.emplace(std::move(r));
                // Task F1: load the FULL checkpoint (population + accumulated state),
                // not just the population blob, so the resumed search restores canon /
                // fitness_cache / behavior_archive / digest / counters byte-identically.
                // latest_checkpoint verifies the whole-payload state_hash (corrupt =>
                // Err, which ATX_TRY propagates — never a silent partial restore).
                ATX_TRY(auto cp, rec->latest_checkpoint());
                rs.emplace();
                rs->start_generation     = static_cast<atx::usize>(found->last_generation);
                rs->population           = store::split_population(cp.population_blob);
                rs->canon_blob           = std::move(cp.state.canon_blob);
                rs->cache_blob           = std::move(cp.state.cache_blob);
                rs->archive_blob         = std::move(cp.state.archive_blob);
                rs->best_per_gen_blob    = std::move(cp.state.best_per_gen_blob);
                rs->digest               = cp.state.digest;
                rs->candidates_generated = static_cast<atx::usize>(cp.state.candidates_generated);
                resume_ptr = &*rs;
                resumed = true;
            }
        }
        if (!resumed) {
            store::PipelineRunRow row;
            row.pipeline_run_id   = fp_hex(fp);
            row.fingerprint       = fp;
            row.stage             = "discover";
            row.master_seed       = static_cast<atx::u64>(cfg.seed);
            row.population        = static_cast<atx::i64>(cfg.population);
            row.total_generations = static_cast<atx::i64>(cfg.generations);
            row.panel_path        = cfg.panel;
            row.config_json       = "";
            row.engine_git_sha    = "";
            row.created_at        = now_unix();
            ATX_TRY(auto r, store::PipelineRecorder::begin(sdb->db(), row));
            rec.emplace(std::move(r));
        }
        sink.emplace(*rec);
        sink_ptr = &*sink;
    }

    // mine_into now returns a Result: a cross-run --library-dir geometry MISMATCH
    // (a reopened library whose fixed period count differs from this run's holdout
    // length) surfaces here as a CLEAN propagated error instead of an ATX_ASSERT abort
    // (debug) / out-of-bounds projection read (release). Mark the run failed and
    // propagate so the CLI prints a clear message rather than crashing/corrupting.
    auto rep_r = fac.mine_into(fcfg, liblib, gate, sink_ptr, resume_ptr);
    if (!rep_r.has_value()) {
        if (rec) { (void)rec->mark_failed(now_unix(), rep_r.error().to_string()); }
        return atx::core::Err(rep_r.error());
    }
    const factory::FactoryReport rep = std::move(*rep_r);
    if (rec) { (void)rec->complete(now_unix()); }

    {
        auto fr = liblib.flush_all();
        if (!fr.has_value()) {
            return atx::core::Err(fr.error());
        }
    }

    // R1: write the durable cumulative-trials sidecar (_manifest.bin) so that
    // a subsequent cross-process `discover --library-dir` run restores the
    // cumulative_trials_ counter from disk. flush_all() above seals the store
    // but does NOT write the sidecar; snapshot() does both (flush + sidecar
    // write). Sidecar write failures are best-effort and silently ignored by
    // snapshot() — worst case: the next run starts at 0, which is the pre-R1
    // single-run baseline (acceptable per the R1 brief).
    (void)liblib.snapshot();

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
        // P2b: build a lookup map from canon_hash -> OosReportEntry for IS/OOS column
        // emission.  Built once; empty when oos_fraction == 0 (legacy path).
        std::unordered_map<atx::u64, factory::OosReportEntry> oos_map;
        for (const auto& entry : rep.oos_metrics) {
            oos_map[entry.canon_hash] = entry;
        }

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
        // W5: capacity-universe size as a RECORDED admission metric — emitted ONLY when
        // the W2 capacity screen was active (capacity_on), gated so the OFF-path manifest
        // is byte-identical (mirrors the OOS / W4b-PBO emit-only-when-active discipline).
        // `panel` here is the post-screen panel, so mean_names_per_day equals the screen's
        // stderr names/day by construction. The turnover bar itself (--max-turnover, gate
        // max_turnover= above) is the other half of the W5 capacity gate.
        {
            const bool capacity_on = cfg.min_adv_usd > 0.0 || cfg.min_price > 0.0;
            if (capacity_on) {
                mf << "capacity_min_price="   << cfg.min_price
                   << " capacity_min_adv="    << cfg.min_adv_usd
                   << " capacity_adv_window=" << cfg.adv_window
                   << " capacity_names_per_day=" << detail::mean_names_per_day(panel)
                   << '\n';
            }
        }
        // P2b/R3a: OOS header lines (only when OOS is active; off-path manifest byte-identical).
        // Uses eff_oos_fraction (R3a: the auto-default 0.25 for accumulation runs if not
        // explicitly set; cfg.oos_fraction for explicit overrides; 0.0 for non-accumulation).
        if (eff_oos_fraction > 0.0) {
            mf << "oos_fraction="    << eff_oos_fraction    << '\n';
            mf << "oos_embargo="     << cfg.oos_embargo     << '\n';
            // R3b: run-level CSCV PBO (NaN when < 2 admitted or too short; toolchain-
            // stable: print "nan" explicitly to avoid "nan"/-nan/-NaN variance).
            if (std::isnan(rep.oos_pbo)) {
                mf << "oos_pbo=nan\n";
            } else {
                mf << "oos_pbo="  << rep.oos_pbo  << '\n';
            }
        }
        // W4b/A3: run-level CSCV-PBO gate-context line — emitted ONLY when the gate is
        // ACTIVELY enabled (cfg.max_pbo < 1.0) AND the PBO was computed/feasible. A3:
        // the OOS paths now compute rep.pbo even at the 1.0 default (always_compute, for
        // the oos_pbo alias), so std::isfinite(rep.pbo) alone would emit this line — and
        // duplicate the oos_pbo= value — on every default OOS run. The cfg.max_pbo < 1.0
        // guard keeps the default manifest minimal: the always-on diagnostic is the
        // oos_pbo= line above; this line is the gate-context line only when the gate is on.
        if (std::isfinite(rep.pbo) && cfg.max_pbo < 1.0) {
            mf << "run_pbo="          << rep.pbo
               << " pbo_gate="        << (rep.pbo_gate_passed ? "pass" : "FAIL")
               << " pbo_n_candidates="<< rep.pbo_n_candidates
               << " pbo_n_splits="    << rep.pbo_n_splits
               << " max_pbo="         << cfg.max_pbo
               << '\n';
        }
        mf << "panel="           << cfg.panel            << '\n';
        for (atx::u64 a = 0; a < n; ++a) {
            const auto rec = liblib.get(library::AlphaId{static_cast<atx::u32>(a)});
            mf << "alpha[" << a << "]"
               << " sharpe="   << rec.metrics.sharpe
               << " fitness="  << rec.metrics.fitness
               << " turnover=" << rec.metrics.turnover
               << " returns="  << rec.metrics.returns
               << " drawdown=" << rec.metrics.drawdown;
            // P2b: IS/OOS columns (only when OOS is active; off-path byte-identical)
            if (eff_oos_fraction > 0.0) {
                const auto it = oos_map.find(rec.canon_hash);
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
            mf << " : " << rec.provenance.expr_source << '\n';
        }
    }

    // W4b: ADVISORY-but-RECORDED gate. When the run-level PBO was computed and BREACHED
    // the threshold, emit a LOUD stderr warning naming the PBO + the bar. The gate does
    // NOT change the process exit code and does NOT un-persist alphas (architecturally
    // impossible — PBO is a post-hoc property of the already-grown admitted set); the
    // recorded verdict (manifest run_pbo / pbo_gate) + this warning ARE the gate. stderr
    // (not stdout) so it never pollutes the stage/factory digests.
    if (std::isfinite(rep.pbo) && cfg.max_pbo < 1.0 && !rep.pbo_gate_passed) {
        (void)std::fprintf(stderr,
            "[W4b PBO gate] WARNING: run-level CSCV-PBO=%.4f EXCEEDS --max-pbo=%.4f over "
            "%zu admitted alpha(s) (n_splits=%zu). The admitted SET shows backtest-overfit "
            "risk; the alphas remain persisted (advisory gate) — review before trading.\n",
            rep.pbo, cfg.max_pbo,
            static_cast<std::size_t>(rep.pbo_n_candidates),
            static_cast<std::size_t>(rep.pbo_n_splits));
    }
    // R3 Q2: --pbo-hard-block: escalate advisory PBO breach to FAIL verdict + non-zero exit.
    // Manifest + library are already persisted above; returning Err keeps all artifacts AND
    // exits non-zero (dispatch.cpp:112-114 maps Err -> "print message, return 1").
    if (cfg.pbo_hard_block && std::isfinite(rep.pbo) && cfg.max_pbo < 1.0 && !rep.pbo_gate_passed) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
            std::string("[PBO hard block] PBO hard block: run-level CSCV-PBO=") +
            std::to_string(rep.pbo) + " EXCEEDS --max-pbo=" + std::to_string(cfg.max_pbo) +
            "; failing run (--pbo-hard-block)");
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
    // R3b: add oos_pbo kv ONLY when OOS is active (eff_oos_fraction > 0) so the
    // non-accumulation path's kvs are byte-identical to the pre-R3 baseline.
    if (eff_oos_fraction > 0.0) {
        const std::string pbo_str = std::isnan(rep.oos_pbo)
            ? "nan"
            : std::to_string(rep.oos_pbo);
        sr.kvs.emplace_back("oos_pbo", pbo_str);
    }
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

    // 2a. W2 capacity screen (opt-in): build a derived Panel whose universe_ is
    // original_universe ∧ (close>min_price) ∧ (adv{W}>=min_adv). NaN propagation
    // then filters illiquid names across the entire eval chain with ZERO changes to
    // streams/fitness/factory/search_driver. Screen is INACTIVE (default) when both
    // thresholds are 0 — the original panel object is passed through UNCHANGED,
    // guaranteeing byte-identical output to a run with no capacity flags.
    const bool capacity_on = cfg.min_adv_usd > 0.0 || cfg.min_price > 0.0;
    if (capacity_on) {
        ATX_TRY(auto screened,
                detail::apply_capacity_screen(panel, cfg.min_price, cfg.min_adv_usd,
                                              cfg.adv_window));
        panel = std::move(screened);
    }

    // 3. Build run-wide objects.
    alpha::Library lib{};

    // Build the book's WeightPolicy from cfg (W1a). The defaults of these cfg
    // fields EXACTLY reproduce engine::WeightPolicy{} (transform=Rank,
    // winsorize_limit=0.025, industry_neutral=false, gross_leverage=1.0), so a
    // discover run with NONE of the new flags constructs a byte-identical policy
    // to the previous `WeightPolicy{}` — the default path is unchanged. `policy`
    // flows to BOTH the gated path (run_discover_gated -> factory::Factory) and
    // the ungated path (factory::SearchDriver) below, so building it once here
    // covers both.
    WeightPolicy policy{};
    policy.transform        = parse_weight_transform(cfg.weight_transform);
    policy.winsorize_limit  = cfg.winsorize_limit;
    policy.industry_neutral = cfg.industry_neutral;
    policy.gross_leverage   = cfg.gross_leverage;

    // CAVEAT (W1a scope): industry_neutral needs a universe-aligned group_map, but
    // the discovery eval path (alpha::extract_streams -> WeightPolicy::
    // to_target_weights) supplies NONE — it calls the 4-arg extract_streams whose
    // group_map defaults to {}. With industry_neutral=true and an empty group_map,
    // to_target_weights ATX_ASSERTs `group_map.size() == n` and aborts (debug) /
    // is UB (release). Rather than emit a silently-degenerate book, reject the
    // flag in discovery: full industry_neutral discovery wiring (sourcing a
    // per-instrument sector group_map from the panel and threading it through
    // SearchDriver/Factory/extract_streams) is OUT OF W1a SCOPE. The Raw transform
    // + winsorize/gross-leverage knobs are the W1a deliverables; the
    // industry_neutral knob is EXPOSED on RunConfig/WeightPolicy but not yet wired
    // into discovery.
    if (cfg.industry_neutral) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
            "discover: --industry-neutral is not yet wired into discovery (no "
            "group_map is supplied to the search eval path); it is exposed but "
            "out of W1a scope. Omit it.");
    }

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
                        ? static_cast<atx::usize>(cfg.population)  : 200;
    sc.generations  = cfg.generations > 0
                        ? static_cast<atx::usize>(cfg.generations) : 5;
    sc.elites       = 2;
    sc.k_tournament = 3;
    sc.p_cross      = 0.5;
    sc.enable_behavioral_novelty = true;
    // W1b: opt-in wrap_in_op mutation (default false -> byte-identical legacy path).
    sc.enable_wrap_in_op = cfg.enable_wrap_in_op;

    // Parallelize the search (digest-invariant: SearchConfig::n_workers affects
    // speed/memory, never bits — F1). --workers overrides; 0 = auto (cores-1).
    // Each worker holds a full per-genome signal buffer, so on a wide panel with
    // limited RAM, cap --workers to bound peak memory.
    {
        const unsigned hw = std::thread::hardware_concurrency();
        const atx::usize autow = hw > 1 ? static_cast<atx::usize>(hw - 1) : 1;
        sc.n_workers = cfg.workers > 0 ? static_cast<atx::usize>(cfg.workers) : autow;
    }

    // Scale random immigrants to population size: 10% of population, floored at 4.
    // This provides meaningful fresh-blood injection on any population size (the
    // struct default of 2 was ~3% on pop-60, too weak to counter convergence).
    sc.n_immigrants = std::max<atx::usize>(sc.population / 10, 4);

    // 5a. W4a robust factor (Deliverable 2): when --robust-holdout-frac > 0, build a
    // DETERMINISTIC seeded weak/holdout sub-universe Panel over the SAME (post-
    // capacity-screen) `panel` the search optimizes, seeded by sc.master_seed
    // (NEVER thread/time). It is OWNED here and threaded into BOTH discover paths
    // (ungated SearchDriver below + gated Factory) so the §0.8 robust factor
    // ACTIVATES (robust = clamp(wq_on(weak)/wq, 0, 1)). frac == 0 (the default) -> no
    // panel built, weak_panel stays nullptr, robust stays 1.0, and BOTH paths are
    // byte-identical to today (the AtxImplDiscover determinism slice is unchanged).
    std::optional<alpha::Panel> weak_panel_owned;
    const alpha::Panel* weak_panel = nullptr;
    if (cfg.robust_holdout_frac > 0.0) {
        ATX_TRY(auto wp, detail::build_robust_holdout_panel(panel, cfg.robust_holdout_frac,
                                                            sc.master_seed));
        weak_panel_owned.emplace(std::move(wp));
        weak_panel = &*weak_panel_owned; // borrowed; weak_panel_owned outlives both paths
    }

    // 5b. R1 typed-fields cardinality scan (opt-in via --typed-fields; default OFF =
    // byte-identical). When ON, do ONE deterministic pass over each numeric panel field
    // to count its distinct non-NaN cardinality; any field with cardinality <=
    // field_cardinality_max OR whose name is in the binary/count backstop list is added
    // to numeric_excluded_fields. The GICS classifier (named "sector" or any "IndClass.*"
    // prefix) is already handled by is_group_field() in SearchDriver — the backstop list
    // here catches a raw integer `gics` column if one is present. Empty when OFF (default)
    // -> SearchDriver's partition is IDENTICAL to today -> byte-identical digest.
    std::vector<std::string> numeric_excluded_fields;
    std::vector<std::string> extra_group_fields;
    if (cfg.typed_fields) {
        detail::classify_typed_fields(panel, fields, cfg.field_cardinality_max,
                                      numeric_excluded_fields, extra_group_fields);
    }

    // 6'. Gated discovery (opt-in via --gated): route every distinct candidate
    //     through the factory's deflated-Sharpe ranking + AlphaGate floors so the
    //     emitted alphas are robust (DSR bar), low-turnover, low-correlation, and
    //     high-fitness. Admitted alphas persist in an on-disk library::Library
    //     (a durable alpha database) and are also written as .dsl for `combine`.
    if (cfg.gated) {
        return run_discover_gated(cfg, panel, lib, policy, sim, sc, fields, weak_panel,
                                  numeric_excluded_fields, extra_group_fields); // R1
    }

    // 6. Run the evolutionary search (default ungated path: top-N by raw fitness).
    // W4a: weak_panel (nullptr unless --robust-holdout-frac > 0) activates the robust
    // factor in the search fitness; the default nullptr keeps this byte-identical.
    // R1: numeric_excluded_fields/extra_group_fields are empty (default) unless
    // --typed-fields is set; empty lists keep the search byte-identical to today.
    factory::SearchDriver driver{lib, panel, policy, sim, cfg.seed_exprs, fields, weak_panel,
                                 numeric_excluded_fields, extra_group_fields};
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
