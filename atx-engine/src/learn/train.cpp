#include "atx/engine/learn/train.hpp"

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

} // namespace atx::engine::learn
