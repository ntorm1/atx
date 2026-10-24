// The D1 production cash-dividend path against D0's INDEPENDENT reference
// (src/pricing/american_discrete_div_reference.hpp, docs/american-cash-dividend-reference.md)
// — the slow suite (label atx_vol_slow; also in ATX_VOL_SLOW_FILTER).
//
//   1. ReScoreAgainstTheIndependentReference — D0's 50-row matrix re-scored: the price
//      residual and the IV error by inversion of the Standard and Refined routes next
//      to the historical 301-step SplicedCrr, every UNRESOLVED / CONVENTION GAP /
//      PRICE-ONLY row reported as such.
//   2. PerEventDriftAgainstLiveReferences — the splice defect: the error per ex-date
//      on the 8- and 12-event chains truncated at k events, against LIVE reference
//      prices of every truncation, before (historical) and after (Standard, Refined).
//   3. GreeksAgainstBumpAndRevalueOnTheReference — every supported sensitivity of the
//      bundle against central bump-and-revalue on the reference PDE (common-mode
//      fixed grid), with a step study and the reference's own u per greek and row;
//      the unsupported ones checked to be NaN + bit + reason.
//
// D0's reference prices, uncertainties and vegas below are its matrix run
// C:\atx-cache\quote-benchmark\D0-20261022-fix1\matrix-run-cc89bc89\matrix.csv
// (sha256 in that directory's sha256.txt), pasted at full precision; the case
// parameters are D0's `matrix()` (tests/american_discrete_div_reference_matrix_test.cpp).
// The drift and Greek studies recompute their references live with D0's engine.
//
// Every table line is printed with a "[d1-...]" prefix for the lane report.
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_discrete_div_kernel.hpp"
#include "pricing/american_discrete_div_reference.hpp"
#include "support/ddiv_matrix_cases.hpp"

namespace {

using atx::vol::CashDividend;
using atx::vol::DiscreteDivAccuracy;
using atx::vol::DiscreteDivGreek;
using atx::vol::DiscreteDivIvOpts;
using atx::vol::DiscreteDivProductionGreeks;
using atx::vol::DiscreteDivProductionOpts;
using atx::vol::DiscreteDivScheme;
using atx::vol::DiscreteDivWorkspace;
using atx::vol::ExerciseStyle;
using atx::vol::Side;
namespace ref = atx::vol::ddiv_ref;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

[[nodiscard]] std::string fmt(const char *format, double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, format, v);
  return buf;
}

[[nodiscard]] std::vector<CashDividend> quarterly(double first, double amount, double T) {
  std::vector<CashDividend> out;
  for (double t = first; t <= T + 1e-12; t += 0.25) {
    out.push_back({t, amount});
  }
  return out;
}

struct Case {
  std::string id;
  double S = 100.0, K = 100.0, T = 1.0, sigma = 0.25, r = 0.04, q = 0.0;
  Side side = Side::Put;
  ref::CashProcess process = ref::CashProcess::SpotLiquidator;
  std::vector<CashDividend> divs;
  // D0's run (matrix.csv).
  double ref = 0.0, ref_u = 0.0, vega = 0.0;
  std::string status;
};

[[nodiscard]] Case make(const char *id, double S, double K, double T, double sigma, double r,
                        Side side, std::vector<CashDividend> divs,
                        ref::CashProcess process = ref::CashProcess::SpotLiquidator) {
  Case c;
  c.id = id;
  c.S = S;
  c.K = K;
  c.T = T;
  c.sigma = sigma;
  c.r = r;
  c.side = side;
  c.process = process;
  c.divs = std::move(divs);
  return c;
}

struct D0Row {
  const char *id;
  double ref, ref_u, vega;
  const char *status;
};

// matrix.csv (cc89bc89): id, ref, ref_u, vega, status.
constexpr D0Row kD0[] = {
    {"A1p", 7.1736404577473447, 1.445289102860199e-06, 27.65717981760174, "RESOLVED"},
    {"A1c", 9.6862934206760603, 4.8141655042676798e-07, 25.108681528516641, "RESOLVED"},
    {"A2p", 7.1735700275736027, 1.4073879775159034e-06, 27.660655162788039, "RESOLVED"},
    {"A2c", 9.690477478431152, 4.8629164328040875e-07, 25.09851093761295, "RESOLVED"},
    {"A3p", 7.1734916814659746, 1.4149818761419088e-06, 27.663732042126021, "RESOLVED"},
    {"A3c", 9.6947022647536567, 4.8056578632929833e-07, 25.088395179264644, "RESOLVED"},
    {"A4p", 7.1735708442410102, 1.4446162715812098e-06, 27.660380431354881, "RESOLVED"},
    {"A4c", 9.6904354359986673, 4.8068486391616264e-07, 25.098612126273068, "RESOLVED"},
    {"A5p", 7.1735692835675824, 1.4114815860719913e-06, 27.660682042464746, "RESOLVED"},
    {"A5c", 9.6905195256653087, 4.8037429976289074e-07, 25.098409457458004, "RESOLVED"},
    {"B1p", 9.6402481234589139, 1.6471971302304277e-06, 37.686072665445991, "RESOLVED"},
    {"B1c", 12.502153045794337, 6.8331837692238102e-07, 36.1620762144792, "RESOLVED"},
    {"B2p", 9.3875343954483732, 6.8517515069007486e-07, 39.34302027543346, "RESOLVED"},
    {"B2c", 14.490989016436732, 2.233349501601385e-07, 35.355612075088416, "RESOLVED"},
    {"B3p", 9.3852415096314061, 3.0408236841682695e-07, 39.343179403739548, "RESOLVED"},
    {"B3c", 14.49975422743506, 7.251795291430335e-07, 35.378195428017456, "RESOLVED"},
    {"C2p", 7.1221956669065607, 1.8678031337143871e-06, 27.72660057053411, "RESOLVED"},
    {"C2c", 9.6074845480852158, 4.4426001067294678e-07, 25.659947740342304, "RESOLVED"},
    {"C4p", 9.9514465383687867, 1.8963110753868812e-06, 38.769109700526272, "RESOLVED"},
    {"C4c", 12.208439220641502, 2.3792772999855024e-07, 36.740268123945263, "RESOLVED"},
    {"C8p", 13.723830866167567, 1.8095140117036732e-06, 53.426364732807485, "RESOLVED"},
    {"C8c", 15.710904572133005, 3.9486858193910244e-07, 51.234093699028094, "RESOLVED"},
    {"D12p", 16.411330157685374, 4.4184373568776493e-06, 63.717961298950243, "RESOLVED"},
    {"D12c", 18.206733741577949, 8.0105496627837204e-07, 61.350175139901708, "RESOLVED"},
    {"DSPYp", 93.318532225080119, 3.2516860882252968e-05, 382.65198873589412, "RESOLVED"},
    {"DSPYc", 149.65318074371024, 9.1555963721587992e-06, 440.58134910027036, "RESOLVED"},
    {"E1p", 8.7370443366198192, 1.6436502783172781e-06, 38.512823120347761, "RESOLVED"},
    {"E1c", 13.877691161003511, 7.0265287683817638e-07, 35.886927324645157, "RESOLVED"},
    {"E5p", 10.755732967598194, 1.8149633254386507e-06, 38.339585202261439, "RESOLVED"},
    {"E5c", 12.163300460999551, 3.2462451365423704e-07, 34.472269160827018, "RESOLVED"},
    {"E20p", 20.630858375006021, 1.0766270346401865e-06, 29.346781534059119, "RESOLVED"},
    {"E20c", 10.873857469168692, 5.573463081859231e-07, 26.512264277606334, "RESOLVED"},
    {"E64p", 2.7180751732997481, 2.4775199425437587e-10, 1.4457324226668788e-09,
     "PRICE-ONLY (IV not identified)"},
    {"E64c", 1.3788047974750886, 1.6263187015520699e-10, -1.4299672557172016e-09,
     "PRICE-ONLY (IV not identified)"},
    {"F1c", 31.310931467107562, 3.9678890921507692e-07, 1.8509245697995169,
     "UNRESOLVED (u > 1e-7 IV)"},
    {"F1p", 33.045386709374775, 2.2736162576883144e-06, 8.7891746749839683,
     "UNRESOLVED (u > 1e-7 IV)"},
    {"F2c", 20.884622241300789, 3.9290821831331948e-07, 2.8569513843184069,
     "UNRESOLVED (u > 1e-7 IV)"},
    {"F2p", 27.898566467935236, 1.6631326802915538e-06, 3.2582521562982691,
     "UNRESOLVED (u > 1e-7 IV)"},
    {"G1p", 11.521059265523864, 1.9369622449748488e-07, 39.30457110173613, "RESOLVED"},
    {"G1c", 11.22026658821548, 2.9736180093368476e-07, 36.2480170727979, "RESOLVED"},
    {"G4p", 12.066321200885017, 1.9808484274988851e-07, 39.040853273073495, "RESOLVED"},
    {"G4c", 10.658339519672758, 5.6432159961864792e-07, 37.013266844623693, "RESOLVED"},
    {"H1p", 9.5541072213006899, 1.7357996538515225e-06, 37.968342760181351, "RESOLVED"},
    {"H1c", 12.654053225543276, 4.5641473316486725e-07, 35.420399128909708, "RESOLVED"},
    {"H4p", 9.764015865488485, 1.9667797282816445e-06, 38.029706968568888, "RESOLVED"},
    {"H4c", 12.027379986501927, 2.774025321126771e-07, 35.994061552542256, "RESOLVED"},
    {"ILp", 4.4750362597052673, 4.4863756541741983e-07, 1.3577616994275843,
     "UNRESOLVED (u > 1e-7 IV)"},
    {"ILc", 2.6775724638610128, 9.3651230141750563e-08, 1.4754549764024461, "RESOLVED"},
    {"ISp", 3.8210111731499974, 4.6949992494185474e-07, -0.31304911876861574,
     "CONVENTION GAP (kernel = liquidator)"},
    {"ISc", 2.7021679435225976, 1.1205005176138567e-07, 1.6068384915672862,
     "CONVENTION GAP (kernel = liquidator)"},
};

