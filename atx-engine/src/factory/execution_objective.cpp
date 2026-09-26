#include "atx/engine/factory/execution_objective.hpp"
#include "execution_cash_claim_internal.hpp"
#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/cost/cost_surface.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>
#include <vector>

namespace atx::engine::factory {
namespace co = atx::core;
using atx::f64;
using atx::u64;
using atx::usize;
namespace execution_objective_detail {
struct Context {
  ExecutionObjectiveConfig config;
  WeightPolicy policy;
  const alpha::Panel *panel_token{};
  const f64 *price_token{};
  usize dates{}, instruments{}, decision_end{}, first_realization{}, realization_end{};
  u64 owned_bytes{}, call_bytes{}, output_bytes{}, scratch_bytes{};
  std::vector<f64> prices;
  std::vector<atx::u8> member, panel_member;
  std::vector<atx::u32> guard, groups;
  std::vector<atx::i64> marks, decisions;
  std::vector<u64> ids;
  std::vector<cost::CostSurface> snapshots;
  std::vector<ExecutionCashClaimEvent> cash_events;
  std::vector<usize> cash_event_names, cash_event_periods, cash_event_for_name;
  std::string identity;
};
} // namespace execution_objective_detail
namespace {
using Context = execution_objective_detail::Context;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 ns_per_day = 86'400'000'000'000.0;

// Error-only formatting: the immutable context has mark/decision clocks rather
// than calendar session labels. Report those exact timestamps, never infer a
// session date by rounding an arbitrary caller's clock. No successful-path math
// or serialized recipe changes. Geometry was checked during preparation.
std::string price_failure(const Context& c, std::string_view reason, usize t, usize i,
                          f64 held, bool held_interval, usize decision = 0,
                          f64 requested = 0, f64 filled = 0) {
  const auto k = t * c.instruments + i;
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17) << reason << " [date_index=" << t
      << " mark_time_ns=" << c.marks[t] << " instrument_index=" << i
      << " instrument_id=" << c.ids[i] << " source_present=" << static_cast<unsigned>(c.panel_member[k])
      << " price=" << c.prices[k] << " held_dollars=" << held;
  if (held_interval) {
    const auto previous = k - c.instruments;
    out << " previous_date_index=" << (t - 1) << " previous_mark_time_ns=" << c.marks[t - 1]
        << " previous_source_present=" << static_cast<unsigned>(c.panel_member[previous])
        << " previous_price=" << c.prices[previous]
        << " guard_crossed=" << (c.config.guard_returns && c.guard[k] != c.guard[previous]);
  } else {
    out << " decision_index=" << decision << " decision_time_ns=" << c.decisions[decision]
        << " requested_dollars=" << requested << " quoted_fill_dollars=" << filled;
  }
  out << ']';
  return out.str();
}

// The dollar-book recipe has no funding-rate input. Permit only tiny cash
// roundoff relative to CURRENT positive NAV; do not scale by gross leverage or
// initial NAV (either could hide economically material borrowing). No clamp.
bool requires_cash_financing(f64 cash, f64 nav) noexcept {
  constexpr f64 relative_roundoff = 32.0 * std::numeric_limits<f64>::epsilon();
  return cash < -(relative_roundoff * nav);
}
bool same(f64 a, f64 b) noexcept { return std::bit_cast<u64>(a) == std::bit_cast<u64>(b); }
bool policy_matches(const WeightPolicy &a, const WeightPolicy &b) noexcept {
  return a.transform == b.transform && a.industry_neutral == b.industry_neutral &&
         a.dollar_neutral == b.dollar_neutral && same(a.gross_leverage, b.gross_leverage) &&
         same(a.truncation, b.truncation) && same(a.winsorize_limit, b.winsorize_limit);
}
bool normalize(ExecutionObjectiveConfig &c, usize dates) noexcept {
  if (c.window_end == 0)
    c.window_end = dates;
  if (c.maturity_end == 0)
    c.maturity_end = c.window_end;
  return c.rule == ExecutionObjectiveRule::DelayedSurfaceV2 && c.delay >= 1 &&
         c.window_begin < c.window_end && c.window_end <= dates && c.maturity_end <= dates &&
         c.delay < c.maturity_end && c.maturity_end - c.delay > 1 &&
         c.window_begin < c.maturity_end - c.delay - 1 && c.min_names >= 2 &&
         std::isfinite(c.initial_nav) && c.initial_nav > 0 &&
         (c.borrow == ExecutionBorrowRule::DisabledExplicitV1 ||
          c.borrow == ExecutionBorrowRule::RequireModeledV2) &&
         std::isfinite(c.borrow_days_per_year) && c.borrow_days_per_year > 0 &&
         c.rebalance_sessions >= 1 && c.rebalance_sessions <= 252 &&
         std::isfinite(c.trade_fraction) && c.trade_fraction > 0 && c.trade_fraction <= 1 &&
         !c.price_field.empty() && c.price_field.size() <= 4096;
}
bool config_matches(const ExecutionObjectiveConfig &a, const ExecutionObjectiveConfig &b,
                    usize dates) noexcept {
  const auto end = a.window_end == 0 ? dates : a.window_end;
  const auto mature = a.maturity_end == 0 ? end : a.maturity_end;
  return a.rule == b.rule && a.delay == b.delay && a.window_begin == b.window_begin &&
         end == b.window_end && mature == b.maturity_end && a.min_names == b.min_names &&
         same(a.initial_nav, b.initial_nav) && a.borrow == b.borrow &&
         same(a.borrow_days_per_year, b.borrow_days_per_year) &&
         a.guard_returns == b.guard_returns && a.price_field == b.price_field &&
         a.max_working_bytes == b.max_working_bytes &&
         a.rebalance_sessions == b.rebalance_sessions && same(a.trade_fraction, b.trade_fraction);
}
struct Budget {
  u64 maximum{}, used{};
  bool add(u64 n, u64 width) noexcept {
    if (width != 0 && n > (maximum - used) / width)
      return false;
    used += n * width;
    return true;
  }
};
struct Hash {
  co::Sha256 value;
  co::Status word(u64 v) {
    std::array<std::byte, 8> bytes{};
    for (usize i = 0; i < 8; ++i)
      bytes[i] = static_cast<std::byte>((v >> (8 * i)) & 255U);
    return value.update(bytes);
  }
  co::Status number(f64 v) { return word(std::bit_cast<u64>(v)); }
  co::Status text(std::string_view v) {
    ATX_TRY_VOID(word(v.size()));
    return value.update(std::as_bytes(std::span{v.data(), v.size()}));
  }
  co::Result<std::string> finish() {
    ATX_TRY(auto digest, value.finalize());
    constexpr char hex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (usize i = 0; i < digest.size(); ++i) {
      const auto b = std::to_integer<unsigned>(digest[i]);
      out[2 * i] = hex[b >> 4];
      out[2 * i + 1] = hex[b & 15];
    }
    return co::Ok(std::move(out));
  }
};
co::Result<std::string> context_hash(const Context &c, const ExecutionObjectiveIdentity &id) {
  Hash h;
  ATX_TRY_VOID(h.text("delayed-fixed-dollar-targets/surface-v2/endpoint-attribution-v1/unfunded-cash-refusal-v1"));
  ATX_TRY_VOID(h.text(id.source_sha256));
  ATX_TRY_VOID(h.text(id.role));
  ATX_TRY_VOID(h.text(id.price_recipe));
  const auto &r = c.config;
  for (u64 word :
       {static_cast<u64>(r.rule), static_cast<u64>(r.delay), static_cast<u64>(r.window_begin),
        static_cast<u64>(r.window_end), static_cast<u64>(r.maturity_end),
        static_cast<u64>(r.min_names), static_cast<u64>(r.borrow),
        static_cast<u64>(r.guard_returns), r.max_working_bytes, static_cast<u64>(c.dates),
        static_cast<u64>(c.instruments), static_cast<u64>(c.policy.transform),
        static_cast<u64>(c.policy.industry_neutral), static_cast<u64>(c.policy.dollar_neutral)})
    ATX_TRY_VOID(h.word(word));
  for (auto v : {r.initial_nav, r.borrow_days_per_year, c.policy.gross_leverage,
                 c.policy.truncation, c.policy.winsorize_limit})
    ATX_TRY_VOID(h.number(v));
  ATX_TRY_VOID(h.text(r.price_field));
  if (r.rebalance_sessions != 1 || r.trade_fraction != 1.0) {
    ATX_TRY_VOID(h.text("scheduled-decision-dollar-partial-v1"));
    ATX_TRY_VOID(h.word(r.rebalance_sessions));
    ATX_TRY_VOID(h.number(r.trade_fraction));
  }
  for (auto v : c.ids)
    ATX_TRY_VOID(h.word(v));
  for (usize d = r.window_begin; d < c.realization_end; ++d) {
    ATX_TRY_VOID(h.word(static_cast<u64>(c.marks[d])));
    if (d < c.decision_end)
      ATX_TRY_VOID(h.word(static_cast<u64>(c.decisions[d])));
    for (usize i = 0; i < c.instruments; ++i) {
      const auto k = d * c.instruments + i;
      ATX_TRY_VOID(h.number(c.prices[k]));
      ATX_TRY_VOID(h.word(c.member[k]));
      ATX_TRY_VOID(h.word(c.panel_member[k]));
      if (r.guard_returns)
        ATX_TRY_VOID(h.word(c.guard[k]));
    }
  }
  for (auto v : c.groups)
    ATX_TRY_VOID(h.word(v));
  for (const auto &snapshot : c.snapshots) {
    ATX_TRY_VOID(h.text(snapshot.recipe_sha256()));
    ATX_TRY_VOID(h.text(snapshot.snapshot_sha256()));
  }
  if (!c.cash_events.empty()) {
    ATX_TRY_VOID(h.text("fixed-usd-research-share-claim-v1/reserve-payable-continue-borrow-v1"));
    ATX_TRY_VOID(h.word(c.cash_events.size()));
    for (const auto& e:c.cash_events) {
      for (const auto* text:{&e.event_id,&e.security_id_namespace,&e.historical_identity,
          &e.panel_source_sha256,&e.identity_evidence_sha256,&e.completion_evidence_sha256,
          &e.basis_evidence_sha256}) ATX_TRY_VOID(h.text(*text));
      for (auto v:{e.revision,e.instrument_id,static_cast<u64>(e.evidence),
          static_cast<u64>(e.reference_mark_ns),static_cast<u64>(e.effective_after_ns),
          static_cast<u64>(e.effective_by_ns),static_cast<u64>(e.available_at_ns),
          static_cast<u64>(e.recognition_mark_ns),
          static_cast<u64>(e.cash_excluded_from_adjusted_close)}) ATX_TRY_VOID(h.word(v));
      for (auto v:{e.reference_raw_close,e.reference_adjusted_close,e.cash_usd_per_raw_share})
        ATX_TRY_VOID(h.number(v));
    }
  }
  return h.finish();
}

co::Result<alpha::AlphaStreams> allocate_streams(const Context &c, usize count) {
  Budget budget{c.config.max_working_bytes, c.owned_bytes};
  if (!budget.add(c.scratch_bytes, 1) || !budget.add(count, c.output_bytes) || count == 0 ||
      count > std::numeric_limits<usize>::max() / c.dates ||
      count * c.dates > std::numeric_limits<usize>::max() / c.instruments)
    return co::Err(co::ErrorCode::OutOfRange, "execution streams: output/scratch budget");
  alpha::AlphaStreams out;
  out.n_alphas_ = count;
  out.n_periods_ = c.dates;
  out.n_instruments_ = c.instruments;
  const auto rows = count * c.dates;
  out.pnl_flat.assign(rows, nan);
  out.pos_flat.assign(rows * c.instruments, nan);
  out.gross_flat.assign(rows, nan);
  out.execution_cost_flat.assign(rows, nan);
  out.borrow_cost_flat.assign(rows, nan);
  out.turnover_flat.assign(rows, nan);
  out.pretrade_nav_flat.assign(rows, nan);
  out.end_nav_flat.assign(rows, nan);
  out.valid_flat.assign(rows, 0);
  out.names_flat.assign(rows, 0);
  out.capped_names_flat.assign(rows, 0);
  out.execution_context_sha256 = c.identity;
  out.first_realization_ = c.first_realization;
  out.realization_end_ = c.realization_end;
  return co::Ok(std::move(out));
}

// One signal is processed chronologically. Dollars denote a total-return mark
// book, with only the explicit fixed-cash claim extension. No future value is used
// to rank, filter or size a decision; entry prices only validate execution units.
co::Status fill(const Context &c, std::span<const f64> signal, f64 sign, usize alpha_index,
                alpha::AlphaStreams &out, ExecutionCashClaimStreams* claim_out=nullptr) {
  const auto n = c.instruments, d0 = c.config.window_begin, delay = c.config.delay;
  if (signal.size() != c.dates * n || (sign != 1.0 && sign != -1.0))
    return co::Err(co::ErrorCode::InvalidArgument, "execution streams: signal shape/sign");
  const auto slots = delay + 1;
  std::vector<f64> holdings(n, 0.0), queued(slots * n, 0.0), masked(n, nan);
  std::vector<usize> queued_names(slots, 0);
  std::vector<InstrumentId> ids(n);
  for (usize i = 0; i < n; ++i)
    ids[i] = InstrumentId{static_cast<atx::u32>(i)};
  const Universe universe{ids};
  WeightPolicyScratch scratch;
  scratch.reserve(n);
  f64 cash = c.config.initial_nav, nav = cash, entry_nav = 0, entry_cost = 0, entry_turnover = 0;
  usize entry_names = 0, entry_capped = 0, entry_decision = 0;
  bool active_interval = false;
  const bool claims_enabled=!c.cash_events.empty();
  std::vector<atx::u8> retired(claims_enabled?n:0,0);
  std::vector<f64> claims(c.cash_events.size(),0), claim_rates(c.cash_events.size(),0);
  if (claims_enabled) {
    for (usize j=0;j<c.cash_events.size();++j)
      if (c.cash_event_names[j]<n && c.cash_event_periods[j]<=d0)
        retired[c.cash_event_names[j]]=1; // Known pre-role extinction, no opening claim.
  }
  f64 receivables=0, payables=0;

  for (usize t = d0; t < c.realization_end; ++t) {
    f64 gross_dollars = 0, borrow_dollars = 0;
    f64 claim_bridge=0, claim_borrow=0;
    // Existing fixed payables continue their last qualified modeled rate; no
    // inferred loan termination/payment. Recognition interval uses old equity.
    if (claims_enabled && t>d0) {
      const auto days=static_cast<f64>(c.marks[t]-c.marks[t-1])/ns_per_day;
      for (usize j=0;j<claims.size();++j)
        if (claims[j]<0)
          claim_borrow+=(-claims[j])*(claim_rates[j]*days/c.config.borrow_days_per_year);
      borrow_dollars+=claim_borrow;
    }
    // Value yesterday's already held positions. Future missingness can refuse a
    // realized interval; it cannot retroactively change decision eligibility.
    if (t > d0) {
      const auto days = static_cast<f64>(c.marks[t] - c.marks[t - 1]) / ns_per_day;
      for (usize i = 0; i < n; ++i) {
        const auto old = holdings[i];
        if (active_interval)
          out.pos_flat[(alpha_index * c.dates + t) * n + i] = old / entry_nav;
        const auto event_index=claims_enabled?c.cash_event_for_name[i]:c.cash_events.size();
        const bool recognize=event_index<c.cash_events.size() &&
                             c.cash_event_periods[event_index]==t;
        if (recognize) {
          retired[i]=1;
          for (usize slot=0;slot<slots;++slot) queued[slot*n+i]=0;
          ATX_TRY(auto record,execution_cash_claim_detail::recognize(
              c.cash_events[event_index],event_index,i,t,old));
          if (old<0 && active_interval) {
            const auto quote=c.snapshots[entry_decision-d0].borrow_annual_rate(
                i,c.decisions[entry_decision]);
            if (quote.status!=cost::CostQuoteStatus::Priced)
              return co::Err(co::ErrorCode::Unavailable,"cash claim: modeled carry unavailable");
            record.continued_annual_borrow_rate=quote.annual_fraction;
            claim_rates[event_index]=quote.annual_fraction;
          }
          claims[event_index]=record.signed_claim_dollars;
          claim_bridge+=record.recognition_pnl_dollars;
          gross_dollars+=record.recognition_pnl_dollars;
          holdings[i]=0;
          claim_out->recognitions.push_back(record);
        }
        if (old == 0) continue;
        const auto a = (t - 1) * n + i, b = t * n + i;
        if (!recognize && (c.panel_member[a] == 0 || c.panel_member[b] == 0 ||
            !std::isfinite(c.prices[a]) || !std::isfinite(c.prices[b]) || c.prices[a] <= 0 ||
            c.prices[b] <= 0 || (c.config.guard_returns && c.guard[b] != c.guard[a])))
          return co::Err(co::ErrorCode::Unavailable,
                         price_failure(c, "execution streams: missing/guarded held return", t, i, old, true));
        if (!recognize) {
          const auto next = old * (c.prices[b] / c.prices[a]);
          if (!std::isfinite(next))
            return co::Err(co::ErrorCode::OutOfRange, "execution streams: holding mark overflow");
          gross_dollars += next - old;
          holdings[i] = next;
        }
        if (active_interval && old < 0 &&
            c.config.borrow == ExecutionBorrowRule::RequireModeledV2) {
          const auto quote =
              c.snapshots[entry_decision - d0].borrow_annual_rate(i, c.decisions[entry_decision]);
          if (quote.status != cost::CostQuoteStatus::Priced)
            return co::Err(co::ErrorCode::Unavailable,
                           "execution streams: modeled borrow unavailable for held short");
          borrow_dollars += (-old) * (quote.annual_fraction * days / c.config.borrow_days_per_year);
        }
      }
    }
    if (claims_enabled) {
      receivables=0; payables=0;
      for (const auto value:claims) {
        if (value>0) receivables+=value;
        else payables-=value;
      }
    }
    if (!std::isfinite(gross_dollars) || !std::isfinite(borrow_dollars) ||
        !std::isfinite(receivables) || !std::isfinite(payables) ||
        !std::isfinite(claim_bridge) || !std::isfinite(claim_borrow))
      return co::Err(co::ErrorCode::OutOfRange,
                     "execution streams: realized return/borrow overflow");
    cash -= borrow_dollars;
    nav = cash;
    for (auto position : holdings)
      nav += position;
    if (claims_enabled) nav+=(receivables-payables);
    if (!std::isfinite(cash) || !std::isfinite(nav) || nav <= 0)
      return co::Err(co::ErrorCode::OutOfRange,
                     "execution streams: nonpositive/nonfinite marked NAV");
    if (requires_cash_financing(claims_enabled?cash-payables:cash, nav))
      return co::Err(co::ErrorCode::Unavailable,
                     "execution streams: cash financing unsupported after borrow/mark");
    if (active_interval) {
      const auto row = alpha_index * c.dates + t;
      out.pnl_flat[row] = (nav - entry_nav) / entry_nav;
      out.gross_flat[row] = gross_dollars / entry_nav;
      out.execution_cost_flat[row] = entry_cost / entry_nav;
      out.borrow_cost_flat[row] = borrow_dollars / entry_nav;
      out.turnover_flat[row] = entry_turnover;
      out.pretrade_nav_flat[row] = entry_nav;
      out.end_nav_flat[row] = nav;
      out.names_flat[row] = entry_names;
      out.capped_names_flat[row] = entry_capped;
      out.valid_flat[row] = 1;
      if (claims_enabled) {
        claim_out->signed_claim_dollars[t]=receivables-payables;
        claim_out->receivable_dollars[t]=receivables;
        claim_out->payable_dollars[t]=payables;
        claim_out->recognition_pnl_dollars[t]=claim_bridge;
        claim_out->claim_borrow_dollars[t]=claim_borrow;
        claim_out->settled_cash_dollars[t]=cash;
      }
      if (!std::isfinite(out.pnl_flat[row]) || !std::isfinite(out.gross_flat[row]) ||
          !std::isfinite(out.execution_cost_flat[row]) || !std::isfinite(out.borrow_cost_flat[row]))
        return co::Err(co::ErrorCode::OutOfRange, "execution streams: interval return overflow");
      active_interval = false;
    }
    // Execute only a queued decision with a complete realized endpoint in this
    // role. Target dollars were fixed at its decision NAV, never today's NAV.
    if (t >= d0 + delay && t - delay < c.decision_end) {
      const auto d = t - delay, slot = (d - d0) % slots;
      const auto &surface = c.snapshots[d - d0];
      const bool rebalance = (d - d0) % c.config.rebalance_sessions == 0;
      entry_nav = nav;
      entry_cost = 0;
      entry_turnover = 0;
      if (rebalance) entry_names = queued_names[slot];
      entry_capped = 0;
      entry_decision = d;
      for (usize i = 0; rebalance && i < n; ++i) {
        if (claims_enabled && retired[i]!=0) continue;
        const auto requested = queued[slot * n + i] - holdings[i];
        if (!std::isfinite(requested))
          return co::Err(co::ErrorCode::OutOfRange,
                         "execution streams: requested dollars overflow");
        const auto quote = surface.quote_dollars(i, c.decisions[d], requested,
                                                 cost::CostFillRule::ParticipationCapped);
        if (!quote.priced())
          return co::Err(co::ErrorCode::Unavailable,
                         price_failure(c, "execution streams: unpriceable nonzero trade", t, i,
                                       holdings[i], false, d, requested, quote.filled_dollars));
        if (quote.filled_dollars != 0) {
          const auto price = c.prices[t * n + i];
          if (c.panel_member[t * n + i] == 0 || !std::isfinite(price) || price <= 0)
            return co::Err(co::ErrorCode::Unavailable,
                           price_failure(c, "execution streams: unavailable entry price", t, i,
                                         holdings[i], false, d, requested, quote.filled_dollars));
          const auto units = quote.filled_dollars / price;
          if (!std::isfinite(units) || units == 0)
            return co::Err(co::ErrorCode::Unavailable,
                           price_failure(c, "execution streams: unavailable execution price/units", t, i,
                                         holdings[i], false, d, requested, quote.filled_dollars));
        }
        holdings[i] += quote.filled_dollars;
        cash -= quote.filled_dollars;
        entry_cost += quote.total_dollars;
        entry_turnover += std::abs(quote.filled_dollars) / entry_nav;
        entry_capped += quote.filled_dollars != requested;
        if (!std::isfinite(holdings[i]) || !std::isfinite(cash))
          return co::Err(co::ErrorCode::OutOfRange,
                         "execution streams: filled holding/cash overflow");
      }
      cash -= entry_cost;
      nav = entry_nav - entry_cost;
      if (!std::isfinite(entry_cost) || !std::isfinite(entry_turnover) || !std::isfinite(cash) ||
          !std::isfinite(nav) || nav <= 0)
        return co::Err(co::ErrorCode::OutOfRange, "execution streams: nonpositive NAV after costs");
      // Check the completed batch: sales/short proceeds can fund earlier buys,
      // so transient per-name cash inside the fill loop is not a refusal.
      if (requires_cash_financing(claims_enabled?cash-payables:cash, nav))
        return co::Err(co::ErrorCode::Unavailable,
                       "execution streams: cash financing unsupported after fills/costs");
      active_interval = true; // The attribution closes at t+1; cash is already debited.
    }
    if (t < c.decision_end && (t - d0) % c.config.rebalance_sessions == 0) {
      usize names = 0;
      f64 known_locked_equity=0;
      for (usize i = 0; i < n; ++i) {
        const auto k = t * n + i;
        const auto event_index=claims_enabled?c.cash_event_for_name[i]:c.cash_events.size();
        const bool known_completion=event_index<c.cash_events.size() &&
            c.cash_events[event_index].effective_by_ns<c.decisions[t] &&
            c.cash_events[event_index].available_at_ns<c.decisions[t];
        // Publication can precede this decision but follow its mark. That
        // known locked asset cannot finance fresh targets while awaiting claim
        // recognition. Remove its current accounted long value, not a guessed
        // marked gain or a cash receipt. Earlier queued targets remain frozen.
        if (known_completion && holdings[i]>0) known_locked_equity+=holdings[i];
        const bool eligible = (!claims_enabled || retired[i]==0) && !known_completion &&
                              c.member[k] != 0 && c.panel_member[k] != 0 &&
                              std::isfinite(signal[k]) && std::isfinite(c.prices[k]) &&
                              c.prices[k] > 0;
        masked[i] = eligible ? sign * signal[k] : nan;
        names += eligible;
      }
      if (!std::isfinite(known_locked_equity))
        return co::Err(co::ErrorCode::OutOfRange,"cash claim: locked decision equity overflow");
      const auto slot = (t - d0) % slots;
      queued_names[slot] = names;
      if (names < c.config.min_names) {
        std::fill_n(queued.begin() + static_cast<std::ptrdiff_t>(slot * n), n, 0.0);
      } else {
        std::span<const atx::u32> groups;
        if (!c.groups.empty())
          groups = c.groups.size() == n ? std::span<const atx::u32>{c.groups}
                                        : std::span<const atx::u32>{c.groups}.subspan(t * n, n);
        c.policy.to_target_weights(SignalView{masked}, universe, scratch, groups);
        for (usize i = 0; i < n; ++i) {
          const auto target_nav=claims_enabled?nav-receivables-known_locked_equity:nav;
          if (!std::isfinite(target_nav) || target_nav<0)
            return co::Err(co::ErrorCode::Unavailable,"cash claim: nonspendable target NAV");
          const auto target = scratch.weights[i] * target_nav;
          if (!std::isfinite(target))
            return co::Err(co::ErrorCode::OutOfRange,
                           "execution streams: decision target overflow");
          queued[slot * n + i] = target;
        }
      }
      if (c.config.trade_fraction != 1.0) {
        for (usize i = 0; i < n; ++i) {
          auto& target = queued[slot * n + i];
          target = holdings[i] + c.config.trade_fraction * (target - holdings[i]);
          if (!std::isfinite(target))
            return co::Err(co::ErrorCode::OutOfRange, "execution streams: partial decision target overflow");
        }
      }
    }
  }
  return co::Ok();
}
} // namespace

