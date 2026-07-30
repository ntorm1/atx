#include "atx/vol/listed_dispersion_reconciliation.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/listed_quote_key.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/priced_surface.hpp"

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

// `LegKey` / `key_of` used to live here, in this anonymous namespace. They are
// now `ListedQuoteKey` / `quote_key_of` in atx/vol/listed_quote_key.hpp — same four
// members, same comparison order — because the OPRA join now FILTERS panel rows
// by the same key this file LOOKS QUOTES UP by. Two private definitions of "same
// contract" could drift apart, and the filter would then drop a leg this file
// cannot mark; one public definition makes them agree by construction.

[[nodiscard]] bool finite(double value) noexcept { return std::isfinite(value); }

void append_u64(std::string &out, std::uint64_t value) {
  char buffer[32];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof buffer, value);
  (void)error;
  out.append(buffer, end);
}

void append_i64(std::string &out, std::int64_t value) {
  char buffer[32];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof buffer, value);
  (void)error;
  out.append(buffer, end);
}

void append_double(std::string &out, double value) {
  char buffer[64];
  const auto [end, error] =
      std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::general,
                    std::numeric_limits<double>::max_digits10);
  (void)error;
  out.append(buffer, end);
}

void append_optional_double(std::string &out, double value, bool present) {
  if (present) {
    append_double(out, value);
  } else {
    out.append("NA");
  }
}

[[nodiscard]] Result<std::map<ListedQuoteKey, const ListedOptionQuote *>>
quote_index(const ListedReconciliationSnapshot &snapshot) {
  std::map<ListedQuoteKey, const ListedOptionQuote *> out;
  for (const ListedOptionQuote &quote : snapshot.quotes) {
    if (quote.trade_date != snapshot.date || quote.quote_ts_ns > snapshot.valuation_ts_ns ||
        quote.quote_ts_ns <= 0) {
      return Err(ErrorCode::InvalidArgument,
                 "listed reconciliation: quote date or timestamp mismatch");
    }
    const auto [position, inserted] = out.emplace(quote_key_of(quote), &quote);
    (void)position;
    if (!inserted) {
      return Err(ErrorCode::AlreadyExists, "listed reconciliation: duplicate daily contract key");
    }
  }
  return Ok(std::move(out));
}