// Original D0 inputs, with their frozen reference records joined by exact ID.
[[nodiscard]] std::vector<Case> matrix() {
  std::vector<Case> m;
  for (const auto &input : atx::vol::test::ddiv_matrix::matrix()) {
    auto c = make(input.id.c_str(), input.S, input.K, input.T, input.sigma, input.r,
                  input.side, input.divs, input.process);
    c.q = input.q;
    for (const D0Row &row : kD0) {
      if (c.id == row.id) {
        c.ref = row.ref;
        c.ref_u = row.ref_u;
        c.vega = row.vega;
        c.status = row.status;
      }
    }
    m.push_back(std::move(c));
  }
  return m;
}

TEST(DiscreteDivProductionManifest, PreservesFrozenOriginalInputFingerprint) {
  const auto cases = matrix();
  ASSERT_EQ(cases.size(), 50U);
  EXPECT_EQ(atx::vol::test::ddiv_matrix::input_fingerprint(cases),
            12110935955959749584ULL);
}

[[nodiscard]] const Case &find(const std::vector<Case> &m, const char *id) {
  for (const Case &c : m) {
    if (c.id == id) {
      return c;
    }
  }
  ADD_FAILURE() << "no case " << id;
  return m.front();
}

[[nodiscard]] DiscreteDivProductionOpts route_opts(DiscreteDivAccuracy accuracy,
                                                   int base = atx::vol::kDiscreteDivDefaultSteps) {
  DiscreteDivProductionOpts o;
  o.accuracy = accuracy;
  o.base_steps = base;
  return o;
}

[[nodiscard]] double production_price(const Case &c, const DiscreteDivProductionOpts &o,
                                      DiscreteDivWorkspace &ws) {
  const auto out = atx::vol::american_discrete_div_production_price(
      c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, c.divs, ExerciseStyle::American, o, ws);
  EXPECT_TRUE(out.has_value()) << c.id;
  return out ? out->price : kNaN;
}

// (sigma the route implies from the REFERENCE price - sigma) x 1e4; NaN if refused.
[[nodiscard]] double iv_bp(const Case &c, const DiscreteDivProductionOpts &o,
                           DiscreteDivWorkspace &ws) {
  DiscreteDivIvOpts iv;
  iv.tol = 1.0e-9;
  iv.max_iter = 96;
  const auto out = atx::vol::american_discrete_div_production_iv(c.ref, c.S, c.K, c.T, c.r, c.q,
                                                                 c.side, c.divs, o, iv, ws);
  return out ? (out->iv.sigma - c.sigma) * 1.0e4 : kNaN;
}

// ── 1. The 50-row re-score ───────────────────────────────────────────────────

using Json = nlohmann::ordered_json;

// A dollar-price comparison. The stored uncertainty is empirical; neither this
// gate nor the historical IV-status label is a proof of reference error.
[[nodiscard]] Json empirical_price_gate(
    const Case &c, double candidate,
    ref::CashProcess candidate_process = ref::CashProcess::SpotLiquidator) {
  constexpr double budget = 0.001;
  Json gate{{"budget_usd", budget}, {"status", "invalid_reference"},
            {"absolute_difference", nullptr}, {"difference_plus_uncertainty", nullptr}};
  if (c.process != candidate_process) {
    gate["status"] = "model_mismatch";
  } else if (!std::isfinite(candidate)) {
    gate["status"] = "candidate_refused";
  } else if (std::isfinite(c.ref) && std::isfinite(c.ref_u) && c.ref_u >= 0.0) {
    const double error = std::abs(candidate - c.ref);
    gate["absolute_difference"] = error;
    gate["difference_plus_uncertainty"] = error + c.ref_u;
    if (c.ref_u > budget) {
      gate["status"] = "reference_unresolved";
    } else if (error + c.ref_u <= budget) {
      gate["status"] = "pass";
    } else if (error - c.ref_u > budget) {
      gate["status"] = "fail";
    } else {
      gate["status"] = "uncertain";
    }
  }
  return gate;
}