usize ExecutionObjectiveContext::dates() const noexcept { return data_ ? data_->dates : 0; }
usize ExecutionObjectiveContext::instruments() const noexcept {
  return data_ ? data_->instruments : 0;
}
usize ExecutionObjectiveContext::first_decision() const noexcept {
  return data_ ? data_->config.window_begin : 0;
}
usize ExecutionObjectiveContext::decision_end() const noexcept {
  return data_ ? data_->decision_end : 0;
}
usize ExecutionObjectiveContext::first_realization() const noexcept {
  return data_ ? data_->first_realization : 0;
}
usize ExecutionObjectiveContext::realization_end() const noexcept {
  return data_ ? data_->realization_end : 0;
}
const ExecutionObjectiveConfig &ExecutionObjectiveContext::config() const noexcept {
  static const ExecutionObjectiveConfig empty{};
  return data_ ? data_->config : empty;
}
std::string_view ExecutionObjectiveContext::identity_sha256() const noexcept {
  return data_ ? data_->identity : std::string_view{};
}
u64 ExecutionObjectiveContext::bytes() const noexcept { return data_ ? data_->owned_bytes : 0; }
u64 ExecutionObjectiveContext::per_signal_working_bytes() const noexcept {
  return data_ ? data_->call_bytes : 0;
}

