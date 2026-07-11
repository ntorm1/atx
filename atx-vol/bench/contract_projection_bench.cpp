// Contract-template projection throughput. The prepared cases model a SPY plus
// 50-name dispersion universe; every request resolves a concrete 40-delta,
// three-calendar-month call from a fitted historical surface.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/contract_projection.hpp"

#include "bench_util.hpp"
#include "support/synth_book.hpp"

namespace {

using atx::vol::OptionProjectionConfig;
using atx::vol::OptionProjectionOutput;
using atx::vol::OptionProjectionSpec;
using atx::vol::PreparedOptionProjection;
using atx::vol::ProjectedMaturitySpec;
using atx::vol::ProjectedOption;
using atx::vol::ProjectedStrikeSpec;
using atx::vol::Side;
using atx::vol::bench::apply_common;
using atx::vol::bench::SynthMarket;

constexpr int kUnderlyings = 51;
constexpr int kSlices = 6;
constexpr int kConvexNodes = 40;

[[nodiscard]] const SynthMarket &market() {
  static const SynthMarket value =
      atx::vol::bench::build_market(kUnderlyings, kSlices, kConvexNodes);
  return value;
}

[[nodiscard]] std::vector<OptionProjectionSpec> make_specs(std::size_t count) {
  std::vector<OptionProjectionSpec> specs;
  specs.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    OptionProjectionSpec spec;
    spec.uid = static_cast<std::uint32_t>(i % kUnderlyings + 1u);
    spec.maturity = ProjectedMaturitySpec::months(3);
    spec.strike = ProjectedStrikeSpec::delta(0.40);
    spec.side = (i & 1u) == 0u ? Side::Call : Side::Put;
    specs.push_back(spec);
  }
  return specs;
}

void emit_counters(benchmark::State &state, std::size_t count,
                   const std::vector<ProjectedOption> &output) {
  const double operations = static_cast<double>(state.iterations()) * static_cast<double>(count);
  state.counters["projections_per_s"] = benchmark::Counter(operations, benchmark::Counter::kIsRate);
  double evaluations = 0.0;
  for (const ProjectedOption &option : output) {
    evaluations += option.delta_evaluations;
  }
  state.counters["delta_evaluations_per_projection"] = evaluations / static_cast<double>(count);
  state.counters["n_projections"] = static_cast<double>(count);
}

void run_scalar(benchmark::State &state, OptionProjectionOutput output_kind) {
  const OptionProjectionSpec spec = make_specs(1u).front();
  OptionProjectionConfig config;
  config.output = output_kind;
  const auto *surface = market().base_set().find(spec.uid);
  ProjectedOption output;
  for (auto _ : state) {
    auto projected = atx::vol::project_option_contract(*surface, spec, config);
    if (!projected) {
      state.SkipWithError(projected.error().to_string().c_str());
      break;
    }
    output = std::move(*projected);
    benchmark::DoNotOptimize(output);
  }
  emit_counters(state, 1u, std::vector<ProjectedOption>{output});
}

void run_prepared(benchmark::State &state, std::size_t count, unsigned threads,
                  OptionProjectionOutput output_kind) {
  const std::vector<OptionProjectionSpec> specs = make_specs(count);
  auto prepared = PreparedOptionProjection::create(specs).value();
  std::vector<ProjectedOption> output(count);
  OptionProjectionConfig config;
  config.output = output_kind;
  config.n_threads = threads;
  for (auto _ : state) {
    const auto status = prepared.project_into(market().base_set(), output, config);
    if (!status) {
      state.SkipWithError(status.error().to_string().c_str());
      break;
    }
    benchmark::DoNotOptimize(output.data());
    benchmark::ClobberMemory();
  }
  emit_counters(state, count, output);
  state.counters["threads"] = static_cast<double>(threads);
}

[[nodiscard]] const char *output_name(OptionProjectionOutput output) {
  switch (output) {
  case OptionProjectionOutput::DefinitionOnly:
    return "definition";
  case OptionProjectionOutput::Mark:
    return "mark";
  case OptionProjectionOutput::FullGreeks:
    return "full_greeks";
  }
  return "unknown";
}

void register_all() {
  for (const auto output : {OptionProjectionOutput::DefinitionOnly, OptionProjectionOutput::Mark,
                            OptionProjectionOutput::FullGreeks}) {
    const std::string scalar_name = std::string("projection/scalar/") + output_name(output);
    apply_common(benchmark::RegisterBenchmark(scalar_name, [output](benchmark::State &state) {
      run_scalar(state, output);
    }))->Unit(benchmark::kMicrosecond);
    for (const std::size_t count : {std::size_t{1}, std::size_t{51}}) {
      for (const unsigned threads : {1u, 4u, 8u, 16u}) {
        char name[128];
        std::snprintf(name, sizeof name, "projection/prepared/%s/n%zu/t%u", output_name(output),
                      count, threads);
        apply_common(
            benchmark::RegisterBenchmark(name,
                                         [count, threads, output](benchmark::State &state) {
                                           run_prepared(state, count, threads, output);
                                         }))
            ->Unit(benchmark::kMicrosecond)
            ->UseRealTime();
      }
    }
  }
}

const bool kRegistered = [] {
  register_all();
  return true;
}();

} // namespace