[[nodiscard]] Json price_record(const Case &c, DiscreteDivAccuracy accuracy,
                               DiscreteDivWorkspace &ws) {
  const auto opts = route_opts(accuracy);
  const auto priced = atx::vol::american_discrete_div_production_price(
      c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, c.divs, ExerciseStyle::American, opts, ws);
  Json row{{"status", priced ? "priced" : "refused"}, {"price", nullptr},
           {"options", {{"accuracy", static_cast<int>(opts.accuracy)},
                        {"base_steps", opts.base_steps}, {"kill_switch", opts.kill_switch}}},
           {"empirical_price_gate", empirical_price_gate(c, priced ? priced->price : kNaN)}};
  if (!priced) {
    row["error_code"] = static_cast<int>(priced.error().code());
    row["error"] = priced.error().to_string();
    return row;
  }
  row["price"] = priced->price;
  const auto &r = priced->route;
  row["route"] = {{"method", atx::vol::to_string(r.method)}, {"steps", r.steps},
                  {"coarse_steps", r.coarse_steps}, {"pre_steps", r.pre_steps},
                  {"events", r.events}, {"first_ex_step", r.first_ex_step},
                  {"max_cash_ratio", r.max_cash_ratio},
                  {"grid_bottom_depth", r.grid_bottom_depth}, {"flags", r.flags}};
  const auto &convention = priced->convention;
  row["convention"] = {{"process", static_cast<int>(convention.process)},
                       {"cum_and_post_exercise", convention.cum_and_post_exercise},
                       {"cash_excess", static_cast<int>(convention.cash_excess)},
                       {"ex_dates", static_cast<int>(convention.ex_dates)},
                       {"spot_volatility", convention.spot_volatility}};
  return row;
}

[[nodiscard]] Json input_record(const atx::vol::test::ddiv_matrix::InputCase &c) {
  Json events = Json::array();
  for (const auto &div : c.divs) {
    events.push_back({{"tau", div.tau}, {"cash", div.amount}});
  }
  return {{"spot", c.S}, {"strike", c.K}, {"maturity", c.T}, {"sigma", c.sigma},
          {"rate", c.r}, {"yield", c.q}, {"side", c.side == Side::Call ? "call" : "put"},
          {"cash_process", ref::to_string(c.process)}, {"exercise", "american"},
          {"cum_and_post_exercise", true}, {"events", std::move(events)}};
}

[[nodiscard]] Json price_manifest() {
  const auto cases = matrix();
  const auto inputs = atx::vol::test::ddiv_matrix::matrix();
  Json out{{"schema", "atx-vol-cash-price-manifest-v1"},
           {"input_fingerprint_fnv1a64", std::to_string(
                atx::vol::test::ddiv_matrix::input_fingerprint(inputs))},
           {"reference_source", "D1-20261023-production/d0-matrix-out/matrix.csv"},
           {"reference_sha256",
            "210fd6287cbfc56bca047cca3fd6302d8b2df86801d4afa36eb958ea4ff4dfba"},
           {"reference_run_source", "cc89bc89"},
           {"reference_uncertainty_kind", "empirical_convergence_indicator"},
           {"time_unit", "model_year"}, {"continuous_yield", "residual_to_cash"},
           {"candidate_cash_process", "spot-liquidator"},
           {"price_budget_usd", 0.001}, {"raw_spread_available", false},
           {"cases", Json::array()}};
  DiscreteDivWorkspace ws(atx::vol::kDiscreteDivMaxRouteSteps,
                          atx::vol::kDiscreteDivMaxPreSteps);
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const auto &c = cases[i];
    out["cases"].push_back(
        {{"id", c.id}, {"group", inputs[i].group}, {"input", input_record(inputs[i])},
         {"same_cash_model", c.process == ref::CashProcess::SpotLiquidator},
         {"reference", {{"price", c.ref}, {"uncertainty", c.ref_u}, {"vega", c.vega},
                        {"original_iv_status", c.status}}},
         {"standard", price_record(c, DiscreteDivAccuracy::Standard, ws)},
         {"refined", price_record(c, DiscreteDivAccuracy::Refined, ws)}});
  }
  return out;
}

[[nodiscard]] std::optional<std::string> manifest_output_directory(
    const char *name = "ATX_VOL_DDIV_MANIFEST_OUT") {
#ifdef _MSC_VER
  char *value = nullptr;
  std::size_t size = 0;
  const int status = ::_dupenv_s(&value, &size, name);
  const std::unique_ptr<char, decltype(&std::free)> owned(value, &std::free);
  if (status != 0 || value == nullptr) {
    return std::nullopt;
  }
  return std::string(value);
#else
  const char *value = std::getenv(name);
  return value == nullptr ? std::nullopt : std::optional<std::string>(value);
#endif
}

TEST(DiscreteDivProductionManifest, ExportsAllInputsAndUnroundedPrices) {
  const auto manifest = price_manifest();
  ASSERT_TRUE(manifest.contains("cases"));
  ASSERT_EQ(manifest.at("cases").size(), 50U);
  const auto roundtrip = Json::parse(manifest.dump());
  const auto inputs = atx::vol::test::ddiv_matrix::matrix();
  const auto refs = matrix();
  std::size_t comparable = 0;
  std::size_t price_only = 0;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    SCOPED_TRACE(inputs[i].id);
    const auto &row = roundtrip.at("cases").at(i);
    const auto &input = row.at("input");
    EXPECT_EQ(row.at("id"), inputs[i].id);
    EXPECT_FALSE(refs[i].status.empty());
    EXPECT_EQ(input.at("cash_process"), ref::to_string(inputs[i].process));
    EXPECT_EQ(input.at("side"), inputs[i].side == Side::Call ? "call" : "put");
    EXPECT_EQ(input.at("exercise"), "american");
    const std::array names{"spot", "strike", "maturity", "sigma", "rate", "yield"};
    const std::array values{inputs[i].S, inputs[i].K, inputs[i].T, inputs[i].sigma,
                            inputs[i].r, inputs[i].q};
    for (std::size_t j = 0; j < names.size(); ++j) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(input.at(names[j]).get<double>()),
                std::bit_cast<std::uint64_t>(values[j]));
    }
    ASSERT_EQ(input.at("events").size(), inputs[i].divs.size());
    for (std::size_t j = 0; j < inputs[i].divs.size(); ++j) {
      const auto &event = input.at("events").at(j);
      EXPECT_EQ(std::bit_cast<std::uint64_t>(event.at("tau").get<double>()),
                std::bit_cast<std::uint64_t>(inputs[i].divs[j].tau));
      EXPECT_EQ(std::bit_cast<std::uint64_t>(event.at("cash").get<double>()),
                std::bit_cast<std::uint64_t>(inputs[i].divs[j].amount));
    }
    EXPECT_EQ(std::bit_cast<std::uint64_t>(row.at("reference").at("price").get<double>()),
              std::bit_cast<std::uint64_t>(refs[i].ref));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(row.at("reference").at("uncertainty").get<double>()),
              std::bit_cast<std::uint64_t>(refs[i].ref_u));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(row.at("reference").at("vega").get<double>()),
              std::bit_cast<std::uint64_t>(refs[i].vega));
    comparable += row.at("same_cash_model").get<bool>() ? 1U : 0U;
    price_only += refs[i].status.starts_with("PRICE-ONLY") ? 1U : 0U;
    for (const char *accuracy : {"standard", "refined"}) {
      const auto &candidate = row.at(accuracy);
      EXPECT_EQ(candidate.at("status"), "priced");
      EXPECT_TRUE(candidate.at("route").contains("pre_steps"));
      EXPECT_EQ(candidate.at("empirical_price_gate").at("budget_usd"), 0.001);
      EXPECT_EQ(std::bit_cast<std::uint64_t>(candidate.at("price").get<double>()),
                std::bit_cast<std::uint64_t>(
                    manifest.at("cases").at(i).at(accuracy).at("price").get<double>()));
    }
  }
  EXPECT_EQ(comparable, 44U);
  EXPECT_EQ(price_only, 2U);
  ASSERT_FALSE(HasFailure());
  // Opt-in export requires a NEW directory, so an existing receipt is never overwritten.
  if (const auto out = manifest_output_directory()) {
    ASSERT_FALSE(out->empty());
    ASSERT_TRUE(std::filesystem::create_directory(*out)) << "output directory exists";
    std::ofstream stream(std::filesystem::path(*out) / "manifest.json", std::ios::binary);
    ASSERT_TRUE(stream.good());
    stream << manifest.dump(2) << '\n';
    stream.close();
    ASSERT_TRUE(stream.good());
  }
}

