#pragma once

// preference_source.hpp -- the seam between a combined alpha and the equity book.
//
// A PreferenceSource turns the combine stage's output (a combo PanelArtifact
// whose `alpha` field holds one combined signal per evaluation row and
// instrument) into per-decision preference vectors for the equity allocation.
// It replaces the book's hard dependency on one fixed signal recipe: any combo
// on the evaluation axes can drive the book.
//
// Contract:
//   * The combo must carry exactly the evaluation panel's axes (instrument ids,
//     original indices and session keys), checked by require_same_panel_axes.
//   * pit_cutoff is the information cutoff (inclusive, session-key units): a
//     decision row whose session key is after it is refused, so a caller cannot
//     read a preference the research snapshot was not allowed to see.
//   * preference(row) reads ONLY that row. Missing (NaN) signals mean "no view"
//     and become 0. The finite entries are demeaned over the names with a view
//     and scaled to sum |p| == target_gross; a row with no dispersion is flat.
//   * The source owns a copy of the combo values; the artifact may be released.

#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "panel_artifact.hpp"

namespace atx::impl {

class PreferenceSource {
public:
    [[nodiscard]] static atx::core::Result<PreferenceSource>
    from_combo(const PanelArtifact &combo, const PanelIdentity &evaluation, atx::i64 pit_cutoff,
               atx::f64 target_gross = 1.0, std::string_view field = "alpha");

    // Cross-sectional preference for one evaluation decision row.
    [[nodiscard]] atx::core::Result<std::vector<atx::f64>> preference(atx::usize decision_row) const;

    [[nodiscard]] atx::usize rows() const noexcept { return keys_.size(); }
    [[nodiscard]] atx::usize instruments() const noexcept { return instruments_; }
    [[nodiscard]] atx::i64 pit_cutoff() const noexcept { return pit_cutoff_; }

private:
    PreferenceSource() = default;
    std::vector<atx::f64> values_; // rows x instruments, row-major.
    std::vector<atx::i64> keys_;
    atx::usize instruments_{};
    atx::i64 pit_cutoff_{};
    atx::f64 target_gross_{1.0};
};

} // namespace atx::impl
