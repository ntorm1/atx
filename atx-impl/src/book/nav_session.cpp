// NAV replay: the session loop of the books (run_books).
// P9 C3 (task 1, move-only): moved unchanged from strategy_nav_replay.cpp.

#include "book/nav_replay_internal.hpp"

namespace atx::impl::strategy {
using namespace nav_book;
namespace {
// Bitwise nonzero: -0.0 counts, so a reported row set is exact (a plan of -0 is state).
bool nonzero_bits(f64 x) { return std::bit_cast<u64>(x) != 0; }
// --emit-holdings: the observed book's names with any state at the end of session t
// (a decision or execution session), in index order, to the sink. Reads only. With a
// declared hold band (v8 E-16, shared.emit_hold) a set rank in the state DECIDE at t read
// (shared.hold_in; empty before the band first ran: every name unset) is state too, and
// every row carries that state.
co::Status emit_holdings(const TargetReplayInput& x, const Book& b, const Construction& shared,
                         const BorrowTiers& tiers, const NavReplayDay& day, usize t,
                         std::vector<NavHolding>& rows, NavHoldingsSink& sink) {
  const auto& s = b.names; const auto& trace = *b.trace; const usize n = x.instruments;
  const auto& hold = shared.hold_in;
  const bool band = shared.emit_hold && hold.rank_set.size() == n && hold.desired_prev.size() == n;
  rows.clear();
  for (usize i = 0; i < n; ++i) {
    const auto fill = static_cast<NavFillStatus>(trace.fill[i]);
    const bool planned = day.decision && nonzero_bits(s.planned[i]);
    const bool ranked = band && std::isfinite(hold.rank_set[i]);
    if (!nonzero_bits(s.held[i]) && !planned && !s.active[i] && fill == NavFillStatus::None &&
        !ranked)
      continue;
    NavHolding h;
    h.index = i; h.instrument_id = x.instrument_ids[i];
    h.member = x.member[t * n + i] != 0; h.stale = s.held[i] != 0 && s.absent[i] != 0;
    if (tiers.active) { h.tier = tiers.tier[i]; h.tier_missing = tiers.missing[i]; }
    h.held_dollars = s.held[i]; h.held_weight = s.held[i] / b.nav_post;
    h.fill = fill; h.filled_dollars = trace.filled[i]; h.fill_cost_dollars = trace.cost[i];
    h.unfilled_dollars = trace.unfilled[i];
    h.desired = day.rebalance ? shared.desired[i] : nan;
    h.rule_weight = day.decision ? trace.rule[i] : nan;
    h.target_weight = day.decision ? s.planned[i] : nan;
    h.order_placed = day.decision && s.planned[i] != s.current[i];
    h.locate_blocked = day.decision && s.planned[i] != trace.rule[i];
    h.order_working = s.active[i] != 0;
    h.order_dollars = s.active[i] ? s.order[i] : nan;
    if (band) { h.rank_set = hold.rank_set[i]; h.desired_prev = hold.desired_prev[i]; }
    rows.push_back(h);
  }
  return sink.session(day, rows);
}
// A cadence decision: (t - begin) % cadence == 0, extended before begin (warm start) so
// the rebalance calendar of the scored rows never depends on the warm-up length.
bool cadence_day(usize t, usize begin, usize cadence) {
  return t >= begin ? (t - begin) % cadence == 0 : (begin - t) % cadence == 0;
}
// UTC calendar date of a session key (ns since the epoch), YYYY-MM-DD.
std::string session_date(i64 session_ns) {
  namespace ch = std::chrono;
  const ch::sys_time<ch::nanoseconds> at{ch::nanoseconds(session_ns)};
  const ch::year_month_day ymd{ch::floor<ch::days>(at)};
  const auto two = [](unsigned v) { return (v < 10 ? "0" : "") + std::to_string(v); };
  return std::to_string(static_cast<int>(ymd.year())) + '-' +
         two(static_cast<unsigned>(ymd.month())) + '-' + two(static_cast<unsigned>(ymd.day()));
}
// Review A-3: a warm start whose K sessions left every book flat on row score_begin (gross
// leverage 0 after that row's EXECUTE) built no book; its scored rows would be a flat start's
// with one more (zero) return row, so the run is refused.
tl::unexpected<co::Error> refuse_inert_warm_start(const TargetReplayInput& x, usize warm) {
  const usize begin = x.decision_begin;
  const i64 first = x.session_keys[begin];
  return co::Err(co::ErrorCode::InvalidArgument,
                 "nav replay: the warm start built no book: after --warm-start-sessions " +
                     std::to_string(warm) + " (warm-up role rows " +
                     std::to_string(begin - warm) + ".." + std::to_string(begin - 1) +
                     ") every book is flat on the first scored session " +
                     session_date(first) + " (session_ns " + std::to_string(first) +
                     ", role row " + std::to_string(begin) +
                     " = score_begin; gross leverage 0 after its EXECUTE): the warm-up placed "
                     "no fill that survived to score_begin");
}
// A fresh row t (every other field is filled by the session's MARK/EXECUTE/DECIDE).
NavReplayDay open_day(const TargetReplayInput& x, usize t, bool return_observation) {
  NavReplayDay day;
  day.session_index = t; day.session = x.session_keys[t];
  day.calendar_month = detail::calendar_month(day.session);
  day.return_observation = return_observation;
  return day;
}
// Warm start: the scoring boundary, after the warm-up MARK of row decision_begin. The book
// is resized to initial_nav (one factor on every dollar state: holdings, cash, working
// orders, delta anchors and written-off exposures; weights and marks are unchanged) and
// every reported accumulator restarts, so the result holds scored quantities only.
// deployment_index keeps a warm-up deployment (it then precedes the first row).
void start_scoring(const Ctx& c, Book& b) {
  const f64 scale = c.cfg.initial_nav / b.nav_pre;
  auto& s = b.names;
  for (auto* dollars : {&s.held, &s.order, &s.anchor, &s.written_exposure})
    for (f64& v : *dollars) v *= scale;
  b.cash *= scale; b.nav_pre = c.cfg.initial_nav;
  auto& r = b.result;
  r.events.clear();
  for (auto* bucket : {&r.gap_run_1, &r.gap_run_2_4, &r.gap_run_5_plus, &r.written_off,
                       &r.reappeared, &r.unresolved, &r.guarded})
    *bucket = NavBucket{};
  r.guard_sensitivity = 0; r.max_return_identity_error = 0; r.max_cash_book_error = 0;
  b.participation = ParticipationHistogram{};
  b.rates = RateStatistics{}; b.rates.reset(c.cfg.rate_min, c.cfg.rate_max);
}
} // namespace
namespace nav_book {
f64 seconds_since(std::chrono::steady_clock::time_point started) {
  return std::chrono::duration<f64>(std::chrono::steady_clock::now() - started).count();
}
} // namespace nav_book
namespace {
// Session t of run_books: the same for every book.
struct Session {
  usize t{}, start{}, begin{}, end{}, first_return{};
  bool execution{}, decision{}, scored{}, boundary{};
};
// A book's first phase of session t: its MARK (after the first session), the warm-start
// boundary, then its EXECUTE (else the no-trade carry of NAV). Book-owned state only.
co::Status open_book(const TargetReplayInput& x, const Ctx& c, Book& b, const BorrowTiers& tiers,
                     const LiquidityCache& cache, const Session& s, NavReplayDay& day) {
  auto& r = b.result;
  if (b.trace) b.trace->clear();
  day = open_day(x, s.t, s.t >= s.first_return);
  if (s.t > s.start) ATX_TRY_VOID(mark_session(c, b, tiers, s.t, day));
  if (s.boundary) { // the warm-up MARK is not a scored row: the base row starts here
    start_scoring(c, b);
    day = open_day(x, s.t, false);
  }
  day.pretrade_nav = b.nav_pre;
  day.pretrade_gross_dollars = gross_dollars(b.names);
  if (s.execution) {
    ATX_TRY_VOID(execute_orders(c, b, s.t, cache, day));
    if (r.deployment_index == s.end && day.traded_dollars > 0) r.deployment_index = s.t;
    day.one_way_turnover_gmv = day.pretrade_gross_dollars > 0
        ? day.traded_dollars / day.pretrade_gross_dollars : nan;
  } else {
    b.nav_post = b.nav_pre; b.pending_cost = 0;
  }
  return co::Ok();
}
// The shared part of decision t: formed when t is a cadence decision of some book. A book
// whose own cadence misses t plans a non-rebalance decision with the idle record, exactly
// as it would alone (where form_desired never runs for it).
struct SharedDecision {
  bool rebalance{};
  TierCensus census;
  ConstructionDay construction;
  // P9 C1: the decision liquidity of a rebalance decision (cost_v2::decision_liquidity over
  // [d - w, d)), formed once here when some book has v7 state; null otherwise.
  const cost_v2::DecisionLiquidity* liquidity{};
};
} // namespace
namespace nav_book {
// The record's size bound for `decisions` decisions of `names` names (every decision a cadence
// decision; tiered: two flag bytes per name).
u64 construction_tape_bytes(usize decisions, usize names, bool tiered) {
  const u64 per_name = sizeof(f64) + (tiered ? 2U : 0U);
  return u64{decisions} * (sizeof(ConstructionTape::Decision) + u64{names} * per_name);
}
} // namespace nav_book
namespace {
void record_construction(ConstructionTape& tape, usize t, bool cadence,
                         const Construction& shared, const BorrowTiers& tiers,
                         const SharedDecision& decided) {
  tape.decisions.push_back({t, cadence, decided.rebalance, decided.census, decided.construction});
  if (cadence)
    tape.desired.insert(tape.desired.end(), shared.desired.begin(), shared.desired.end());
  if (tiers.active) {
    tape.tier.insert(tape.tier.end(), tiers.tier.begin(), tiers.tier.end());
    tape.missing.insert(tape.missing.end(), tiers.missing.begin(), tiers.missing.end());
  }
}
} // namespace
namespace nav_book {
// The record's vectors reserved at construction_tape_bytes' bound once (review S9): the budget
// bounds their capacity too (no growth factor, no reallocation copy while recording).
void reserve_construction(ConstructionTape& tape, usize decisions, usize names, bool tiered) {
  tape.decisions.reserve(decisions);
  tape.desired.reserve(decisions * names);
  if (tiered) {
    tape.tier.reserve(decisions * names);
    tape.missing.reserve(decisions * names);
  }
}
} // namespace nav_book
namespace {
// Decision t of the published pass from the record (Internal on any mismatch: the record is
// of another calendar, so nothing may be restored from it).
co::Status restore_construction(ConstructionTape& tape, usize t, bool cadence,
                                Construction& shared, BorrowTiers& tiers,
                                SharedDecision& decided) {
  const auto broken = [] {
    return co::Err(co::ErrorCode::Internal,
                   "nav calibrate: the recorded construction does not match the published pass");
  };
  if (tape.next >= tape.decisions.size()) return broken();
  const auto& rec = tape.decisions[tape.next];
  if (rec.t != t || rec.cadence != cadence) return broken();
  const usize n = shared.desired.size();
  if (cadence) {
    if (tape.desired.size() - tape.next_desired < n) return broken();
    const auto from = tape.desired.begin() + static_cast<std::ptrdiff_t>(tape.next_desired);
    std::copy(from, from + static_cast<std::ptrdiff_t>(n), shared.desired.begin());
    tape.next_desired += n;
  }
  if (tiers.active) {
    if (tape.tier.size() - tape.next_tier < n) return broken();
    const auto at = static_cast<std::ptrdiff_t>(tape.next_tier);
    const auto width = static_cast<std::ptrdiff_t>(n);
    std::copy(tape.tier.begin() + at, tape.tier.begin() + at + width, tiers.tier.begin());
    std::copy(tape.missing.begin() + at, tape.missing.begin() + at + width,
              tiers.missing.begin());
    tape.next_tier += n;
  }
  decided.rebalance = rec.rebalance;
  decided.census = rec.census;
  decided.construction = rec.construction;
  ++tape.next;
  return co::Ok();
}
// A book's second phase of session t: its DECIDE (on a decision session), close, the
// final-session report, then (scored rows only) the observer and the row. Book-owned
// state only (the observed book alone touches the sink and `holdings`); per-name-v1 rates
// write the shared cache, so replay_books runs that rate on one thread.
co::Status close_book(const TargetReplayInput& x, const Ctx& c, Book& b,
                      const Construction& shared, const BorrowTiers& tiers,
                      LiquidityCache& cache, const Session& s, const SharedDecision& decided,
                      const Observer& observer, std::vector<NavHolding>& holdings,
                      NavReplayDay& day) {
  if (s.decision) {
    const bool mine = cadence_day(s.t, s.begin, c.cfg.target.cadence);
    const ConstructionDay idle{};
    ATX_TRY_VOID(plan_decision(c, b, shared, tiers, cache, s.t, mine && decided.rebalance,
                               mine ? decided.construction : idle, decided.liquidity, day));
    day.member_tiers = decided.census.members;
    day.member_missing_predictors = decided.census.missing;
  }
  ATX_TRY_VOID(close_day(c, b, day));
  if (s.t + 1 == s.end) ATX_TRY_VOID(report_unresolved(c, b, s.t));
  if (!s.scored) return co::Ok(); // warm-up rows are never reported
  if (b.trace && (s.decision || s.execution))
    ATX_TRY_VOID(emit_holdings(x, b, shared, tiers, day, s.t, holdings, *observer.sink));
  b.result.days.push_back(day);
  return co::Ok();
}
} // namespace
namespace nav_book {
// Sessions [begin - K, end) (K = warm_start_sessions; 0 without a warm start): MARK
// (t > begin - K) -> EXECUTE (begin - K < t <= end-2) -> DECIDE (t < end-2); rows t >=
// begin are reported. Row t reads data at rows <= t only. The books run in lockstep:
// each book performs exactly its own single-book sequence (mark, execute, decide, close)
// on its own state; only the decision's desired target and borrow tiers (and, with rate
// per-name-v1 or the liquidity cache, the session's liquidity windows) are formed once and
// shared, so every book is bit-identical to a replay on its own. The per-book phases run
// on `pool` when given (engine for_each_lane: lane-owned writes only, the lowest failing
// book's error). An observer only reads the observed book (its trace is written beside,
// never read by, the arithmetic). `stages` (--stage-timers) receives the construction and
// exposure seconds. `tape` (P9 C2, --calibrate-gross; null: every decision formed here, the
// path before C2): record mode appends every decision's shared construction, replay mode
// restores it instead of decide_construction.
co::Result<std::vector<NavReplayResult>> run_books(const TargetReplayInput& x,
                                                   const NavFinancingFields& fields,
                                                   std::span<const Ctx> ctxs,
                                                   const Observer& observer,
                                                   par::DetPool* pool, StageClock* stages,
                                                   ConstructionTape* tape) {
  const usize begin = x.decision_begin, end = x.decision_end, count = ctxs.size();
  // The shared construction's config (neutralization, price risk, guard) is every book's:
  // replay_books admits only variants that agree on it (with the ADV cap also on the aim
  // leverage: one call per leverage group, review A-1); the cadence may differ per book.
  const auto& target = ctxs.front().cfg.target;
  // The rate, order basis, locate-in-aim, cache and warm start are the base config's,
  // identical for every book.
  const auto& base = ctxs.front().cfg;
  const usize warm = base.warm_start_sessions, start = begin - warm; // validated: warm <= begin
  // A flat start's row begin+1 marks an empty book; a warm book earns from row begin+1.
  const usize first_return = begin + (warm ? 1U : 2U);
  const bool delta_basis = base.order_basis == NavOrderBasis::Delta;
  std::vector<std::unique_ptr<Book>> books;
  books.reserve(count);
  bool v7_books = false; // some book plans through the v7 seam (P9 C1)
  for (const auto& c : ctxs) {
    auto& b = *books.emplace_back(std::make_unique<Book>(x.instruments, delta_basis));
    b.cash = b.nav_pre = b.nav_post = c.cfg.initial_nav;
    b.result.deployment_index = end; b.result.days.reserve(end - begin);
    b.rates.reset(c.cfg.rate_min, c.cfg.rate_max);
    // P9 C1 (DEC-10): the book's own leverage rule and v7 state, made here on the calling
    // thread (the v7 extension is thread-local), each from a clean state.
    const auto& sc = c.cfg.scenario;
    b.label = sc.id + "+" + sc.financing.id;
    b.capacity = v7::capacity_book(sc);
    if (c.cfg.leverage.law != NavLeverageLaw::Fixed)
      b.leverage = std::make_unique<risk_target::BookLeverage>(
          risk_target::options_of(c.cfg.leverage), c.cfg.leverage.risk);
    b.v7_state = v7::make_book_state(sc);
    v7_books = v7_books || b.v7_state != nullptr;
  }
  cost_v2::DecisionLiquidity liquidity; // the rebalance decision's, shared (v7 books only)
  Construction shared(x.instruments, base.locate_in_aim);
  // v8 D-1: every decision's price exposures from the session ring (each session logged
  // once per replay); the bits of recomputing each window (the ring's contract: x's
  // prices are immutable for this call).
  if (target.neutralize != TargetNeutralize::None)
    enable_session_ring(shared.price.exposure,
                        PriceExposureInput{x.dates, x.instruments, x.close, x.raw_close,
                                           x.volume, x.present});
  // Tiers of the latest decision: MARK t reads those of decision t-1 (or earlier).
  BorrowTiers tiers(x.instruments, !fields.shares_out.empty());
  // per-name-v1 caches its decision windows too; --liquidity-cache only execution's.
  const bool per_name = base.rate == NavRateRule::PerNameV1;
  LiquidityCache cache(x.instruments, liquidity_cached(base), per_name);
  std::vector<NavHolding> holdings;
  if (observer.sink) {
    books[observer.book]->trace = std::make_unique<Trace>(x.instruments);
    holdings.reserve(x.instruments);
    shared.emit_hold = hold_band_declared(target); // v8 E-16: the export carries the band
  }
  std::vector<NavReplayDay> days(count);
  for (usize t = start; t < end; ++t) {
    Session s;
    s.t = t; s.start = start; s.begin = begin; s.end = end; s.first_return = first_return;
    s.execution = t > start && t + 2 <= end; s.decision = t + 2 < end;
    s.scored = t >= begin; s.boundary = warm && t == begin;
    const bool rate_decision = per_name && s.decision;
    if (cache.on() && (s.execution || rate_decision))
      fill_liquidity(ctxs.front(), books, t, s.execution, rate_decision, cache);
    // SAFETY (data races): each lane writes only book k and days[k]; tiers, cache and the
    // shared construction are written only here between the phases, on this thread.
    ATX_TRY_VOID(par::for_each_lane(pool, count, [&](usize k) {
      return open_book(x, ctxs[k], *books[k], tiers, cache, s, days[k]);
    }));
    SharedDecision decided;
    // v8 E-16: the band's state DECIDE at t reads, before this session's construction
    // advances it (copied into reused storage; --emit-holdings with a declared band only).
    if (shared.emit_hold) shared.hold_in = shared.state.hold;
    if (s.decision) {
      const bool cadence = std::any_of(ctxs.begin(), ctxs.end(), [&](const Ctx& c) {
        return cadence_day(t, begin, c.cfg.target.cadence);
      });
      const auto started = stages ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
      if (tape && tape->replay) {
        ATX_TRY_VOID(restore_construction(*tape, t, cadence, shared, tiers, decided));
      } else {
        ATX_TRY(decided.rebalance,
                decide_construction(x, fields, base, t, cadence, {}, shared, tiers,
                                    decided.census, decided.construction));
        if (tape) record_construction(*tape, t, cadence, shared, tiers, decided);
      }
      if (stages) stages->construction += seconds_since(started);
      // P9 C1: the v7 rules' decision liquidity, once for every book of a rebalance decision
      // (the same function and inputs each book's rule read before: x's rows [t - w, t)).
      if (v7_books && decided.rebalance) {
        cost_v2::decision_liquidity(x, t, base.liquidity_window, base.min_vol_pairs, liquidity);
        decided.liquidity = &liquidity;
      }
    }
    ATX_TRY_VOID(par::for_each_lane(pool, count, [&](usize k) {
      return close_book(x, ctxs[k], *books[k], shared, tiers, cache, s, decided, observer,
                        holdings, days[k]);
    }));
    // Review A-3: the warm-up must have built a book (row score_begin, EXECUTE included, of
    // some book not flat). Without a warm start there is no boundary: nothing is checked. P9
    // C1: the main books and the capacity books are checked apart, as their own passes were.
    if (s.boundary) {
      bool main_books = false, main_built = false, capacity_books = false;
      bool capacity_built = false;
      for (usize k = 0; k < count; ++k) {
        const bool built = days[k].gross_leverage > 0;
        if (books[k]->capacity) {
          capacity_books = true; capacity_built = capacity_built || built;
        } else {
          main_books = true; main_built = main_built || built;
        }
      }
      if ((main_books && !main_built) || (capacity_books && !capacity_built))
        return refuse_inert_warm_start(x, warm);
    }
  }
  // P9 C2: a restored pass consumed the whole record (else it was another calendar's).
  if (tape && tape->replay && tape->next != tape->decisions.size())
    return co::Err(co::ErrorCode::Internal,
                   "nav calibrate: the recorded construction has decisions the published pass "
                   "never reached");
  // Accumulated: a grid of several leverage groups (review A-1) calls run_books per group.
  if (stages) stages->exposures += shared.price.exposure.ring.seconds;
  std::vector<NavReplayResult> results;
  results.reserve(count);
  for (auto& book : books) {
    auto& b = *book; auto& r = b.result;
    r.participation_fills = b.participation.total();
    r.participation_p95 = b.participation.upper_quantile(.95);
    r.participation_max = b.participation.maximum();
    if (per_name) r.construction.rate_stats = b.rates.stats();
    if (b.v7_state) r.transfer = std::move(b.v7_state->tc); // P9 C1: the book's own records
    results.push_back(std::move(r));
  }
  return co::Ok(std::move(results));
}
} // namespace nav_book
} // namespace atx::impl::strategy
