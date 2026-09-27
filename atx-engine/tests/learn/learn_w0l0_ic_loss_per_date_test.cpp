// W0-L0 — IcLoss on date-grouped batches (plan v2 §7 W0-L0; finding L-08, IcLoss part).
//
// IcLoss used to compute one pooled Pearson over a shuffled minibatch that mixed dates,
// which rewards predicting the date-level mean (market timing) instead of the
// cross-section. Now:
//   * IcLoss::PerGroupMeanV2 (default) reduces as 1 - mean over dates of the per-date
//     correlation when row groups are set; PooledV1 (and "no groups") keep the old value;
//   * nn::train(..., RowGroups) builds minibatches of WHOLE dates and hands the loss the
//     batch's date labels; without groups it is byte-identical to the legacy trainer.

#include <algorithm> // std::min_element, std::max_element
#include <cmath>
#include <cstring> // std::memcmp
#include <map>
#include <memory>
#include <span>
#include <vector>

#include <Eigen/Dense>

#include <gtest/gtest.h>

#include "atx/core/random.hpp" // Xoshiro256pp
#include "atx/core/types.hpp"

#include "atx/core/linalg/linalg.hpp" // MatX

#include "atx/engine/learn/nn/layers.hpp"    // nn::Linear, nn::Identity
#include "atx/engine/learn/nn/loss.hpp"      // nn::IcLoss, nn::MseLoss
#include "atx/engine/learn/nn/module.hpp"    // nn::Sequential
#include "atx/engine/learn/nn/optimizer.hpp" // nn::Sgd
#include "atx/engine/learn/nn/trainer.hpp"   // nn::train, nn::RowGroups
#include "atx/engine/learn/train.hpp"        // seed_for