TEST(DiscreteDivProductionManifest, RecordsTypedRefusalWithoutDroppingTheRequest) {
  auto c = matrix().front();
  c.sigma = -1.0;
  DiscreteDivWorkspace ws(atx::vol::kDiscreteDivMaxRouteSteps);
  const auto out = price_record(c, DiscreteDivAccuracy::Standard, ws);
  ASSERT_TRUE(out.contains("status"));
  EXPECT_EQ(out.at("status"), "refused");
  EXPECT_TRUE(out.at("price").is_null());
  EXPECT_EQ(out.at("error_code"), static_cast<int>(atx::core::ErrorCode::InvalidArgument));
  EXPECT_EQ(out.at("empirical_price_gate").at("status"), "candidate_refused");
}

TEST(DiscreteDivProductionManifest, MonetaryGateRetainsUncertaintyAndModelDenominators) {
  auto c = matrix().front();
  c.ref = 10.0;
  c.ref_u = 0.0001;
  EXPECT_EQ(empirical_price_gate(c, 10.0008).at("status"), "pass");
  EXPECT_EQ(empirical_price_gate(c, 10.0012).at("status"), "fail");
  EXPECT_EQ(empirical_price_gate(c, 10.0010).at("status"), "uncertain");
  c.ref_u = 0.002;
  EXPECT_EQ(empirical_price_gate(c, 10.0).at("status"), "reference_unresolved");
  c.ref_u = -0.001;
  EXPECT_EQ(empirical_price_gate(c, 10.0).at("status"), "invalid_reference");
  c.ref_u = kNaN;
  EXPECT_EQ(empirical_price_gate(c, 10.0).at("status"), "invalid_reference");
  c.ref_u = 0.0;
  c.ref = 0.0;
  EXPECT_EQ(empirical_price_gate(c, 0.001).at("status"), "pass");
  EXPECT_EQ(empirical_price_gate(c, kNaN).at("status"), "candidate_refused");
  c.status = "PRICE-ONLY (IV not identified)";
  EXPECT_EQ(empirical_price_gate(c, 0.0).at("status"), "pass");
  c.process = ref::CashProcess::Escrowed;
  EXPECT_EQ(empirical_price_gate(c, 0.0).at("status"), "model_mismatch");
  c.process = ref::CashProcess::SpotSurvivor;
  EXPECT_EQ(empirical_price_gate(c, 0.0).at("status"), "model_mismatch");
}

[[nodiscard]] Json pde_record(const Case &c, const ref::PdeGrid &grid) {
  const ref::Problem problem{.S = c.S, .K = c.K, .T = c.T, .sigma = c.sigma,
                            .r = c.r, .q = c.q, .side = c.side,
                            .exercise = ExerciseStyle::American,
                            .process = c.process, .dividends = c.divs};
  const auto priced = ref::pde_price(problem, grid);
  Json row{{"status", priced ? "priced" : "refused"}, {"price", nullptr},
           {"cash_process", ref::to_string(c.process)},
           {"grid", {{"space_base", grid.space_base}, {"space_level", grid.space_level},
                     {"time_base", grid.time_base}, {"time_level", grid.time_level},
                     {"width_sd", grid.width_sd}, {"rannacher_steps", grid.rannacher_steps},
                     {"domain_sigma", grid.domain_sigma}, {"time_grading", grid.time_grading}}},
           {"empirical_price_gate",
            empirical_price_gate(c, priced ? priced->price : kNaN, c.process)}};
  if (!priced) {
    row["error_code"] = static_cast<int>(priced.error().code());
    row["error"] = priced.error().to_string();
    return row;
  }
  row["price"] = priced->price;
  row["work"] = {{"space_intervals", priced->space_intervals},
                 {"implicit_solves", priced->time_steps},
                 {"max_policy_iterations", priced->max_policy_iterations},
                 {"worst_policy_calendar_time", priced->max_policy_time},
                 {"max_lcp_violation", priced->max_lcp_violation},
                 {"lcp_rounding_ties", priced->lcp_rounding_ties}};
  return row;
}

[[nodiscard]] Json pde_replay_manifest() {
  auto out = price_manifest();
  out["schema"] = "atx-vol-cash-pde-three-grid-v1";
  out["production_cash_process"] = out.at("candidate_cash_process");
  out.erase("candidate_cash_process");
  out["pde_method"] = "event-aligned-cn-rannacher-howard";
  out["pde_extrapolation"] = false;
  out["pde_reference_model"] = "each_original_case_process";
  out["work_limit"] = "maximum policy iterations is not a total sweep counter";
  const auto cases = matrix();
  for (std::size_t i = 0; i < cases.size(); ++i) {
    auto &results = out["cases"][i]["pde_grids"];
    results = Json::array();
    // Three declared complete solves, with no tuning, extrapolation or selection.
    for (const int scale : {1, 2, 4}) {
      ref::PdeGrid grid;
      grid.space_base = 400 * scale;
      grid.time_base = 200 * scale;
      results.push_back(pde_record(cases[i], grid));
    }
  }
  return out;
}