[[nodiscard]] Result<ListedContractMark>
mark_leg(const ListedScheduleLeg &leg, const ListedReconciliationSnapshot &snapshot,
         ListedMarkRole role, const std::map<ListedQuoteKey, const ListedOptionQuote *> &quotes,
         const ListedReconciliationConfig &config) {
  ListedContractMark mark;
  mark.date = snapshot.date;
  mark.valuation_ts_ns = snapshot.valuation_ts_ns;
  mark.role = role;
  mark.cohort = leg.cohort;
  mark.symbol = leg.symbol;
  mark.uid = leg.uid;
  mark.raw_symbol = leg.raw_symbol;
  mark.expiry_ts_ns = leg.expiry_ts_ns;
  mark.strike = leg.strike;
  mark.side = leg.side;
  mark.quantity = leg.quantity;
  mark.multiplier = leg.multiplier;

  const SurfaceRef surface = snapshot.surfaces->find(leg.uid);
  if (surface == nullptr) {
    mark.status = ListedMarkStatus::NoSurface;
    if (config.strict_model) {
      return Err(ErrorCode::NotFound, "listed reconciliation: held surface missing");
    }
  } else {
    const double residual_t =
        static_cast<double>(leg.expiry_ts_ns - snapshot.valuation_ts_ns) / kNsPerYear;
    const Result<double> model = surface->fair_value(leg.strike, residual_t, leg.side);
    if (!model || !finite(*model)) {
      mark.status = ListedMarkStatus::PricingError;
      if (config.strict_model) {
        return Err(model ? ErrorCode::Unavailable : model.error().code(),
                   "listed reconciliation: held contract pricing failed");
      }
    } else {
      mark.model_mark = *model;
      // M3: relative few-ULP tolerance. The build route (evaluate) and reconcile
      // route (fair_value) can disagree by a handful of ULPs on a real board; a
      // relative bound absorbs that benign divergence while still rejecting a true
      // schedule/archive economic mismatch. (tol == 0 => strict bit-for-bit.)
      if (role == ListedMarkRole::Entry &&
          !listed_entry_mark_agrees(mark.model_mark, leg.model_mark,
                                    config.entry_mark_tolerance)) {
        return Err(ErrorCode::InvalidArgument,
                   "listed reconciliation: entry archive mark differs from schedule");
      }
    }
  }

  const auto raw = quotes.find(quote_key_of(leg));
  if (raw == quotes.end()) {
    if (mark.status == ListedMarkStatus::Ok) {
      mark.status = ListedMarkStatus::NoRawQuote;
    }
    return Ok(std::move(mark));
  }
  const ListedOptionQuote &quote = *raw->second;
  if (quote.symbol != leg.symbol || quote.multiplier != leg.multiplier) {
    return Err(ErrorCode::InvalidArgument,
               "listed reconciliation: daily quote economics disagree with schedule");
  }
  mark.instrument_id = quote.instrument_id;
  mark.raw_bid = quote.bid;
  mark.raw_ask = quote.ask;
  if (!is_valid_listed_quote(quote)) {
    if (mark.status == ListedMarkStatus::Ok) {
      // FIX-F M3: distinguish the reason. F6's `bid > 0` tightening routed every
      // zero-bid quote through the crossed-book label; a zero bid is an absent
      // bid, not an inverted book. The drop itself is unchanged.
      const bool zero_bid = std::isfinite(quote.bid) && !(quote.bid > 0.0) &&
                            std::isfinite(quote.ask) && quote.ask > 0.0;
      mark.status = zero_bid ? ListedMarkStatus::ZeroBidQuote : ListedMarkStatus::CrossedQuote;
    }
    return Ok(std::move(mark));
  }
  mark.raw_mid = 0.5 * (quote.bid + quote.ask);
  mark.model_in_spread = mark.model_mark >= quote.bid && mark.model_mark <= quote.ask;
  return Ok(std::move(mark));
}

[[nodiscard]] bool has_raw_mid(const ListedContractMark &mark) noexcept {
  return mark.status == ListedMarkStatus::Ok;
}

} // namespace

const char *to_string(ListedMarkRole role) noexcept {
  switch (role) {
  case ListedMarkRole::Entry:
    return "Entry";
  case ListedMarkRole::Held:
    return "Held";
  }
  return "Unknown";
}

const char *to_string(ListedMarkStatus status) noexcept {
  switch (status) {
  case ListedMarkStatus::Ok:
    return "Ok";
  case ListedMarkStatus::NoRawQuote:
    return "NoRawQuote";
  case ListedMarkStatus::CrossedQuote:
    return "CrossedQuote";
  case ListedMarkStatus::NoSurface:
    return "NoSurface";
  case ListedMarkStatus::PricingError:
    return "PricingError";
  case ListedMarkStatus::ZeroBidQuote:
    return "ZeroBidQuote";
  }
  return "Unknown";
}

