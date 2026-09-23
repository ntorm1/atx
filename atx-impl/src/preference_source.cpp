#include "preference_source.hpp"

#include <cmath>
#include <span>
#include <string>
#include <utility>

namespace atx::impl {
namespace {
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
} // namespace

Result<PreferenceSource> PreferenceSource::from_combo(const PanelArtifact &combo,
                                                      const PanelIdentity &evaluation,
                                                      atx::i64 pit_cutoff, atx::f64 target_gross,
                                                      std::string_view field) {
    if (!std::isfinite(target_gross) || target_gross <= 0.0)
        return Err(ErrorCode::InvalidArgument, "preference source: target gross must be positive");
    ATX_TRY_VOID(require_same_panel_axes(combo.identity, evaluation));
    const auto &panel = combo.panel;
    ATX_TRY(auto id, panel.field_id(std::string(field)));
    const auto values = panel.field_all(id);
    if (panel.dates() != evaluation.session_keys.size() ||
        panel.instruments() != evaluation.instrument_ids.size() ||
        values.size() != panel.dates() * panel.instruments()) {
        return Err(ErrorCode::InvalidArgument, "preference source: combo shape differs from axes");
    }
    PreferenceSource source;
    source.values_.assign(values.begin(), values.end());
    source.keys_ = evaluation.session_keys;
    source.instruments_ = panel.instruments();
    source.pit_cutoff_ = pit_cutoff;
    source.target_gross_ = target_gross;
    return Ok(std::move(source));
}

Result<std::vector<atx::f64>> PreferenceSource::preference(atx::usize decision_row) const {
    if (decision_row >= keys_.size())
        return Err(ErrorCode::OutOfRange, "preference source: decision row outside combo axes");
    if (keys_[decision_row] > pit_cutoff_) {
        return Err(ErrorCode::InvalidArgument,
                   "preference source: decision row after information cutoff row=" +
                       std::to_string(decision_row));
    }
    const auto row = std::span<const atx::f64>(values_).subspan(decision_row * instruments_,
                                                                instruments_);
    atx::f64 sum = 0.0;
    atx::usize viewed = 0;
    for (const auto x : row) {
        if (std::isnan(x)) continue;
        if (!std::isfinite(x))
            return Err(ErrorCode::InvalidArgument, "preference source: infinite combo signal");
        sum += x;
        ++viewed;
    }
    std::vector<atx::f64> out(instruments_, 0.0);
    if (viewed == 0) return Ok(std::move(out));
    const auto mean = sum / static_cast<atx::f64>(viewed);
    atx::f64 gross = 0.0;
    for (atx::usize i = 0; i < instruments_; ++i) {
        if (std::isnan(row[i])) continue;
        out[i] = row[i] - mean;
        gross += std::abs(out[i]);
    }
    if (!std::isfinite(gross))
        return Err(ErrorCode::OutOfRange, "preference source: combo signal gross overflow");
    if (gross == 0.0) return Ok(std::vector<atx::f64>(instruments_, 0.0));
    const auto scale = target_gross_ / gross;
    for (auto &p : out) p *= scale;
    return Ok(std::move(out));
}

} // namespace atx::impl
