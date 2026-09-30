#pragma once

#include <array>
#include <span>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

// Causal ex-ante price-risk exposures (trailing market beta, volatility, log
// dollar ADV) and neutralization of one decision's desired target against them.
// Deterministic: fixed loop order, no threads, no hidden state between calls unless the
// caller binds a session ring (enable_session_ring), whose cached values are the
// stateless path's own bits.
namespace atx::impl::strategy {
// Column order of every row-major instruments x kPriceExposureCount matrix.
inline constexpr atx::usize kPriceExposureCount = 3;
inline constexpr atx::usize kExposureBeta = 0, kExposureVol = 1, kExposureLogAdv = 2;
// Within-groups neutralization (industry ids): a group id is an integer in
// [0, kMaxGroupId] stored as f64 (Fama-French 12/49 numbers, SIC2), NaN = unknown.
// A group with fewer than kMinGroupNames regressed rows has no level of its own.
inline constexpr atx::usize kMinGroupNames = 5, kMaxGroupId = 9999;
// Group slots: one per id, the unknown (NaN) group, the pooled fallback group.
inline constexpr atx::usize kGroupSlots = kMaxGroupId + 3;
// The slot table's bytes: per slot a row count and kPriceExposureCount + 1 sums.
inline constexpr atx::usize kGroupTableBytes =
    kGroupSlots * (sizeof(atx::usize) + (kPriceExposureCount + 1) * sizeof(atx::f64));

struct PriceExposureConfig {
  atx::usize beta_window{252}, vol_window{63}, adv_window{63}, min_return_pairs{126};
  atx::usize min_names{50};
  atx::f64 clip_z{5.0};
};
// All spans are borrowed for one synchronous call, date-major dates x instruments,
// immutable. close is adjusted, raw_close unadjusted, volume raw shares, present
// the physical vendor row flag (0/1; absent rows carry NaN prices). Decision d is
// known after the close of session d.
struct PriceExposureInput {
  atx::usize dates{}, instruments{};
  std::span<const atx::f64> close, raw_close, volume;
  std::span<const atx::u8> present;
};

// Opt-in session ring of compute_price_exposures (v8 D-1, review P-9). Unbound (the
// default) it is empty and unused. Bound to one panel (enable_session_ring), every interval
// (t-1, t] is computed once per bound panel -- each session logged once -- and kept, with
// its equal-weight market return, in a mirrored ring of max(beta_window, vol_window)
// intervals, so a decision computes only the intervals its window adds. An interval's
// values are the stateless path's own expressions on the same two sessions (one kernel),
// so exposures are bit-identical to a fresh scratch. Contract: while bound, the bound
// panel's close, raw_close and present spans (data and geometry) do not change; a call on
// any other panel runs the stateless path and leaves the ring as it is. Its size is
// session_ring_bytes. Contents private to the implementation, except `seconds` (a clock
// callers may read: observation only, never an input).
struct PriceExposureRing {
  const atx::f64* close{};
  const atx::f64* raw_close{};
  const atx::u8* present{};
  atx::usize dates{}, instruments{};
  bool bound{};
  atx::usize capacity{};            // intervals kept; 0 = not sized for the current config
  std::vector<atx::f64> returns;    // instrument-major, 2 x capacity mirrored slots per name
  std::vector<atx::f64> market;     // 2 x capacity mirrored slots
  std::vector<atx::usize> interval; // per slot: the interval held (none: max usize)
  std::vector<atx::f64> logs;       // two sessions of (adjusted, raw) logs, instruments each
  atx::usize last{};                // the half of logs holding session `logged`
  atx::usize logged{};
  bool has_logged{};
  atx::f64 seconds{}; // wall time spent in compute_price_exposures while serving from it
};

// Working storage owned by the caller and reused across calls. Its contents are
// private to the implementation: default-construct once per replay and pass it to
// every call. Buffers only grow (to the largest geometry seen), so repeated calls
// on one panel geometry allocate nothing after the first.
struct PriceExposureScratch {
  std::vector<atx::f64> returns; // instrument-major valid interval returns, NaN if invalid
  std::vector<atx::f64> market;  // equal-weight market return per interval
  std::vector<atx::f64> logs;    // previous/current session log adjusted and raw closes
  std::vector<atx::f64> dollars; // per-instrument dollar volume sum over the ADV window
  PriceExposureRing ring;        // unbound unless enable_session_ring
};
// Binds scratch's session ring to `panel` (dropping its contents and zeroing its clock).
// The caller keeps the panel's spans unchanged while the scratch serves it.
void enable_session_ring(PriceExposureScratch& scratch, const PriceExposureInput& panel) noexcept;
// Bytes a bound ring holds for `instruments` names under `cfg`: 2 x capacity + 4 f64 per
// name plus 2 f64 and one tag per slot (0 for windows outside [2, 4096], which
// compute_price_exposures refuses anyway).
[[nodiscard]] atx::u64 session_ring_bytes(const PriceExposureConfig& cfg,
                                          atx::usize instruments) noexcept;
struct NeutralizeScratch {
  std::vector<atx::usize> rows;   // regressed row indices, ascending
  std::vector<atx::f64> z;        // rows x kPriceExposureCount clipped z-scores
  std::vector<atx::f64> residual; // rows
  // Within groups only (grown on first use): each regressed row's group slot, and per
  // slot the row count and the sums of the target and of every z column.
  std::vector<atx::usize> slot, slot_count;
  std::vector<atx::f64> slot_sum;
};
// compute_price_exposures + neutralize_target state for the one-call step below.
struct PriceRiskScratch {
  PriceExposureScratch exposure;
  NeutralizeScratch neutralize;
  std::vector<atx::f64> exposures; // instruments x kPriceExposureCount at the last decision
  std::vector<atx::u8> ok;         // instruments at the last decision
};
struct NeutralizeStats {
  atx::usize used{};     // member && ok rows regressed and rescaled
  atx::usize excluded{}; // member && !ok rows forced to zero
  atx::f64 gross{};          // sum |target| over members on entry; also the exit gross
  atx::f64 excluded_gross{}; // share of that gross that sat on excluded rows
  atx::f64 residual_gross{}; // sum |OLS residual| over used rows, before rescaling
  // OLS coefficients on [1, z_beta, z_vol, z_log_adv] (clipped z-scores), after
  // one refinement step. All zero for a flat target.
  std::array<atx::f64, kPriceExposureCount + 1> coefficients{};
  // Within groups only (0 otherwise), over the used rows: demeaning groups (the pooled
  // fallback counts as one), rows with an unknown (NaN) id, and rows pooled into the
  // fallback because their group had fewer than kMinGroupNames used rows.
  atx::usize groups{}, unknown_group_names{}, fallback_names{};
};

// Exposures known at decision d; every window ends at session d inclusive and no
// session after d is read (nor validated). Interval t is (t-1, t]. Its adjusted
// simple return is valid iff both endpoints are present with finite positive
// close and raw_close, the return is finite, and the interval is not guarded:
// |log adjusted ratio| > 1.5 or > |log raw ratio| + 0.10 (the target replay's
// rough-return guard, identical operations).
//  - market(t): equal-weight mean of valid returns across ALL instruments; NaN
//    when none is valid.
//  - beta: cov(r_i, market)/var(market) (sample, two-pass) over valid pairs in the
//    last beta_window intervals; NaN below min_return_pairs pairs or var == 0.
//  - vol: sample SD of valid r_i over the last vol_window intervals; NaN below
//    max(2, ceil(vol_window / 2)) returns.
//  - log_adv: log of the mean of raw_close * volume over the adv_window sessions
//    ending at d. A session counts 0 unless present with finite positive raw_close,
//    finite nonnegative volume and a finite product. NaN if the full window is not
//    inside the panel (d + 1 < adv_window) or the mean is not positive.
// Windows are clipped at the panel start; the pair minimums then gate beta/vol.
// out: row-major instruments x kPriceExposureCount (beta, vol, log_adv), each
// finite or NaN. ok[i] = 1 iff all three are finite, else 0.
// Errors (out/ok unspecified): InvalidArgument for config (windows in [2, 4096],
// adv_window in [1, 4096], 2 <= min_return_pairs <= beta_window, min_names >= 5,
// finite clip_z > 0), span geometry, d >= dates or a presence byte > 1 in the
// read sessions; OutOfRange if instruments x max(beta_window, vol_window) exceeds
// 2^26 return cells or scratch allocation fails.
// Cost: O((max(beta_window, vol_window) + adv_window) x instruments) per call.
[[nodiscard]] atx::core::Status compute_price_exposures(
    const PriceExposureInput&, const PriceExposureConfig&, atx::usize d,
    PriceExposureScratch&, std::span<atx::f64> out, std::span<atx::u8> ok);

// In-place cross-sectional neutralization of one decision's target. Rows used:
// member && ok. Each exposure is z-scored over those rows (mean, sample SD) and
// clipped to +-clip_z; the target is regressed by OLS on [1, z_beta, z_vol,
// z_log_adv] (Jacobi-equilibrated Cholesky with a pivot floor, one iterative
// refinement step) and replaced by the residual, rescaled so sum |target| over
// the used rows equals the entry gross (sum |target| over all members).
// member && !ok rows become 0; nonmembers are untouched and must already be 0.
// A flat (zero-gross) target is returned unchanged.
// Preconditions (else InvalidArgument): equal lengths, exposures row-major
// target.size() x kPriceExposureCount, member/ok bytes 0/1, members finite,
// nonmembers exactly 0, ok rows finite exposures.
// Data refusals (Unavailable): fewer than min_names used rows, a constant
// exposure column, an ill-conditioned normal matrix, or a residual that is a
// negligible fraction of the gross. OutOfRange if scratch allocation fails.
// Strong guarantee: on any error target is unmodified. stats is reset on entry;
// on a refusal it still reports used/excluded/gross.
[[nodiscard]] atx::core::Status neutralize_target(
    std::span<atx::f64> target, std::span<const atx::u8> member,
    std::span<const atx::f64> exposures, std::span<const atx::u8> ok,
    const PriceExposureConfig&, NeutralizeScratch&, NeutralizeStats&);

// One-call decision step: compute_price_exposures at d, then neutralize_target on
// the target (instruments long) with that decision's member row. Same contracts
// and errors as the two primitives; target is unmodified on any error.
[[nodiscard]] atx::core::Status neutralize_price_risk(
    const PriceExposureInput&, const PriceExposureConfig&, atx::usize d,
    std::span<atx::f64> target, std::span<const atx::u8> member, PriceRiskScratch&,
    NeutralizeStats&);

// neutralize_target plus within-group demeaning (price-risk-ind-v1/-v2): the OLS
// residual of the target on [group indicators, z_beta, z_vol, z_log_adv]. Exact order:
//  1. used rows = member && ok, entry gross, too-few-names refusal: as neutralize_target;
//  2. each exposure z-scored over the used rows and clipped: as neutralize_target;
//  3. group slot of each used row: its id; every NaN id forms ONE unknown group; every
//     group (the unknown one included) with fewer than kMinGroupNames used rows is
//     pooled into one fallback group: it has no level of its own and loads only on the
//     common intercept, which (FWL, every other group absorbed by its own indicator)
//     is its names demeaned together;
//  4. the target and every z column demeaned within slot (means over the used rows,
//     summed in ascending row order); a z column the slots span (within-slot sum of
//     squares <= 1e-8 x its sum of squares, the pivot floor) refuses Unavailable;
//     then every HELD used row (hold_zero[i] != 0 and target[i] == 0 on entry) has its
//     demeaned target reset to 0 (NAV locate-in-aim, review I3: a zeroed special-tier
//     short cannot come back as -(its group mean)); its z row stays in the fit;
//  5. the demeaned target regressed on [1, demeaned z] by neutralize_target's OLS
//     (with no held row the intercept coefficient is 0 up to rounding: every demeaned
//     column sums to 0) and replaced by the residual;
//  6. rescaled to the entry gross; member && !ok rows to 0; nonmembers untouched.
// By Frisch-Waugh-Lovell the result is orthogonal to every slot indicator (within-slot
// sums 0) and to the clipped z. With held rows it stays orthogonal to the intercept
// and the demeaned z only. Before the rescale, slot G sums to h_G m_G - n_G c (h_G held
// and n_G used rows, m_G the slot mean of the entry target, c the intercept, up to
// rounding sum_G h_G m_G / used rows) and a held row's residual is -(c + its demeaned
// z row's fit).
// group: one id per instrument (target.size()), read only on the used rows. hold_zero:
// empty (the default: nothing held, the path above bit for bit) or one byte per
// instrument. Same preconditions, refusals and strong guarantee as neutralize_target,
// plus InvalidArgument for a group (or non-empty hold_zero) span of another length or
// a finite id on a used row that is not an integer in [0, kMaxGroupId].
[[nodiscard]] atx::core::Status neutralize_target_within_groups(
    std::span<atx::f64> target, std::span<const atx::u8> member,
    std::span<const atx::f64> exposures, std::span<const atx::u8> ok,
    std::span<const atx::f64> group, const PriceExposureConfig&, NeutralizeScratch&,
    NeutralizeStats&, std::span<const atx::u8> hold_zero = {});

// One-call decision step of the industry ids: compute_price_exposures at d, then
// neutralize_target_within_groups with that decision's member row, group row and hold
// mask (all instruments long; hold_zero may be empty). Target unmodified on any error.
[[nodiscard]] atx::core::Status neutralize_price_risk_within_groups(
    const PriceExposureInput&, const PriceExposureConfig&, atx::usize d,
    std::span<atx::f64> target, std::span<const atx::u8> member,
    std::span<const atx::f64> group, PriceRiskScratch&, NeutralizeStats&,
    std::span<const atx::u8> hold_zero = {});
} // namespace atx::impl::strategy
