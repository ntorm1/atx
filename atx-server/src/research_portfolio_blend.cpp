#include "research_portfolio.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <memory>
#include <stdexcept>

#include "atx/engine/combine/combined_source.hpp"
#include "research_dsl.hpp"

namespace atx::server::research_detail {
namespace {
namespace engine = atx::engine;
namespace alpha = engine::alpha;

/// A causal cached DSL root. The input view's actual date axis and membership select
/// the immutable row, so repeated evaluation of the same view returns the same signal.
class RowSignalSource final : public engine::ISignalSource {
public:
  RowSignalSource(const alpha::SignalSet& signal, std::size_t lookback)
      : signal_(signal), lookback_(lookback), output_(signal.instruments) {}
  ~RowSignalSource() override = default;
  RowSignalSource(const RowSignalSource&) = delete;
  RowSignalSource& operator=(const RowSignalSource&) = delete;
  RowSignalSource(RowSignalSource&&) = delete;
  RowSignalSource& operator=(RowSignalSource&&) = delete;
  engine::ISignalSource* identity() { return this; }
  std::size_t max_lookback() const noexcept override { return lookback_; }
  atx::core::Result<engine::SignalView> evaluate(engine::PanelView panel) override {
    if (panel.rows() == 0 || panel.rows() > signal_.dates ||
        panel.instruments() != signal_.instruments)
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "portfolio cached root view has incompatible axes");
    const auto row = signal_.alpha_cross_section(0, panel.rows() - 1);
    for (std::size_t name = 0; name < output_.size(); ++name)
      output_[name] = panel.present(0, name) ? row[name] :
          std::numeric_limits<double>::quiet_NaN();
    return engine::SignalView{output_};
  }
private:
  const alpha::SignalSet& signal_;
  std::size_t lookback_;
  std::vector<double> output_;
};

/// Real close/volume and eligibility backing for the loop's ring-view seam.
/// Unbound open/high/low fields stay missing. No future row is addressable in a view.
struct LoopPanel {
  std::size_t capacity{}, names{}, words{};
  std::vector<double> fields;
  std::vector<std::uint64_t> mask;
  std::vector<engine::InstrumentId> universe;

  explicit LoopPanel(const DatasetPanel& data)
      : capacity(std::bit_ceil(data.panel.dates())), names(data.panel.instruments()),
        words((names + 63) / 64),
        fields(engine::kPanelFieldCount * capacity * names,
               std::numeric_limits<double>::quiet_NaN()), mask(capacity * words, 0) {
    for (std::size_t name = 0; name < names; ++name)
      universe.push_back({static_cast<std::uint32_t>(name)});
    const auto close = data.panel.field_all(data.panel.field_id("close").value());
    const auto volume = data.panel.field_all(data.panel.field_id("volume").value());
    for (std::size_t day = 0; day < data.panel.dates(); ++day) {
      for (std::size_t name = 0; name < names; ++name) {
        const auto index = day * names + name;
        fields[static_cast<std::size_t>(engine::PanelField::Close) * capacity * names + index] =
            close[index];
        fields[static_cast<std::size_t>(engine::PanelField::Volume) * capacity * names + index] =
            volume[index];
        if (data.panel.in_universe(day, name))
          mask[day * words + name / 64] |= std::uint64_t{1} << (name % 64);
      }
    }
  }

  engine::PanelView view(std::size_t day) const {
    return {fields.data(), mask.data(), universe, capacity, day, day + 1, words};
  }
};
} // namespace

alpha::SignalSet blend_portfolio_signals(const DatasetPanel& data, const PortfolioRecipe& recipe,
    const ResearchOptions& options, std::stop_token stop, const Progress& progress) {
  std::vector<alpha::SignalSet> signals;
  signals.reserve(recipe.components.size());
  const auto cells = static_cast<std::uint64_t>(data.panel.cells());
  for (const auto& component : recipe.components) {
    checkpoint(stop, progress, 0.46 + 0.09 * static_cast<double>(signals.size()) /
        static_cast<double>(recipe.components.size()), "Evaluating frozen constituent DSL roots");
    const auto retained = static_cast<std::uint64_t>(signals.size()) * cells * sizeof(double);
    signals.push_back(evaluate_research_signal(component.effective_expression, data.panel,
        options.max_working_bytes - retained, stop));
  }
  std::vector<std::unique_ptr<RowSignalSource>> adapters;
  std::vector<engine::ISignalSource*> sources;
  engine::combine::Combination frozen{{}, 0, 0};
  for (std::size_t i = 0; i < signals.size(); ++i) {
    adapters.push_back(std::make_unique<RowSignalSource>(signals[i],
        recipe.components[i].program.required_lookback));
    sources.push_back(adapters.back()->identity());
    frozen.weights.push_back(recipe.components[i].weight);
  }
  engine::combine::CombinedSignalSource combined(std::move(sources), std::move(frozen),
      engine::combine::CombineMethod::EqualWeight,
      engine::combine::CombinedSourceRule::StandardizedFixedGrossV2);
  const LoopPanel panel(data);
  alpha::SignalSet result;
  result.dates = data.panel.dates(); result.instruments = data.panel.instruments();
  result.alphas.push_back({"manual_portfolio", std::vector<double>(data.panel.cells(),
      std::numeric_limits<double>::quiet_NaN())});
  for (std::size_t day = 0; day < data.panel.dates(); ++day) {
    checkpoint(stop, {}, 0, {});
    const auto row = combined.evaluate(panel.view(day));
    if (!row) throw std::invalid_argument(row.error().message());
    std::copy(row->values.begin(), row->values.end(), result.alphas[0].values.begin() +
        static_cast<std::ptrdiff_t>(day * result.instruments));
  }
  checkpoint(stop, progress, 0.56, "Decision-time native fixed-gross signal blend complete");
  return result;
}
} // namespace atx::server::research_detail
