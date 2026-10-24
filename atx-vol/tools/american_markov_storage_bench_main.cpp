#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

#include <nlohmann/json.hpp>

#include "atx/vol/api/pricing/american_markov_batch.hpp"
#include "atx/vol/api/pricing/american_markov_grid.hpp"

// Standalone executable only. Counts ordinary and aligned C++ allocations on
// this thread, armed ONLY across prepared successful pricing calls. It does not
// intercept direct C malloc, operating-system allocation, or other threads.
namespace allocation_probe {
thread_local bool armed = false;
thread_local std::size_t count = 0;
void record() noexcept { if (armed) ++count; }
void *allocate_aligned(std::size_t size, std::size_t alignment) {
  record();
  const auto bytes = std::max(size, std::size_t{1});
#if defined(_WIN32)
  void *pointer = _aligned_malloc(bytes, alignment);
#else
  if (bytes > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
    throw std::bad_alloc();
  }
  void *pointer = std::aligned_alloc(alignment, (bytes + alignment - 1) / alignment * alignment);
#endif
  if (pointer == nullptr) throw std::bad_alloc();
  return pointer;
}
void free_aligned(void *pointer) noexcept {
#if defined(_WIN32)
  _aligned_free(pointer);
#else
  std::free(pointer);
#endif
}
struct Window {
  Window() noexcept { count = 0; armed = true; }
  ~Window() { armed = false; }
  std::size_t finish() const noexcept { armed = false; return count; }
};
} // namespace allocation_probe

void *operator new(std::size_t size) {
  allocation_probe::record();
  void *pointer = std::malloc(std::max(size, std::size_t{1}));
  if (pointer == nullptr) throw std::bad_alloc();
  return pointer;
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *pointer) noexcept { std::free(pointer); }
void operator delete[](void *pointer) noexcept { std::free(pointer); }
void operator delete(void *pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void *pointer, std::size_t) noexcept { std::free(pointer); }
void *operator new(std::size_t size, std::align_val_t alignment) {
  return allocation_probe::allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void *operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}
void operator delete(void *pointer, std::align_val_t) noexcept {
  allocation_probe::free_aligned(pointer);
}
void operator delete[](void *pointer, std::align_val_t) noexcept {
  allocation_probe::free_aligned(pointer);
}
void operator delete(void *pointer, std::size_t, std::align_val_t) noexcept {
  allocation_probe::free_aligned(pointer);
}
void operator delete[](void *pointer, std::size_t, std::align_val_t) noexcept {
  allocation_probe::free_aligned(pointer);
}