TEST(DiscreteDivProductionMatrix, DiagnosticThreeGridPdeReplay) {
  const auto manifest = pde_replay_manifest();
  ASSERT_TRUE(manifest.contains("cases"));
  ASSERT_EQ(manifest.at("cases").size(), 50U);
  const auto roundtrip = Json::parse(manifest.dump());
  const auto cases = matrix();
  std::size_t requested = 0;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const auto &c = cases[i];
    SCOPED_TRACE(c.id);
    const auto &row = roundtrip.at("cases").at(i);
    EXPECT_EQ(row.at("id"), c.id);
    ASSERT_EQ(row.at("pde_grids").size(), 3U);
    for (std::size_t level = 0; level < 3; ++level) {
      ++requested;
      const auto &pde = row.at("pde_grids").at(level);
      EXPECT_EQ(pde.at("cash_process"), ref::to_string(c.process));
      EXPECT_EQ(pde.at("grid").at("space_base"), 400U << level);
      EXPECT_EQ(pde.at("grid").at("time_base"), 200U << level);
      EXPECT_EQ(pde.at("grid").at("space_level"), 0);
      EXPECT_EQ(pde.at("grid").at("time_level"), 0);
      EXPECT_EQ(pde.at("grid").at("width_sd"), 8.0);
      EXPECT_EQ(pde.at("grid").at("rannacher_steps"), 2);
      EXPECT_EQ(pde.at("grid").at("time_grading"), 2.0);
      EXPECT_EQ(pde.at("grid").at("domain_sigma"), 0.0);
      EXPECT_NE(pde.at("empirical_price_gate").at("status"), "model_mismatch");
      if (pde.at("status") == "priced") {
        EXPECT_TRUE(std::isfinite(pde.at("price").get<double>()));
        EXPECT_GT(pde.at("work").at("implicit_solves").get<int>(), 0);
        EXPECT_EQ(std::bit_cast<std::uint64_t>(pde.at("price").get<double>()),
                  std::bit_cast<std::uint64_t>(manifest.at("cases").at(i)
                      .at("pde_grids").at(level).at("price").get<double>()));
      } else {
        EXPECT_EQ(pde.at("status"), "refused");
        EXPECT_TRUE(pde.at("price").is_null());
        EXPECT_TRUE(pde.contains("error_code"));
      }
    }
  }
  EXPECT_EQ(requested, 150U);
  ASSERT_FALSE(HasFailure());
  if (const auto out = manifest_output_directory("ATX_VOL_DDIV_PDE_REPLAY_OUT")) {
    ASSERT_FALSE(out->empty());
    ASSERT_TRUE(std::filesystem::create_directory(*out)) << "output directory exists";
    std::ofstream stream(std::filesystem::path(*out) / "replay.json", std::ios::binary);
    ASSERT_TRUE(stream.good());
    stream << manifest.dump(2) << '\n';
    stream.close();
    ASSERT_TRUE(stream.good());
  }
}

TEST(DiscreteDivProductionManifest, PdeReplayKeepsTypedUnsupportedGridRefusal) {
  auto c = matrix().front();
  c.S = 100.0;
  c.K = 200.0;
  c.T = 1.0;
  c.sigma = 0.005;
  c.r = 0.1;
  c.q = 0.0;
  const auto out = pde_record(c, {});
  ASSERT_TRUE(out.contains("status"));
  EXPECT_EQ(out.at("status"), "refused");
  EXPECT_TRUE(out.at("price").is_null());
  EXPECT_EQ(out.at("error_code"), static_cast<int>(atx::core::ErrorCode::OutOfRange));
  EXPECT_EQ(out.at("empirical_price_gate").at("status"), "candidate_refused");
}

TEST(DiscreteDivProductionMatrix, ReScoreAgainstTheIndependentReference) {
  const std::vector<Case> m = matrix();
  ASSERT_EQ(m.size(), 50U);
  DiscreteDivWorkspace ws(atx::vol::kDiscreteDivMaxRouteSteps, atx::vol::kDiscreteDivMaxPreSteps);
  std::printf("[d1-rescore] | id | status (D0) | ref +- u | hist301 bp (inv) | std route | "
              "std USD | std bp (inv) | refined USD | refined bp (inv) |\n");
  std::printf("[d1-rescore] |---|---|---|---|---|---|---|---|---|\n");
  double worst_std = 0.0;
  double worst_refined = 0.0;
  std::string worst_std_id;
  std::string worst_refined_id;
  for (const Case &c : m) {
    ASSERT_GT(c.ref, 0.0) << c.id;
    const bool kernel_model = c.process == ref::CashProcess::SpotLiquidator;
    const bool identified = c.status.rfind("PRICE-ONLY", 0) != 0 && kernel_model;
    const auto std_route = atx::vol::discrete_div_lattice_route(
        c.S, c.T, c.divs, route_opts(DiscreteDivAccuracy::Standard));
    ASSERT_TRUE(std_route.has_value()) << c.id;
    const double p_std = production_price(c, route_opts(DiscreteDivAccuracy::Standard), ws);
    const double p_ref = production_price(c, route_opts(DiscreteDivAccuracy::Refined), ws);
    double hist_bp = kNaN;
    double std_bp = kNaN;
    double ref_bp = kNaN;
    if (identified) {
      DiscreteDivIvOpts iv;
      iv.tol = 1.0e-9;
      iv.max_iter = 96;
      iv.tier = atx::vol::DiscreteDivTier::Accurate;
      const auto hist = atx::vol::american_discrete_div_implied_vol(
          c.ref, c.S, c.K, c.T, c.r, c.q, c.side, c.divs, atx::vol::kDiscreteDivDefaultSteps,
          DiscreteDivScheme::SplicedCrr, iv, ws);
      hist_bp = hist ? (hist->sigma - c.sigma) * 1.0e4 : kNaN;
      std_bp = iv_bp(c, route_opts(DiscreteDivAccuracy::Standard), ws);
      ref_bp = iv_bp(c, route_opts(DiscreteDivAccuracy::Refined), ws);
      EXPECT_TRUE(std::isfinite(std_bp) && std::isfinite(ref_bp)) << c.id;
      if (c.status == "RESOLVED" && std::abs(std_bp) > std::abs(worst_std)) {
        worst_std = std_bp;
        worst_std_id = c.id;
      }
      if (c.status == "RESOLVED" && std::abs(ref_bp) > std::abs(worst_refined)) {
        worst_refined = ref_bp;
        worst_refined_id = c.id;
      }
    }
    std::string route = std::string(atx::vol::to_string(std_route->method)) + " " +
                        std::to_string(std_route->steps) + "+" +
                        std::to_string(std_route->pre_steps);
    if (std_route->has(atx::vol::DiscreteDivRouteFlag::GridBottomExtrapolated)) {
      route += " grid-bottom " + fmt("%.1f", std_route->grid_bottom_depth);
    }
    const std::string note = kernel_model ? "" : " (not the D1 convention)";
    std::printf("[d1-rescore] | %s | %s%s | %.7f +- %.1e | %s | %s | %+.2e | %s | %+.2e | %s |\n",
                c.id.c_str(), c.status.c_str(), note.c_str(), c.ref, c.ref_u,
                fmt("%+.3f", hist_bp).c_str(), route.c_str(), p_std - c.ref,
                fmt("%+.3f", std_bp).c_str(), p_ref - c.ref, fmt("%+.3f", ref_bp).c_str());
  }
  std::printf("[d1-rescore] worst RESOLVED |IV bp|: standard %+.3f (%s), refined %+.3f (%s)\n",
              worst_std, worst_std_id.c_str(), worst_refined, worst_refined_id.c_str());
  std::fflush(stdout);
  // The route claims (kernel header section 4): Standard within 3 bp of IV on every
  // RESOLVED kernel-model row (the historical 301 lattice misses by up to 34), Refined
  // within 1 bp.
  EXPECT_LE(std::abs(worst_std), 3.0) << worst_std_id;
  EXPECT_LE(std::abs(worst_refined), 1.0) << worst_refined_id;
}

// ── 2. The per-event drift (the splice defect) ──────────────────────────────

