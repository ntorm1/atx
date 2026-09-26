#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/streaming_engine.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/parallel/det_pool.hpp"

namespace atxtest_alpha_ts_sliding_routing {
using namespace atx::engine::alpha;

TEST(AlphaPairRouting_Production, FastBatchStreamingAndOracleAcrossHolesFlatnessAndReseeds) {
  constexpr atx::usize dates = 117, names = 67, cells = dates * names;
  std::vector<atx::f64> x(cells), y(cells);
  for (atx::usize t = 0; t < dates; ++t) {
    for (atx::usize j = 0; j < names; ++j) {
      const atx::f64 phase = static_cast<atx::f64>(t * 7 + j);
      x[t * names + j] = 100.0 + std::sin(phase * 0.17) + phase * 0.01;
      y[t * names + j] = 20.0 + std::cos(phase * 0.13) + phase * 0.02;
      if (j == 0) x[t * names] = 0.1; // decimal constant: relative guard required
      if (j == 1) y[t * names + j] = 1.0e8 + 1e-4 * std::sin(phase); // near-flat predictor
    }
  }
  x[23 * names + 3] = std::numeric_limits<atx::f64>::quiet_NaN();
  y[31 * names + 65] = std::numeric_limits<atx::f64>::infinity();
  x[11 * names + 66] = 1e200;
  y[11 * names + 66] = 1e200; // overflowed pair products must not poison later clean windows
  auto panel = Panel::create(dates, names, {"close", "open"}, {x, y}, {});
  ASSERT_TRUE(panel);
  const Library lib;
  for (const atx::usize d : {atx::usize{3}, atx::usize{5}, atx::usize{17}}) {
    const std::string source = "c=correlation(close,open," + std::to_string(d) + ")\n" +
        "v=covariance(close,open," + std::to_string(d) + ")\n" +
        "b=ts_regression(close,open," + std::to_string(d) + ")\n";
    auto ast = parse_program(source, lib);
    ASSERT_TRUE(ast);
    auto analysis = analyze(*ast);
    ASSERT_TRUE(analysis);
    auto program = compile(*ast, *analysis);
    ASSERT_TRUE(program);
    auto reference = evaluate_reference(*program, *panel);
    ASSERT_TRUE(reference);
    for (const EvalMode mode : {EvalMode::AuditExact, EvalMode::ResearchFast}) {
      Engine engine{*panel};
      engine.set_eval_mode(mode);
      atx::engine::parallel::DetPool pool{2};
      engine.set_ts_pool(&pool);
      auto result = engine.evaluate(*program);
      ASSERT_TRUE(result);
      auto stream = StreamingEngine::create(*program, static_cast<atx::u32>(names), mode);
      ASSERT_TRUE(stream);
      for (atx::usize t = 0; t < dates; ++t) {
        CrossSection cs;
        for (const std::string &field : program->fields)
          cs.fields.emplace_back((field == "close" ? x.data() : y.data()) + t * names, names);
        ASSERT_TRUE(stream->step(cs));
        for (atx::usize root = 0; root < program->roots.size(); ++root) {
          for (atx::usize j = 0; j < names; ++j) {
            const atx::usize i = t * names + j;
            const atx::f64 got = result->alphas[root].values[i];
            const atx::f64 expected = reference->alphas[root].values[i];
            ASSERT_EQ(std::bit_cast<atx::u64>(got), std::bit_cast<atx::u64>(stream->output(root)[j]))
                << "mode=" << static_cast<unsigned>(mode) << " root=" << root << " cell=" << i;
            if (std::isnan(expected)) EXPECT_TRUE(std::isnan(got));
            else if (std::isinf(expected)) EXPECT_EQ(got, expected);
            else if (mode == EvalMode::AuditExact)
              EXPECT_EQ(std::bit_cast<atx::u64>(got), std::bit_cast<atx::u64>(expected));
            else EXPECT_NEAR(got, expected, 1e-9 * (1.0 + std::abs(expected)));
          }
        }
      }
    }
  }
}

TEST(AlphaDecayExpO1_Production, FiniteWindowInitializationRecoveryFactorsAndStreamingBits) {
  constexpr atx::usize dates = 143, names = 67, cells = dates * names;
  std::vector<atx::f64> x(cells);
  for (atx::usize t = 0; t < dates; ++t) {
    for (atx::usize j = 0; j < names; ++j) {
      const atx::f64 phase = static_cast<atx::f64>(t * 3 + j);
      x[t * names + j] = j == 0 ? 0.1 : 5.0 + std::sin(phase * 0.13);
      if (j == 1) x[t * names + j] -= 5.0; // signed/cancelling values
      if (j == 6) x[t * names + j] = 1.0e308; // finite weighted-sum overflow
    }
  }
  x[31 * names + 2] = std::numeric_limits<atx::f64>::quiet_NaN();
  x[35 * names + 3] = std::numeric_limits<atx::f64>::infinity();
  x[40 * names + 4] = -std::numeric_limits<atx::f64>::infinity();
  x[41 * names + 4] = std::numeric_limits<atx::f64>::infinity();
  x[37 * names + 5] = 1.0e300; // removing a huge observation must causally reseed
  auto panel = Panel::create(dates, names, {"close"}, {x}, {});
  ASSERT_TRUE(panel);
  const Library lib;
  for (const std::string_view factor : {"0.000000000000000000000000000001", "0.9",
                                       "0.999999999999", "1.0", "1.5", "2.0"}) {
    for (const atx::usize d : {atx::usize{1}, atx::usize{7}, atx::usize{31}}) {
      SCOPED_TRACE(factor);
      SCOPED_TRACE(d);
      const std::string source = "a=ts_decay_exp(close," + std::to_string(d) + "," +
                                  std::string{factor} + ")\n";
      auto ast = parse_program(source, lib);
      ASSERT_TRUE(ast);
      auto analysis = analyze(*ast);
      ASSERT_TRUE(analysis);
      auto program = compile(*ast, *analysis);
      ASSERT_TRUE(program);
      auto reference = evaluate_reference(*program, *panel);
      ASSERT_TRUE(reference);
      for (const EvalMode mode : {EvalMode::AuditExact, EvalMode::ResearchFast}) {
        Engine engine{*panel};
        engine.set_eval_mode(mode);
        atx::engine::parallel::DetPool pool{2};
        engine.set_ts_pool(&pool);
        auto result = engine.evaluate(*program);
        ASSERT_TRUE(result);
        auto stream = StreamingEngine::create(*program, static_cast<atx::u32>(names), mode);
        ASSERT_TRUE(stream);
        for (atx::usize t = 0; t < dates; ++t) {
          CrossSection cs;
          cs.fields.emplace_back(x.data() + t * names, names);
          ASSERT_TRUE(stream->step(cs));
          for (atx::usize j = 0; j < names; ++j) {
            const atx::usize i = t * names + j;
            const atx::f64 got = result->alphas[0].values[i];
            const atx::f64 expected = reference->alphas[0].values[i];
            ASSERT_EQ(std::bit_cast<atx::u64>(got), std::bit_cast<atx::u64>(stream->output(0)[j]))
                << "mode=" << static_cast<unsigned>(mode) << " cell=" << i;
            if (std::isnan(expected)) EXPECT_TRUE(std::isnan(got));
            else if (std::isinf(expected)) EXPECT_EQ(got, expected);
            else if (mode == EvalMode::AuditExact)
              EXPECT_EQ(std::bit_cast<atx::u64>(got), std::bit_cast<atx::u64>(expected));
            else EXPECT_NEAR(got, expected, 1e-9 * (1.0 + std::abs(expected)));
          }
        }
      }
    }
  }
}
} // namespace atxtest_alpha_ts_sliding_routing
