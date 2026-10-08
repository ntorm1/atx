#include "research_paper.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace atx::server::research_detail {
namespace {
namespace book = atx::engine::book;

std::uint32_t index(std::size_t value) {
  if (value >= 63) throw std::runtime_error("paper native index exceeds bounded calendar");
  return static_cast<std::uint32_t>(value);
}

void reconcile(double actual, double reconstructed, std::string_view label) {
  const auto scale = std::max({1.0, std::abs(actual), std::abs(reconstructed)});
  if (!std::isfinite(actual) || !std::isfinite(reconstructed) ||
      std::abs(actual - reconstructed) > 1024 * std::numeric_limits<double>::epsilon() * scale)
    throw std::runtime_error("paper ledger does not reconcile: " + std::string(label));
}

std::vector<PaperPosition> positions(const DatasetPanel& data, std::span<const double> close,
                                   std::span<const double> units, std::size_t day) {
  std::vector<PaperPosition> result;
  result.reserve(units.size());
  for (std::size_t name = 0; name < units.size(); ++name) {
    const auto mark = close[day * units.size() + name];
    const auto dollars = units[name] == 0 ? 0 : units[name] * mark;
    if (!std::isfinite(units[name]) || !std::isfinite(dollars) ||
        (units[name] != 0 && (!std::isfinite(mark) || mark <= 0)))
      throw std::runtime_error("paper held position has no valid TRI mark");
    result.push_back({data.instruments.at(name), units[name], dollars});
  }
  return result;
}

PaperAllocation allocation(const book::ReplayAllocation& native, const Json& diagnostic) {
  PaperAllocation out;
  out.decision_session_index = index(native.decision_period);
  out.execution_session_index = index(native.execution_period);
  out.target_weights = native.target_weights;
  out.pretrade_held_weights = diagnostic.at("pretrade_held_weights").get<std::vector<double>>();
  out.achieved_weights = diagnostic.at("achieved_posttrade_weights").get<std::vector<double>>();
  out.pretrade_cash = native.pretrade_cash; out.pretrade_nav = native.pretrade_nav;
  out.posttrade_cash = native.posttrade_cash; out.posttrade_nav = native.posttrade_nav;
  out.traded_dollars = native.traded_dollars; out.trade_cost = native.trade_cost;
  out.target_net = diagnostic.at("target_net").get<double>();
  out.target_gross = diagnostic.at("target_gross").get<double>();
  out.target_max_name = diagnostic.at("target_max_name").get<double>();
  out.target_turnover = diagnostic.at("target_turnover").get<double>();
  out.solver_primal_residual = diagnostic.at("solver_primal_residual").get<double>();
  out.solver_dual_residual = diagnostic.at("solver_dual_residual").get<double>();
  out.solver_rho_final = diagnostic.at("solver_rho_final").get<double>();
  out.solver_polished = diagnostic.at("solver_polished").get<bool>();
  return out;
}

std::string source_name(book::TerminalReturnSource source) {
  switch (source) {
    case book::TerminalReturnSource::Table: return "table";
    case book::TerminalReturnSource::LastMarkZero: return "last_mark_zero";
    case book::TerminalReturnSource::ShumwayNyseAmex: return "shumway_nyse_amex";
    case book::TerminalReturnSource::ShumwayNasdaq: return "shumway_nasdaq";
    case book::TerminalReturnSource::ShumwayUnknownAdverse: return "shumway_unknown_adverse";
    case book::TerminalReturnSource::AssumedMissingPriceAdverse:
      return "assumed_missing_price_adverse";
  }
  throw std::runtime_error("unknown native paper liquidation source");
}

void collect_fills(PaperReplayResult& result, const DatasetPanel& data,
    const PortfolioRecipe& recipe, const PortfolioReplayOutput& output,
    std::span<const double> close) {
  std::size_t next_allocation = 0;
  for (const auto& trade : output.replay.replay.trades) {
    while (next_allocation < output.replay.allocations.size() &&
           output.replay.allocations[next_allocation].execution_period < trade.period)
      ++next_allocation;
    if (next_allocation == output.replay.allocations.size() ||
        output.replay.allocations[next_allocation].execution_period != trade.period)
      throw std::runtime_error("paper fill has no native allocation");
    const auto& sizing = output.sizing.at(next_allocation);
    const auto old_units = sizing.old_units.at(trade.instrument);
    const auto new_units = sizing.new_units.at(trade.instrument);
    const auto mark = close[trade.period * data.instruments.size() + trade.instrument];
    // Same representable target units and operation order as native scalar sizing.
    const auto delta = new_units * mark - old_units * mark;
    if (delta != trade.dollar_delta)
      throw std::runtime_error("paper fill does not match exact native unit sizing");
    result.fills.push_back({index(trade.period), index(trade.decision_period),
        data.instruments.at(trade.instrument), old_units, new_units, mark, trade.dollar_delta,
        std::abs(trade.dollar_delta) * (recipe.data.trade_bps * 1e-4)});
  }
}

void collect_liquidations(PaperReplayResult& result, const DatasetPanel& data,
                          const book::ReplayResult& native) {
  for (const auto& row : native.delistings)
    result.liquidations.push_back({index(row.period), data.instruments.at(row.instrument),
        row.tri_units, row.last_value, row.delist_return, row.proceeds, source_name(row.source),
        row.source == book::TerminalReturnSource::AssumedMissingPriceAdverse, row.flagged});
  for (const auto& row : native.unfilled_targets)
    result.unfilled_targets.push_back({index(row.period), index(row.decision_period),
        data.instruments.at(row.instrument), row.weight});
}

void apply_allocation(std::size_t current, const PortfolioReplayOutput& output,
    std::vector<double>& units, double& cash, std::span<const double> close) {
  const auto& row = output.replay.allocations.at(current);
  const auto& sizing = output.sizing.at(current);
  if (units != sizing.old_units || sizing.new_units.size() != units.size())
    throw std::runtime_error("paper units differ from native allocation start state");
  reconcile(cash, row.pretrade_cash, "pretrade cash");
  double traded = 0;
  for (std::size_t name = 0; name < units.size(); ++name) {
    const auto mark = close[row.execution_period * units.size() + name];
    const auto dollars = sizing.new_units[name] == 0 ? 0 : sizing.new_units[name] * mark;
    if (dollars != row.posttrade_marked_dollars[name])
      throw std::runtime_error("paper target units differ from native posttrade dollars");
    const auto delta = dollars - row.pretrade_marked_dollars[name];
    if (delta != 0) { cash -= delta; traded += std::abs(delta); }
  }
  reconcile(traded, row.traded_dollars, "traded dollars");
  cash -= row.trade_cost;
  reconcile(cash, row.posttrade_cash, "posttrade cash");
  units = sizing.new_units;
}

void apply_liquidations(std::size_t end, const book::ReplayResult& native,
                       std::vector<double>& units, double& cash) {
  for (const auto& row : native.delistings) if (row.period == end) {
    if (units.at(row.instrument) != row.tri_units)
      throw std::runtime_error("paper removed units differ from native liquidation");
    reconcile(row.proceeds, row.last_value * (1 + row.delist_return), "liquidation proceeds");
    cash += row.proceeds;
    units[row.instrument] = 0;
  }
}

PaperValuation valuation(const DatasetPanel& data, std::span<const double> close,
    std::span<const double> units, const book::ReplayInterval& native) {
  PaperValuation row{index(native.end_period), native.cash, native.assets, native.end_gross,
      native.nav, native.gross_pnl, native.trade_cost, native.borrow_cost, native.net_return,
      positions(data, close, units, native.end_period)};
  double assets = 0, gross = 0;
  for (const auto& position : row.positions) {
    assets += position.marked_dollars; gross += std::abs(position.marked_dollars);
  }
  reconcile(assets, row.assets, "marked assets"); reconcile(gross, row.gross, "marked gross");
  reconcile(row.cash + row.assets, row.nav, "cash plus assets NAV");
  reconcile(native.pretrade_nav + row.gross_pnl - row.trade_fees - row.borrow_charge,
            row.nav, "interval NAV bridge");
  return row;
}

void collect_valuations(PaperReplayResult& result, const DatasetPanel& data,
    const PortfolioRecipe& recipe, const PortfolioReplayOutput& output,
    std::span<const double> close, std::stop_token stop) {
  const auto& native = output.replay.replay;
  std::vector<double> units(data.instruments.size(), 0);
  double cash = native.initial_nav;
  result.valuations.push_back({0, cash, 0, 0, cash, 0, 0, 0, 0,
                              positions(data, close, units, 0)});
  std::size_t next_allocation = 0;
  for (const auto& interval : native.intervals) {
    checkpoint(stop, {}, 0, {});
    if (next_allocation < output.replay.allocations.size() &&
        output.replay.allocations[next_allocation].execution_period == interval.start_period)
      apply_allocation(next_allocation++, output, units, cash, close);
    double shorts = 0;
    for (const auto& position : positions(data, close, units, interval.start_period))
      if (position.marked_dollars < 0) shorts -= position.marked_dollars;
    const auto begin_key = data.sessions[data.score_begin + interval.start_period];
    const auto end_key = data.sessions[data.score_begin + interval.end_period];
    const auto days = static_cast<double>(end_key - begin_key) / 86'400'000'000'000.0;
    const auto expected_borrow = shorts * ((recipe.data.borrow_bps * 1e-4 / 365) * days);
    reconcile(interval.borrow_cost, expected_borrow, "calendar-day borrow");
    result.financing.push_back({index(interval.start_period), index(interval.end_period),
                               interval.borrow_cost, days, 365});
    cash -= interval.borrow_cost;
    apply_liquidations(interval.end_period, native, units, cash);
    reconcile(cash, interval.cash, "completed interval cash");
    result.valuations.push_back(valuation(data, close, units, interval));
  }
  if (units != native.final_tri_units || next_allocation != output.replay.allocations.size())
    throw std::runtime_error("paper final units/allocation count do not match native replay");
  reconcile(cash, native.final_cash, "terminal cash");
  result.final_positions = result.valuations.back().positions;
}
} // namespace

PaperReplayResult collect_paper_ledger(const DatasetPanel& data, const PortfolioRecipe& recipe,
    const PortfolioReplayOutput& output, std::stop_token stop) {
  checkpoint(stop, {}, 0, {});
  if (output.sizing.size() != output.replay.allocations.size() ||
      output.diagnostics.size() != output.replay.allocations.size())
    throw std::runtime_error("paper requires every native allocation and exact sizing snapshot");
  const auto panel = make_dataset_score_panel(data);
  const auto close = panel.field_all(panel.field_id("close").value());
  PaperReplayResult result;
  result.cohort_security_ids = data.instruments;
  result.through_session_count = static_cast<std::uint32_t>(panel.dates());
  result.allocations.reserve(output.replay.allocations.size());
  for (std::size_t i = 0; i < output.replay.allocations.size(); ++i)
    result.allocations.push_back(allocation(output.replay.allocations[i], output.diagnostics[i]));
  collect_fills(result, data, recipe, output, close);
  collect_liquidations(result, data, output.replay.replay);
  collect_valuations(result, data, recipe, output, close, stop);
  checkpoint(stop, {}, 0, {});
  return result;
}
} // namespace atx::server::research_detail
