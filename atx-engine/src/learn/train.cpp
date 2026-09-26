#include "atx/engine/learn/train.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector> // std::vector

#include "atx/core/types.hpp" // usize, u16

#include "atx/engine/eval/cpcv.hpp"            // eval::LabelSpan, eval::CpcvFold
#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix

namespace atx::engine::learn {

[[nodiscard]] std::vector<eval::LabelSpan> date_label_spans(const FeatureMatrix &fm,
                                                            atx::u16 horizon) {
  std::vector<eval::LabelSpan> spans;
  // Rows are in (date, instrument) order, so row_date is non-decreasing — collect
  // each distinct date once, ascending, with no map / sort needed.
  atx::usize prev = fm.n_dates; // sentinel: no date equals n_dates
  for (const atx::usize d : fm.row_date) {
    if (d != prev) {
      const atx::usize t1 = (d + static_cast<atx::usize>(horizon) < fm.n_dates)
                                ? d + static_cast<atx::usize>(horizon)
                                : fm.n_dates;
      spans.push_back(eval::LabelSpan{d, t1});
      prev = d;
    }
  }
  return spans;
}

atx::core::Result<std::vector<eval::LabelSpan>>
date_label_spans_v2(std::span<const atx::usize> dates, atx::u16 horizon) {
  if (horizon == 0U)
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "learn DateV2: zero horizon");
  std::vector<eval::LabelSpan> out;
  atx::usize distinct = 0;
  for (atx::usize i = 0; i < dates.size(); ++i) {
    if (i != 0U && dates[i] < dates[i-1U])
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "learn DateV2: date order");
    if (i == 0U || dates[i] != dates[i-1U]) ++distinct;
  }
  out.reserve(distinct);
  const auto width = static_cast<atx::usize>(horizon) + 1U;
  for (const auto d : dates) {
    if (d > std::numeric_limits<atx::usize>::max() - width ||
        (!out.empty() && d < out.back().t0))
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "learn DateV2: date order/overflow");
    if (out.empty() || out.back().t0 != d) out.push_back({d, d + width});
  }
  return atx::core::Ok(std::move(out));
}

atx::core::Status validate_date_cpcv_inputs(const FeatureMatrix& fm,
    std::span<const atx::u16> horizons, const eval::CpcvConfig& cfg) {
  if (cfg.rule == eval::CpcvRule::ObservationV1) return atx::core::Ok();
  if (cfg.rule != eval::CpcvRule::DateV2 || horizons.empty() ||
      fm.row_valid.size() != fm.n_rows() || fm.row_inst.size() != fm.n_rows() ||
      fm.Y.size() != horizons.size() ||
      (fm.n_features != 0U && fm.n_rows() > std::numeric_limits<atx::usize>::max() / fm.n_features) ||
      fm.X.size() != fm.n_rows() * fm.n_features)
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "learn DateV2: invalid feature/label geometry");
  // Downstream date-IC/OOF arrays still use the declared dense date axis.
  // Refuse pathological sparse-axis declarations before those allocations.
  if (fm.n_dates > cfg.max_working_bytes / 128U)
    return atx::core::Err(atx::core::ErrorCode::OutOfRange, "learn DateV2: date-axis workspace budget");
  for (atx::usize h = 0; h < horizons.size(); ++h) {
    if (horizons[h] == 0U || fm.Y[h].size() != fm.n_rows() ||
        (!fm.label_horizons.empty() &&
         (fm.label_horizons.size() != horizons.size() || fm.label_horizons[h] != horizons[h])))
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "learn DateV2: label horizon mismatch");
  }
  for (atx::usize r = 0; r < fm.n_rows(); ++r)
    if (fm.row_date[r] >= fm.n_dates || fm.row_inst[r] >= fm.n_instruments ||
        (r != 0U && fm.row_date[r] < fm.row_date[r-1U]))
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "learn DateV2: invalid date/instrument axis");
  return atx::core::Ok();
}

atx::core::Result<eval::CpcvPlan>
learn_cpcv_plan(const FeatureMatrix& fm, atx::u16 horizon, const eval::CpcvConfig& cfg) {
  if (cfg.rule == eval::CpcvRule::ObservationV1)
    return eval::cpcv_plan(date_label_spans(fm, horizon), cfg);
  if (fm.row_valid.size() != fm.row_date.size())
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "learn DateV2: row geometry");
  for (const auto date : fm.row_date)
    if (date >= fm.n_dates)
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "learn DateV2: date outside axis");
  atx::usize distinct = 0;
  for (atx::usize i = 0; i < fm.row_date.size(); ++i)
    if (i == 0U || fm.row_date[i] != fm.row_date[i-1U]) ++distinct;
  if (distinct > cfg.max_working_bytes / 128U)
    return atx::core::Err(atx::core::ErrorCode::OutOfRange, "learn DateV2: span budget");
  ATX_TRY(auto spans, date_label_spans_v2(fm.row_date, horizon));
  return eval::cpcv_plan(spans, cfg);
}