struct DriftRef {
  double value = 0.0;
  double u = 0.0;
};

// D0's matrix ladder (matrix_opts: PDE 1200 / 300 per year, 4 levels).
[[nodiscard]] DriftRef live_reference(double K, double T, Side side,
                                      std::span<const CashDividend> divs) {
  ref::Problem p;
  p.S = 100.0;
  p.K = K;
  p.T = T;
  p.sigma = 0.25;
  p.r = 0.04;
  p.side = side;
  p.dividends = divs;
  ref::PdeLadder ladder;
  ladder.base.space_base = 1200;
  ladder.base.time_base = std::max(150, static_cast<int>(std::lround(300.0 * T)));
  ladder.levels = 4;
  const auto e = ref::pde_reference(p, ladder);
  EXPECT_TRUE(e.has_value());
  return e ? DriftRef{e->value, e->uncertainty} : DriftRef{kNaN, kNaN};
}

TEST(DiscreteDivProductionMatrix, PerEventDriftAgainstLiveReferences) {
  struct Base {
    const char *id;
    double K, T;
    Side side;
  };
  const Base bases[] = {{"C8p", 100, 2, Side::Put},
                        {"C8c", 95, 2, Side::Call},
                        {"D12p", 100, 3, Side::Put},
                        {"D12c", 95, 3, Side::Call}};
  DiscreteDivWorkspace ws(atx::vol::kDiscreteDivMaxRouteSteps, atx::vol::kDiscreteDivMaxPreSteps);
  std::printf("[d1-drift] | case | k | ref +- u | hist301 err | std err | refined err |\n");
  std::printf("[d1-drift] |---|---|---|---|---|---|\n");
  for (const Base &b : bases) {
    const auto full = quarterly(0.1, 1.0, b.T);
    std::vector<double> ks;
    std::vector<double> e_hist;
    std::vector<double> e_std;
    std::vector<double> e_ref;
    std::vector<double> us;
    for (const std::size_t k : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{4},
                                std::size_t{8}, std::size_t{12}}) {
      if (k > full.size()) {
        continue;
      }
      const std::vector<CashDividend> divs(full.begin(),
                                           full.begin() + static_cast<std::ptrdiff_t>(k));
      const DriftRef rf = live_reference(b.K, b.T, b.side, divs);
      const auto hist = atx::vol::american_discrete_div_price(
          100, b.K, b.T, 0.25, 0.04, 0.0, b.side, divs, atx::vol::kDiscreteDivDefaultSteps,
          ExerciseStyle::American, DiscreteDivScheme::SplicedCrr, ws);
      ASSERT_TRUE(hist.has_value());
      const Case c = make(b.id, 100, b.K, b.T, 0.25, 0.04, b.side, divs);
      const double p_std = production_price(c, route_opts(DiscreteDivAccuracy::Standard), ws);
      const double p_ref = production_price(c, route_opts(DiscreteDivAccuracy::Refined), ws);
      ks.push_back(static_cast<double>(k));
      us.push_back(rf.u);
      e_hist.push_back(*hist - rf.value);
      e_std.push_back(p_std - rf.value);
      e_ref.push_back(p_ref - rf.value);
      std::printf("[d1-drift] | %s | %zu | %.10f +- %.2e | %+.3e | %+.3e | %+.3e |\n", b.id, k,
                  rf.value, rf.u, e_hist.back(), e_std.back(), e_ref.back());
      std::fflush(stdout);
    }
    // Per-event drift: endpoint (e(K) - e(0)) / K, its reference uncertainty
    // (u(K) + u(0)) / K, and the least-squares slope over every truncation.
    const std::size_t last = ks.size() - 1;
    const double n_events = ks[last];
    const auto endpoint = [&](const std::vector<double> &e) {
      return (e[last] - e[0]) / n_events;
    };
    const auto slope = [&](const std::vector<double> &e) {
      double mk = 0.0;
      double me = 0.0;
      for (std::size_t i = 0; i < ks.size(); ++i) {
        mk += ks[i];
        me += e[i];
      }
      mk /= static_cast<double>(ks.size());
      me /= static_cast<double>(ks.size());
      double num = 0.0;
      double den = 0.0;
      for (std::size_t i = 0; i < ks.size(); ++i) {
        num += (ks[i] - mk) * (e[i] - me);
        den += (ks[i] - mk) * (ks[i] - mk);
      }
      return num / den;
    };
    const double u_drift = (us[last] + us[0]) / n_events;
    // The brief's claim, stated per case: |per-event drift| <= the reference's own
    // uncertainty on it, (u(K) + u(0)) / K. Printed, not asserted: the lattice's
    // residual scatter between truncations (~3e-5 USD on the Refined route) divided
    // by K is the floor of this estimator, so the verdict is a finding to report.
    const auto within = [&](const std::vector<double> &e) {
      return std::abs(endpoint(e)) <= u_drift ? "yes" : "NO";
    };
    std::printf("[d1-drift-summary] | %s | events %.0f | u(K) %.2e | u_drift %.2e | hist %+.3e "
                "(slope %+.3e) | std %+.3e (slope %+.3e) | refined %+.3e (slope %+.3e) | "
                "within u_drift: std %s, refined %s |\n",
                b.id, n_events, us[last], u_drift, endpoint(e_hist), slope(e_hist),
                endpoint(e_std), slope(e_std), endpoint(e_ref), slope(e_ref), within(e_std),
                within(e_ref));
    std::fflush(stdout);
    // The defect is O(1/N) per event; the cubic splice leaves the per-event term
    // at most 1/20 of it on the Standard route, and the Refined route's per-event
    // drift under 1e-5 USD (< 0.002 IV bp at these vegas).
    EXPECT_LE(std::abs(endpoint(e_std)), std::abs(endpoint(e_hist)) / 20.0) << b.id;
    EXPECT_LE(std::abs(endpoint(e_ref)), 1.0e-5) << b.id;
  }
}

// ── 3. Sensitivities against bump-and-revalue on the reference ──────────────

// The reference's common-mode fixed grid for bumps: D0's derivative ladder (800 /
// 200 per year) at space and time level 2, its domain pinned at the base sigma.
[[nodiscard]] ref::PdeGrid bump_grid(const Case &c) {
  ref::PdeGrid g;
  g.space_base = 800;
  g.space_level = 2;
  g.time_base = std::max(100, static_cast<int>(std::lround(200.0 * c.T)));
  g.time_level = 2;
  g.domain_sigma = c.sigma;
  return g;
}

[[nodiscard]] ref::Problem problem_of(const Case &c, std::span<const CashDividend> divs) {
  ref::Problem p;
  p.S = c.S;
  p.K = c.K;
  p.T = c.T;
  p.sigma = c.sigma;
  p.r = c.r;
  p.q = c.q;
  p.side = c.side;
  p.process = c.process;
  p.dividends = divs;
  return p;
}

[[nodiscard]] double grid_price(const ref::Problem &p, const ref::PdeGrid &g) {
  const auto out = ref::pde_price(p, g);
  EXPECT_TRUE(out.has_value());
  return out ? out->price : kNaN;
}

struct RefGreek {
  double value = kNaN;
  double u = kNaN; // the reference's own uncertainty for this greek on this row
};

