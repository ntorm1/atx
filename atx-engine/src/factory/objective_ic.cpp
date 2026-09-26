#include "atx/engine/factory/objective_ic.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

#include <Eigen/QR>
#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/exposure_panel.hpp"
#include "atx/engine/eval/hac.hpp"

namespace atx::engine::factory {
namespace {
using core::Err;
using core::ErrorCode;
using core::Ok;
using core::Result;
using core::Status;
using Json = nlohmann::json;
constexpr std::array<usize, 3> kHorizons{21, 63, 126};
constexpr usize kContinuous = 7;
constexpr usize kMaxColumns = kContinuous + 49;
constexpr f64 kNan = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 kEps = std::numeric_limits<f64>::epsilon();

bool digest(std::string_view s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
bool charge(u64 &total, usize count, u64 each) {
    if (count > (std::numeric_limits<u64>::max() - total) / each) return false;
    total += static_cast<u64>(count) * each;
    return true;
}
bool equal(std::span<const i64> a, std::span<const i64> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}
Status hash_word(core::Sha256 &hash, u64 value) {
    std::array<std::byte, 8> bytes{};
    for (usize i = 0; i < 8; ++i) bytes[i] = static_cast<std::byte>((value >> (i * 8U)) & 255U);
    return hash.update(bytes);
}
Result<std::string> finish_hash(core::Sha256 &hash) {
    ATX_TRY(auto bytes, hash.finalize());
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto b : bytes) {
        const auto v = std::to_integer<unsigned>(b);
        result.push_back(digits[v >> 4U]); result.push_back(digits[v & 15U]);
    }
    return result;
}
Result<ObjectiveIcConfig> resolve(const ObjectiveIcConfig &input, usize dates) {
    auto cfg = input;
    if (!cfg.window_end) cfg.window_end = dates;
    if (!cfg.maturity_end) cfg.maturity_end = cfg.window_end;
    if (cfg.rule != ObjectiveIcRule::ResidualSqrtCapSpearmanV1 ||
        cfg.hac != ObjectiveIcHacRule::HorizonAwareV3 ||
        (cfg.missing != ObjectiveIcMissingPolicy::RefuseIncomplete &&
         cfg.missing != ObjectiveIcMissingPolicy::DropUnavailableNames) ||
        cfg.window_begin >= cfg.maturity_end || cfg.maturity_end > cfg.window_end ||
        cfg.window_end > dates || cfg.execution_delay < 1 || cfg.execution_delay > 4096 ||
        cfg.min_names < 3 || cfg.min_dates < 2 || !cfg.workers || cfg.workers > 256 ||
        !(cfg.min_member_fraction > 0 && cfg.min_member_fraction <= 1) ||
        !(cfg.min_paired_fraction > 0 && cfg.min_paired_fraction <= 1) ||
        !(cfg.min_date_fraction > 0 && cfg.min_date_fraction <= 1) || !cfg.max_working_bytes)
        return Err(ErrorCode::InvalidArgument, "objective IC: invalid version/window/coverage/budget");
    return cfg;
}

f64 correlation(std::span<const f64> x, std::span<const f64> y) {
    if (x.size() != y.size() || x.size() < 2) return kNan;
    const auto n = static_cast<f64>(x.size());
    const f64 mx = std::accumulate(x.begin(), x.end(), 0.0) / n;
    const f64 my = std::accumulate(y.begin(), y.end(), 0.0) / n;
    f64 xx = 0, yy = 0, xy = 0;
    for (usize i = 0; i < x.size(); ++i) {
        const auto a = x[i] - mx, b = y[i] - my;
        xx += a * a; yy += b * b; xy += a * b;
    }
    if (!(xx > 0 && yy > 0)) return kNan;
    const auto r = xy / std::sqrt(xx) / std::sqrt(yy);
    return std::isfinite(r) ? std::clamp(r, -1.0, 1.0) : kNan;
}
void rank(std::span<const f64> values, std::span<f64> ranks, std::vector<usize> &order) {
    order.resize(values.size());
    std::iota(order.begin(), order.end(), usize{0});
    std::sort(order.begin(), order.end(), [&](usize a, usize b) {
        return values[a] < values[b] || (values[a] == values[b] && a < b);
    });
    for (usize first = 0; first < order.size();) {
        usize end = first + 1;
        while (end < order.size() && values[order[end]] == values[order[first]]) ++end;
        const f64 mid = 0.5 * (static_cast<f64>(first) + static_cast<f64>(end - 1));
        for (usize j = first; j < end; ++j) ranks[order[j]] = mid;
        first = end;
    }
}
} // namespace

namespace objective_ic_detail {
struct Row {
    data::ExposureCrossSection exposure;
    std::vector<u8> decision_allowed;
    bool available{};
    usize known_member_names{}, unknown_membership_names{};
};
struct Context {
    ObjectiveIcConfig cfg;
    usize dates{}, names{};
    data::ExposurePanel exposure_owner;
    std::vector<Row> rows;
    std::array<std::vector<f64>, 3> labels;
    std::string identity;
    u64 bytes{}, scratch_bytes{};
};
struct Scratch {
    std::string identity;
    std::vector<f64> residual;
    std::array<std::vector<f64>, 3> ic;
    std::array<std::vector<usize>, 3> paired;
    std::vector<ObjectiveIcDate> dates;
    std::vector<usize> rows, order;
    std::vector<f64> x, y, rx, ry, influence;
    Eigen::MatrixXd design, weighted;
    Eigen::VectorXd signal, multipliers, beta, fitted;
    Eigen::ColPivHouseholderQR<Eigen::MatrixXd> qr;
};
} // namespace objective_ic_detail

namespace {
using Context = objective_ic_detail::Context;
using Scratch = objective_ic_detail::Scratch;

Status check_axes(const alpha::Panel &prices, const data::ExposurePanel &exposures,
                  const ObjectiveIcInputs &in) {
    const usize t = prices.dates(), n = prices.instruments();
    if (!t || !n || n > std::numeric_limits<usize>::max() / t ||
        t > static_cast<usize>(std::numeric_limits<Eigen::Index>::max()) ||
        n > static_cast<usize>(std::numeric_limits<Eigen::Index>::max()) ||
        exposures.dates() != t || exposures.instruments() != n ||
        in.session_keys.size() != t || in.decision_times_ns.size() != t ||
        in.mark_times_ns.size() != t || in.instrument_ids.size() != n ||
        !equal(in.session_keys, exposures.session_keys()) ||
        !equal(in.decision_times_ns, exposures.decision_times_ns()) ||
        !equal(in.instrument_ids, exposures.instrument_ids()) ||
        in.instrument_namespace != exposures.instrument_namespace() ||
        !digest(in.expected_exposure_axis_sha256) || !digest(in.expected_exposure_content_sha256) ||
        !digest(in.price_source_sha256) || in.expected_exposure_axis_sha256 != exposures.axis_sha256() ||
        in.expected_exposure_content_sha256 != exposures.content_sha256() ||
        in.price_field.empty() || in.price_field.size() > 128 ||
        (!in.decision_membership.empty() && in.decision_membership.size() != t * n) ||
        (!in.bad_return_prefix.empty() && in.bad_return_prefix.size() != t * n))
        return Err(ErrorCode::InvalidArgument, "objective IC: explicit axes/source/support mismatch");
    usize price_parents = 0;
    for (const auto &p : exposures.parents()) if (p.role == "prices") {
        if (p.sha256 != in.price_source_sha256)
            return Err(ErrorCode::InvalidArgument, "objective IC: price-source parent differs");
        ++price_parents;
    }
    if (price_parents != 1)
        return Err(ErrorCode::InvalidArgument, "objective IC: exactly one prices parent required");
    for (usize d = 0; d < t; ++d) {
        if (in.mark_times_ns[d] <= 0 || in.decision_times_ns[d] <= in.mark_times_ns[d] ||
            (d + 1 < t && in.decision_times_ns[d] >= in.mark_times_ns[d + 1]))
            return Err(ErrorCode::InvalidArgument, "objective IC: marks/decision timing is not causal");
    }
    return Ok();
}
Status budget(Context &c) {
    const auto cells = c.dates * c.names; // checked at the input boundary
    c.bytes = c.exposure_owner.bytes();
    c.scratch_bytes = 4096;
    if (!charge(c.bytes, cells, 128) || !charge(c.bytes, c.dates, 2048) ||
        !charge(c.bytes, 1, 4096) || !charge(c.scratch_bytes, cells, sizeof(f64)) ||
        !charge(c.scratch_bytes, c.dates, 256) ||
        !charge(c.scratch_bytes, c.names, 12U * kMaxColumns * sizeof(f64) + 256U) ||
        c.bytes > c.cfg.max_working_bytes ||
        c.scratch_bytes > (c.cfg.max_working_bytes - c.bytes) / c.cfg.workers)
        return Err(ErrorCode::OutOfRange, "objective IC: retained context/worker budget exceeded");
    return Ok();
}

Result<std::string> capture(Context &c, const alpha::Panel &panel, const ObjectiveIcInputs &in) {
    ATX_TRY(const auto field, panel.field_id(in.price_field));
    const auto price = panel.field_all(field);
    if (price.size() != c.dates * c.names)
        return Err(ErrorCode::InvalidArgument, "objective IC: price extent mismatch");
    core::Sha256 hash;
    const auto &cfg = c.cfg;
    const Json recipe{{"rule", "ResidualSqrtCapSpearmanV1"}, {"horizons", kHorizons},
        {"begin", cfg.window_begin}, {"end", cfg.window_end}, {"maturity", cfg.maturity_end},
        {"delay", cfg.execution_delay}, {"min_names", cfg.min_names}, {"min_dates", cfg.min_dates},
        {"member_fraction", cfg.min_member_fraction}, {"paired_fraction", cfg.min_paired_fraction},
        {"date_fraction", cfg.min_date_fraction},
        {"missing", static_cast<unsigned>(cfg.missing)}, {"allow_synthetic", cfg.allow_synthetic_exposures},
        {"hac", "HorizonAwareV3-calendar-influence-v1"}, {"price_field", in.price_field},
        {"price_source_sha256", in.price_source_sha256}, {"exposure_axis", in.expected_exposure_axis_sha256},
        {"exposure_content", in.expected_exposure_content_sha256}, {"exposure_recipe", c.exposure_owner.recipe_sha256()},
        {"weights", "sqrt-cap-statistical;fourth-root-row;relative-rank64eps"},
        {"rank_ic", "paired-tied-Spearman-after-decision-residualization"},
        {"half_life", "same-common-dates-log-abs-mean-three-horizon-v1"}};
    const auto encoded = recipe.dump();
    ATX_TRY_VOID(hash.update(std::as_bytes(std::span(encoded.data(), encoded.size()))));
    c.rows.resize(c.dates);
    for (auto &labels : c.labels) labels.assign(c.dates * c.names, kNan);
    data::ExposureExtractConfig extract;
    extract.missing = cfg.missing == ObjectiveIcMissingPolicy::RefuseIncomplete
        ? data::MissingExposurePolicy::RefuseIncomplete : data::MissingExposurePolicy::DropUnavailableNames;
    extract.min_names = cfg.min_names;
    extract.min_member_fraction = cfg.min_member_fraction;
    extract.allow_synthetic = cfg.allow_synthetic_exposures;
    extract.max_working_bytes = cfg.max_working_bytes;
    for (usize d = cfg.window_begin; d < cfg.maturity_end; ++d) {
        ATX_TRY_VOID(hash_word(hash, static_cast<u64>(in.mark_times_ns[d])));
        ATX_TRY(const auto summary, c.exposure_owner.date_summary(d));
        auto row = data::extract_exposure_date(c.exposure_owner, d, in.expected_exposure_axis_sha256,
            in.expected_exposure_content_sha256, in.decision_times_ns[d], extract);
        if (!row && row.error().code() != ErrorCode::Unavailable) return Err(row.error());
        auto &saved = c.rows[d];
        saved.known_member_names = summary.known_member_names;
        saved.unknown_membership_names = summary.unknown_membership_names;
        if (row) {
            const auto count = row->original_slots.size();
            if (count > c.names || row->security_ids.size() != count ||
                row->market_cap_usd.size() != count || row->ff49.size() != count ||
                row->values.size() != count * kContinuous || row->descriptors != extract.descriptors)
                return Err(ErrorCode::InvalidArgument, "objective IC: extracted row geometry differs");
            for (usize r = 0; r < count; ++r) {
                if (row->ff49[r] < 1 || row->ff49[r] > 49 ||
                    !std::isfinite(row->market_cap_usd[r]) || !(row->market_cap_usd[r] > 0))
                    return Err(ErrorCode::InvalidArgument, "objective IC: invalid extracted cap/industry");
            }
            if (!std::all_of(row->values.begin(), row->values.end(), [](f64 v) { return std::isfinite(v); }))
                return Err(ErrorCode::InvalidArgument, "objective IC: nonfinite extracted exposure");
            saved.available = true;
            saved.exposure = std::move(*row);
            saved.decision_allowed.resize(saved.exposure.original_slots.size());
            for (usize r = 0; r < saved.exposure.original_slots.size(); ++r) {
                const auto i = saved.exposure.original_slots[r];
                if (i >= c.names || saved.exposure.security_ids[r] != in.instrument_ids[i])
                    return Err(ErrorCode::InvalidArgument, "objective IC: extracted row lost original axis");
                saved.decision_allowed[r] = panel.in_universe(d, i) &&
                    (in.decision_membership.empty() || in.decision_membership[d * c.names + i] != 0);
            }
        }
        for (usize i = 0; i < c.names; ++i) {
            const auto cell = d * c.names + i;
            if (!in.decision_membership.empty() && in.decision_membership[cell] > 1)
                return Err(ErrorCode::InvalidArgument, "objective IC: nonbinary decision restriction");
            if (!in.bad_return_prefix.empty() && d > cfg.window_begin &&
                in.bad_return_prefix[cell] < in.bad_return_prefix[cell - c.names])
                return Err(ErrorCode::InvalidArgument, "objective IC: nonmonotonic return guard");
            ATX_TRY_VOID(hash_word(hash, std::bit_cast<u64>(price[cell])));
            ATX_TRY_VOID(hash_word(hash, panel.in_universe(d, i) ? 1U : 0U));
            ATX_TRY_VOID(hash_word(hash, in.decision_membership.empty() ? 1U : in.decision_membership[cell]));
            ATX_TRY_VOID(hash_word(hash, in.bad_return_prefix.empty() ? 0U : in.bad_return_prefix[cell]));
        }
        for (usize k = 0; k < kHorizons.size(); ++k) {
            const auto remaining = cfg.maturity_end - d;
            if (cfg.execution_delay >= remaining || kHorizons[k] >= remaining - cfg.execution_delay) continue;
            const auto entry = d + cfg.execution_delay, end = entry + kHorizons[k];
            for (usize i = 0; i < c.names; ++i) {
                const auto a = entry * c.names + i, b = end * c.names + i;
                if (!panel.in_universe(entry, i) || !panel.in_universe(end, i) ||
                    !std::isfinite(price[a]) || !std::isfinite(price[b]) || !(price[a] > 0 && price[b] > 0) ||
                    (!in.bad_return_prefix.empty() && in.bad_return_prefix[b] != in.bad_return_prefix[a])) continue;
                const auto label = price[b] / price[a] - 1;
                if (std::isfinite(label)) c.labels[k][d * c.names + i] = label;
            }
        }
    }
    return finish_hash(hash);
}

void residualize(usize d, std::span<const f64> raw, const Context &c, Scratch &s) {
    auto &diag = s.dates[d];
    const auto &row = c.rows[d];
    diag.status = ObjectiveIcDateStatus::ExposureUnavailable;
    diag.known_member_names = row.known_member_names;
    diag.unknown_membership_names = row.unknown_membership_names;
    if (!row.available) return;
    const auto &e = row.exposure;
    diag.known_member_names = e.known_member_names;
    diag.unknown_membership_names = e.unknown_membership_names;
    diag.exposure_names = e.original_slots.size();
    s.rows.clear();
    f64 scale = 0, max_cap = 0;
    std::array<bool, 50> industries{};
    for (usize r = 0; r < e.original_slots.size(); ++r) {
        if (!row.decision_allowed[r]) continue;
        ++diag.decision_names;
        const auto v = raw[d * c.names + e.original_slots[r]];
        if (!std::isfinite(v)) continue;
        s.rows.push_back(r);
        scale = std::max(scale, std::abs(v));
        max_cap = std::max(max_cap, e.market_cap_usd[r]);
        industries[e.ff49[r]] = true;
    }
    diag.fitted_names = s.rows.size();
    const usize denominator = diag.known_member_names + diag.unknown_membership_names;
    diag.status = ObjectiveIcDateStatus::TooFewNames;
    if (s.rows.size() < c.cfg.min_names || !denominator ||
        static_cast<f64>(s.rows.size()) < c.cfg.min_member_fraction * static_cast<f64>(denominator)) return;
    std::array<usize, 50> industry_column{};
    usize base_industry = 0, columns = kContinuous + 1;
    for (usize i = 1; i <= 49; ++i) if (industries[i]) {
        if (!base_industry) base_industry = i;
        else industry_column[i] = columns++;
    }
    diag.design_columns = columns;
    diag.status = ObjectiveIcDateStatus::RankDeficient;
    if (s.rows.size() <= columns + 1) return;
    diag.status = ObjectiveIcDateStatus::ResidualDegenerate;
    if (!(scale > 0)) return;
    const auto m = static_cast<Eigen::Index>(s.rows.size());
    const auto p = static_cast<Eigen::Index>(columns);
    s.design.topLeftCorner(m, p).setZero();
    const f64 max_fourth = std::sqrt(std::sqrt(max_cap));
    diag.status = ObjectiveIcDateStatus::NumericalRangeUnsupported;
    for (usize r = 0; r < s.rows.size(); ++r) {
        const auto source = s.rows[r];
        const auto er = static_cast<Eigen::Index>(r);
        const auto value = raw[d * c.names + e.original_slots[source]];
        const f64 y = value / scale;
        const f64 mult = std::sqrt(std::sqrt(e.market_cap_usd[source])) / max_fourth;
        if (!std::isfinite(y) || (value != 0 && y == 0) ||
            !(mult >= std::sqrt(std::numeric_limits<f64>::min()) && mult <= 1)) return;
        s.signal[er] = y;
        s.multipliers[er] = mult;
        s.design(er, 0) = 1;
        for (usize k = 0; k < kContinuous; ++k)
            s.design(er, static_cast<Eigen::Index>(k + 1)) = e.values[source * kContinuous + k];
        if (e.ff49[source] != base_industry)
            s.design(er, static_cast<Eigen::Index>(industry_column[e.ff49[source]])) = 1;
    }
    // Column rescaling preserves the regression span and stabilizes rank decisions.
    for (Eigen::Index j = 0; j < p; ++j) {
        const f64 maximum = s.design.col(j).head(m).cwiseAbs().maxCoeff();
        if (!(maximum > 0) || !std::isfinite(maximum)) {
            diag.status = ObjectiveIcDateStatus::RankDeficient; return;
        }
        s.design.col(j).head(m) /= maximum;
    }
    for (Eigen::Index r = 0; r < m; ++r)
        s.weighted.row(r).head(p) = s.design.row(r).head(p) * s.multipliers[r];
    s.qr.setThreshold(64 * kEps * static_cast<f64>(std::max(m, p)));
    s.qr.compute(s.weighted.topLeftCorner(m, p));
    diag.rank = static_cast<usize>(s.qr.rank());
    if (s.qr.rank() != p) { diag.status = ObjectiveIcDateStatus::RankDeficient; return; }
    s.beta.head(p) = s.qr.solve((s.signal.head(m).array() * s.multipliers.head(m).array()).matrix());
    s.fitted.head(m).noalias() = s.design.topLeftCorner(m, p) * s.beta.head(p);
    if (!s.beta.head(p).allFinite() || !s.fitted.head(m).allFinite()) return;
    s.x.resize(s.rows.size());
    f64 maximum = 0, prediction = 0;
    for (Eigen::Index r = 0; r < m; ++r) {
        s.x[static_cast<usize>(r)] = s.signal[r] - s.fitted[r];
        maximum = std::max(maximum, std::abs(s.x[static_cast<usize>(r)]));
        prediction = std::max(prediction, std::abs(s.fitted[r]));
    }
    const f64 numerical_floor = 64 * kEps * static_cast<f64>(columns + 1) * (1 + prediction);
    if (!(maximum > numerical_floor)) { diag.status = ObjectiveIcDateStatus::ResidualDegenerate; return; }
    for (Eigen::Index j = 0; j < p; ++j) {
        f64 error = 0;
        for (Eigen::Index r = 0; r < m; ++r)
            error += s.design(r, j) * s.multipliers[r] * s.multipliers[r] * s.x[static_cast<usize>(r)];
        diag.max_weighted_orthogonality_error = std::max(diag.max_weighted_orthogonality_error,
                                                        std::abs(error) / static_cast<f64>(m));
    }
    for (usize r = 0; r < s.rows.size(); ++r)
        s.residual[d * c.names + e.original_slots[s.rows[r]]] = s.x[r];
    diag.status = ObjectiveIcDateStatus::Valid;
}

f64 paired_rank(std::span<const f64> x, std::span<const f64> y, Scratch &s, usize minimum,
                 f64 minimum_fraction, usize denominator, usize &paired) {
    s.x.clear(); s.y.clear();
    for (usize i = 0; i < x.size(); ++i) if (std::isfinite(x[i]) && std::isfinite(y[i])) {
        s.x.push_back(x[i]); s.y.push_back(y[i]);
    }
    paired = s.x.size();
    if (paired < minimum || !denominator ||
        static_cast<f64>(paired) < minimum_fraction * static_cast<f64>(denominator)) return kNan;
    s.rx.resize(paired); s.ry.resize(paired);
    rank(s.x, s.rx, s.order); rank(s.y, s.ry, s.order);
    return correlation(s.rx, s.ry);
}

ObjectiveIcHorizon aggregate(usize k, const Context &c, Scratch &s) {
    ObjectiveIcHorizon out;
    out.horizon = kHorizons[k];
    const auto available = c.cfg.maturity_end - c.cfg.window_begin;
    if (c.cfg.execution_delay >= available || out.horizon >= available - c.cfg.execution_delay) return out;
    out.calendar_dates = available - c.cfg.execution_delay - out.horizon;
    f64 sum = 0;
    for (usize r = 0; r < out.calendar_dates; ++r) {
        const auto d = c.cfg.window_begin + r;
        if (std::isfinite(s.ic[k][d])) {
            ++out.valid_dates; sum += s.ic[k][d]; out.paired_names += s.paired[k][d];
        }
    }
    if (!out.valid_dates) return out;
    out.mean = sum / static_cast<f64>(out.valid_dates);
    if (out.valid_dates < c.cfg.min_dates || out.calendar_dates <= out.horizon ||
        static_cast<f64>(out.valid_dates) < c.cfg.min_date_fraction * static_cast<f64>(out.calendar_dates)) return out;
    s.influence.resize(out.calendar_dates);
    for (usize r = 0; r < out.calendar_dates; ++r) {
        const f64 value = s.ic[k][c.cfg.window_begin + r];
        s.influence[r] = std::isfinite(value) ? value - out.mean : 0;
    }
    out.hac_lag = std::max(out.horizon - 1, eval::hac::newey_west_rule_of_thumb_lag(out.calendar_dates));
    f64 variance_sum = eval::hac::long_run_sum(s.influence, 0, eval::hac::Kernel::UniformV1, out.hac_lag);
    const f64 zero_lag = eval::hac::long_run_sum(s.influence, 0, eval::hac::Kernel::BartlettV1, 0);
    out.effective_kernel = ObjectiveIcKernel::Uniform;
    if (out.hac_lag == out.calendar_dates - 1 || !(variance_sum > 64 * kEps * zero_lag)) {
        variance_sum = eval::hac::long_run_sum(s.influence, 0, eval::hac::Kernel::BartlettV1, out.hac_lag);
        out.fell_back = true;
        out.effective_kernel = ObjectiveIcKernel::Bartlett;
    }
    const auto n = static_cast<f64>(out.valid_dates);
    const auto variance = variance_sum / (n * (n - 1));
    if (!(variance > 0) || !std::isfinite(variance)) return out;
    out.standard_error = std::sqrt(variance);
    out.t_stat = out.mean / out.standard_error;
    out.hac_ir = out.t_stat / std::sqrt(n);
    out.inference_defined = std::isfinite(out.t_stat) && std::isfinite(out.hac_ir);
    return out;
}
ObjectiveIcHalfLife half_life(const Context &c, const Scratch &s) {
    ObjectiveIcHalfLife out;
    for (usize d = c.cfg.window_begin; d < c.cfg.maturity_end; ++d) {
        if (!std::isfinite(s.ic[0][d]) || !std::isfinite(s.ic[1][d]) || !std::isfinite(s.ic[2][d])) continue;
        ++out.common_dates;
        for (usize k = 0; k < 3; ++k) out.common_mean_ic[k] += s.ic[k][d];
    }
    if (!out.common_dates) return out;
    for (auto &mean : out.common_mean_ic) mean /= static_cast<f64>(out.common_dates);
    if (out.common_dates < c.cfg.min_dates) return out;
    std::array<f64, 3> logs{};
    for (usize k = 0; k < 3; ++k) {
        if (out.common_mean_ic[k] == 0 || std::signbit(out.common_mean_ic[k]) != std::signbit(out.common_mean_ic[0])) return out;
        logs[k] = std::log(std::abs(out.common_mean_ic[k]));
    }
    const f64 xm = (21.0 + 63.0 + 126.0) / 3;
    const f64 ym = (logs[0] + logs[1] + logs[2]) / 3;
    f64 xx = 0, xy = 0, yy = 0;
    for (usize k = 0; k < 3; ++k) {
        const f64 x = static_cast<f64>(kHorizons[k]) - xm, y = logs[k] - ym;
        xx += x * x; xy += x * y; yy += y * y;
    }
    out.log_slope = xy / xx;
    if (!(out.log_slope < 0) || !(yy > 0)) return out;
    out.days = -std::log(2.0) / out.log_slope;
    out.r_squared = std::clamp(xy * xy / (xx * yy), 0.0, 1.0);
    out.defined = std::isfinite(out.days) && out.days > 0;
    return out;
}
} // namespace

usize ObjectiveIcContext::dates() const noexcept { return data_ ? data_->dates : 0; }
usize ObjectiveIcContext::instruments() const noexcept { return data_ ? data_->names : 0; }
const ObjectiveIcConfig &ObjectiveIcContext::config() const noexcept {
    static const ObjectiveIcConfig empty{};
    return data_ ? data_->cfg : empty;
}
std::string_view ObjectiveIcContext::identity_sha256() const noexcept { return data_ ? data_->identity : std::string_view{}; }
u64 ObjectiveIcContext::bytes() const noexcept { return data_ ? data_->bytes : 0; }
u64 ObjectiveIcContext::per_signal_working_bytes() const noexcept { return data_ ? data_->scratch_bytes : 0; }
ObjectiveIcScratch::ObjectiveIcScratch() = default;
ObjectiveIcScratch::~ObjectiveIcScratch() = default;
ObjectiveIcScratch::ObjectiveIcScratch(ObjectiveIcScratch &&) noexcept = default;
ObjectiveIcScratch &ObjectiveIcScratch::operator=(ObjectiveIcScratch &&) noexcept = default;
std::span<const f64> ObjectiveIcScratch::residuals() const noexcept { return data_ ? std::span<const f64>(data_->residual) : std::span<const f64>{}; }
std::span<const f64> ObjectiveIcScratch::rank_ic_series(usize h) const noexcept {
    return data_ && h < 3 ? std::span<const f64>(data_->ic[h]) : std::span<const f64>{};
}
std::span<const ObjectiveIcDate> ObjectiveIcScratch::date_diagnostics() const noexcept {
    return data_ ? std::span<const ObjectiveIcDate>(data_->dates) : std::span<const ObjectiveIcDate>{};
}
Result<ObjectiveIcContext> prepare_objective_ic(const alpha::Panel &prices,
    const data::ExposurePanel &exposures, const ObjectiveIcInputs &input, const ObjectiveIcConfig &cfg) {
    ATX_TRY_VOID(check_axes(prices, exposures, input));
    ATX_TRY(auto resolved, resolve(cfg, prices.dates()));
    auto c = std::make_shared<Context>();
    c->cfg = resolved; c->dates = prices.dates(); c->names = prices.instruments(); c->exposure_owner = exposures;
    ATX_TRY_VOID(budget(*c));
    ATX_TRY(c->identity, capture(*c, prices, input));
    ObjectiveIcContext result;
    result.data_ = std::move(c);
    return result;
}
Result<ObjectiveIcScratch> prepare_objective_ic_scratch(const ObjectiveIcContext &context) {
    if (!context.data_) return Err(ErrorCode::InvalidArgument, "objective IC: unprepared context");
    const auto &c = *context.data_;
    ObjectiveIcScratch result;
    result.data_ = std::make_unique<Scratch>();
    auto &s = *result.data_;
    s.identity = c.identity;
    s.residual.assign(c.dates * c.names, kNan); s.dates.resize(c.dates);
    for (auto &ic : s.ic) ic.assign(c.dates, kNan);
    for (auto &paired : s.paired) paired.assign(c.dates, 0);
    s.rows.reserve(c.names); s.order.reserve(c.names);
    s.x.reserve(c.names); s.y.reserve(c.names); s.rx.reserve(c.names); s.ry.reserve(c.names);
    s.influence.reserve(c.dates);
    const auto n = static_cast<Eigen::Index>(c.names), p = static_cast<Eigen::Index>(kMaxColumns);
    s.design.resize(n, p); s.weighted.resize(n, p);
    s.signal.resize(n); s.multipliers.resize(n); s.fitted.resize(n); s.beta.resize(p);
    return result;
}
Result<ObjectiveIcResult> evaluate_objective_ic(std::span<const f64> signal,
    const ObjectiveIcContext &context, ObjectiveIcScratch &scratch) {
    if (!context.data_ || !scratch.data_ || scratch.data_->identity != context.data_->identity ||
        signal.size() != context.data_->dates * context.data_->names)
        return Err(ErrorCode::InvalidArgument, "objective IC: signal/scratch/context mismatch");
    const auto &c = *context.data_;
    auto &s = *scratch.data_;
    std::fill(s.residual.begin(), s.residual.end(), kNan);
    std::fill(s.dates.begin(), s.dates.end(), ObjectiveIcDate{});
    for (auto &ic : s.ic) std::fill(ic.begin(), ic.end(), kNan);
    for (auto &paired : s.paired) std::fill(paired.begin(), paired.end(), 0);
    ObjectiveIcResult result;
    result.context_sha256 = c.identity;
    for (usize d = c.cfg.window_begin; d < c.cfg.maturity_end; ++d) {
        residualize(d, signal, c, s);
        const auto status = s.dates[d].status;
        result.residual_degenerate_dates += status == ObjectiveIcDateStatus::ResidualDegenerate;
        result.rank_deficient_dates += status == ObjectiveIcDateStatus::RankDeficient;
        if (status != ObjectiveIcDateStatus::Valid) continue;
        ++result.residual_valid_dates;
        const auto residual = std::span<const f64>(s.residual).subspan(d * c.names, c.names);
        for (usize k = 0; k < 3; ++k) s.ic[k][d] = paired_rank(residual,
            std::span<const f64>(c.labels[k]).subspan(d * c.names, c.names), s, c.cfg.min_names,
            c.cfg.min_paired_fraction, s.dates[d].fitted_names, s.paired[k][d]);
        if (d > c.cfg.window_begin && s.dates[d - 1].status == ObjectiveIcDateStatus::Valid) {
            usize paired = 0;
            const auto rho = paired_rank(residual,
                std::span<const f64>(s.residual).subspan((d - 1) * c.names, c.names), s,
                c.cfg.min_names, c.cfg.min_paired_fraction,
                std::max(s.dates[d].fitted_names, s.dates[d - 1].fitted_names), paired);
            if (std::isfinite(rho)) { ++result.persistence_pairs; result.mean_rank_autocorrelation += rho; }
        }
    }
    for (usize k = 0; k < 3; ++k) result.horizons[k] = aggregate(k, c, s);
    result.half_life = half_life(c, s);
    if (result.persistence_pairs) {
        result.persistence_defined = true;
        result.mean_rank_autocorrelation /= static_cast<f64>(result.persistence_pairs);
        if (result.mean_rank_autocorrelation >= 0 && 1 - result.mean_rank_autocorrelation > 64 * kEps) {
            result.persistence_holding_proxy = 1 / (1 - result.mean_rank_autocorrelation);
            result.persistence_holding_proxy_defined = std::isfinite(result.persistence_holding_proxy);
        }
    }
    return result;
}
} // namespace atx::engine::factory