namespace detail {

[[nodiscard]] std::vector<atx::usize> used_dates(const FeatureMatrix &fm) {
  std::vector<atx::usize> dates;
  atx::usize prev = fm.n_dates;
  for (const atx::usize d : fm.row_date) {
    if (d != prev) {
      dates.push_back(d);
      prev = d;
    }
  }
  return dates;
}

[[nodiscard]] std::vector<atx::usize> rows_for_dates(const FeatureMatrix &fm,
                                                     const std::vector<bool> &in_set) {
  std::vector<atx::usize> rows;
  for (atx::usize r = 0; r < fm.n_rows(); ++r) {
    if (in_set[fm.row_date[r]] && fm.row_valid[r] != 0) {
      rows.push_back(r);
    }
  }
  return rows;
}

} // namespace detail

[[nodiscard]] Folds expand_date_folds(const std::vector<eval::CpcvFold> &folds,
                                      const FeatureMatrix &fm) {
  const std::vector<atx::usize> dates = detail::used_dates(fm);
  Folds out;
  out.reserve(folds.size());
  for (const eval::CpcvFold &f : folds) {
    std::vector<bool> in_train(fm.n_dates, false);
    std::vector<bool> in_test(fm.n_dates, false);
    for (const atx::usize o : f.train_idx) {
      if (o < dates.size()) {
        in_train[dates[o]] = true;
      }
    }
    for (const atx::usize o : f.test_idx) {
      if (o < dates.size()) {
        in_test[dates[o]] = true;
      }
    }
    out.push_back(RowFold{detail::rows_for_dates(fm, in_train),
                          detail::rows_for_dates(fm, in_test)});
  }
  return out;
}

atx::core::Result<Folds> expand_date_folds_checked(
    const std::vector<eval::CpcvFold>& folds, const FeatureMatrix& fm,
    const eval::CpcvConfig& cfg) {
  if (cfg.rule == eval::CpcvRule::ObservationV1)
    return atx::core::Ok(expand_date_folds(folds, fm));
  atx::u64 remaining = cfg.max_working_bytes;
  const auto charge = [&](atx::u64 count, atx::u64 bytes) {
    if (bytes != 0U && count > remaining / bytes) return false;
    remaining -= count * bytes; return true;
  };
  if (!charge(folds.size(),128U) || !charge(fm.n_rows(),32U) ||
      (fm.n_rows() != 0U &&
       (folds.size() > remaining / fm.n_rows() / 32U)))
    return atx::core::Err(atx::core::ErrorCode::OutOfRange, "learn DateV2: expanded row-fold budget");
  const auto dates = detail::used_dates(fm);
  Folds out; out.reserve(folds.size());
  std::vector<atx::u8> membership(dates.size());
  for (const auto& fold : folds) {
    std::fill(membership.begin(),membership.end(),atx::u8{0});
    for (const auto o : fold.train_idx) {
      if (o >= dates.size()) return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "date fold: train ordinal");
      membership[o]=1;
    }
    for (const auto o : fold.test_idx) {
      if (o >= dates.size() || membership[o] != 0U)
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "date fold: test ordinal/overlap");
      membership[o]=2;
    }
    RowFold expanded;
    atx::usize ordinal=0;
    for (atx::usize row=0; row<fm.n_rows(); ++row) {
      while (ordinal<dates.size() && dates[ordinal]<fm.row_date[row]) ++ordinal;
      if (ordinal==dates.size() || dates[ordinal]!=fm.row_date[row] || row>=fm.row_valid.size())
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "date fold: invalid row date");
      if (fm.row_valid[row] == 0U) continue;
      if (membership[ordinal]==1U) expanded.train_rows.push_back(row);
      else if (membership[ordinal]==2U) expanded.test_rows.push_back(row);
    }
    out.push_back(std::move(expanded));
  }
  return atx::core::Ok(std::move(out));
}

atx::core::Status retain_cpcv_metadata(std::vector<eval::CpcvMetadata>& retained,
    eval::CpcvMetadata metadata, atx::u64 budget) {
  const auto charge = [&](const eval::CpcvMetadata& m) {
    const auto take = [&](atx::u64 n, atx::u64 bytes) {
      if (n > budget / bytes) return false;
      budget -= n * bytes; return true;
    };
    if (!take(1,256U) || !take(m.group_offsets.capacity(),sizeof(atx::usize)) ||
        !take(m.paths.capacity(),sizeof(std::vector<atx::usize>))) return false;
    for (const auto& path : m.paths)
      if (!take(path.capacity(),sizeof(atx::usize))) return false;
    return true;
  };
  for (const auto& prior : retained) if (!charge(prior))
    return atx::core::Err(atx::core::ErrorCode::OutOfRange, "learn DateV2: retained path budget");
  if (!charge(metadata))
    return atx::core::Err(atx::core::ErrorCode::OutOfRange, "learn DateV2: retained path budget");
  retained.push_back(std::move(metadata));
  return atx::core::Ok();
}

} // namespace atx::engine::learn