co::Result<ExecutionObjectiveContext> prepare_execution_objective(
    const alpha::Panel &panel, const WeightPolicy &policy, const ExecutionObjectiveConfig &input,
    std::span<const cost::CostSurface> snapshots, std::span<const atx::i64> marks,
    std::span<const atx::i64> decisions, std::span<const u64> ids,
    const ExecutionObjectiveIdentity &identity, std::span<const atx::u8> member,
    std::span<const atx::u32> guard, std::span<const atx::u32> groups) {
  return prepare_execution_objective_claims(panel,policy,input,snapshots,marks,decisions,ids,
      identity,{},member,guard,groups);
}
co::Result<ExecutionObjectiveContext> prepare_execution_objective_claims(
    const alpha::Panel &panel, const WeightPolicy &policy, const ExecutionObjectiveConfig &input,
    std::span<const cost::CostSurface> snapshots, std::span<const atx::i64> marks,
    std::span<const atx::i64> decisions, std::span<const u64> ids,
    const ExecutionObjectiveIdentity &identity, std::span<const ExecutionCashClaimEvent> events,
    std::span<const atx::u8> member, std::span<const atx::u32> guard,
    std::span<const atx::u32> groups) {
  const auto d = panel.dates(), n = panel.instruments();
  if (input.price_field.size() > 4096)
    return co::Err(co::ErrorCode::InvalidArgument, "execution objective: price field too long");
  auto cfg = input;
  if (n == 0 || n > std::numeric_limits<atx::u32>::max() || d == 0 ||
      d > std::numeric_limits<usize>::max() / n || !normalize(cfg, d) ||
      !std::isfinite(policy.gross_leverage) || policy.gross_leverage < 0 ||
      !std::isfinite(policy.truncation) || policy.truncation < 0 ||
      !std::isfinite(policy.winsorize_limit) || policy.winsorize_limit < 0 ||
      policy.winsorize_limit > 0.5 ||
      (policy.transform != Transform::Rank && policy.transform != Transform::ZScore &&
       policy.transform != Transform::Raw))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "execution objective: invalid geometry/recipe/policy");
  const auto cells = d * n;
  const auto last = std::min(cfg.window_end, cfg.maturity_end - cfg.delay - 1);
  const auto first_realized = cfg.window_begin + cfg.delay + 1,
             last_realized = last + cfg.delay + 1;
  const auto decisions_count = last - cfg.window_begin;
  if (marks.size() != d || decisions.size() != d || ids.size() != n ||
      snapshots.size() != decisions_count || (!member.empty() && member.size() != cells) ||
      (cfg.guard_returns && guard.size() != cells) ||
      (!groups.empty() && groups.size() != n && groups.size() != cells) ||
      (policy.industry_neutral && groups.empty()))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "execution objective: clocks/snapshots/support shape differs");
  if (identity.source_sha256.size() != 64 || identity.role.empty() || identity.role.size() > 4096 ||
      identity.price_recipe.empty() || identity.price_recipe.size() > 4096)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "execution objective: missing bounded source/role/price identity");
  for (char ch : identity.source_sha256)
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
      return co::Err(co::ErrorCode::InvalidArgument,
                     "execution objective: source hash is not lowercase SHA256");
  if (events.size()>1024 || (!events.empty() && cfg.borrow!=ExecutionBorrowRule::RequireModeledV2))
    return co::Err(co::ErrorCode::InvalidArgument,"cash claim: event count/borrow policy");
  for (usize j=0;j<events.size();++j) {
    ATX_TRY_VOID(execution_cash_claim_detail::validate(events[j]));
    if (events[j].panel_source_sha256!=identity.source_sha256)
      return co::Err(co::ErrorCode::InvalidArgument,"cash claim: role source pin differs");
    for (usize k=0;k<j;++k)
      if (events[j].instrument_id==events[k].instrument_id || events[j].event_id==events[k].event_id)
        return co::Err(co::ErrorCode::InvalidArgument,"cash claim: duplicate event/instrument");
  }
  // Charge retained immutable snapshots too, even though copies share their rows.
  Budget owned{cfg.max_working_bytes, 0};
  if (!owned.add(1, sizeof(Context) + 16384) || !owned.add(cells, sizeof(f64) + 2) ||
      (cfg.guard_returns && !owned.add(cells, sizeof(atx::u32))) ||
      !owned.add(groups.size(), sizeof(atx::u32)) || !owned.add(d, 2 * sizeof(atx::i64)) ||
      !owned.add(n, sizeof(u64)) || !owned.add(decisions_count, sizeof(cost::CostSurface)))
    return co::Err(co::ErrorCode::OutOfRange, "execution objective: retained context budget");
  if (!events.empty() && (!owned.add(events.size(),sizeof(ExecutionCashClaimEvent)+2048+
                                                  2*sizeof(usize)) ||
                         !owned.add(n,sizeof(usize))))
    return co::Err(co::ErrorCode::OutOfRange,"cash claim: retained event budget");
  for (const auto &snapshot : snapshots)
    if (!owned.add(snapshot.bytes(), 1))
      return co::Err(co::ErrorCode::OutOfRange, "execution objective: retained snapshot budget");
  Budget output{cfg.max_working_bytes, 0}, scratch{cfg.max_working_bytes, 0};
  if (!output.add(cells, sizeof(f64)) ||
      !output.add(d, 7 * sizeof(f64) + sizeof(atx::u8) + 2 * sizeof(usize)) ||
      !output.add(1, sizeof(alpha::AlphaStreams) + 256) || !scratch.add(1, 4096) ||
      !scratch.add(cfg.delay + 1, n * sizeof(f64) + sizeof(usize)) || !scratch.add(n, 160) ||
      output.used > cfg.max_working_bytes - owned.used ||
      scratch.used > cfg.max_working_bytes - owned.used - output.used)
    return co::Err(co::ErrorCode::OutOfRange,
                   "execution objective: output/pending-target/worker scratch budget");
  if (!events.empty() && (!output.add(d,6*sizeof(f64)) ||
      !output.add(events.size(),sizeof(ExecutionCashClaimRecognition)+sizeof(ExecutionCashClaimEventUse)) ||
      !output.add(1,sizeof(ExecutionCashClaimStreams)+512) || !scratch.add(n,sizeof(atx::u8)) ||
      !scratch.add(events.size(),2*sizeof(f64)) ||
      output.used>cfg.max_working_bytes-owned.used ||
      scratch.used>cfg.max_working_bytes-owned.used-output.used))
    return co::Err(co::ErrorCode::OutOfRange,"cash claim: result/worker budget");
  for (usize t = cfg.window_begin; t < last_realized; ++t) {
    if (marks[t] <= 0 || (t > cfg.window_begin && marks[t] <= marks[t - 1]))
      return co::Err(co::ErrorCode::InvalidArgument,
                     "execution objective: nonincreasing mark clock");
    if (t < last && !(marks[t] < decisions[t] && decisions[t] < marks[t + 1]))
      return co::Err(co::ErrorCode::InvalidArgument,
                     "execution objective: price availability/decision clock");
  }
  for (usize k = 0; k < cells; ++k) {
    if (!member.empty() && member[k] > 1)
      return co::Err(co::ErrorCode::InvalidArgument, "execution objective: nonbinary membership");
    if (cfg.guard_returns && k >= n && guard[k] < guard[k - n])
      return co::Err(co::ErrorCode::InvalidArgument,
                     "execution objective: noncumulative return guard");
  }
  for (usize j = 0; j < decisions_count; ++j) {
    const auto &snapshot = snapshots[j];
    if (snapshot.instruments() != n ||
        snapshot.decision_time_ns() != decisions[cfg.window_begin + j])
      return co::Err(co::ErrorCode::InvalidArgument,
                     "execution objective: cost snapshot clock/shape mismatch");
    for (usize i = 0; i < n; ++i)
      if (snapshot.rows()[i].instrument_id != ids[i])
        return co::Err(co::ErrorCode::InvalidArgument,
                       "execution objective: cost snapshot instrument order differs");
  }
  ATX_TRY(auto close_id, panel.field_id(cfg.price_field));
  const auto prices = panel.field_all(close_id);
  if (prices.size() != cells)
    return co::Err(co::ErrorCode::InvalidArgument, "execution objective: close geometry");
  auto c = std::make_shared<Context>();
  c->config = std::move(cfg);
  c->policy = policy;
  c->panel_token = &panel;
  c->price_token = prices.data();
  c->dates = d;
  c->instruments = n;
  c->decision_end = last;
  c->first_realization = first_realized;
  c->realization_end = last_realized;
  c->owned_bytes = owned.used;
  c->output_bytes = output.used;
  c->scratch_bytes = scratch.used;
  c->call_bytes = output.used + scratch.used;
  c->prices.assign(prices.begin(), prices.end());
  c->member.assign(cells, 1);
  c->panel_member.resize(cells);
  if (!member.empty())
    std::copy(member.begin(), member.end(), c->member.begin());
  for (usize t = 0; t < d; ++t)
    for (usize i = 0; i < n; ++i)
      c->panel_member[t * n + i] = panel.in_universe(static_cast<alpha::DateIdx>(t), i) ? 1 : 0;
  if (c->config.guard_returns)
    c->guard.assign(guard.begin(), guard.end());
  c->groups.assign(groups.begin(), groups.end());
  c->marks.assign(marks.begin(), marks.end());
  c->decisions.assign(decisions.begin(), decisions.end());
  c->ids.assign(ids.begin(), ids.end());
  c->snapshots.assign(snapshots.begin(), snapshots.end());
  if (!events.empty()) {
    c->cash_events.assign(events.begin(),events.end());
    c->cash_event_for_name.assign(n,events.size());
    c->cash_event_names.assign(events.size(),n);
    c->cash_event_periods.assign(events.size(),last_realized);
    for (usize j=0;j<events.size();++j) {
      const auto& e=events[j];
      const auto id=std::find(ids.begin(),ids.end(),e.instrument_id);
      if (id==ids.end()) continue; // Explicitly outside this ordered role axis.
      const auto i=static_cast<usize>(id-ids.begin());
      c->cash_event_names[j]=i; c->cash_event_for_name[i]=j;
      const auto known=std::max(e.effective_by_ns,e.available_at_ns);
      if (known<marks[c->config.window_begin]) {
        if (e.recognition_mark_ns>marks[c->config.window_begin])
          return co::Err(co::ErrorCode::InvalidArgument,"cash claim: delayed pre-role recognition");
        c->cash_event_periods[j]=c->config.window_begin;
        continue; // Known extinction only. No hypothetical opening entitlement.
      }
      if (known>=marks[last_realized-1]) continue;
      const auto mark=std::upper_bound(
          marks.begin()+static_cast<std::ptrdiff_t>(c->config.window_begin),
          marks.begin()+static_cast<std::ptrdiff_t>(last_realized),known);
      if (*mark!=e.recognition_mark_ns)
        return co::Err(co::ErrorCode::InvalidArgument,
                       "cash claim: recognition must be first strictly known role mark");
      const auto t=static_cast<usize>(mark-marks.begin()), k=(t-1)*n+i;
      if (e.reference_mark_ns!=marks[t-1] || c->panel_member[k]==0 ||
          !same(e.reference_adjusted_close,prices[k]))
        return co::Err(co::ErrorCode::InvalidArgument,"cash claim: previous observed adjusted basis");
      ATX_TRY(auto raw_id,panel.field_id("raw_close"));
      const auto raw=panel.field_all(raw_id);
      if (raw.size()!=cells || !same(e.reference_raw_close,raw[k]))
        return co::Err(co::ErrorCode::InvalidArgument,"cash claim: previous observed raw basis");
      c->cash_event_periods[j]=t;
    }
  }
  ATX_TRY(c->identity, context_hash(*c, identity));
  ExecutionObjectiveContext out;
  out.data_ = std::move(c);
  return co::Ok(std::move(out));
}

