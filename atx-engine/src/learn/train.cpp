#include "atx/engine/learn/train.hpp"

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