// Central difference at h and h / 2 with one Richardson step (order 2). u is the
// bump study's own spread |central(h) - central(h / 2)|.
template <class Fn> [[nodiscard]] RefGreek central(double h, Fn &&price_at) {
  const double d1 = (price_at(h) - price_at(-h)) / (2.0 * h);
  const double d2 = (price_at(0.5 * h) - price_at(-0.5 * h)) / h;
  RefGreek g;
  g.value = d2 + (d2 - d1) / 3.0;
  g.u = std::abs(d2 - d1);
  return g;
}

// The PDE ladder's own uncertainty of a level-2 value x2 from the level-3 value x3:
// the second-order estimate (4 / 3) |x3 - x2|.
[[nodiscard]] double ladder_u(double x2, double x3) { return (4.0 / 3.0) * std::abs(x3 - x2); }

[[nodiscard]] ref::PdeGrid finer(ref::PdeGrid g) {
  ++g.space_level;
  ++g.time_level;
  return g;
}

// D0 section 8 (doc table "D0 DERIVATIVE TABLE"): delta and vega bumped on the finest
// PDE ladder at three sizes and extrapolated; u = the extrapolation's uncertainty plus
// the bumped prices' own. The study carries u = max(D0's u, its own estimate).
struct D0GreekU {
  const char *id;
  double delta_u, vega_u;
};
constexpr D0GreekU kD0GreekU[] = {{"A2p", 2.2e-5, 1.3e-3},  {"B3p", 1.2e-5, 2.9e-4},
                                  {"C4c", 1.7e-5, 3.3e-4},  {"D12p", 1.6e-5, 1.8e-3},
                                  {"E20p", 1.6e-5, 1.8e-3}, {"F1c", 1.2e-5, 6.7e-4}};

[[nodiscard]] const D0GreekU *d0_greek_u(const char *id) {
  for (const D0GreekU &g : kD0GreekU) {
    if (std::string(g.id) == id) {
      return &g;
    }
  }
  return nullptr;
}

// The claims on the Refined route (fix round 1, review-D1 M1). Each bound is
//   min(the checkpoint's bound, max(3 u, the class)),
// so it is never looser than before, it is 3 u wherever the reference resolves the
// greek finer than the class, and it is the class where the lattice's own
// discretisation sits above 3 u. The vega class is that case: C4c's Refined vega
// converges to -0.08 % as the bump goes to 0 (bump scan, D1-20261023-production
// diag-scratch-bumps.log), 80 times D0's u, and no runtime rule can single the
// row out; a pure 3 u bound would refuse a well-defined greek.
struct Claim {
  DiscreteDivGreek greek;
  bool relative;
  double before; // the checkpoint's bound
  double cls;    // the class (relative to |reference| when `relative`)
};
constexpr Claim kClaims[] = {
    {DiscreteDivGreek::Delta, false, 2.0e-4, 5.0e-5},
    {DiscreteDivGreek::Vega, true, 2.0e-3, 1.0e-3},
    {DiscreteDivGreek::Rho, true, 5.0e-3, 5.0e-3},
    {DiscreteDivGreek::DividendAmount, false, 2.0e-3, 2.0e-3},
    {DiscreteDivGreek::Roll, false, 2.0e-3, 2.0e-3},
};

[[nodiscard]] double claim_bound(const Claim &c, const RefGreek &r) {
  const double scale = c.relative ? std::abs(r.value) : 1.0;
  return std::min(c.before * scale, std::max(3.0 * r.u, c.cls * scale));
}