bool execution_objective_matches(const ExecutionObjectiveContext &context,
                                 const alpha::Panel &panel, const WeightPolicy &policy,
                                 const ExecutionObjectiveConfig &input) noexcept {
  if (!context.data_)
    return false;
  const auto &c = *context.data_;
  if (c.panel_token != &panel || c.dates != panel.dates() || c.instruments != panel.instruments() ||
      !config_matches(input, c.config, panel.dates()) || !policy_matches(policy, c.policy))
    return false;
  // Avoid an allocating error construction inside this noexcept predicate.
  usize column = 0;
  for (; column < panel.num_fields(); ++column)
    if (panel.field_name(column) == c.config.price_field)
      break;
  if (column == panel.num_fields())
    return false;
  const auto prices = panel.field_all(static_cast<alpha::FieldId>(column));
  if (prices.data() != c.price_token || prices.size() != c.prices.size())
    return false;
  for (usize t = c.config.window_begin; t < c.realization_end; ++t)
    for (usize i = 0; i < c.instruments; ++i) {
      const auto k = t * c.instruments + i;
      if (!same(prices[k], c.prices[k]) ||
          panel.in_universe(static_cast<alpha::DateIdx>(t), i) != (c.panel_member[k] != 0))
        return false;
    }
  if (!c.cash_events.empty()) {
    usize raw_column=0;
    for (;raw_column<panel.num_fields();++raw_column)
      if (panel.field_name(raw_column)=="raw_close") break;
    for (usize j=0;j<c.cash_events.size();++j) {
      const auto t=c.cash_event_periods[j],i=c.cash_event_names[j];
      if (i==c.instruments || t<=c.config.window_begin || t>=c.realization_end) continue;
      if (raw_column==panel.num_fields()) return false;
      const auto raw=panel.field_all(static_cast<alpha::FieldId>(raw_column));
      if (raw.size()!=c.prices.size() || !same(raw[(t-1)*c.instruments+i],
                                             c.cash_events[j].reference_raw_close)) return false;
    }
  }
  return true;
}
bool execution_support_matches(const ExecutionObjectiveContext &context,
                               std::span<const atx::u8> member,
                               std::span<const atx::u32> guard) noexcept {
  if (!context.data_)
    return false;
  const auto &c = *context.data_;
  if (!member.empty() && member.size() != c.member.size())
    return false;
  for (usize i = 0; i < c.member.size(); ++i)
    if (c.member[i] != (member.empty() ? 1 : member[i]))
      return false;
  return !c.config.guard_returns || (guard.size() == c.guard.size() &&
                                     std::equal(guard.begin(), guard.end(), c.guard.begin()));
}
co::Result<alpha::AlphaStreams> extract_execution_streams(const alpha::SignalSet &signals,
                                                          const ExecutionObjectiveContext &context,
                                                          f64 sign) {
  if (!context.data_ || signals.dates != context.dates() ||
      signals.instruments != context.instruments())
    return co::Err(co::ErrorCode::InvalidArgument,
                   "execution streams: missing context or signal geometry");
  const auto &c = *context.data_;
  if (!c.cash_events.empty())
    return co::Err(co::ErrorCode::InvalidArgument,"cash claim: explicit claim result required");
  ATX_TRY(auto out, allocate_streams(c, signals.alphas.size()));
  for (usize a = 0; a < signals.alphas.size(); ++a)
    ATX_TRY_VOID(fill(c, signals.alphas[a].values, sign, a, out));
  return co::Ok(std::move(out));
}
co::Result<alpha::AlphaStreams> extract_execution_signal(std::span<const f64> signal,
                                                         const ExecutionObjectiveContext &context,
                                                         f64 sign) {
  if (!context.data_)
    return co::Err(co::ErrorCode::InvalidArgument, "execution streams: empty context");
  const auto &c = *context.data_;
  if (!c.cash_events.empty())
    return co::Err(co::ErrorCode::InvalidArgument,"cash claim: explicit claim result required");
  ATX_TRY(auto out, allocate_streams(c, 1));
  ATX_TRY_VOID(fill(c, signal, sign, 0, out));
  return co::Ok(std::move(out));
}
co::Result<ExecutionCashClaimStreams> extract_execution_signal_claims(
    std::span<const f64> signal,const ExecutionObjectiveContext& context,f64 sign) {
  if (!context.data_)
    return co::Err(co::ErrorCode::InvalidArgument,"cash claim: empty context");
  const auto& c=*context.data_;
  ExecutionCashClaimStreams out;
  ATX_TRY(out.streams,allocate_streams(c,1));
  if (!c.cash_events.empty()) {
    for (auto* values:{&out.signed_claim_dollars,&out.receivable_dollars,&out.payable_dollars,
        &out.recognition_pnl_dollars,&out.claim_borrow_dollars,&out.settled_cash_dollars})
      values->assign(c.dates,nan);
    out.recognitions.reserve(c.cash_events.size());
    out.event_uses.reserve(c.cash_events.size());
    for (usize j=0;j<c.cash_events.size();++j) {
      const auto use=c.cash_event_names[j]==c.instruments?ExecutionCashClaimUse::OutsideAxis:
          c.cash_event_periods[j]<=c.config.window_begin?ExecutionCashClaimUse::PreRoleRetired:
          c.cash_event_periods[j]>=c.realization_end?ExecutionCashClaimUse::AfterRole:
                                                   ExecutionCashClaimUse::InRole;
      out.event_uses.push_back({j,c.cash_event_names[j],use});
    }
  }
  ATX_TRY_VOID(fill(c,signal,sign,0,out.streams,&out));
  return co::Ok(std::move(out));
}
} // namespace atx::engine::factory