namespace atx_test_w0_l0_ic_loss_per_date {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
namespace learn = atx::engine::learn;
namespace nn = atx::engine::learn::nn;
namespace lin = atx::core::linalg;

[[nodiscard]] lin::MatX col(const std::vector<f64> &v) {
  lin::MatX m(static_cast<Eigen::Index>(v.size()), 1);
  for (usize i = 0; i < v.size(); ++i) {
    m(static_cast<Eigen::Index>(i), 0) = v[i];
  }
  return m;
}

// Two dates of four names. Date-level means move together (pred and target both high
// on date 1), but WITHIN each date the prediction ranks the names backwards.
const std::vector<f64> kPred{0.0, 1.0, 2.0, 3.0, 10.0, 11.0, 12.0, 13.0};
const std::vector<f64> kTarg{3.0, 2.0, 1.0, 0.0, 13.0, 12.0, 11.0, 10.0};
const std::vector<u32> kDate{0, 0, 0, 0, 1, 1, 1, 1};

TEST(LearnIcLossPerDate_Loss, PerDateMeanIgnoresDateLevelComovement) {
  nn::IcLoss pooled{1e-12, nn::IcReduction::PooledV1};
  const f64 lp = pooled.value(col(kPred), col(kTarg));
  EXPECT_LT(lp, 0.25) << "pooled Pearson is fooled by the date-level co-movement";

  nn::IcLoss per_date; // default PerGroupMeanV2
  per_date.set_row_groups(std::span<const u32>{kDate});
  EXPECT_DOUBLE_EQ(per_date.value(col(kPred), col(kTarg)), 2.0)
      << "each date's IC is -1, so L = 1 - (-1)";
  // PooledV1 ignores groups entirely.
  pooled.set_row_groups(std::span<const u32>{kDate});
  EXPECT_EQ(pooled.value(col(kPred), col(kTarg)), lp);
}

TEST(LearnIcLossPerDate_Loss, NoGroupsIsByteIdenticalToPooled) {
  atx::core::Xoshiro256pp rng{91};
  lin::MatX p(9, 2);
  lin::MatX t(9, 2);
  for (Eigen::Index c = 0; c < 2; ++c) {
    for (Eigen::Index r = 0; r < 9; ++r) {
      p(r, c) = rng.normal();
      t(r, c) = rng.normal();
    }
  }
  nn::IcLoss v2;
  nn::IcLoss v1{1e-12, nn::IcReduction::PooledV1};
  const f64 a = v2.value(p, t);
  const f64 b = v1.value(p, t);
  EXPECT_EQ(std::memcmp(&a, &b, sizeof(f64)), 0);
  const lin::MatX ga = v2.grad(p, t);
  const lin::MatX gb = v1.grad(p, t);
  EXPECT_EQ(std::memcmp(ga.data(), gb.data(), static_cast<usize>(ga.size()) * sizeof(f64)), 0);
}

TEST(LearnIcLossPerDate_Loss, GradMatchesFiniteDifferenceAndSkipsDegenerateDates) {
  atx::core::Xoshiro256pp rng{7};
  // Dates 0 and 2 are regular; date 1 has a constant target (degenerate -> excluded);
  // date 3 has a single row (excluded). Rows are interleaved (groups need not be
  // contiguous).
  const std::vector<u32> g{0, 2, 1, 0, 2, 1, 0, 2, 3, 1, 0, 2};
  lin::MatX p(12, 1);
  lin::MatX t(12, 1);
  for (Eigen::Index r = 0; r < 12; ++r) {
    p(r, 0) = rng.normal();
    t(r, 0) = (g[static_cast<usize>(r)] == 1U) ? 0.5 : rng.normal();
  }
  nn::IcLoss loss;
  loss.set_row_groups(std::span<const u32>{g});
  const lin::MatX grad = loss.grad(p, t);
  const f64 h = 1e-6;
  for (Eigen::Index r = 0; r < 12; ++r) {
    lin::MatX pp = p;
    lin::MatX pm = p;
    pp(r, 0) += h;
    pm(r, 0) -= h;
    const f64 fd = (loss.value(pp, t) - loss.value(pm, t)) / (2.0 * h);
    EXPECT_NEAR(grad(r, 0), fd, 1e-7) << "row " << r;
    const u32 gid = g[static_cast<usize>(r)];
    if (gid == 1U || gid == 3U) {
      EXPECT_EQ(grad(r, 0), 0.0) << "excluded date carries no gradient";
    }
  }
  // Mean of the two surviving dates' correlations.
  nn::IcLoss single;
  std::vector<f64> p0;
  std::vector<f64> t0;
  std::vector<f64> p2;
  std::vector<f64> t2;
  for (usize r = 0; r < g.size(); ++r) {
    if (g[r] == 0U) {
      p0.push_back(p(static_cast<Eigen::Index>(r), 0));
      t0.push_back(t(static_cast<Eigen::Index>(r), 0));
    } else if (g[r] == 2U) {
      p2.push_back(p(static_cast<Eigen::Index>(r), 0));
      t2.push_back(t(static_cast<Eigen::Index>(r), 0));
    }
  }
  const f64 rho0 = 1.0 - single.value(col(p0), col(t0));
  const f64 rho2 = 1.0 - single.value(col(p2), col(t2));
  EXPECT_NEAR(loss.value(p, t), 1.0 - 0.5 * (rho0 + rho2), 1e-14);
}

TEST(LearnIcLossPerDate_Loss, AllDatesDegenerateGivesNoSignal) {
  const std::vector<u32> g{0, 1, 2};
  nn::IcLoss loss;
  loss.set_row_groups(std::span<const u32>{g});
  const lin::MatX p = col({1.0, 2.0, 3.0});
  EXPECT_EQ(loss.value(p, p), 1.0);
  EXPECT_TRUE(loss.grad(p, p).isZero(0.0));
}

// ---------------------------------------------------------------------------
//  Trainer: whole-date minibatches
// ---------------------------------------------------------------------------

// A loss that records the group labels of every value()/grad() call (MSE numerics).
class SpyLoss final : public nn::Loss {
public:
  [[nodiscard]] f64 value(const lin::MatX &pred, const lin::MatX &target) override {
    value_groups.push_back(groups_);
    return mse_.value(pred, target);
  }
  [[nodiscard]] lin::MatX grad(const lin::MatX &pred, const lin::MatX &target) override {
    grad_groups.push_back(groups_);
    batch_rows.push_back(static_cast<usize>(pred.rows()));
    return mse_.grad(pred, target);
  }
  void set_row_groups(std::span<const u32> groups) override {
    groups_.assign(groups.begin(), groups.end());
  }
  std::vector<std::vector<u32>> value_groups;
  std::vector<std::vector<u32>> grad_groups;
  std::vector<usize> batch_rows;

private:
  nn::MseLoss mse_;
  std::vector<u32> groups_;
};

[[nodiscard]] nn::ModelFactory linear_factory(usize in) {
  return [in](u64 seed) -> std::unique_ptr<nn::Module> {
    auto seq = std::make_unique<nn::Sequential>();
    seq->add(std::make_unique<nn::Linear>(in, static_cast<usize>(1), /*bias=*/false));
    seq->add(std::make_unique<nn::Identity>());
    seq->build();
    atx::core::Xoshiro256pp rng{seed};
    for (f64 &p : seq->params()) {
      p = 0.01 * rng.normal();
    }
    return seq;
  };
}

// Date sizes 3,5,2,4,6,1 (21 rows), rows interleaved across dates.
[[nodiscard]] std::vector<u32> ragged_groups() {
  const std::vector<usize> sizes{3, 5, 2, 4, 6, 1};
  std::vector<u32> g;
  for (usize round = 0; round < 6U; ++round) {
    for (usize d = 0; d < sizes.size(); ++d) {
      if (round < sizes[d]) {
        g.push_back(static_cast<u32>(d));
      }
    }
  }
  return g;
}

TEST(LearnIcLossPerDate_Trainer, EveryMinibatchHoldsWholeDates) {
  const std::vector<u32> g = ragged_groups();
  ASSERT_EQ(g.size(), 21U);
  std::map<u32, usize> date_size;
  for (const u32 d : g) {
    ++date_size[d];
  }
  atx::core::Xoshiro256pp rng{5};
  lin::MatX x(21, 2);
  lin::MatX y(21, 1);
  for (Eigen::Index r = 0; r < 21; ++r) {
    x(r, 0) = rng.normal();
    x(r, 1) = rng.normal();
    y(r, 0) = rng.normal();
  }
  const std::vector<u32> gv{7, 7, 8, 8};
  const lin::MatX xv = x.topRows(4);
  const lin::MatX yv = y.topRows(4);
  nn::TrainConfig cfg;
  cfg.epochs = 3;
  cfg.batch_size = 7;
  cfg.ckpt_every = 1;
  cfg.ensemble_size = 2;
  cfg.master_seed = 3;
  nn::Sgd opt{0.05, 0.0};
  SpyLoss spy;
  const auto st = nn::train(linear_factory(2), opt, spy, x, y, xv, yv, cfg,
                            nn::RowGroups{std::span<const u32>{g}, std::span<const u32>{gv}});
  ASSERT_TRUE(st.has_value());
  ASSERT_FALSE(spy.grad_groups.empty());
  usize rows_seen = 0;
  for (usize b = 0; b < spy.grad_groups.size(); ++b) {
    const std::vector<u32> &bg = spy.grad_groups[b];
    ASSERT_EQ(bg.size(), spy.batch_rows[b]) << "the loss must see this batch's labels";
    std::map<u32, usize> in_batch;
    for (const u32 d : bg) {
      ++in_batch[d];
    }
    for (const auto &[d, n] : in_batch) {
      EXPECT_EQ(n, date_size[d]) << "batch " << b << " splits date " << d;
    }
    // A batch only exceeds batch_size when it is a single oversized date.
    if (bg.size() > cfg.batch_size) {
      EXPECT_EQ(in_batch.size(), 1U);
    }
    rows_seen += bg.size();
  }
  EXPECT_EQ(rows_seen, 21U * cfg.epochs * cfg.ensemble_size) << "every row once per epoch";
  // Every checkpoint pass scored the validation design with its own date labels.
  for (const std::vector<u32> &vg : spy.value_groups) {
    EXPECT_EQ(vg, gv);
  }
  // Labels are cleared on return.
  static_cast<void>(spy.value(y, y));
  EXPECT_TRUE(spy.value_groups.back().empty());
}

TEST(LearnIcLossPerDate_Trainer, NoGroupsIsByteIdenticalToLegacyTrainer) {
  atx::core::Xoshiro256pp rng{12};
  lin::MatX x(30, 3);
  lin::MatX y(30, 1);
  for (Eigen::Index r = 0; r < 30; ++r) {
    for (Eigen::Index c = 0; c < 3; ++c) {
      x(r, c) = rng.normal();
    }
    y(r, 0) = rng.normal();
  }
  nn::TrainConfig cfg;
  cfg.epochs = 6;
  cfg.batch_size = 8;
  cfg.ckpt_every = 2;
  cfg.ensemble_size = 2;
  cfg.master_seed = 44;
  nn::Adam o1{0.01};
  nn::Adam o2{0.01};
  nn::IcLoss l1;
  nn::IcLoss l2;
  const auto a = nn::train(linear_factory(3), o1, l1, x, y, x, y, cfg);
  const auto b = nn::train(linear_factory(3), o2, l2, x, y, x, y, cfg, nn::RowGroups{});
  ASSERT_TRUE(a.has_value() && b.has_value());
  ASSERT_EQ(a->size(), b->size());
  for (usize m = 0; m < a->size(); ++m) {
    ASSERT_EQ((*a)[m].size(), (*b)[m].size());
    EXPECT_EQ(std::memcmp((*a)[m].data(), (*b)[m].data(), (*a)[m].size() * sizeof(f64)), 0);
  }
}

TEST(LearnIcLossPerDate_Trainer, RejectsMismatchedGroupLengths) {
  const lin::MatX x = lin::MatX::Ones(6, 2);
  const lin::MatX y = lin::MatX::Ones(6, 1);
  const std::vector<u32> short_g{0, 0, 1};
  nn::TrainConfig cfg;
  cfg.epochs = 1;
  nn::Sgd opt{0.01, 0.0};
  nn::IcLoss loss;
  EXPECT_FALSE(nn::train(linear_factory(2), opt, loss, x, y, x, y, cfg,
                         nn::RowGroups{std::span<const u32>{short_g}, {}})
                   .has_value());
  EXPECT_FALSE(nn::train(linear_factory(2), opt, loss, x, y, x, y, cfg,
                         nn::RowGroups{{}, std::span<const u32>{short_g}})
                   .has_value());
}

// Fix pass 1 (review major): RowGroups{train = {}, val = gv} used to leave the epoch-0
// validation labels (length n_val) on the loss for every training batch, so IcLoss's
// ATX_CHECK(groups.size() == B) aborted — or, when a batch had exactly n_val rows,
// silently grouped it by the validation dates.
[[nodiscard]] nn::TrainConfig small_cfg(usize batch_size) {
  nn::TrainConfig cfg;
  cfg.epochs = 4;
  cfg.batch_size = batch_size;
  cfg.ckpt_every = 1;
  cfg.ensemble_size = 2;
  cfg.master_seed = 17;
  return cfg;
}

TEST(LearnIcLossPerDate_Trainer, UngroupedTrainWithGroupedValDoesNotAbort) {
  atx::core::Xoshiro256pp rng{23};
  lin::MatX x(20, 2);
  lin::MatX y(20, 1);
  for (Eigen::Index r = 0; r < 20; ++r) {
    x(r, 0) = rng.normal();
    x(r, 1) = rng.normal();
    y(r, 0) = 0.5 * x(r, 0) + rng.normal();
  }
  const std::vector<u32> gv{4, 4, 4, 9, 9, 9};
  const lin::MatX xv = x.bottomRows(6);
  const lin::MatX yv = y.bottomRows(6);
  const nn::RowGroups rg{{}, std::span<const u32>{gv}};

  // Real IcLoss, batch_size (8) != n_val (6), n_train 20 -> batches 8, 8, 4.
  nn::Sgd o1{0.05, 0.0};
  nn::IcLoss ic;
  const auto st = nn::train(linear_factory(2), o1, ic, x, y, xv, yv, small_cfg(8), rg);
  ASSERT_TRUE(st.has_value());
  EXPECT_EQ(st->size(), 2U);

  // Spy: every training batch sees "no groups"; every validation pass sees gv. Run with
  // batch_size == n_val too (the silent mis-grouping case) and != n_val.
  for (const usize bs : {usize{6}, usize{8}}) {
    nn::Sgd o2{0.05, 0.0};
    SpyLoss spy;
    const auto s2 = nn::train(linear_factory(2), o2, spy, x, y, xv, yv, small_cfg(bs), rg);
    ASSERT_TRUE(s2.has_value());
    ASSERT_FALSE(spy.grad_groups.empty());
    for (usize b = 0; b < spy.grad_groups.size(); ++b) {
      EXPECT_TRUE(spy.grad_groups[b].empty()) << "bs=" << bs << " batch " << b
                                              << " saw stale validation labels";
    }
    // 2 members x (1 baseline + 4 checkpoints) validation passes, all grouped by gv.
    ASSERT_EQ(spy.value_groups.size(), 2U * (1U + 4U));
    for (const std::vector<u32> &vg : spy.value_groups) {
      EXPECT_EQ(vg, gv);
    }
  }
}

// A real IcLoss (per-date by default) that records every value() it returns. The
// trainer only calls value() for checkpoint passes, so `values` is the sequence of
// validation losses the checkpoint was selected on.
class RecordingIcLoss final : public nn::Loss {
public:
  [[nodiscard]] f64 value(const lin::MatX &pred, const lin::MatX &target) override {
    const f64 v = ic_.value(pred, target);
    values.push_back(v);
    return v;
  }
  [[nodiscard]] lin::MatX grad(const lin::MatX &pred, const lin::MatX &target) override {
    return ic_.grad(pred, target);
  }
  void set_row_groups(std::span<const u32> groups) override { ic_.set_row_groups(groups); }
  std::vector<f64> values;

private:
  nn::IcLoss ic_;
};

TEST(LearnIcLossPerDate_Trainer, UngroupedTrainWithGroupedValSelectsOnPerDateLoss) {
  // Validation: 3 dates x 4 names with a large date-level offset in BOTH features and
  // the target, so the pooled and per-date IC of a prediction differ.
  atx::core::Xoshiro256pp rng{57};
  lin::MatX xv(12, 2);
  lin::MatX yv(12, 1);
  std::vector<u32> gv;
  for (Eigen::Index r = 0; r < 12; ++r) {
    const f64 level = 4.0 * static_cast<f64>(r / 4);
    xv(r, 0) = level + rng.normal();
    xv(r, 1) = level + rng.normal();
    yv(r, 0) = level + xv(r, 0) - xv(r, 1) + 0.3 * rng.normal();
    gv.push_back(static_cast<u32>(r / 4));
  }
  lin::MatX x(24, 2);
  lin::MatX y(24, 1);
  for (Eigen::Index r = 0; r < 24; ++r) {
    x(r, 0) = rng.normal();
    x(r, 1) = rng.normal();
    y(r, 0) = x(r, 0) - x(r, 1) + 0.5 * rng.normal();
  }
  nn::TrainConfig cfg = small_cfg(5); // batch 5 != n_val 12
  cfg.epochs = 10;
  cfg.ensemble_size = 1;
  nn::Sgd opt{0.2, 0.0};
  RecordingIcLoss loss;
  const auto st = nn::train(linear_factory(2), opt, loss, x, y, xv, yv, cfg,
                            nn::RowGroups{{}, std::span<const u32>{gv}});
  ASSERT_TRUE(st.has_value());
  ASSERT_EQ(loss.values.size(), 1U + cfg.epochs) << "baseline + one pass per epoch";
  const f64 lo = *std::min_element(loss.values.begin(), loss.values.end());
  const f64 hi = *std::max_element(loss.values.begin(), loss.values.end());
  EXPECT_GT(hi - lo, 1e-6) << "training moved the validation loss (non-vacuous)";

  // Re-score the kept state: its PER-DATE validation loss is the recorded minimum, and
  // its pooled loss is different, so the recorded criterion was grouped by gv.
  std::unique_ptr<nn::Module> m = linear_factory(2)(0U);
  m->state_from((*st)[0]);
  m->train(false);
  const lin::MatX pred = m->forward(xv);
  nn::IcLoss per_date;
  per_date.set_row_groups(std::span<const u32>{gv});
  nn::IcLoss pooled{1e-12, nn::IcReduction::PooledV1};
  const f64 kept_per_date = per_date.value(pred, yv);
  const f64 kept_pooled = pooled.value(pred, yv);
  EXPECT_EQ(kept_per_date, lo);
  EXPECT_GT(std::fabs(kept_pooled - kept_per_date), 1e-3);
}

TEST(LearnIcLossPerDate_Trainer, RejectsGroupedTrainWithUngroupedVal) {
  const std::vector<u32> g = ragged_groups();
  atx::core::Xoshiro256pp rng{31};
  lin::MatX x(21, 2);
  lin::MatX y(21, 1);
  for (Eigen::Index r = 0; r < 21; ++r) {
    x(r, 0) = rng.normal();
    x(r, 1) = rng.normal();
    y(r, 0) = rng.normal();
  }
  const nn::TrainConfig cfg = small_cfg(7);
  const nn::RowGroups rg{std::span<const u32>{g}, {}};
  {
    // A validation design without its dates: the checkpoint would pool mixed dates.
    nn::Sgd opt{0.05, 0.0};
    nn::IcLoss loss;
    const auto st = nn::train(linear_factory(2), opt, loss, x, y, x.topRows(5), y.topRows(5),
                              cfg, rg);
    ASSERT_FALSE(st.has_value());
    EXPECT_EQ(st.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
  {
    // No validation design: the checkpoint scores train, grouped by g — allowed.
    nn::Sgd opt{0.05, 0.0};
    SpyLoss spy;
    const lin::MatX x0(0, 2);
    const lin::MatX y0(0, 1);
    const auto st = nn::train(linear_factory(2), opt, spy, x, y, x0, y0, cfg, rg);
    ASSERT_TRUE(st.has_value());
    ASSERT_FALSE(spy.value_groups.empty());
    for (const std::vector<u32> &vg : spy.value_groups) {
      EXPECT_EQ(vg, g);
    }
  }
}

// The market-timing trap. Feature 0 is constant within a date and equals the date's
// target level; feature 1 carries the cross-sectional signal; feature 2 is
// cross-sectional noise. Per-date IC cannot see a date-constant feature (its gradient
// sums to zero inside every date), so date-grouped IC training leaves weight 0 where it
// started while it tilts the within-date weights toward feature 1. Pooled IC on
// mixed-date batches instead loads on the date level (market timing).
TEST(LearnIcLossPerDate_Trainer, DateGroupedIcDoesNotLearnMarketTiming) {
  atx::core::Xoshiro256pp rng{77};
  const usize n_dates = 12;
  const usize n_names = 6;
  lin::MatX x(static_cast<Eigen::Index>(n_dates * n_names), 3);
  lin::MatX y(static_cast<Eigen::Index>(n_dates * n_names), 1);
  std::vector<u32> g;
  for (usize d = 0; d < n_dates; ++d) {
    const f64 level = 3.0 * rng.normal();
    for (usize i = 0; i < n_names; ++i) {
      const auto r = static_cast<Eigen::Index>(d * n_names + i);
      const f64 xs = rng.normal();
      x(r, 0) = level;
      x(r, 1) = xs;
      x(r, 2) = rng.normal();
      y(r, 0) = level + 0.5 * xs + 0.1 * rng.normal();
      g.push_back(static_cast<u32>(d));
    }
  }
  nn::TrainConfig cfg;
  cfg.epochs = 40;
  cfg.batch_size = 18;
  cfg.ckpt_every = 0; // checkpoint only on the last epoch
  cfg.ensemble_size = 1;
  cfg.master_seed = 8;
  const auto init_model =
      linear_factory(3)(learn::seed_for(cfg.master_seed, "nn-ensemble", 0, 0));
  std::vector<f64> w_init;
  init_model->state_to(w_init);

  nn::Sgd og{0.002, 0.0};
  nn::IcLoss grouped_loss;
  const auto grouped =
      nn::train(linear_factory(3), og, grouped_loss, x, y, x, y, cfg,
                nn::RowGroups{std::span<const u32>{g}, std::span<const u32>{g}});
  nn::Sgd op{0.002, 0.0};
  nn::IcLoss pooled_loss{1e-12, nn::IcReduction::PooledV1};
  const auto pooled = nn::train(linear_factory(3), op, pooled_loss, x, y, x, y, cfg);
  ASSERT_TRUE(grouped.has_value() && pooled.has_value());
  const std::vector<f64> &wg = (*grouped)[0];
  const std::vector<f64> &wp = (*pooled)[0];
  EXPECT_LT(std::fabs(wg[0] - w_init[0]), 1e-9) << "date-constant feature must not move";
  EXPECT_GT(wg[1], 0.0) << "the cross-sectional signal gets a positive weight";
  EXPECT_GT(std::fabs(wg[1]), 3.0 * std::fabs(wg[2])) << "and dominates the noise feature";
  EXPECT_GT(std::fabs(wp[0] - w_init[0]), 1e-2) << "pooled IC loads on the date level";
}

} // namespace atx_test_w0_l0_ic_loss_per_date
