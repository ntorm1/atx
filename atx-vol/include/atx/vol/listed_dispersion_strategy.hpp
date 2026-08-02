#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/backtest.hpp"                   // Lot, MarketSnapshot, PortfolioState
#include "atx/vol/listed_dispersion_schedule.hpp" // ListedDispersionSchedule
#include "atx/vol/strategy.hpp"                   // IStrategy, HedgeSpec
#include "atx/vol/types.hpp"                      // Result, Status, Side

namespace atx::vol {

// How schedule replay reconciles each leg's live-surface mark against the frozen
// `leg.model_mark`.
//   ExactArchive: current behavior — a bit-exact gate against `leg.model_mark`,
//     replaying the frozen archive; any divergence fails the step.
//   Record: no gate — accept the live seed mark, for repricing the same frozen
//     definitions through a different query route (e.g. an interpolated
//     `QueryExecution::Configured`), recording divergence instead of failing.
enum class ScheduleMarkPolicy { ExactArchive, Record };

// WS-F F2 (BT-P1-1). Which price a scheduled leg actually FILLS at. The listed
// route already records the NBBO it selected on (`raw_bid`/`raw_ask`/`raw_mid`)
// but has always filled at `model_mark` — the fitted surface's own value — so any
// signal correlated with the fit's deviation from tradeable quotes (dispersion
// implied-correlation is exactly such a signal) booked that deviation as day-0
// PnL no market participant could capture.
//
// NOTE the fill price only reaches NAV when the engine is told to book the
// fill-vs-mark difference: set `RunConfig::book_entry_fill_slippage`. Without it
// the engine carries the book at its model mark and the fill/mark gap is invisible
// (it shows up as a NAV-vs-liquidation drift under `RunConfig::reconcile_nav`).
// Frictions are additive: a run using CrossSpread should normally set
// `FrictionModel::half_spread_bps` to 0, or it pays the spread twice.
//
// CLOSES are unaffected: the engine closes a rolled cohort at its model mark, and
// the schedule carries no quote for a leg on any date other than its own roll
// date, so a quote-side CLOSE is not derivable from this artifact. Documented,
// not silently approximated.
enum class ScheduleFillPolicy : std::uint8_t {
  ModelMark = 0,   // compatibility default — fill at the (frozen or live) model mark
  QuoteMid = 1,    // fill at the recorded NBBO mid
  CrossSpread = 2, // pay the ask on buys, hit the bid on sells
};

// The fill price for one scheduled leg under `policy`. Quote-side policies fail
// closed (NotFound) on a leg with no usable two-sided quote — bid <= 0, a crossed
// book, or a non-finite side — consistent with F1's fail-closed discipline.
[[nodiscard]] Result<double> listed_leg_fill_price(const ListedScheduleLeg &leg,
                                                   ScheduleFillPolicy policy);

// One leg whose live-surface mark diverged from its frozen schedule mark during a
// Record-policy replay step. `schedule_mark` is the frozen `leg.model_mark`;
// `live_mark` is the seed price re-derived from the surface this step.
struct MarkDivergence {
  std::uint32_t uid{0};
  double strike{0.0};
  std::int64_t expiry_ts_ns{0};
  Side side{Side::Call};
  double schedule_mark{0.0};
  double live_mark{0.0};
};

// Convert one validated roll to absolute-expiry lots without mutating strategy
// state. `first_lot_id` is applied in schedule order.
[[nodiscard]] Result<std::vector<Lot>>
materialize_listed_dispersion_roll(const ListedScheduleRoll &roll, std::int64_t valuation_ts_ns,
                                   std::uint64_t first_lot_id,
                                   ScheduleFillPolicy fill = ScheduleFillPolicy::ModelMark);

// Strategy adapter over an immutable, externally persisted listed-contract
// schedule. Roll dates/marks are validated against each loaded archive before
// the existing cohort is replaced; no signal calculation or daily restriking is
// performed here. Current schedules are cold-authored and carry no pricing-route
// metadata, so fast schedules require a future versioned route field plus an
// explicit economic-error gate before they can safely relax ColdReference.
class ListedDispersionStrategy final : public IStrategy {
public:
  [[nodiscard]] static Result<ListedDispersionStrategy>
  create(ListedDispersionSchedule schedule, double delta_band = 0.0,
         ScheduleMarkPolicy policy = ScheduleMarkPolicy::ExactArchive,
         ScheduleFillPolicy fill = ScheduleFillPolicy::ModelMark);

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override;
  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id, const PriceOptions &price_options) override;
  [[nodiscard]] std::span<const FullGreekSeed> entry_risk_seeds() const noexcept override {
    return last_entry_seeds_;
  }
  // Under ScheduleMarkPolicy::Record, the legs whose live mark diverged from the
  // frozen schedule mark on the most recent step (cleared every step; always
  // empty under ExactArchive, which fails the step on the first divergence).
  [[nodiscard]] const std::vector<MarkDivergence> &last_mark_divergences() const noexcept {
    return last_mark_divergences_;
  }
  // ExactArchive replays the frozen archive marks exactly, so its economics stay
  // ColdReference (the engine gate keeps it off any fast query tier). Record
  // reprices the same frozen definitions live; returning Configured means "no
  // cold requirement" — the engine gate (backtest.cpp) only enforces anything
  // when this returns ColdReference, so a Record run may execute under ANY
  // QueryExecution, including an explicit ColdReference override (the canonical
  // projected-cold route relies on exactly that).
  [[nodiscard]] QueryExecution required_economic_execution() const noexcept override {
    return policy_ == ScheduleMarkPolicy::Record ? QueryExecution::Configured
                                                 : QueryExecution::ColdReference;
  }

