#pragma once

// equity-mine — real-data DSL alpha mining stage (quant-platform swarm lane 9).
//
// Mines the alpha DSL zoo on identified yearly equity contexts and publishes an
// admitted alpha library with honest statistics:
//
//   1. STITCH   yearly `equity_scorecard16_ctx_<year>` contexts into three span
//               panels (train / validation / holdout). A cell (date, security)
//               is taken from the first context, in the caller's (chronological)
//               order, whose axes contain both; later contexts supply the warmup
//               history of names that only enter the universe in a later year.
//   2. MASK     trading eligibility is the AS-OF point-in-time membership of the
//               checkpoint 15 universe (membership.bin, cut top-N/band): a name is
//               tradeable on session d only when the last rebalance effective on
//               or before d lists it. W0-I0b / I-16: `--membership` is REQUIRED;
//               the pre-W0 context year-union fallback (a within-year selection
//               look-ahead) is reachable only by asking for it explicitly with
//               `--membership-rule year-union-v1` (declared in the gate report).
//   3. SEARCH   factory::SearchDriver (L3 multi-fidelity racing, semantic canon,
//               output-fingerprint dedup) over the TRAIN span only, seeded by the
//               WQ101 fixture + literature families + grammar-random genomes.
//   4. SCORE    every seed and every searched genome is re-scored on the train
//               window by one honest evaluator: delay-`delay` rank-weighted
//               dollar-neutral long/short (gross 1), net of `cost_bps` per unit of
//               one-way traded weight. The sign is fixed on TRAIN gross Sharpe.
//               Every scored candidate is recorded in an eval::TrialRegistry with
//               its window, IS flag and family/theme tags (E-16), and its train
//               DSR is the cluster-N DSR of TrialRegistry::accounting() (E-01).
//               `--delay 0` (same-close fills) needs --allow-same-close (B-02).
//   5. FAMILY   the validation family = top `max_validate` by train net Sharpe,
//               greedily de-duplicated by train-pnl correlation (SketchIndex).
//   6. GATE     the family is evaluated ONCE on the validation window; one-sided
//               Newey-West p-values are Benjamini-Yekutieli adjusted and a
//               Romano-Wolf stepdown runs on the family's validation net pnl.
//   7. HOLDOUT  only with `--holdout publish` (default off: the holdout contexts
//               are never loaded): admitted alphas, their blend and the family
//               blend are scored on the holdout window and reported; nothing is
//               re-selected. `--holdout-prior-reads` records earlier reads.
//
// Every role's stitched span must end before the next role starts (train <
// validation-start, validation < holdout-start, holdout < holdout-end), and
// realized returns failing the ReturnGuard (adjusted vs raw close / |log| cap)
// are treated as missing and counted per role in the gate report.
//
// Nothing here reads a session >= the seal (2020-01-01 by default): the stage
// refuses any context carrying such a session key.

#include <array>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/eval/multiple_testing.hpp"
#include "atx/engine/eval/trial_registry.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/factory/ic_screen.hpp"
#include "atx/engine/factory/execution_objective.hpp"

#include "stages.hpp"

namespace atx::engine::data { struct PitMembershipImage; }
namespace atx::engine::alpha { class Library; }

