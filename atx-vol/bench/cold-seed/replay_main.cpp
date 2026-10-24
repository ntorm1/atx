// Isolated replay. Vector prices remain diagnostics, never cold-proof inputs.
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <immintrin.h>
#include <nlohmann/json.hpp>

#include "bench_clocks.hpp"
#include "atx/vol/api/pricing/american_region.hpp"
#include "atx/vol/api/simd/cpu.hpp"
#include "pricing/american_iv_search.hpp"
#include "simd/american_boundary_avx2_kernel.hpp"

namespace {
using namespace atx::vol;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr double nan = std::numeric_limits<double>::quiet_NaN();
struct Request {
  std::uint64_t batch, index;
  double S, K, T, r, q, target, budget, tol, sigma;
  Side side;
  std::optional<AlOpts> opts;
};
struct Value { double price = nan, boundary = nan; AlExerciseClass cls = AlExerciseClass::UnsupportedRegime;
  bool patched = false; };

Value value(const Result<AlRegionPrice> &p) {
  return p ? Value{p->price, p->boundary, p->cls, false} : Value{};
}
Value scalar(const Request &r) {
  return value(andersen_lake_classified(r.S, r.K, r.T, r.sigma, r.r, r.q, r.side,
                                       r.opts, AlExerciseRegion::KernelFloor));
}
bool same(double a, double b) { return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b); }
bool same_opts(const std::optional<AlOpts> &a, const std::optional<AlOpts> &b) {
  if (a.has_value() != b.has_value()) return false;
  return !a || (a->n_collocation == b->n_collocation && a->n_quadrature == b->n_quadrature &&
      a->n_quad_price == b->n_quad_price && a->max_newton_iter == b->max_newton_iter &&
      same(a->tol, b->tol) && a->profile == b->profile);
}
bool same_model(const Request &a, const Request &b) {
  return same(a.T,b.T) && same(a.r,b.r) && same(a.q,b.q) && same_opts(a.opts,b.opts);
}
std::vector<Value> geometry(const std::vector<Request> &rs) {
  // Setup, exact-key lookup, allocations and destruction belong to this call.
  struct Prepared { std::size_t index; AlExpiryGeometry geometry; };
  std::vector<Prepared> models;
  std::vector<Value> out; out.reserve(rs.size());
  for (std::size_t i = 0; i < rs.size(); ++i) {
    const auto &r = rs[i];
    auto it = std::find_if(models.begin(), models.end(), [&](const auto &g) {
      return same_model(rs[g.index], r);
    });
    if (it == models.end()) {
      models.push_back({i, prepare_al_geometry(r.T,r.r,r.q,r.opts)});
      it = models.end() - 1;
    }
    out.push_back(value(andersen_lake_classified(it->geometry,r.S,r.K,r.sigma,r.side,
                                                AlExerciseRegion::KernelFloor)));
  }
  return out;
}
std::vector<Value> vector_prices(const std::vector<Request> &rs) {
  namespace detail = atx::vol::simd::detail;
  // Preserve each captured operation's grouping and order; no cross-expiry
  // amalgamation. Include call-to-put packing, allocation and scalar tails.
  std::vector<Value> out(rs.size());
  std::size_t begin = 0;
  while (begin < rs.size()) {
    std::size_t end = begin + 1;
    while (end < rs.size() && rs[end].batch == rs[begin].batch) ++end;
    for (std::size_t i = begin; i < end;) {
      if (end-i < 4) { out[i] = scalar(rs[i]); out[i].patched = true; ++i; continue; }
      alignas(32) std::array<double,4> S{},K{},T{},sigma{},r{},q{},price{};
      for (std::size_t j=0;j<4;++j) {
        const auto &a=rs[i+j];
        if (!same_opts(a.opts,rs[i].opts)) throw std::runtime_error("Mixed pack options");
        const bool call=a.side==Side::Call;
        S[j]=call?a.K:a.S; K[j]=call?a.S:a.K; T[j]=a.T; sigma[j]=a.sigma;
        r[j]=call?a.q:a.r; q[j]=call?a.r:a.q;
      }
      amer::AlBoundary bnd[4]; amer::AlWorkspace ws[4]; detail::PutPackBoundary pack;
      bool eligible[4]{}; int ref=-1;
      detail::solve_put_boundary_pack_avx2(S.data(),K.data(),T.data(),sigma.data(),r.data(),q.data(),
          4,amer::scheme_from_opts(rs[i].opts),bnd,ws,pack,eligible,ref);
      std::array<AlExerciseClass,4> classes{};
      if (ref>=0) _mm256_store_pd(price.data(),detail::price_put_pack_at_avx2(pack,
          _mm256_load_pd(S.data()),pack.Kv,pack.XMAX,AlExerciseRegion::KernelFloor,classes.data()));
      for (std::size_t j=0;j<4;++j) {
        if (ref<0 || !eligible[j] || !std::isfinite(price[j])) {
          out[i+j]=scalar(rs[i+j]); out[i+j].patched=true;
        } else out[i+j]={price[j],nan,classes[j],false};
      }
      i+=4;
    }
    begin=end;
  }
  return out;
}
bool price_gate(const Request &r,const Value &v) {
  return std::isfinite(v.price) && v.cls!=AlExerciseClass::UnsupportedRegime &&
      v.cls!=AlExerciseClass::ExercisePlateau && std::abs(v.price-r.target)<=r.budget;
}
Json describe(const Value &v,const Request &r) {
  return {{"price",std::isfinite(v.price)?Json(v.price):Json(nullptr)},
      {"price_bits",std::to_string(std::bit_cast<std::uint64_t>(v.price))},
      {"boundary",std::isfinite(v.boundary)?Json(v.boundary):Json(nullptr)},
      {"class",to_string(v.cls)},{"patched",v.patched},{"price_gate",price_gate(r,v)}};
}
Json split_work(const std::vector<Request> &rs) {
  // Separate diagnostic pass: unchanged exact public boundary handles expose
  // solve versus price work without instrumentation inside numerical routines.
  struct Model { std::size_t request; AlExpiryGeometry geometry; };
  struct Handle { std::size_t model; Side side; double anchor;
                  std::unique_ptr<AlBoundaryHandle> value; };
  std::vector<Model> models;
  std::vector<Handle> handles; // destroyed before the borrowed geometries
  std::vector<double> prices;
  double setup=0.,solve=0.,evaluate=0.;
  std::size_t failures=0;
  const auto total_start=Clock::now();
  for(std::size_t i=0;i<rs.size();++i) {
    const auto &r=rs[i];
    auto model=std::find_if(models.begin(),models.end(),[&](const auto &m){
      return same_model(rs[m.request],r); });
    if(model==models.end()) {
      const auto t=Clock::now();
      models.push_back({i,prepare_al_geometry(r.T,r.r,r.q,r.opts)});
      setup+=std::chrono::duration<double>(Clock::now()-t).count();
      model=models.end()-1;
    }
    const auto index=static_cast<std::size_t>(model-models.begin());
    const double anchor=r.side==Side::Call?r.S:r.K;
    auto handle=std::find_if(handles.begin(),handles.end(),[&](const auto &h){
      return h.model==index&&h.side==r.side&&same(h.anchor,anchor); });
    if(handle==handles.end()) {
      const auto t=Clock::now();
      handles.push_back({index,r.side,anchor,
          std::make_unique<AlBoundaryHandle>(model->geometry,r.side,anchor)});
      setup+=std::chrono::duration<double>(Clock::now()-t).count();
      handle=handles.end()-1;
    }
    const auto begin_solve=Clock::now();
    const auto status=handle->value->solve(r.sigma);
    solve+=std::chrono::duration<double>(Clock::now()-begin_solve).count();
    const auto begin_price=Clock::now();
    const double price=status?handle->value->price_from_boundary(r.S,r.K):nan;
    evaluate+=std::chrono::duration<double>(Clock::now()-begin_price).count();
    failures+=std::isfinite(price)?0u:1u;
    prices.push_back(price);
  }
  const auto model_count=models.size(),handle_count=handles.size();
  const auto destruction=Clock::now();handles.clear();models.clear();
  setup+=std::chrono::duration<double>(Clock::now()-destruction).count();
  const double total=std::chrono::duration<double>(Clock::now()-total_start).count();
  Json values=Json::array();
  for(double price:prices) values.push_back({{"price",std::isfinite(price)?Json(price):Json(nullptr)},
      {"price_bits",std::to_string(std::bit_cast<std::uint64_t>(price))}});
  return {{"models",model_count},{"handles",handle_count},{"requests",rs.size()},
      {"nonfinite",failures},{"setup_and_destruction_s",setup},{"boundary_solve_s",solve},
      {"price_evaluation_s",evaluate},{"inclusive_s",total},{"other_s",total-setup-solve-evaluate},
      {"prices",values},{"scope","Separate diagnostic pass; price evaluation includes European leg, premium integral and clamp. No classification supplied by handle."}};
}
std::vector<Request> read(const Json &input) {
  if(input.at("schema")!="atx.cold-seed.requests.v1") throw std::runtime_error("Wrong input schema");
  std::vector<Request> rs;
  for(const auto &a:input.at("requests")) {
    for(const char *key:{"batch","index","side","n_collocation","n_quadrature",
        "n_quad_price","max_newton_iter","profile"})
      if(!a.at(key).is_number_integer() || a.at(key).get<std::int64_t>()<0)
        throw std::runtime_error("Invalid integer input");
    for(const char *key:{"n_collocation","n_quadrature","n_quad_price","max_newton_iter"})
      if(a.at(key).get<std::uint64_t>()>65535) throw std::runtime_error("Option narrowing");
    for(const char *key:{"S","K","T","r","q","target","budget","tol","sigma","opts_tol"})
      if(!a.at(key).is_number() || !std::isfinite(a.at(key).get<double>()))
        throw std::runtime_error("Invalid numeric input");
    const int side=a.at("side").get<int>();
    if(side!=static_cast<int>(Side::Call)&&side!=static_cast<int>(Side::Put)) throw std::runtime_error("Bad side");
    Request r{a.at("batch"),a.at("index"),a.at("S"),a.at("K"),a.at("T"),a.at("r"),a.at("q"),
        a.at("target"),a.at("budget"),a.at("tol"),a.at("sigma"),static_cast<Side>(side),std::nullopt};
    for(double x:{r.S,r.K,r.T,r.r,r.q,r.target,r.budget,r.tol,r.sigma})
      if(!std::isfinite(x)) throw std::runtime_error("Nonfinite request");
    if(!(r.S>0&&r.K>0&&r.T>0&&r.budget>0&&r.tol>0&&r.sigma>kIvMin))
      throw std::runtime_error("Invalid positive input");
    if(a.at("opts_explicit").get<bool>()) {
      r.opts=AlOpts{.n_collocation=a.at("n_collocation").get<std::uint16_t>(),
          .n_quadrature=a.at("n_quadrature").get<std::uint16_t>(),
          .n_quad_price=a.at("n_quad_price").get<std::uint16_t>(),
          .max_newton_iter=a.at("max_newton_iter").get<std::uint16_t>(),
          .tol=a.at("opts_tol"),.profile=static_cast<AlProfile>(a.at("profile").get<int>())};
    }
    if(a.at("profile").get<int>()!=static_cast<int>(AlProfile::Reference))
      throw std::runtime_error("Research scope is Reference only");
    if(!rs.empty() && std::pair(r.batch,r.index)<=std::pair(rs.back().batch,rs.back().index))
      throw std::runtime_error("Nonunique or unordered request identity");
    rs.push_back(r);
  }
  if(rs.empty())throw std::runtime_error("Empty input");
  return rs;
}
Json run(const Json &input,int reps,std::optional<unsigned> cpu) {
  const auto rs=read(input); Json rows=Json::array(), timings=Json::array();
  std::array<std::vector<Value>,3> expected;
  std::vector<bool> accepted;
  const auto before=bench::sample_cpus();
  for(int rep=0;rep<reps;++rep) {
    for(int turn=0;turn<4;++turn) {
      const int route=(turn+rep)%4; const auto t0=Clock::now();
      std::vector<Value> values; std::vector<bool> decisions;
      if(route==0) { values.reserve(rs.size());for(const auto &r:rs) values.push_back(scalar(r)); }
      else if(route==1) values=geometry(rs);
      else if(route==2) values=vector_prices(rs);
      else {
        std::optional<IvPriceSeedFloorCache> cache;
        std::uint64_t previous=std::numeric_limits<std::uint64_t>::max();
        for(const auto &r:rs) {
          if(previous!=r.batch) { cache.emplace();previous=r.batch; }
          IvOpts opts; opts.method=AmericanMethod::AndersenLake;opts.tol=r.tol;opts.al_opts=r.opts;
          opts.refuse_non_monotone=true;
          decisions.push_back(american_implied_vol_price_seed(r.target,r.S,r.K,r.T,r.r,r.q,r.side,
              opts,r.budget,r.sigma,&*cache).has_value());
        }
      }
      const double seconds=std::chrono::duration<double>(Clock::now()-t0).count();
      timings.push_back({{"rep",rep},{"order",turn},{"route",route},{"seconds",seconds}});
      if(rep==0) { if(route<3) expected[static_cast<std::size_t>(route)]=std::move(values);
                   else accepted=std::move(decisions); }
      else if(route<3) {
        const auto &old=expected[static_cast<std::size_t>(route)];
        for(std::size_t i=0;i<rs.size();++i)
          if(!same(values[i].price,old[i].price)||values[i].cls!=old[i].cls)
            throw std::runtime_error("Repeated result differs");
      } else if(decisions!=accepted) throw std::runtime_error("Repeated proof differs");
    }
  }
  const auto attribution=split_work(rs);
  const auto after=bench::sample_cpus();
  for(std::size_t i=0;i<rs.size();++i) rows.push_back({{"request",input.at("requests").at(i)},
      {"scalar",describe(expected[0][i],rs[i])},{"geometry",describe(expected[1][i],rs[i])},
      {"avx2",describe(expected[2][i],rs[i])},{"scalar_full_proof",accepted[i]}});
  return {{"schema","atx.cold-seed.replay.v1"},{"request_count",rs.size()},
      {"repetitions",reps},{"routes",{"scalar_price","prepared_geometry_price","avx2_price","scalar_full_proof"}},
      {"timings",timings},{"rows",rows},{"attribution",attribution},
      {"build",bench::build_info_json()},{"ipo",ATX_VOL_COLD_SEED_REPLAY_IPO!=0},
      {"affinity",bench::affinity_json(cpu,cpu.has_value(),before,after)},
      {"process",bench::process_identity_json()},
      {"scope","Price routes include output allocation, packing, setup and scalar fallback; scalar_full_proof includes original floor/chord logic. No AVX2 full proof is implemented or qualified."}};
}
} // namespace
int main(int argc,char **argv) {
  try {
    if(argc<3||argc>5)throw std::runtime_error("Usage: replay input.json output.json [reps] [PcoreCPU]");
    const int reps=argc>=4?std::stoi(argv[3]):1;
    if(reps<1||reps>1000)throw std::runtime_error("Bad repetition count");
    if(!atx::vol::simd::have_avx2())throw std::runtime_error("AVX2+FMA required");
    const auto cpu=argc==5?std::optional<unsigned>(static_cast<unsigned>(std::stoul(argv[4]))):std::nullopt;
    if(cpu) { atx::vol::bench::pin_to_logical_cpu(*cpu);
      if(atx::vol::bench::is_performance_cpu(*cpu)!=true)throw std::runtime_error("Not a P-core"); }
    std::ifstream source(argv[1]); if(!source)throw std::runtime_error("Cannot open input");
    Json input;source>>input;
    const auto result=run(input,reps,cpu);
    std::ofstream output(argv[2]);output<<result.dump(2)<<'\n';
    if(!output)throw std::runtime_error("Cannot write output");
    return 0;
  } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