Result<ListedDispersionReconciliation>
reconcile_listed_dispersion(const ListedDispersionSchedule &schedule,
                            std::span<const ListedReconciliationSnapshot> snapshots,
                            const ListedReconciliationConfig &config) {
  ATX_TRY_VOID(validate_listed_dispersion_schedule(schedule));
  if (snapshots.empty() || !finite(config.entry_mark_tolerance) ||
      config.entry_mark_tolerance < 0.0) {
    return Err(ErrorCode::InvalidArgument, "listed reconciliation: invalid config or timeline");
  }
  // C2: the schedule builder legitimately DEFERS the first roll past the first
  // snapshot (coverage gate), so the first entry need not land on snapshots.front().
  // Do NOT require snapshots.front().date == rolls.front().roll_date; instead the
  // pre-entry snapshots are emitted as flat, position-free rows and the first entry
  // is recorded when its roll_date is reached — one row per snapshot throughout, so
  // the timeline stays aligned with the canonical backtest (which likewise emits a
  // flat leading row per pre-entry date). The first roll_date must still appear in
  // the timeline exactly (a missing entry date is a hard error, below).

  ListedDispersionReconciliation out;
  std::size_t active_roll = 0;
  bool entered = false; // has the first roll's Entry been recorded yet
  std::map<ListedQuoteKey, ListedContractMark> previous_marks;
  double model_nav = 0.0;
  double quote_nav = 0.0;

  for (std::size_t date_index = 0; date_index < snapshots.size(); ++date_index) {
    const ListedReconciliationSnapshot &snapshot = snapshots[date_index];
    if (snapshot.date.empty() || snapshot.valuation_ts_ns <= 0 || snapshot.surfaces == nullptr ||
        (date_index > 0 &&
         (snapshot.date <= snapshots[date_index - 1].date ||
          snapshot.valuation_ts_ns <= snapshots[date_index - 1].valuation_ts_ns))) {
      return Err(ErrorCode::InvalidArgument,
                 "listed reconciliation: snapshots not strictly ordered");
    }
    ATX_TRY(auto quotes, quote_index(snapshot));

    ListedReconciliationRow row;
    row.date = snapshot.date;
    row.valuation_ts_ns = snapshot.valuation_ts_ns;
    row.held_cohort = schedule.rolls[active_roll].cohort;

    if (!entered) {
      const std::string &first_roll_date = schedule.rolls.front().roll_date;
      if (snapshot.date < first_roll_date) {
        // Pre-entry flat date: no position is held yet. Emit an aligned zero row.
        row.n_held_lots = 0u;
        row.n_quote_mid_lots = 0u;
        row.quote_mid_coverage = 0.0;
        out.rows.push_back(std::move(row));
        continue;
      }
      if (snapshot.date > first_roll_date) {
        return Err(ErrorCode::InvalidArgument,
                   "listed reconciliation: first scheduled roll date missing from timeline");
      }
      // snapshot.date == first_roll_date: record the first cohort's Entry marks.
      const ListedScheduleRoll &entry = schedule.rolls.front();
      for (const ListedScheduleLeg &leg : entry.legs) {
        ATX_TRY(ListedContractMark mark,
                mark_leg(leg, snapshot, ListedMarkRole::Entry, quotes, config));
        previous_marks.emplace(quote_key_of(leg), mark);
        out.marks.push_back(std::move(mark));
      }
      row.n_held_lots = static_cast<std::uint32_t>(entry.legs.size());
      row.n_quote_mid_lots = static_cast<std::uint32_t>(
          std::count_if(out.marks.begin(), out.marks.end(), has_raw_mid));
      row.quote_mid_coverage = row.n_held_lots == 0 ? 0.0
                                                    : static_cast<double>(row.n_quote_mid_lots) /
                                                          static_cast<double>(row.n_held_lots);
      entered = true;
      out.rows.push_back(std::move(row));
      continue;
    }

    const ListedScheduleRoll &held = schedule.rolls[active_roll];
    std::map<ListedQuoteKey, ListedContractMark> current_held;
    for (const ListedScheduleLeg &leg : held.legs) {
      ATX_TRY(ListedContractMark mark,
              mark_leg(leg, snapshot, ListedMarkRole::Held, quotes, config));
      const auto previous = previous_marks.find(quote_key_of(leg));
      if (previous == previous_marks.end()) {
        return Err(ErrorCode::InvalidArgument, "listed reconciliation: previous held mark missing");
      }
      const double scale = leg.quantity * leg.multiplier;
      row.model_option_pnl += scale * (mark.model_mark - previous->second.model_mark);
      ++row.n_held_lots;
      if (has_raw_mid(mark) && has_raw_mid(previous->second)) {
        row.quote_mid_pnl += scale * (mark.raw_mid - previous->second.raw_mid);
        ++row.n_quote_mid_lots;
      }
      current_held.emplace(quote_key_of(leg), mark);
      out.marks.push_back(std::move(mark));
    }
    row.model_minus_quote_pnl = row.model_option_pnl - row.quote_mid_pnl;
    model_nav += row.model_option_pnl;
    quote_nav += row.quote_mid_pnl;
    row.model_nav = model_nav;
    row.quote_mid_nav = quote_nav;
    row.quote_mid_coverage = row.n_held_lots == 0 ? 0.0
                                                  : static_cast<double>(row.n_quote_mid_lots) /
                                                        static_cast<double>(row.n_held_lots);

    const std::size_t next_roll = active_roll + 1;
    if (next_roll < schedule.rolls.size() && schedule.rolls[next_roll].roll_date < snapshot.date) {
      return Err(ErrorCode::InvalidArgument,
                 "listed reconciliation: scheduled roll date missing from timeline");
    }
    if (next_roll < schedule.rolls.size() && schedule.rolls[next_roll].roll_date == snapshot.date) {
      const ListedScheduleRoll &entry = schedule.rolls[next_roll];
      previous_marks.clear();
      for (const ListedScheduleLeg &leg : entry.legs) {
        ATX_TRY(ListedContractMark mark,
                mark_leg(leg, snapshot, ListedMarkRole::Entry, quotes, config));
        previous_marks.emplace(quote_key_of(leg), mark);
        out.marks.push_back(std::move(mark));
      }
      active_roll = next_roll;
    } else {
      previous_marks = std::move(current_held);
    }
    out.rows.push_back(std::move(row));
  }

  if (!entered) {
    return Err(ErrorCode::InvalidArgument,
               "listed reconciliation: no snapshot on/after the first scheduled roll date");
  }
  if (active_roll + 1 != schedule.rolls.size()) {
    return Err(ErrorCode::InvalidArgument,
               "listed reconciliation: timeline does not consume every scheduled roll");
  }
  return Ok(std::move(out));
}

