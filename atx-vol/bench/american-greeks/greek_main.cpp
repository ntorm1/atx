// Offline Greek diagnostics. Every price stencil is exported for independent
// reconstruction; this tool defines no Greek accuracy or latency threshold.
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "atx/vol/api/pricing/american.hpp"

namespace {
using namespace atx::vol;
using Json = nlohmann::json;
constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
constexpr std::array<const char *, 9> kFields = {
    "delta", "gamma", "vega", "theta", "rho", "phi", "vanna", "volga", "charm"};
using Offset = std::array<int, 5>; // S, sigma, T, r, q; K and side never move
struct Contract { double S, K, T, sigma, r, q; Side side; };

Json number(double x) { return std::isfinite(x) ? Json(x) : Json(nullptr); }
Json status(const Status &s) {
  return s ? Json{{"status", "ok"}}
           : Json{{"status", "refused"}, {"code", static_cast<unsigned>(s.error().code())},
                  {"message", s.error().message()}};
}
Contract contract(const Json &j) {
  for (const char *key : {"S", "K", "T", "sigma", "r", "q"}) {
    if (!j.at(key).is_number() || !std::isfinite(j.at(key).get<double>()))
      throw std::invalid_argument("nonfinite or nonnumeric contract");
  }
  const std::string side = j.at("side");
  if (side != "put" && side != "call") throw std::invalid_argument("unknown side");
  Contract c{j.at("S"), j.at("K"), j.at("T"), j.at("sigma"), j.at("r"), j.at("q"),
             side == "call" ? Side::Call : Side::Put};
  if (!(c.S > 0 && c.K > 0 && c.T > 0 && c.sigma > 0))
    throw std::invalid_argument("nonpositive contract");
  return c;
}
Json sample(double value, std::string why, const Contract &c) {
  const bool finite = std::isfinite(value);
  const double intrinsic = std::max(c.side == Side::Put ? c.K-c.S : c.S-c.K, 0.);
  return {{"price", number(value)}, {"status", finite ? "finite" : why},
          {"intrinsic_band", finite ? Json(intrinsic > 0 && value <= intrinsic + 1e-9*c.K)
                                      : Json(nullptr)}};
}
Json cold(const Contract &c, const AlOpts &opts) {
  const auto p = andersen_lake(c.S,c.K,c.T,c.sigma,c.r,c.q,c.side,opts);
  Json s = sample(p ? *p : kNan, p ? "nonfinite_success" : "refused", c);
  if (!p) s["code"] = static_cast<unsigned>(p.error().code());
  return s;
}
std::vector<Offset> offsets() {
  std::vector<Offset> out{Offset{}};
  for (std::size_t axis=0; axis<5; ++axis) {
    for (int sign : {1,-1}) { Offset d{}; d[axis]=sign; out.push_back(d); }
  }
  for (std::size_t axis : {std::size_t{1},std::size_t{2}}) {
    for (int a : {1,-1}) for (int b : {1,-1}) {
      Offset d{}; d[0]=a; d[axis]=b; out.push_back(d);
    }
  }
  return out;
}
template<class Price> Json stencils(const Contract &c, Price price) {
  Json levels = Json::array();
  for (double scale : {1.,.5,.25}) {
    const std::array<double,5> h = {
      scale*std::min(.001*c.S,.02*c.S*c.sigma*std::sqrt(c.T)),
      scale*.001*std::max(c.sigma,.1), scale*.002*c.T, scale*.0001, scale*.0001};
    Json samples = Json::array();
    for (const auto &d : offsets()) {
      Contract x=c;
      x.S+=d[0]*h[0]; x.sigma+=d[1]*h[1]; x.T+=d[2]*h[2];
      x.r+=d[3]*h[3]; x.q+=d[4]*h[4];
      Json s=price(x); s["offset"]=d; samples.push_back(std::move(s));
    }
    levels.push_back({{"scale",scale},{"h",h},{"samples",samples}});
  }
  return levels;
}
Json options(const AlOpts &o) {
  return {{"n_collocation",o.n_collocation},{"n_quadrature",o.n_quadrature},
          {"n_quad_price",o.n_quad_price},{"max_newton_iter",o.max_newton_iter},
          {"tol",o.tol},{"profile","Reference"}};
}
Json field(double value, bool supported, const std::string &reason) {
  // Preserve unsupported/nonfinite outcomes rather than changing their denominator.
  return {{"requested",true},{"supported",supported && std::isfinite(value)},
          {"value",number(value)},{"reason",supported && !std::isfinite(value)
                                                        ? "NonfiniteSuccess" : reason}};
}
Json cold_bundle(const Contract &c, bool pde) {
  const auto g=pde ? american_greeks_al(c.S,c.K,c.T,c.sigma,c.r,c.q,c.side)
                   : american_greeks_fd(c.S,c.K,c.T,c.sigma,c.r,c.q,c.side);
  Json fields=Json::object();
  std::array<double,9> values{};
  values.fill(kNan);
  if (g) values={g->delta,g->gamma,g->vega,g->theta,g->rho,kNan,g->vanna,g->volga,g->charm};
  for (std::size_t i=0;i<kFields.size();++i) {
    fields[kFields[i]]=field(values[i],g.has_value() && i!=5,
                             i==5 ? "NotInApi" : g ? "ApiValueUnqualified" : "ApiRefused");
  }
  Json out={{"price",number(g ? g->price : kNan)},
            {"price_status",g ? "success" : "refused"},{"fields",fields}};
  if (!g) out["code"]=static_cast<unsigned>(g.error().code());
  return out;
}
Json sigma_bundle(const AlSigmaFamily &fam, const AlSigmaFamilyCarryBumps &carry,
                  const Contract &c) {
  const auto g=family_greeks(fam,carry,c.S,c.K,c.sigma,AlSigmaFamilyGreeksMask::all());
  const std::array<double,9> values={g.delta,g.gamma,g.vega,g.theta,g.rho,g.phi,g.vanna,g.volga,g.charm};
  constexpr std::array<const char *,7> reasons={"NotRequested","Served","Exercised","Refused",
                                               "NoCarryBumps","CarryBumpsMismatch","NearExerciseEdge"};
  Json fields=Json::object();
  for (std::size_t i=0;i<values.size();++i) {
    fields[kFields[i]]=field(values[i],g.supported.has(static_cast<std::uint16_t>(1u<<i)),
                             reasons.at(static_cast<std::size_t>(g.reason[i])));
  }
  return {{"price",number(g.price)},{"price_status",g.status==AlSigmaFamilyStatus::Ok ? "success":"refused"},
          {"native_status",static_cast<unsigned>(g.status)},{"fields",fields},
          {"edge_sd",number(g.edge_sd)},{"near_edge",g.near_edge},
          {"exercised",g.exercised},{"floored",g.floored}};
}
Json q_bundle(const AlSigmaQFamily &fam, const AlSigmaQFamilyRateBumps &rate,
              const Contract &c) {
  AlSigmaQGreeksRequest request; request.mask=AlFamilyGreekMask::all();
  const auto g=family_greeks(fam,rate,c.S,c.K,c.sigma,c.q,request);
  const std::array<double,9> values={g.delta,g.gamma,g.vega,g.theta,g.rho,g.phi,kNan,kNan,kNan};
  constexpr std::array<const char *,10> reasons={"NotRequested","Served","Exercised","Refused",
      "NoCarryBumps","CarryBumpRefused","NoQAxis","NearNodeDegenerate","ConventionUnsupported","NearExerciseEdge"};
  Json fields=Json::object();
  for (std::size_t i=0;i<values.size();++i) {
    fields[kFields[i]]=field(values[i],i<6 && g.supported.has(static_cast<AlFamilyGreek>(i)),
      i<6 ? reasons.at(static_cast<std::size_t>(g.reason[i])) : "NotInApi");
  }
  return {{"price",number(g.price)},{"price_status",g.status==AlSigmaQFamilyStatus::Ok ? "success":"refused"},
          {"native_status",static_cast<unsigned>(g.status)},{"fields",fields},
          {"edge_sd",number(g.edge_sd)},{"near_edge",g.near_edge},
          {"exercised",g.exercised},{"floored",g.floored}};
}
Json evaluate(const Json &raw) {
  const Contract c=contract(raw);
  Json out={{"input",raw},{"engines",Json::object()},{"references",Json::object()},
            {"served_maps",Json::object()}};
  const std::array<AlOpts,2> profiles={
    AlOpts{.n_collocation=24,.n_quadrature=48,.n_quad_price=64,.max_newton_iter=24,.tol=1e-13},
    AlOpts{.n_collocation=32,.n_quadrature=64,.n_quad_price=64,.max_newton_iter=32,.tol=1e-14}};
  for (std::size_t i=0;i<profiles.size();++i) {
    out["references"][i==0 ? "refined24" : "refined32"] =
      {{"options",options(profiles[i])},{"stencils",stencils(c,[&](const Contract &x){return cold(x,profiles[i]);})}};
  }
  out["engines"]["cold_fd"]=cold_bundle(c,false);
  out["engines"]["cold_al"]=cold_bundle(c,true);
  const auto geometry=prepare_al_geometry(c.T,c.r,c.q);
  const double anchor=c.side==Side::Put ? c.K : c.S;
  const double lo=.8*c.sigma, hi=1.2*c.sigma, qlo=c.q-.0005, qhi=c.q+.0005;
  AlSigmaFamily fam(geometry,c.side,anchor);
  const auto prepared=fam.prepare(lo,hi,12);
  AlSigmaFamilyCarryBumps carry;
  const auto bumped=carry.prepare(fam,AlSigmaFamilyGreeksMask::all());
  auto sigma=sigma_bundle(fam,carry,c);
  sigma["prepare"]=status(prepared); sigma["carry_prepare"]=status(bumped);
  sigma["box"]={{"anchor",anchor},{"sigma_lo",lo},{"sigma_hi",hi},{"n_sigma",12}};
  out["engines"]["sigma_family"]=sigma;
  out["served_maps"]["sigma_family"]=stencils(c,[&](const Contract &x){
    if (x.T!=c.T || x.r!=c.r || x.q!=c.q) return sample(kNan,"NoModelAxis",x);
    const auto p=fam.price(x.S,x.K,x.sigma);
    return sample(p.price,p.status==AlSigmaFamilyStatus::Ok ? "nonfinite_success":"refused",x);
  });
  AlSigmaQFamily qfam(geometry,c.side,anchor);
  const auto qp=qfam.prepare(lo,hi,12,qlo,qhi,4);
  AlSigmaQFamilyRateBumps rate;
  const auto rp=rate.prepare(qfam);
  auto q=q_bundle(qfam,rate,c); q["prepare"]=status(qp); q["carry_prepare"]=status(rp);
  q["box"]={{"anchor",anchor},{"sigma_lo",lo},{"sigma_hi",hi},{"n_sigma",12},
             {"q_lo",qlo},{"q_hi",qhi},{"n_q",4}};
  out["engines"]["sigma_q_family"]=q;
  out["served_maps"]["sigma_q_family"]=stencils(c,[&](const Contract &x){
    if (x.T!=c.T || x.r!=c.r) return sample(kNan,"NoModelAxis",x);
    const auto p=qfam.price(x.S,x.K,x.sigma,x.q);
    return sample(p.price,p.status==AlSigmaQFamilyStatus::Ok ? "nonfinite_success":"refused",x);
  });
  return out;
}
Json read(const char *path) {
  std::ifstream stream(path); if (!stream) throw std::runtime_error("input open failed");
  Json j; stream>>j; return j;
}
} // namespace

