// Offline explicit-step diagnostics; public Greek defaults remain unchanged.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>
#include "pricing/american_greek_fd_audit.hpp"

namespace {
using namespace atx::vol;
using Json = nlohmann::json;
Json number(double x) { return std::isfinite(x) ? Json(x) : Json(nullptr); }
Json read(const char *path) {
  std::ifstream stream(path);
  if (!stream) throw std::runtime_error("input open failed");
  Json out; stream >> out; return out;
}
Json options(const AlOpts &o, bool explicit_options) {
  return {{"n_collocation",o.n_collocation},{"n_quadrature",o.n_quadrature},
    {"n_quad_price",o.n_quad_price},{"max_newton_iter",o.max_newton_iter},
    {"tol",o.tol},{"profile","Reference"},{"explicit",explicit_options}};
}
Json evaluate(const Json &c, const char *design, double scale, const char *profile,
              const std::optional<AlOpts> &opts) {
  const double S=c.at("S"), K=c.at("K"), T=c.at("T"), sigma=c.at("sigma"),
               r=c.at("r"), q=c.at("q");
  const Side side=c.at("side")=="put" ? Side::Put : Side::Call;
  const std::string name=design;
  const bool scaled_spot=name=="spot_scaled" || name=="spot_time";
  const bool relative_time=name=="time_relative" || name=="spot_time";
  const detail::AlGreekFdSteps steps{
      scaled_spot ? scale*std::min(.001*S,.02*S*sigma*std::sqrt(T)) : .001*S,
      .001,.0001,relative_time ? scale*.002*T : .001};
  detail::AlGreekFdAudit audit;
  const auto start=std::chrono::steady_clock::now();
  const auto result=detail::audit_american_greeks_fd(S,K,T,sigma,r,q,side,steps,audit,
                                                 AmericanMethod::AndersenLake,opts,false);
  const double wall=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
  const auto g=result ? *result : AmericanGreeks{};
  Json values=Json::array();
  for (double value : {g.delta,g.gamma,g.vega,g.theta,g.rho,g.vanna,g.volga,g.charm,g.price})
    values.push_back(result ? number(value) : Json(nullptr));
  Json samples=Json::array();
  for (const auto &sample:audit.samples)
    samples.push_back({{"model",{sample.S,sample.K,sample.T,sample.sigma,sample.r,sample.q}},
      {"price",number(sample.price)},{"internal_boundary",number(sample.internal_boundary)},
      {"boundary_required",sample.boundary_required},{"exercised",sample.exercised},
      {"evaluated",sample.evaluated}});
  auto resolved=opts.value_or(al_fast_profile(std::nullopt));
  resolved.profile=AlProfile::Reference;
  Json out={{"design",design},{"scale",scale},{"profile",profile},
    {"options",options(resolved,opts.has_value())},
    {"requested_steps",{steps.spot,steps.volatility,steps.rate,steps.time}},
    {"actual_steps",{audit.steps.spot,audit.steps.volatility,audit.steps.rate,audit.steps.time}},
    {"rho_forward",audit.rho_forward},{"time_forward",audit.time_forward},
    {"result_success",result.has_value()},{"requested",static_cast<std::uint32_t>(g.requested)},
    {"supported",static_cast<std::uint32_t>(g.supported)},{"reasons",g.reasons},
    {"values",values},{"samples",samples},{"wall_s",wall},
    {"work",{{"sample_attempts",audit.sample_attempts},
      {"boundary_cold_solves",audit.boundary_cold_solves},
      {"boundary_warm_solves",audit.boundary_warm_solves},
      {"boundary_price_calls",audit.boundary_price_calls},
      {"public_price_calls",audit.public_price_calls},
      {"european_price_calls",audit.european_price_calls},
      {"degenerate_samples",audit.degenerate_samples}}}};
  if (!result) out["error"]={{"code",static_cast<unsigned>(result.error().code())},
                              {"message",result.error().message()}};
  return out;
}
} // namespace

int main(int argc,char **argv) {
  try {
    if (argc!=4) throw std::invalid_argument("usage: fd-refinement corpus selection output");
    const Json corpus=read(argv[1]), selection=read(argv[2]);
    if (corpus.at("schema")!="atx-american-greek-corpus-v1" ||
        corpus.at("case_count")!=corpus.at("cases").size() ||
        corpus.at("derivative_tolerances")!=nullptr || !selection.is_array())
      throw std::invalid_argument("corpus schema");
    std::set<std::string> all, selected;
    for (const auto &c:corpus.at("cases")) {
      if (!all.insert(c.at("id").get<std::string>()).second)
        throw std::invalid_argument("duplicate corpus ID");
      for (const char *key:{"S","K","T","sigma","r","q"})
        if (!c.at(key).is_number() || !std::isfinite(c.at(key).get<double>()))
          throw std::invalid_argument("contract numeric input");
      if (!(c.at("S").get<double>()>0 && c.at("K").get<double>()>0 &&
            c.at("T").get<double>()>0 && c.at("sigma").get<double>()>0) ||
          (c.at("side")!="put" && c.at("side")!="call"))
        throw std::invalid_argument("contract domain");
    }
    for (const auto &id:selection) {
      const auto value=id.get<std::string>();
      if (!all.contains(value) || !selected.insert(value).second)
        throw std::invalid_argument("selection identity");
    }
    if (selected.empty()) throw std::invalid_argument("empty selection");
    const std::array<std::optional<AlOpts>,3> profiles={std::nullopt,
      AlOpts{.n_collocation=24,.n_quadrature=48,.n_quad_price=64,.max_newton_iter=24,.tol=1e-13},
      AlOpts{.n_collocation=32,.n_quadrature=64,.n_quad_price=64,.max_newton_iter=32,.tol=1e-14}};
    constexpr std::array names={"default","refined24","refined32"};
    Json rows=Json::array();
    for (const auto &c:corpus.at("cases")) {
      if (!selected.contains(c.at("id").get<std::string>())) continue;
      Json entries=Json::array();
      for (std::size_t p=0;p<profiles.size();++p)
        for (const char *design:{"legacy","spot_scaled","time_relative","spot_time"})
          for (double scale:{1.,.5,.25,.125,.0625}) {
            if (std::string(design)=="legacy" && scale!=1.) continue;
            entries.push_back(evaluate(c,design,scale,names[p],profiles[p]));
          }
      rows.push_back({{"input",c},{"evaluations",entries}});
      std::cout<<c.at("id").get<std::string>()<<" complete\n"<<std::flush;
    }
    Json report={{"schema","atx-american-fd-refinement-v2"},
      {"admission_contract","model-regime-v2"},{"case_count",rows.size()},
      {"input_case_count",all.size()},{"selected_ids",selected},{"cases",rows},
      {"convention",corpus.at("convention")},{"derivative_tolerances",nullptr},
      {"qualified",false},{"latency_qualified",false},{"warm_start",false},
      {"continuous_carry_only",true},{"reference_independent_method",false}};
    std::ofstream output(argv[3]);
    if (!output) throw std::runtime_error("output open failed");
    output<<report.dump(2)<<'\n';
    if (!output) throw std::runtime_error("output write failed");
    return 0;
  } catch (const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