namespace {
using namespace atx::vol;
using Json = nlohmann::ordered_json;
using Clock = std::chrono::steady_clock;

struct Fixture {
  std::string name;
  std::vector<AmericanMarkovInterval> intervals;
  double width = 0.1;
};

std::vector<Fixture> fixtures() {
  static constexpr std::array local{AmericanMarkovLocalVolNode{-0.3, 1.2},
                                    AmericanMarkovLocalVolNode{0.0, 1.0},
                                    AmericanMarkovLocalVolNode{0.3, 0.8}};
  return {
      {"martingale", {{0.25, 0.25, 0.0, 0.0, 0.2, 0.0}}},
      {"short_low_vol", {{7.0 / 365.0, 1.0 / 252.0, 0.04, 0.01, 0.02, 0.0}},
       0.02 * std::sqrt(1.0 / 252.0)},
      {"high_vol", {{1.0, 1.0, 0.02, 0.0, 0.8, 0.0}}, 0.8},
      {"dated_cash", {{0.17, 0.10, 0.04, 0.01, 0.25, 2.0},
                       {0.30, 0.20, 0.03, 0.01, 0.30, 0.0},
                       {0.60, 0.45, 0.03, 0.01, 0.28, 1.0}}, 0.2},
      {"local_vol", {{0.10, 0.06, -0.03, 0.01, 0.25, 1.0, local},
                      {0.20, 0.15, 0.03, 0.0, 0.30, 0.0, local}}, 0.2},
      {"clock_pause", {{0.02, 0.01, 0.03, 0.01, 0.3, 0.0},
                        {0.03, 0.01, -0.01, 0.02, 0.3, 0.0},
                        {0.10, 0.07, 0.03, 0.01, 0.3, 0.0}}},
      {"deterministic_cash", {{0.17, 0.10, -0.03, -0.06, 0.0, 2.0},
                               {0.30, 0.20, 0.03, 0.01, 0.0, 0.0},
                               {0.60, 0.45, -0.02, 0.01, 0.0, 120.0}}}};
}

struct Fingerprint {
  std::uint64_t value = 14695981039346656037ULL;
  void append(std::uint64_t bits) noexcept {
    // Hash explicit little-endian value bytes, never struct padding or addresses.
    for (unsigned shift = 0; shift < 64; shift += 8) {
      value ^= (bits >> shift) & 255U;
      value *= 1099511628211ULL;
    }
  }
  void append(double number) noexcept { append(std::bit_cast<std::uint64_t>(number)); }
};

Json describe(const AmericanMarkovModel &model) {
  Fingerprint hash;
  hash.append(model.spot());
  hash.append(model.initial_state_index());
  hash.append(model.layers().size());
  hash.append(model.transitions().size());
  std::size_t expanded_states = 0;
  std::set<const double *> state_blocks;
  std::set<const AmericanMarkovTransitionRow *> row_blocks;
  std::set<const AmericanMarkovResolventFactor *> factor_blocks;
  std::set<std::pair<std::size_t, std::uint64_t>> diffusion_keys;
  std::size_t physical_states = 0;
  std::size_t physical_rows = 0;
  std::size_t physical_factors = 0;
  for (const auto &layer : model.layers()) {
    hash.append(layer.calendar_time);
    hash.append(layer.variance_time);
    hash.append(layer.growth);
    hash.append(layer.states().size());
    expanded_states += layer.states().size();
    if (state_blocks.insert(layer.states().data()).second) physical_states += layer.states().size();
    for (const double state : layer.states()) hash.append(state);
  }
  for (const auto &transition : model.transitions()) {
    hash.append(static_cast<std::uint64_t>(transition.kind));
    hash.append(transition.interval_index);
    hash.append(transition.discount);
    hash.append(transition.calendar_step);
    hash.append(transition.variance_step);
    hash.append(transition.volatility);
    hash.append(transition.cash_dividend);
    hash.append(transition.rows().size());
    if (!transition.rows().empty() && row_blocks.insert(transition.rows().data()).second) {
      physical_rows += transition.rows().size();
    }
    if (!transition.resolvent_factors().empty() &&
        factor_blocks.insert(transition.resolvent_factors().data()).second) {
      physical_factors += transition.resolvent_factors().size();
    }
    if (transition.kind != AmericanMarkovStepKind::CashDividend) {
      diffusion_keys.emplace(transition.interval_index,
                             std::bit_cast<std::uint64_t>(transition.variance_step));
    }
    for (const auto &row : transition.rows()) {
      for (const auto destination : row.destination) hash.append(std::uint64_t{destination});
      for (const double probability : row.probability) hash.append(probability);
    }
    // Leave explicit-mode fingerprints unchanged for baseline replay.
    for (const auto &factor : transition.resolvent_factors()) {
      hash.append(factor.forward_input_weight);
      hash.append(factor.backward_input_weight);
    }
  }
  return { {"operator_fingerprint", std::to_string(hash.value)},
           {"expanded_state_values", expanded_states},
           {"expanded_transition_rows", model.diagnostics().stored_rows},
           {"physical_state_values", physical_states}, {"physical_transition_rows", physical_rows},
           {"state_blocks", state_blocks.size()}, {"row_blocks", row_blocks.size()},
           {"physical_resolvent_factors", physical_factors},
           {"factor_blocks", factor_blocks.size()},
           {"transition_work", model.diagnostics().transition_work},
           {"max_probability_error", model.diagnostics().max_probability_sum_error},
           {"max_relative_mean_error", model.diagnostics().max_relative_mean_error},
           {"max_forward_mass_error", model.diagnostics().max_forward_mass_error},
           {"distinct_interval_variance_step_bits", diffusion_keys.size()},
           {"transitions", model.transitions().size()},
           {"terminal_zero_probability", model.diagnostics().terminal_zero_probability},
           {"terminal_upper_probability", model.diagnostics().terminal_upper_probability} };
}

double elapsed_us(Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

Json run(const Fixture &fixture, std::size_t repetitions,
         AmericanMarkovDiffusionMethod method, std::size_t steps,
         std::size_t nodes, std::size_t upper, std::optional<double> concentration_scale) {
  const double width = concentration_scale.value_or(fixture.width);
  Json requested{{"fixture", fixture.name}, {"spot", 100.0},
      {"fixture_concentration_scale", fixture.width},
      {"requested_concentration_scale", concentration_scale ? Json(*concentration_scale) : Json(nullptr)},
      {"width", width}, {"grid_nodes", nodes}, {"upper_multiple", upper},
      {"max_clock_step", 1.0 / static_cast<double>(steps)},
      {"diffusion_method", method == AmericanMarkovDiffusionMethod::ExplicitEuler
          ? "explicit_euler" : "variance_resolvent"}};
  requested["intervals"] = Json::array();
  for (const auto &interval : fixture.intervals) {
    Json item{{"calendar_end", interval.calendar_end}, {"variance_end", interval.variance_end},
              {"rate", interval.rate}, {"continuous_yield", interval.continuous_yield},
              {"volatility", interval.volatility}, {"cash_at_end", interval.cash_dividend_at_end}};
    if (!interval.local_volatility.empty()) {
      item["local_volatility"] = Json::array();
      for (const auto &node : interval.local_volatility) {
        item["local_volatility"].push_back({node.log_state_over_spot, node.volatility_multiplier});
      }
    }
    requested["intervals"].push_back(std::move(item));
  }
  auto grid = make_american_markov_sinh_grid(
      100.0, {nodes, static_cast<double>(upper), width, 1025});
  if (!grid) {
    requested["model_status"] = "grid_refused";
    requested["model_error"] = grid.error().to_string();
    return requested;
  }
  std::vector<double> compile_times;
  std::optional<AmericanMarkovModel> model;
  AmericanMarkovConfig config;
  config.diffusion_method = method;
  config.max_calendar_step = 1.0 / static_cast<double>(steps);
  config.max_variance_step = config.max_calendar_step;
  for (std::size_t repeat = 0; repeat < repetitions; ++repeat) {
    const auto start = Clock::now();
    auto compiled = compile_american_markov({100.0, fixture.intervals, grid->states, config});
    compile_times.push_back(elapsed_us(start));
    if (!compiled) {
      requested["model_status"] = "refused";
      requested["model_error"] = compiled.error().to_string();
      requested["compile_us"] = compile_times;
      return requested;
    }
    model = std::move(*compiled);
  }
  Json out = describe(*model);
  out.update(requested);
  out["model_status"] = "ok";
  out["compile_us"] = compile_times;
  const std::array strikes{80.0, 90.0, 95.0, 99.5, 99.9, 100.0,
                           100.1, 100.5, 105.0, 110.0, 120.0};
  out["strikes"] = strikes;
  AmericanMarkovWorkspace scalar;
  AmericanMarkovBatchWorkspace batch;
  if (!scalar.prepare(*model) || !batch.prepare(*model, strikes.size())) {
    throw std::runtime_error("workspace refused");
  }
  Fingerprint price_hash;
  std::size_t prices = 0;
  std::size_t batch_mismatches = 0;
  std::size_t scalar_allocations = 0;
  std::size_t batch_allocations = 0;
  double max_batch_absolute_error = 0.0;
  Json price_bits = Json::array();
  std::array<double, strikes.size()> output{};
  const auto start = Clock::now();
  for (std::size_t expiry = 0; expiry < fixture.intervals.size(); ++expiry) {
    for (const auto side : {Side::Call, Side::Put}) {
      for (const auto exercise : {ExerciseStyle::American, ExerciseStyle::European}) {
        const allocation_probe::Window batch_window;
        const auto status = price_american_markov_batch(*model, strikes, side, expiry,
                                                         output, batch, exercise);
        batch_allocations += batch_window.finish();
        if (!status) throw std::runtime_error(status.error().to_string());
        for (std::size_t i = 0; i < strikes.size(); ++i) {
          const allocation_probe::Window scalar_window;
          const auto value = price_american_markov(*model, strikes[i], side, expiry, scalar, exercise);
          scalar_allocations += scalar_window.finish();
          if (!value) throw std::runtime_error(value.error().to_string());
          price_hash.append(*value);
          price_bits.push_back(std::to_string(std::bit_cast<std::uint64_t>(*value)));
          batch_mismatches += std::bit_cast<std::uint64_t>(*value) !=
                              std::bit_cast<std::uint64_t>(output[i]) ? 1U : 0U;
          max_batch_absolute_error = std::max(max_batch_absolute_error, std::abs(*value - output[i]));
          ++prices;
        }
      }
    }
  }
  out["scalar_and_batch_with_receipt_us"] = elapsed_us(start);
  out["price_count"] = prices;
  out["scalar_batch_bit_mismatches"] = batch_mismatches;
  out["scalar_batch_max_absolute_error"] = max_batch_absolute_error;
  out["prepared_scalar_cpp_allocations"] = scalar_allocations;
  out["prepared_batch_cpp_allocations"] = batch_allocations;
  out["price_fingerprint"] = std::to_string(price_hash.value);
  out["scalar_price_bits"] = std::move(price_bits);
  return out;
}
} // namespace

int main(int argc, char **argv) {
  try {
    // Explicit replacement-function calls make counter calibration independent
    // of permitted new-expression allocation elision by the optimizer.
    const allocation_probe::Window calibration;
    void *ordinary = ::operator new(17);
    void *aligned = ::operator new(65, std::align_val_t{64});
    const auto calibration_count = calibration.finish();
    ::operator delete(ordinary);
    ::operator delete(aligned, std::align_val_t{64});
    if (calibration_count != 2) throw std::runtime_error("allocation counter calibration failed");
    std::size_t repetitions = 1;
    std::size_t steps = 52;
    std::size_t nodes = 129;
    std::size_t upper = 4;
    std::optional<double> concentration_scale;
    auto method = AmericanMarkovDiffusionMethod::ExplicitEuler;
    std::set<std::string_view> seen;
    for (int i = 1; i < argc; i += 2) {
      if (i + 1 >= argc) throw std::invalid_argument("each option requires a value");
      const std::string_view option(argv[i]);
      const std::string_view input(argv[i + 1]);
      if (!seen.insert(option).second) throw std::invalid_argument("duplicate probe option");
      if (option == "--method") {
        if (input == "explicit") method = AmericanMarkovDiffusionMethod::ExplicitEuler;
        else if (input == "resolvent") method = AmericanMarkovDiffusionMethod::VarianceResolvent;
        else throw std::invalid_argument("method must be explicit or resolvent");
      } else if (option == "--concentration-scale") {
        double parsed = 0.0;
        const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), parsed);
        if (error != std::errc{} || end != input.data() + input.size() ||
            !std::isfinite(parsed) || parsed < 1.0e-6 || parsed > 100.0) {
          throw std::invalid_argument("concentration-scale must be finite in [1e-6,100]");
        }
        concentration_scale = parsed;
      } else if (option == "--repetitions" || option == "--steps" ||
                 option == "--nodes" || option == "--upper") {
        std::size_t parsed = 0;
        const auto [end, error] =
            std::from_chars(input.data(), input.data() + input.size(), parsed);
        const std::size_t minimum = option == "--nodes" ? 3U : option == "--upper" ? 2U : 1U;
        const std::size_t maximum = option == "--repetitions" ? 100U : option == "--nodes" ? 1025U
                                                        : option == "--upper" ? 64U : 4096U;
        if (error != std::errc{} || end != input.data() + input.size() ||
            parsed < minimum || parsed > maximum) {
          throw std::invalid_argument(
              "repetitions[1,100], steps[1,4096], nodes[3,1025], upper[2,64]");
        }
        if (option == "--repetitions") repetitions = parsed;
        else if (option == "--steps") steps = parsed;
        else if (option == "--nodes") nodes = parsed;
        else upper = parsed;
      } else {
        throw std::invalid_argument(
            "options: --repetitions N --steps N --nodes N --upper N --method explicit|resolvent "
            "--concentration-scale X");
      }
    }
    Json results = Json::array();
    for (const auto &fixture : fixtures()) {
      results.push_back(run(fixture, repetitions, method, steps, nodes, upper, concentration_scale));
    }
    std::cout << Json{{"schema", 1}, {"repetitions", repetitions},
                      {"cpp_allocation_counter_calibration", calibration_count},
                      {"cpp_allocation_scope", "calling thread ordinary and aligned operator new"},
                      {"timing_claim", "unqualified; requires quiet pinned host"},
                      {"cases", std::move(results)}}.dump(2) << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