int main(int argc,char **argv) {
  try {
    if (argc!=4) throw std::invalid_argument("usage: greek-audit corpus selection-json-or-dash output");
    const Json corpus=read(argv[1]);
    if (corpus.at("schema")!="atx-american-greek-corpus-v1" ||
        corpus.at("case_count")!=corpus.at("cases").size() ||
        corpus.at("derivative_tolerances")!=nullptr) throw std::invalid_argument("corpus schema");
    std::set<std::string> all, selected;
    for (const auto &row:corpus.at("cases")) {
      (void)contract(row);
      if (!all.insert(row.at("id").get<std::string>()).second) throw std::invalid_argument("duplicate input ID");
    }
    if (std::string(argv[2])=="-") selected=all;
    else for (const auto &id:read(argv[2])) {
      const auto value=id.get<std::string>();
      if (!all.contains(value) || !selected.insert(value).second) throw std::invalid_argument("selection ID");
    }
    if (selected.empty()) throw std::invalid_argument("empty selection");
    Json rows=Json::array();
    for (const auto &row:corpus.at("cases")) {
      if (selected.contains(row.at("id").get<std::string>())) rows.push_back(evaluate(row));
    }
    auto resolved=al_fast_profile(std::nullopt); resolved.profile=AlProfile::Reference;
    Json candidate_options=options(resolved); candidate_options["explicit"]=false;
    Json report={{"schema","atx-american-greek-native-v1"},{"input_case_count",all.size()},
      {"case_count",rows.size()},{"selected_ids",selected},{"requested_fields",kFields},
      {"convention",corpus.at("convention")},{"derivative_tolerances",nullptr},
      {"continuous_carry_only",true},{"reference_independent_method",false},
      {"family_setup","fresh per contract, default accurate Reference geometry"},
      {"candidate_options",candidate_options},
      {"family_q_half_width",.0005},{"family_sigma_factors",{.8,1.2}},
      {"step_scales",{1.,.5,.25}},{"cases",rows}};
    std::ofstream output(argv[3]); if (!output) throw std::runtime_error("output open failed");
    output<<report.dump(2)<<'\n';
    if (!output) throw std::runtime_error("output write failed");
    return 0;
  } catch (const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