TEST(DiscreteDivProductionMatrix, GreeksAgainstBumpAndRevalueOnTheReference) {
  const std::vector<Case> m = matrix();
  // D0 section 8's six rows, B1p (the first-step ex-date) and the other three rows of
  // D0 group F (deep in the money before a late ex-date), which bracket the
  // exercise-dominated vega rule: F2c is a call 1.90 sd in the money there, F1p / F2p
  // are puts.
  const std::array<const char *, 10> ids{"A2p", "B3p", "C4c", "D12p", "E20p",
                                          "F1c", "B1p", "F1p", "F2c", "F2p"};
  // The rows the exercise-dominated vega rule covers (kernel header, the event-aware
  // sensitivities): F1c, a call 2.23 sd in the money before a late ex-date.
  const auto exercise_dominated = [](const std::string &id) { return id == "F1c"; };
  DiscreteDivWorkspace ws(atx::vol::kDiscreteDivMaxRouteSteps, atx::vol::kDiscreteDivMaxPreSteps);
  constexpr double kHorizon = 1.0 / 252.0;
  std::printf("[d1-greeks] | id | greek | reference +- u | std 301 | std 601 | std 1201 | "
              "refined | err std 301 | err refined | refined err / u | bound |\n");
  std::printf("[d1-greeks] |---|---|---|---|---|---|---|---|---|---|---|\n");
  for (const char *id : ids) {
    const Case &c = find(m, id);
    const ref::PdeGrid g = bump_grid(c);
    const ref::PdeGrid g3 = finer(g);
    const auto price_with = [&](auto &&edit) {
      std::vector<CashDividend> d = c.divs;
      ref::Problem p = problem_of(c, d);
      edit(p, d);
      p.dividends = d;
      return grid_price(p, g);
    };
    const auto base_grid = ref::pde_price(problem_of(c, c.divs), g);
    const auto base_fine = ref::pde_price(problem_of(c, c.divs), g3);
    ASSERT_TRUE(base_grid.has_value() && base_fine.has_value()) << id;
    const D0GreekU *d0u = d0_greek_u(id);
    // The reference greeks.
    std::array<RefGreek, 9> rg{};
    rg[static_cast<std::size_t>(DiscreteDivGreek::Delta)] = {
        base_grid->delta, std::max(ladder_u(base_grid->delta, base_fine->delta),
                                   d0u != nullptr ? d0u->delta_u : 0.0)};
    rg[static_cast<std::size_t>(DiscreteDivGreek::Gamma)] = {
        base_grid->gamma, ladder_u(base_grid->gamma, base_fine->gamma)};
    rg[static_cast<std::size_t>(DiscreteDivGreek::Vega)] = central(0.01, [&](double h) {
      return price_with([&](ref::Problem &p, std::vector<CashDividend> &) { p.sigma += h; });
    });
    if (d0u != nullptr) {
      double &vu = rg[static_cast<std::size_t>(DiscreteDivGreek::Vega)].u;
      vu = std::max(vu, d0u->vega_u);
    }
    rg[static_cast<std::size_t>(DiscreteDivGreek::Rho)] = central(2.0e-3, [&](double h) {
      return price_with([&](ref::Problem &p, std::vector<CashDividend> &) { p.r += h; });
    });
    rg[static_cast<std::size_t>(DiscreteDivGreek::Phi)] = central(2.0e-3, [&](double h) {
      return price_with([&](ref::Problem &p, std::vector<CashDividend> &) { p.q += h; });
    });
    // Theta: dP/dt at fixed spot, calendar — the life AND every ex-date shorten.
    const double min_tau = c.divs.empty() ? c.T : c.divs.front().tau;
    const double ht = std::min(1.0e-3, 0.25 * min_tau);
    rg[static_cast<std::size_t>(DiscreteDivGreek::Theta)] = central(ht, [&](double h) {
      return -price_with([&](ref::Problem &p, std::vector<CashDividend> &d) {
        p.T += h;
        for (CashDividend &dv : d) {
          dv.tau += h;
        }
      });
    });
    // dP/dD of the first event.
    rg[static_cast<std::size_t>(DiscreteDivGreek::DividendAmount)] = central(0.05, [&](double h) {
      return price_with(
          [&](ref::Problem &, std::vector<CashDividend> &d) { d.front().amount += h; });
    });
    // The roll over one trading day at fixed spot, crossed cash off the stock.
    double crossed = 0.0;
    std::vector<CashDividend> rolled;
    for (const CashDividend &dv : c.divs) {
      if (dv.tau <= kHorizon) {
        crossed += dv.amount;
      } else {
        rolled.push_back({dv.tau - kHorizon, dv.amount});
      }
    }
    {
      ref::Problem p = problem_of(c, rolled);
      p.S = c.S - crossed;
      p.T = c.T - kHorizon;
      const double roll2 = grid_price(p, g) - base_grid->price;
      const double roll3 = grid_price(p, g3) - base_fine->price;
      rg[static_cast<std::size_t>(DiscreteDivGreek::Roll)] = {roll2, ladder_u(roll2, roll3)};
    }

    // The lattice bundles: Standard at 301 / 601 / 1201 and Refined.
    struct Run {
      const char *name;
      DiscreteDivProductionOpts opts;
    };
    const std::array<Run, 4> runs{{{"std301", route_opts(DiscreteDivAccuracy::Standard, 301)},
                                   {"std601", route_opts(DiscreteDivAccuracy::Standard, 601)},
                                   {"std1201", route_opts(DiscreteDivAccuracy::Standard, 1201)},
                                   {"refined", route_opts(DiscreteDivAccuracy::Refined)}}};
    std::array<DiscreteDivProductionGreeks, 4> bundles{};
    std::array<double, 4> dpdd{};
    for (std::size_t i = 0; i < runs.size(); ++i) {
      std::vector<double> slots(c.divs.size(), kNaN);
      const auto b = atx::vol::american_discrete_div_production_greeks(
          c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, c.divs, ExerciseStyle::American, slots,
          kHorizon, runs[i].opts, ws);
      ASSERT_TRUE(b.has_value()) << id << " " << runs[i].name;
      bundles[i] = *b;
      dpdd[i] = slots.empty() ? kNaN : slots.front();
    }
    const auto lattice_value = [&](std::size_t run, DiscreteDivGreek greek) {
      const DiscreteDivProductionGreeks &b = bundles[run];
      if (!b.supported(greek)) {
        return kNaN;
      }
      switch (greek) {
      case DiscreteDivGreek::Delta:
        return b.delta;
      case DiscreteDivGreek::Gamma:
        return b.gamma;
      case DiscreteDivGreek::Vega:
        return b.vega;
      case DiscreteDivGreek::Rho:
        return b.rho;
      case DiscreteDivGreek::Phi:
        return b.phi;
      case DiscreteDivGreek::Theta:
        return b.theta;
      case DiscreteDivGreek::Charm:
        return b.charm;
      case DiscreteDivGreek::DividendAmount:
        return dpdd[run];
      case DiscreteDivGreek::Roll:
        return b.roll.roll;
      }
      return kNaN;
    };
    const auto claim_of = [](DiscreteDivGreek greek) -> const Claim * {
      for (const Claim &cl : kClaims) {
        if (cl.greek == greek) {
          return &cl;
        }
      }
      return nullptr;
    };
    const std::array<std::pair<DiscreteDivGreek, const char *>, 8> greeks{
        {{DiscreteDivGreek::Delta, "delta"},
         {DiscreteDivGreek::Gamma, "gamma"},
         {DiscreteDivGreek::Vega, "vega"},
         {DiscreteDivGreek::Rho, "rho"},
         {DiscreteDivGreek::Phi, "phi"},
         {DiscreteDivGreek::Theta, "theta"},
         {DiscreteDivGreek::DividendAmount, "dP/dD1"},
         {DiscreteDivGreek::Roll, "roll 1d"}}};
    for (const auto &[greek, name] : greeks) {
      const RefGreek &r = rg[static_cast<std::size_t>(greek)];
      std::string cells;
      for (std::size_t i = 0; i < runs.size(); ++i) {
        const double v = lattice_value(i, greek);
        cells += std::isfinite(v) ? fmt("%.6g", v) : std::string("UNSUPPORTED (NaN)");
        cells += " | ";
      }
      const double e301 = lattice_value(0, greek) - r.value;
      const double eref = lattice_value(3, greek) - r.value;
      const Claim *claim = claim_of(greek);
      const std::string bound =
          claim != nullptr ? fmt("%.2e", claim_bound(*claim, r)) : std::string("-");
      std::printf("[d1-greeks] | %s | %s | %.8g +- %.1e | %s%s | %s | %s | %s |\n", id, name,
                  r.value, r.u, cells.c_str(), fmt("%+.2e", e301).c_str(),
                  fmt("%+.2e", eref).c_str(),
                  r.u > 0.0 ? fmt("%.1f", std::abs(eref) / r.u).c_str() : "-", bound.c_str());
      // Every bundle field is either finite (supported) or NaN with its bit (never 0).
      for (std::size_t i = 0; i < runs.size(); ++i) {
        const double v = lattice_value(i, greek);
        EXPECT_TRUE(bundles[i].supported(greek) ? std::isfinite(v) : std::isnan(v))
            << id << " " << name;
      }
      if (claim == nullptr) {
        continue;
      }
      // The claim: within its bound on the Refined route, or typed unsupported with
      // its reason where (and only where) the rule says so.
      if (greek == DiscreteDivGreek::Vega && exercise_dominated(id)) {
        for (std::size_t i = 0; i < runs.size(); ++i) {
          EXPECT_FALSE(bundles[i].supported(greek)) << id << " " << runs[i].name;
          EXPECT_TRUE(bundles[i].has(atx::vol::DiscreteDivUnsupportedReason::ExerciseDominated))
              << id << " " << runs[i].name;
        }
        continue;
      }
      ASSERT_TRUE(bundles[3].supported(greek)) << id << " " << name;
      EXPECT_LE(std::abs(eref), claim_bound(*claim, r)) << id << " " << name;
    }
    std::printf("[d1-greeks] | %s | roll events crossed | %d (cash %.4f) | | | | | | | | |\n", id,
                bundles[0].roll.events_crossed, bundles[0].roll.cash_crossed);
    std::fflush(stdout);
    // The ex-date on step 1 of B1p: theta / charm are unsupported, never 0, and the
    // one-day roll crosses it as an EVENT.
    if (std::string(id) == "B1p") {
      EXPECT_FALSE(bundles[0].supported(DiscreteDivGreek::Theta));
      EXPECT_FALSE(bundles[0].supported(DiscreteDivGreek::Charm));
      EXPECT_TRUE(bundles[0].has(atx::vol::DiscreteDivUnsupportedReason::ExDateInStencil));
      EXPECT_EQ(bundles[0].roll.events_crossed, 1);
    }
  }
}

} // namespace