Status validate_listed_reconciliation_backtest(const ListedDispersionReconciliation &reconciliation,
                                               const BacktestResult &backtest,
                                               double absolute_tolerance) {
  if (!finite(absolute_tolerance) || absolute_tolerance < 0.0) {
    return Err(ErrorCode::InvalidArgument, "listed reconciliation/backtest: invalid tolerance");
  }
  if (reconciliation.rows.empty()) {
    // An empty reconciliation is consistent only with an empty backtest.
    if (backtest.size() != 0) {
      return Err(ErrorCode::InvalidArgument,
                 "listed reconciliation/backtest: empty reconciliation against a non-empty "
                 "backtest");
    }
    return Ok();
  }
  // M1. The reconciliation timeline is TRIMMED to start at the first roll date
  // (assemble_reconciliation_snapshots): any leading warm-up / low-coverage
  // session ahead of the first roll is dropped, because the strategy emits
  // nothing there. The backtest still carries one row per clock session. So the
  // reconciliation is a contiguous SUFFIX of the backtest, not necessarily the
  // whole of it — requiring equal row counts here rejected exactly the corpus
  // the trim exists to admit, merely moving the abort one call downstream under
  // a misleading message. Align on the reconciliation's first date and require
  // it to run to the end. When the lead-in is zero the offset is zero and this
  // is bit-identical to the historical row-for-row comparison.
  std::size_t offset = backtest.size();
  for (std::size_t i = 0; i < backtest.size(); ++i) {
    if (backtest.date[i] == reconciliation.rows.front().date) {
      offset = i;
      break;
    }
  }
  if (offset == backtest.size()) {
    return Err(ErrorCode::InvalidArgument,
               "listed reconciliation/backtest: reconciliation start date absent from the backtest");
  }
  if (offset + reconciliation.rows.size() != backtest.size()) {
    return Err(ErrorCode::InvalidArgument,
               "listed reconciliation/backtest: reconciliation is not a contiguous suffix of the "
               "backtest");
  }
  for (std::size_t i = 0; i < reconciliation.rows.size(); ++i) {
    const std::size_t b = offset + i;
    if (reconciliation.rows[i].date != backtest.date[b]) {
      return Err(ErrorCode::InvalidArgument, "listed reconciliation/backtest: date mismatch");
    }
    double option_pnl = backtest.pnl_total[b];
    option_pnl -= backtest.pnl_settlement[b];
    option_pnl -= backtest.pnl_shares[b];
    option_pnl -= backtest.financing[b];
    option_pnl += backtest.cost[b];
    if (std::fabs(option_pnl - reconciliation.rows[i].model_option_pnl) > absolute_tolerance) {
      return Err(ErrorCode::InvalidArgument,
                 "listed reconciliation/backtest: model option P&L mismatch");
    }
  }
  return Ok();
}