namespace atx::impl {

// CLI entry: `atx-impl equity-mine --flag value ...`. Parses its own flags (the
// shared RunConfig parser is not extended), runs the stage, prints the digest
// line unless --quiet, and returns the process exit code (0 ok, 1 stage error,
// 2 usage error).
[[nodiscard]] int dispatch_equity_mine(int argc, char **argv, std::ostream &out,
                                       std::ostream &err);

namespace mine {

// ---------------------------------------------------------------------------
// Span construction
// ---------------------------------------------------------------------------

// One identified context held in memory (a read_panel_artifact result reduced to
// what stitching needs). instrument_ids are the parsed identity strings.
struct SpanSource {
    atx::engine::alpha::Panel panel;
    std::vector<atx::i64> session_keys;
    std::vector<atx::i64> instrument_ids;
};

struct SpanPanel {
    atx::engine::alpha::Panel panel;
    std::vector<atx::i64> session_keys;   // ascending union of the sources' keys
    std::vector<atx::i64> instrument_ids; // first-seen order over the sources
    // Per date: the index (into the stitched sources) whose own span "owns" the
    // date, i.e. the first source containing it. Used for the year-union
    // fallback membership.
    std::vector<atx::u32> owner_source;
    // Diagnostic: cells present in two or more sources, and how many of those
    // disagree on any field (bitwise, NaN == NaN). Expected 0 for one archive.
    atx::usize overlap_cells{};
    atx::usize overlap_mismatch_cells{};
};

// Stitch sources (given in preference order) into one span panel. Every source
// must carry the same field names in the same order and strictly ascending
// session keys. Err(InvalidArgument) otherwise or when `sources` is empty.
[[nodiscard]] atx::core::Result<SpanPanel> stitch_span(std::span<const SpanSource> sources);

// As-of membership mask (dates x instruments, date-major): 1 iff the last
// rebalance whose effective_session_key <= session_keys[d] lists the id in
// cut `cut`. Err(InvalidArgument) when `cut` is out of range.
[[nodiscard]] atx::core::Result<std::vector<atx::u8>>
asof_membership_mask(const atx::engine::data::PitMembershipImage &image, atx::usize cut,
                     std::span<const atx::i64> session_keys,
                     std::span<const atx::i64> instrument_ids);

// Year-union fallback: a cell is a member iff the instrument appears in the
// axes of the source owning that date.
[[nodiscard]] std::vector<atx::u8> owner_union_mask(const SpanPanel &span,
                                                   std::span<const SpanSource> sources);

// ---------------------------------------------------------------------------
// Honest signal scoring
// ---------------------------------------------------------------------------

struct ScoreCfg {
    atx::f64 cost_bps{5.0};    // per unit of one-way traded weight (gross book 1.0)
    // Realized-return plausibility guard (see ReturnGuard). A one-day adjusted
    // return is excluded (treated as missing, and counted) when its |log| exceeds
    // max_abs_log_return, or when a raw (unadjusted) close exists and the
    // adjusted |log return| exceeds the raw one by more than adj_raw_log_tol --
    // an adjustment can shrink a raw corporate-action jump (split, dividend) but
    // never legitimately create one.
    bool guard_returns{true};
    atx::f64 max_abs_log_return{1.5}; // ~ +348% / -78% in one session
    atx::f64 adj_raw_log_tol{0.10};
    std::string raw_close_field{"raw_close"};
    atx::usize delay{1};       // sessions between signal close and trade close
    atx::usize min_names{20};  // fewer eligible names on a date -> flat that date
    std::array<atx::usize, 3> ic_horizons{{1, 5, 21}};
    atx::usize nw_lags{5};     // Newey-West lag for the pnl t-statistic
    atx::f64 periods_per_year{252.0};
    // Programmatic opt-in only. V2 requires a prepared context on every role;
    // its AUM, borrow and cost recipe are authoritative. delay/min_names/guard
    // and the role window must match this scorer. No CLI cost source is implied.
    atx::engine::factory::ExecutionObjectiveRule execution_rule{
        atx::engine::factory::ExecutionObjectiveRule::LegacyStreamsV1};
};

// [begin, end) over signal dates of a span panel.
struct EvalWindow {
    atx::usize begin{};
    atx::usize end{};
    [[nodiscard]] atx::usize size() const noexcept { return end > begin ? end - begin : 0; }
};

// Per-panel table of excluded one-day realized returns. bad_prefix[d * I + i]
// counts the excluded one-day returns of instrument i ending at sessions
// 1..d, so any multi-day return (from, to] is excluded iff the count differs.
struct ReturnGuard {
    struct Excluded {
        atx::usize date{};  // session index the one-day return ends on
        atx::usize inst{};
        atx::f64 adj_return{};
        atx::f64 raw_return{}; // NaN when no raw close
        bool cap{};            // true: |log| cap; false: adjusted/raw disagreement
    };
    atx::usize dates{};
    atx::usize instruments{};
    bool has_raw{};
    std::vector<atx::u32> bad_prefix;
    std::vector<Excluded> excluded; // every excluded cell, date-major order
    [[nodiscard]] bool empty() const noexcept { return bad_prefix.empty(); }
    [[nodiscard]] bool bad(atx::usize from, atx::usize to, atx::usize inst) const noexcept {
        return bad_prefix[to * instruments + inst] != bad_prefix[from * instruments + inst];
    }
    // Excluded one-day cells whose end session lies in [begin, end).
    [[nodiscard]] atx::usize count_in(atx::usize begin, atx::usize end) const noexcept;
};

// Build the guard for `panel` from its adjusted close field and, when present,
// cfg.raw_close_field. With cfg.guard_returns false the table is all-clear.
[[nodiscard]] atx::core::Result<ReturnGuard>
build_return_guard(const atx::engine::alpha::Panel &panel, atx::u32 close_field,
                   const ScoreCfg &cfg);

struct SignalScore {
    // Per signal date in the window (length window.size()). A flat date (too
    // few eligible names) carries 0 pnl, 0 turnover and NaN IC.
    std::vector<atx::f64> gross;
    std::vector<atx::f64> net;
    std::vector<atx::f64> turnover;
    std::vector<atx::f64> ic; // rank IC at ic_horizons[0]
    atx::f64 sharpe_gross{};  // annualized
    atx::f64 sharpe_net{};    // annualized
    atx::f64 mean_net{};      // per period
    atx::f64 t_nw{};          // Newey-West t of the net series
    atx::f64 p_one_sided{1.0};
    std::array<atx::f64, 3> ic_mean{};
    atx::f64 icir{};          // mean/sd of the ic_horizons[0] IC series (per period)
    atx::f64 mean_turnover{};
    atx::f64 coverage{};      // fraction of window dates that traded
    atx::f64 mean_names{};    // mean eligible names on traded dates
    atx::usize excluded_returns{}; // held-name pnl terms dropped by the legacy return guard
    atx::engine::factory::ExecutionObjectiveRule execution_rule{
        atx::engine::factory::ExecutionObjectiveRule::LegacyStreamsV1};
    std::string execution_context_sha256{};
    std::string execution_recipe{};
    // V2 vectors contain exactly this contiguous realized-endpoint interval of
    // the original panel. No structural zero tail or missing-date compression.
    // Legacy vectors retain their historical signal-date alignment.
    atx::usize realization_begin{};
    atx::usize realization_end{};
    atx::f64 total_cost_return{};
    atx::f64 total_borrow_return{};
};

// Score one signal (dates x instruments, date-major, same shape as `panel`)
// multiplied by `sign` (+1/-1). In the explicit legacy rule, positions formed from signal date d are
// rank-demeaned, scaled to gross 1 and held over the close-to-close return
// ending at d + delay + 1. Eligibility uses information at d only (member,
// finite signal, finite positive close); a missing realized return contributes
// 0. `close` is the adjusted research close field id. Realized returns (pnl
// and every IC horizon) flagged by the return guard are treated as missing;
// `guard` must be built for `panel` (nullptr: built here from cfg).
// Explicit DelayedSurfaceV2 instead consumes `execution`, errors on an unpriced
// nonzero fill or missing held return, and returns its mature realized prefix.
// It recomputes signed marked-dollar holdings through the shared engine kernel.
[[nodiscard]] atx::core::Result<SignalScore>
score_signal(std::span<const atx::f64> signal, atx::f64 sign,
             const atx::engine::alpha::Panel &panel, atx::u32 close_field,
             std::span<const atx::u8> member, EvalWindow window, const ScoreCfg &cfg,
             const ReturnGuard *guard = nullptr,
             const atx::engine::factory::ExecutionObjectiveContext *execution = nullptr);

// Flip a +1-signed score to sign -1 without re-scoring (gross/IC negate,
// turnover and cost are sign-invariant); statistics are recomputed. Legacy only;
// V2 must be independently rescored because borrow, caps and NAV are asymmetric.
[[nodiscard]] SignalScore flip_score(const SignalScore &s, const ScoreCfg &cfg);

// Recompute the scalar statistics of `s` from its series (used after flips and
// for blends).
void finalize_score(SignalScore &s, const ScoreCfg &cfg);

// ---------------------------------------------------------------------------
// Mining pipeline
// ---------------------------------------------------------------------------

enum class GateMode : atx::u8 { By, RomanoWolf, Both };

struct SeedExpr {
    std::string dsl;
    std::string origin; // "wq101:<id>", "lit:<name>", "extra"
};

// E-16 registry tags from a candidate origin. family = the origin up to its first
// ':' or '+' ("wq101", "lit", "search", "extra"); theme = the origin without any
// "+decay<N>" smoothing suffix ("lit:momentum_12_1+decay5" -> "lit:momentum_12_1").
[[nodiscard]] std::string trial_family_of(std::string_view origin);
[[nodiscard]] std::string trial_theme_of(std::string_view origin);

// One role's evaluation data. `panel` is borrowed for the call.
struct MineData {
    const atx::engine::alpha::Panel *panel{nullptr};
    std::vector<atx::u8> member;
    EvalWindow window;
    // Optional prebuilt return guard for `panel` (empty: built per call).
    ReturnGuard guard{};
    // Borrowed for the whole call. Required only for explicit DelayedSurfaceV2.
    // The context owns immutable costs/price/support; caller data must match.
    const atx::engine::factory::ExecutionObjectiveContext *execution{nullptr};
};

// W0-I0b (RULES §2): the versioned rule behind every row's report-only dsr_train.
//   ClusterMcFloorV2 (default): the cluster-N DSR of TrialRegistry::accounting()
//     (E-01 wiring, E0b note), falling back to SummaryRawNV2 when accounting is
//     unavailable (the report names the reason).
//   SummaryRawNV2: the registry-summary DSR with N = n_raw (no accounting).
//   SummaryNEffV1: the pre-W0 rule, N = n_eff over the registry summary (E-01
//     double discount), kept so pre-W0 dsr_train values can be re-derived.
enum class TrainDsrRule : atx::u8 {
    ClusterMcFloorV2 = 0,
    SummaryRawNV2 = 1,
    SummaryNEffV1 = 2,
};

// The stable label of a TrainDsrRule ("cluster-mc-floor-v2", "summary-raw-n-v2",
// "summary-n-eff-v1"): the --dsr-rule spelling and the gate report's dsr_rule value.
[[nodiscard]] std::string_view train_dsr_rule_label(TrainDsrRule rule) noexcept;

struct MineConfig {
    bool run_search{true};
    atx::engine::factory::SearchConfig search{};
    std::vector<std::string> search_fields; // field-swap candidates (panel spellings)
    ScoreCfg score{};
    atx::usize max_validate{100};
    atx::f64 max_corr{0.7};
    atx::f64 min_train_sharpe{0.0};
    atx::f64 min_coverage{0.5};
    atx::f64 fdr_q{0.10};
    atx::f64 rw_alpha{0.10};
    atx::engine::eval::BootstrapCfg boot{};
    GateMode gate{GateMode::By};
    atx::usize threads{1};
    TrainDsrRule dsr_rule{TrainDsrRule::ClusterMcFloorV2};
};

struct CandidateRow {
    std::string dsl;
    std::string origin;
    atx::u64 config_hash{};
    atx::f64 sign{1.0};
    bool scored{false};     // compiled, evaluated and produced a non-degenerate pnl
    std::string error;      // why not scored
    SignalScore train;
    atx::f64 dsr_train{};   // registry-fed deflated Sharpe of the train net pnl
    bool validated{false};
    SignalScore validation;
    atx::f64 p_by{1.0};
    atx::f64 p_rw{1.0};
    bool admitted{false};
    atx::u64 canonical_hash{};
    bool ic_screen_evaluated{false};
    bool ic_screen_unavailable{false};
    bool ic_rejected{false};
    atx::engine::factory::IcScreenReason ic_screen_reason{
        atx::engine::factory::IcScreenReason::Disabled};
    bool dsr_marginal_floor_applied{false};
    atx::f64 dsr_selection_benchmark{};
};

struct MineOutcome {
    std::vector<CandidateRow> candidates; // seeds first (input order), then search
    atx::engine::factory::ExecutionObjectiveRule execution_rule{
        atx::engine::factory::ExecutionObjectiveRule::LegacyStreamsV1};
    std::string train_execution_context_sha256{};
    std::string validation_execution_context_sha256{};
    std::vector<atx::usize> family;       // validation family, selection order
    std::vector<atx::usize> admitted;     // subset of family, selection order
    atx::engine::eval::TrialSummary trials{};
    atx::usize seeds_invalid{};
    atx::usize degenerate{};              // compiled but flat/non-finite pnl
    atx::usize family_rejected_corr{};
    // Pre-registered breadth hypothesis: the equal-weight rank blend of the whole
    // validation family (members and signs fixed on TRAIN), tested on validation
    // as hypothesis K+1 inside the same BY / Romano-Wolf family.
    bool family_blend_scored{false};
    SignalScore family_blend_validation;
    atx::f64 family_blend_p_by{1.0};
    atx::f64 family_blend_p_rw{1.0};
    bool family_blend_admitted{false};
    atx::u64 search_digest{};
    atx::usize search_trial_count{};
    atx::usize search_fidelity_evals{};
    atx::usize search_fidelity_rejected{};
    atx::usize search_fingerprint_hits{};
    atx::engine::factory::IcScreenConfig ic_screen{}; // resolved TRAIN-only bounds
    std::string ic_screen_recipe;
    std::string ic_screen_unavailable_reason;
    atx::usize ic_screen_evaluations{};
    atx::usize ic_screen_unavailable{};
    atx::usize ic_screen_rejected{}; // candidate rows (registry deduplicates identities)
    atx::usize search_ic_screen_evaluations{};
    atx::usize search_ic_screen_unavailable{};
    atx::usize search_ic_screen_rejected{};
    atx::usize search_ic_prepass_vm_evaluations{};
    bool search_ic_screen_resume_mismatch{false};
    atx::usize dsr_marginal_floor_count{};
    // W0-I0b recording (E-16 / E-01 wiring). dsr_rule names the rule behind every
    // row's dsr_train (train_dsr_rule_label): "cluster-mc-floor-v2" (the default,
    // TrialRegistry::accounting()), "summary-raw-n-v2" (requested, or the default's
    // fallback when accounting is unavailable, reason given) or "summary-n-eff-v1"
    // (the pre-W0 rule, requested explicitly).
    std::string dsr_rule;
    std::string dsr_fallback_reason;
    atx::usize dsr_clusters{};  // ONC clusters behind the cluster-N DSR (0 on fallback)
    atx::f64 dsr_sr_star_mc{};  // Monte-Carlo E[max SR] under the estimated correlation
    atx::engine::eval::TrialChainHead chain_head{}; // registry head after recording
};

// Steps 3-5 on the TRAIN span only (search, score, registry, family). The
// Library must be the one every parse in the run uses and must outlive the
// call. `registry` must be configured with pnl_len >= train.window.size(): its
// calendar may extend past the train window (the stage uses train + validation,
// E-16); train trials are recorded on the window [0, train.window.size() - 1] as
// TrialSample::InSample with family / theme tags derived from the seed origin.
// V2 instead records the actual mature realized subinterval, offset relative to
// train.window.begin, with no padded zeros and a context-bound trial identity.
// The validation span is not needed yet, so a caller can free the train span
// before building it (memory bound on a shared 16 GB machine).
[[nodiscard]] atx::core::Result<MineOutcome>
mine_train(const atx::engine::alpha::Library &lib, const MineData &train,
           std::span<const SeedExpr> seeds, const MineConfig &cfg,
           atx::engine::eval::TrialRegistry &registry);

// Step 6: evaluate the family ONCE on the validation window and apply the gate
// (fills validation / p_by / p_rw / admitted and outcome.admitted).
[[nodiscard]] atx::core::Status mine_validate(const atx::engine::alpha::Library &lib,
                                              const MineData &validation,
                                              const MineConfig &cfg, MineOutcome &outcome);

// mine_train followed by mine_validate.
[[nodiscard]] atx::core::Result<MineOutcome>
mine(const atx::engine::alpha::Library &lib, const MineData &train, const MineData &validation,
     std::span<const SeedExpr> seeds, const MineConfig &cfg,
     atx::engine::eval::TrialRegistry &registry);

struct HoldoutRow {
    std::string dsl;       // "<equal-weight blend>" for the blend row
    SignalScore score;
};

// Evaluate the admitted alphas (with their train-fixed signs) and their
// equal-weight blend on the holdout window. The blend averages the per-alpha
// cross-sectional ranks (each alpha's rank weights), i.e. it is itself one
// rank-L/S book scored by score_signal.
[[nodiscard]] atx::core::Result<std::vector<HoldoutRow>>
evaluate_holdout(const atx::engine::alpha::Library &lib, const MineData &holdout,
                 std::span<const CandidateRow> admitted, const ScoreCfg &cfg);

// Score the equal-weight blend of `rows` (each with its train-fixed sign) on
// `data`: per date, the mean over alphas of each alpha's centered cross-
// sectional rank, itself scored as one rank-L/S book. Err when rows is empty.
[[nodiscard]] atx::core::Result<SignalScore>
evaluate_blend(const atx::engine::alpha::Library &lib, const MineData &data,
               std::span<const CandidateRow> rows, const ScoreCfg &cfg);

// Built-in literature seed families (momentum, reversal, low-vol, liquidity,
// value-of-range, implied-vol level/slope/change, size, 52-week high, ...).
[[nodiscard]] std::vector<SeedExpr> literature_seeds();

// Parse `<id>: <dsl>` fixture lines ('#' comments) into seeds "wq101:<id>".
[[nodiscard]] std::vector<SeedExpr> parse_fixture_seeds(std::string_view text);

// Parse an ISO date YYYY-MM-DD into UTC-midnight Unix nanoseconds.
[[nodiscard]] atx::core::Result<atx::i64> parse_iso_date_ns(std::string_view text);

} // namespace mine

} // namespace atx::impl