  [[nodiscard]] HedgeSpec hedge_spec() const override { return hedge_; }
  // F5 (BT-T2): the schedule enumerates every uid this strategy will ever touch,
  // so the engine can subset-deserialize each date instead of loading the whole
  // board. Computed once at `create` in ascending uid order (deterministic, and
  // independent of roll/leg order in the artifact).
  //
  // BORROW of a vector this strategy owns. Unlike `entry_risk_seeds` (whose
  // single-step borrow rule is stated on the `IStrategy` base) this one is
  // STABLE: it is computed once at `create` and no `on_step` rewrites it, so the
  // span is valid for the strategy's whole run and the engine may hold it across
  // steps. Destroying the strategy, or moving it, invalidates it — the engine
  // reads it from the stepping thread like every other accessor here. Same rule
  // applies to `schedule()`'s reference and `last_mark_divergences()`'s vector
  // reference, except that the divergence list IS cleared and rebuilt every step.
  [[nodiscard]] std::span<const std::uint32_t> referenced_uids() const noexcept override {
    return referenced_uids_;
  }
  [[nodiscard]] ScheduleFillPolicy fill_policy() const noexcept { return fill_; }
  [[nodiscard]] const ListedDispersionSchedule &schedule() const noexcept { return schedule_; }
  [[nodiscard]] bool all_rolls_consumed() const noexcept {
    return next_roll_ == schedule_.rolls.size();
  }
  [[nodiscard]] std::size_t next_roll_index() const noexcept { return next_roll_; }

  // Relative tolerance for the entry-mark cross-check against the loaded archive.
  // Defaults to the SHARED kListedEntryMarkTolerance so this guard and the
  // reconciliation guard agree by construction; 0.0 restores a bit-exact compare.
  [[nodiscard]] double entry_mark_tolerance() const noexcept { return entry_mark_tolerance_; }
  void set_entry_mark_tolerance(double tol) noexcept { entry_mark_tolerance_ = tol; }

private:
  ListedDispersionStrategy(ListedDispersionSchedule schedule, HedgeSpec hedge,
                           ScheduleMarkPolicy policy, ScheduleFillPolicy fill,
                           std::vector<std::uint32_t> referenced_uids) noexcept
      : schedule_{std::move(schedule)}, hedge_{hedge}, policy_{policy}, fill_{fill},
        referenced_uids_{std::move(referenced_uids)} {}

  ListedDispersionSchedule schedule_{};
  HedgeSpec hedge_{};
  ScheduleMarkPolicy policy_{ScheduleMarkPolicy::ExactArchive};
  ScheduleFillPolicy fill_{ScheduleFillPolicy::ModelMark};
  std::size_t next_roll_{0};
  double entry_mark_tolerance_{kListedEntryMarkTolerance};
  std::vector<std::uint32_t> referenced_uids_{}; // F5: ascending, deduped
  std::vector<FullGreekSeed> last_entry_seeds_{};
  std::vector<MarkDivergence> last_mark_divergences_{};
};

} // namespace atx::vol