std::string serialize_listed_contract_marks(const ListedDispersionReconciliation &reconciliation) {
  std::string out = "date\tvaluation_ts_ns\trole\tcohort\tsymbol\tuid\tinstrument_id\traw_symbol\t"
                    "expiry_ts_ns\tstrike\tside\tquantity\tmultiplier\tstatus\traw_bid\traw_ask\t"
                    "raw_mid\tmodel_mark\tmodel_in_spread\n";
  for (const ListedContractMark &mark : reconciliation.marks) {
    out.append(mark.date).push_back('\t');
    append_i64(out, mark.valuation_ts_ns);
    out.push_back('\t');
    out.append(to_string(mark.role)).push_back('\t');
    append_u64(out, mark.cohort);
    out.push_back('\t');
    out.append(mark.symbol).push_back('\t');
    append_u64(out, mark.uid);
    out.push_back('\t');
    append_u64(out, mark.instrument_id);
    out.push_back('\t');
    out.append(mark.raw_symbol).push_back('\t');
    append_i64(out, mark.expiry_ts_ns);
    out.push_back('\t');
    append_double(out, mark.strike);
    out.push_back('\t');
    out.push_back(mark.side == Side::Call ? 'C' : 'P');
    out.push_back('\t');
    append_double(out, mark.quantity);
    out.push_back('\t');
    append_double(out, mark.multiplier);
    out.push_back('\t');
    out.append(to_string(mark.status)).push_back('\t');
    const bool raw = has_raw_mid(mark);
    append_optional_double(out, mark.raw_bid, raw);
    out.push_back('\t');
    append_optional_double(out, mark.raw_ask, raw);
    out.push_back('\t');
    append_optional_double(out, mark.raw_mid, raw);
    out.push_back('\t');
    const bool model =
        mark.status != ListedMarkStatus::NoSurface && mark.status != ListedMarkStatus::PricingError;
    append_optional_double(out, mark.model_mark, model);
    out.push_back('\t');
    out.push_back(mark.model_in_spread ? '1' : '0');
    out.push_back('\n');
  }
  return out;
}

std::string serialize_listed_reconciliation(const ListedDispersionReconciliation &reconciliation) {
  std::string out = "date\tvaluation_ts_ns\theld_cohort\tmodel_option_pnl\tquote_mid_pnl\t"
                    "model_minus_quote_pnl\tmodel_nav\tquote_mid_nav\tquote_mid_coverage\t"
                    "n_held_lots\tn_quote_mid_lots\n";
  for (const ListedReconciliationRow &row : reconciliation.rows) {
    out.append(row.date).push_back('\t');
    append_i64(out, row.valuation_ts_ns);
    out.push_back('\t');
    append_u64(out, row.held_cohort);
    out.push_back('\t');
    append_double(out, row.model_option_pnl);
    out.push_back('\t');
    append_double(out, row.quote_mid_pnl);
    out.push_back('\t');
    append_double(out, row.model_minus_quote_pnl);
    out.push_back('\t');
    append_double(out, row.model_nav);
    out.push_back('\t');
    append_double(out, row.quote_mid_nav);
    out.push_back('\t');
    append_double(out, row.quote_mid_coverage);
    out.push_back('\t');
    append_u64(out, row.n_held_lots);
    out.push_back('\t');
    append_u64(out, row.n_quote_mid_lots);
    out.push_back('\n');
  }
  return out;
}

} // namespace atx::vol
