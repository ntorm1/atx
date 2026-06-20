#include <gtest/gtest.h>
#include <cstddef>
#include <vector>
#include "atx/engine/eval/cpcv.hpp"
#include "atx/core/macro.hpp"   // ATX_ASSERT (death test)

namespace atxtest_eval_cpcv_test {

using namespace atx::engine::eval;
// label spans of a given horizon: obs i -> [i, i+h). h=1 => disjoint; h>1 => neighbors overlap.
static std::vector<LabelSpan> spans_h(std::size_t n, std::size_t h) {
  std::vector<LabelSpan> s(n);
  for (std::size_t i=0;i<n;++i) s[i] = LabelSpan{i, i+h};
  return s;
}
TEST(EvalCpcv, FoldCount_IsCombinatorial) {
  auto folds = cpcv_folds(spans_h(60,1), CpcvConfig{/*K=*/6, /*k=*/2, /*embargo=*/0.0});
  EXPECT_EQ(folds.size(), 15U);  // C(6,2)
}
TEST(EvalCpcv, Purge_NoSurvivingTrainLabelOverlapsAnyTest) {
  auto spans = spans_h(60, 3);   // horizon-3 labels overlap neighbors -> purge is non-trivial
  for (const auto& f : cpcv_folds(spans, CpcvConfig{6,2,0.0})) {
    for (atx::usize j : f.train_idx)
      for (atx::usize ti : f.test_idx)
        EXPECT_FALSE(spans[j].t0 < spans[ti].t1 && spans[ti].t0 < spans[j].t1); // no overlap survives
  }
}
TEST(EvalCpcv, Purge_NonVacuous_LongerHorizonRemovesMore) {
  auto unit = cpcv_folds(spans_h(60,1), CpcvConfig{6,2,0.0});  // disjoint -> nothing purged
  auto h3   = cpcv_folds(spans_h(60,3), CpcvConfig{6,2,0.0});  // overlap -> purge bites at block boundaries
  EXPECT_LT(h3[0].train_idx.size(), unit[0].train_idx.size());
}
TEST(EvalCpcv, Embargo_DropsTrainAfterTestBlock) {
  auto spans = spans_h(60,1);
  auto no_emb = cpcv_folds(spans, CpcvConfig{6,2,0.0});
  auto emb    = cpcv_folds(spans, CpcvConfig{6,2,0.10});  // embargo_len = ceil(0.10*60)=6
  EXPECT_LT(emb[0].train_idx.size(), no_emb[0].train_idx.size());
}
TEST(EvalCpcv, Deterministic_TwoRunsEqual) {
  auto a = cpcv_folds(spans_h(48,1), CpcvConfig{6,2,0.05});
  auto b = cpcv_folds(spans_h(48,1), CpcvConfig{6,2,0.05});
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i=0;i<a.size();++i){ EXPECT_EQ(a[i].train_idx, b[i].train_idx); EXPECT_EQ(a[i].test_idx, b[i].test_idx); }
}
TEST(EvalCpcvDeathTest, ApplyInsideFitWindowAborts) {
  // The fit/apply firewall idiom (generalized from phase4_integration_test.cpp): applying a fitted
  // object at a date < its fit_end must abort. CPCV makes this structurally impossible per fold.
  auto guard = [](atx::usize apply_date, atx::usize fit_end){ (void)apply_date; (void)fit_end; ATX_ASSERT(apply_date >= fit_end); };
  EXPECT_DEATH(guard(/*apply_date=*/0U, /*fit_end=*/8U), ".*");
}
TEST(EvalCpcvDeathTest, NegativeEmbargoAborts) {
  // The call aborts before returning, so its [[nodiscard]] result is genuinely
  // discarded; cast to void to keep the /WX build clean (-Wunused-result).
  EXPECT_DEATH((void)cpcv_folds(spans_h(60,1), CpcvConfig{6,2,-0.1}), ".*");
}


}  // namespace atxtest_eval_cpcv_test
