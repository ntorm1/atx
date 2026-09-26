#pragma once

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::alpha { class Panel; }
namespace atx::engine::data { class ExposurePanel; }

namespace atx::engine::factory {

enum class ObjectiveIcRule : u8 { ResidualSqrtCapSpearmanV1 = 1 };
enum class ObjectiveIcMissingPolicy : u8 { RefuseIncomplete = 1, DropUnavailableNames = 2 };
enum class ObjectiveIcHacRule : u8 { HorizonAwareV3 = 3 };
enum class ObjectiveIcKernel : u8 { Unavailable = 0, Bartlett = 1, Uniform = 2 };
enum class ObjectiveIcDateStatus : u8 {
    OutsideWindow, ExposureUnavailable, TooFewNames, RankDeficient,
    NumericalRangeUnsupported, ResidualDegenerate, Valid
};

struct ObjectiveIcConfig {
    ObjectiveIcRule rule{ObjectiveIcRule::ResidualSqrtCapSpearmanV1};
    usize window_begin{};
    usize window_end{};   // exclusive; 0 means prices.dates()
    usize maturity_end{}; // exclusive; 0 means window_end, may not exceed it
    usize execution_delay{1};
    usize min_names{20};
    usize min_dates{128};
    f64 min_member_fraction{0.8};
    f64 min_paired_fraction{0.8};
    f64 min_date_fraction{0.8};
    ObjectiveIcMissingPolicy missing{ObjectiveIcMissingPolicy::RefuseIncomplete};
    ObjectiveIcHacRule hac{ObjectiveIcHacRule::HorizonAwareV3};
    bool allow_synthetic_exposures{false};
    usize workers{1};
    u64 max_working_bytes{512ULL * 1024 * 1024};
};

// All spans are borrowed only during preparation. Full axes, no broadcasting.
// A session label is not an observation/publication clock: mark_times_ns names
// the close observation; require mark[d] < decision[d] < mark[d+1]. Expected
// hashes are caller-pinned identities, never inferred from the supplied panel.
struct ObjectiveIcInputs {
    std::span<const i64> session_keys;
    std::span<const i64> decision_times_ns;
    std::span<const i64> mark_times_ns;
    std::span<const i64> instrument_ids;
    std::string_view instrument_namespace;
    std::string_view expected_exposure_axis_sha256;
    std::string_view expected_exposure_content_sha256;
    std::string_view price_source_sha256; // must equal I2's required prices parent
    std::string_view price_field{"close"};
    // Additional decision-only restriction, intersected with I2's qualified
    // membership and Panel presence. No future membership checks in labels.
    std::span<const u8> decision_membership{};
    // Full [date][instrument] cumulative excluded 1-day increments; endpoint
    // minus entry determines interval validity. Missing guard means no guard.
    std::span<const u32> bad_return_prefix{};
};

struct ObjectiveIcDate {
    ObjectiveIcDateStatus status{ObjectiveIcDateStatus::OutsideWindow};
    usize known_member_names{}, unknown_membership_names{};
    usize exposure_names{}, decision_names{}, fitted_names{}, design_columns{}, rank{};
    f64 max_weighted_orthogonality_error{};
};
struct ObjectiveIcHorizon {
    usize horizon{};
    usize valid_dates{}, calendar_dates{}, paired_names{}, hac_lag{};
    f64 mean{}, standard_error{}, t_stat{}, hac_ir{}; // IR = t/sqrt(n_valid), not annualized
    ObjectiveIcKernel effective_kernel{ObjectiveIcKernel::Unavailable};
    bool inference_defined{}, fell_back{};
};
struct ObjectiveIcHalfLife {
    bool defined{};
    usize common_dates{};
    std::array<f64, 3> common_mean_ic{};
    f64 days{}, log_slope{}, r_squared{};
};
struct ObjectiveIcResult {
    std::array<ObjectiveIcHorizon, 3> horizons;
    ObjectiveIcHalfLife half_life;
    usize residual_valid_dates{}, residual_degenerate_dates{}, rank_deficient_dates{};
    usize persistence_pairs{};
    f64 mean_rank_autocorrelation{};
    bool persistence_defined{};
    // Diagnostic 1/(1-rho), only for 0 <= rho < 1; not executed turnover/holding time.
    f64 persistence_holding_proxy{};
    bool persistence_holding_proxy_defined{};
    std::string context_sha256;
};

namespace objective_ic_detail { struct Context; struct Scratch; }
class ObjectiveIcScratch;
class ObjectiveIcContext {
public:
    ObjectiveIcContext() = default;
    [[nodiscard]] usize dates() const noexcept;
    [[nodiscard]] usize instruments() const noexcept;
    [[nodiscard]] const ObjectiveIcConfig &config() const noexcept;
    [[nodiscard]] std::string_view identity_sha256() const noexcept;
    [[nodiscard]] u64 bytes() const noexcept;
    [[nodiscard]] u64 per_signal_working_bytes() const noexcept;
private:
    std::shared_ptr<const objective_ic_detail::Context> data_;
    friend core::Result<ObjectiveIcContext> prepare_objective_ic(
        const alpha::Panel &, const data::ExposurePanel &, const ObjectiveIcInputs &,
        const ObjectiveIcConfig &);
    friend core::Result<ObjectiveIcScratch> prepare_objective_ic_scratch(const ObjectiveIcContext &);
    friend core::Result<ObjectiveIcResult> evaluate_objective_ic(
        std::span<const f64>, const ObjectiveIcContext &, ObjectiveIcScratch &);
};

// One worker-owned scratch. Residuals are full-calendar [date][instrument],
// NaN outside valid decision support. Values use a positive per-date numerical
// scaling of the original signal, preserving ranks (not original signal units).
// The last call's diagnostics/series remain borrowed until the next evaluation.
class ObjectiveIcScratch {
public:
    ObjectiveIcScratch();
    ~ObjectiveIcScratch();
    ObjectiveIcScratch(ObjectiveIcScratch &&) noexcept;
    ObjectiveIcScratch &operator=(ObjectiveIcScratch &&) noexcept;
    ObjectiveIcScratch(const ObjectiveIcScratch &) = delete;
    ObjectiveIcScratch &operator=(const ObjectiveIcScratch &) = delete;
    [[nodiscard]] std::span<const f64> residuals() const noexcept;
    [[nodiscard]] std::span<const f64> rank_ic_series(usize horizon_index) const noexcept;
    [[nodiscard]] std::span<const ObjectiveIcDate> date_diagnostics() const noexcept;
private:
    std::unique_ptr<objective_ic_detail::Scratch> data_;
    friend core::Result<ObjectiveIcScratch> prepare_objective_ic_scratch(const ObjectiveIcContext &);
    friend core::Result<ObjectiveIcResult> evaluate_objective_ic(
        std::span<const f64>, const ObjectiveIcContext &, ObjectiveIcScratch &);
};

// Strict dated I2 support; requires all 7 continuous descriptors, positive PIT
// cap and FF49. Statistical WLS minimizes sum sqrt(cap)*(signal-X beta)^2, so
// scaled design row multipliers are cap^(1/4). Intercept plus industry contrasts
// (lowest observed category omitted); further rank deficiency is unavailable.
// Fits use only decision-time support/finite signal, NEVER forward-label masks.
// Labels h={21,63,126}: close[d+delay+h]/close[d+delay]-1, both prices present and
// finite positive, endpoint < maturity_end and window_end. Context owns captured
// labels/support; no input span may dangle. It retains immutable exposure ownership.
// Budget includes retained exposure panel, cached rows/labels and declared worker
// scratch; caller's input price Panel/VM buffers remain separately owned.
[[nodiscard]] core::Result<ObjectiveIcContext> prepare_objective_ic(
    const alpha::Panel &, const data::ExposurePanel &, const ObjectiveIcInputs &,
    const ObjectiveIcConfig & = {});
[[nodiscard]] core::Result<ObjectiveIcScratch> prepare_objective_ic_scratch(const ObjectiveIcContext &);

// Tied Spearman is computed on finite mature evaluation pairs after residual
// formation. Missing dates stay missing; only HAC influence entries use zeros,
// preserving original calendar lags. HorizonAwareV3 uniform lag>=h-1 uses guarded
// Bartlett fallback; effective kernel is reported. Numerical residual dust is
// unavailable before ranking. Persistence uses consecutive calendar decisions.
// Half-life is a diagnostic log|mean IC(h)| fit on the SAME common valid dates,
// requiring consistent nonzero sign and negative fitted slope. No alpha admission,
// net objective, half-life penalty, or raw-screen/default consumer is installed.
[[nodiscard]] core::Result<ObjectiveIcResult> evaluate_objective_ic(
    std::span<const f64> signal, const ObjectiveIcContext &, ObjectiveIcScratch &);
} // namespace atx::engine::factory
