#include "strategy_ic_runner.hpp"
#include "strategy_ic_composition.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <new>
#include <ostream>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/data/strategy_data.hpp"
#include "atx/engine/factory/ic_research.hpp"
#include "atx/engine/parallel/det_pool.hpp"

namespace atx::impl::strategy {
namespace {
using namespace atx;
namespace co=atx::core;
namespace al=atx::engine::alpha;
namespace ex=atx::engine::factory;
using Json=nlohmann::json;
struct Candidate { std::string id,family,dsl_sha; al::Program program; };
struct Library { std::string id; std::vector<Candidate> candidates; usize max_slots{},lookback{}; };
struct Role { std::string path,sha,name; Json metadata; u64 bytes{}; };
struct Budget {
  u64 limit{},used{};
  bool add(u64 n,u64 width) { if (width && n>(limit-used)/width) return false; used+=n*width; return true; }
};
bool hash_valid(std::string_view value) {
  return value.size()==64 && std::all_of(value.begin(),value.end(),[](char c) {
    return (c>='0' && c<='9') || (c>='a' && c<='f');
  });
}
co::Result<std::string> metadata_text(const std::string& path) {
  std::ifstream in(path,std::ios::binary|std::ios::ate);
  if (!in || in.tellg()<=0 || static_cast<u64>(in.tellg())>(1ULL<<20))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: metadata missing/over1MiB");
  std::string text(static_cast<usize>(in.tellg()),'\0');
  in.seekg(0); in.read(text.data(),static_cast<std::streamsize>(text.size()));
  if (!in || in.peek()!=std::char_traits<char>::eof())
    return co::Err(co::ErrorCode::IoError,"IC runner: metadata extent changed");
  return co::Ok(std::move(text));
}
co::Result<Json> pinned_json(const std::string& path,const std::string& pin) {
  if (!hash_valid(pin)) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: external SHA256 required");
  ATX_TRY(auto text,metadata_text(path));
  ATX_TRY(auto actual,co::sha256_hex(text));
  if (actual!=pin) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: external metadata pin differs");
  return co::Ok(Json::parse(text));
}
Json method_recipe(const IcRunnerConfig& cfg,bool parallel_ic=true) {
  Json recipe{{"schema","atx.dsl-fast-ic/v1"},{"library_sha256",cfg.library_sha256},
      {"horizons",{5,21,63}},{"active_horizons",3},{"require_endpoint_presence",true},
      {"execution_delay",1},{"min_names",cfg.min_names},{"min_dates",cfg.min_dates},
      {"screen_rule","equivalence-v3"},{"practical_abs_ic",.002},{"confidence_multiplier",3.5},
      {"max_working_bytes",cfg.max_working_bytes},{"vm","ResearchFast;full-historical-asof-member-mask"},
      {"labels","close[d+1+h]/close[d+1]-1;strict-positive-observed-endpoints;role-maturity"},
      {"guard","observed-adjacent-log1.5;adjusted-log-vs-raw+.10;no-missing-zero-fill"},
      {"orientation","TRAIN21h-nonzero-sample-rank-mean;undefined=0;screen-diagnostic-only;freeze-before-validation"},
      {"composition","fixed-equal-family/equal-within;centered-tied-rank;missing-or-unoriented-neutral;no-redistribution"},
      {"planned_targets","final-rank-neutral-gross1;cadence5;fraction.25;no-drift;offcycle-membership-exit-zero;deployment-included"},
      {"scope","IC-and-planned-weight-change-only;no-costs-trades-NAV-Sharpe-or-holdout"}};
  if (cfg.workers!=1) recipe["vm_workers"]=cfg.workers;
  if (parallel_ic && cfg.workers!=1) recipe["research_ic_workers"]=cfg.workers;
  return recipe;
}
struct FrozenTrain { Json artifact,recipe; std::vector<int> signs; std::string recipe_sha; };
co::Result<FrozenTrain> frozen_train(const IcRunnerConfig& cfg,const Library& lib,const Role& train) {
  ATX_TRY(auto artifact,pinned_json(cfg.orientations_path,cfg.orientations_sha256));
  if (artifact.at("schema")!="atx.dsl-ic-orientations/v1" ||
      artifact.at("library_sha256")!=cfg.library_sha256 || artifact.at("train_manifest_sha256")!=cfg.train_sha256)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN artifact identity");
  const auto recipe_sha=artifact.at("recipe_sha256").get<std::string>();
  if (!hash_valid(recipe_sha)) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN recipe SHA");
  // The externally pinned artifact binds the adjacent recipe by canonical JSON
  // hash, not by trusting a second self-reported filename/hash pair.
  ATX_TRY(auto text,metadata_text((std::filesystem::path(cfg.orientations_path).parent_path()/"recipe.json").string()));
  auto recipe=Json::parse(text); ATX_TRY(auto actual,co::sha256_hex(recipe.dump()));
  if (actual!=recipe_sha) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN recipe hash differs");
  if (!recipe.at("max_working_bytes").is_number_integer() ||
      (recipe.contains("vm_workers") && !recipe.at("vm_workers").is_number_integer()))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN resource types");
  const auto source_bytes=recipe.at("max_working_bytes").get<i64>();
  const auto source_workers=recipe.value("vm_workers",i64{1});
  if (source_bytes<(32LL<<20) || source_bytes>(16LL<<30) || source_workers<1 || source_workers>4)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN resource bounds");
  auto source_cfg=cfg; source_cfg.max_working_bytes=static_cast<u64>(source_bytes);
  source_cfg.workers=static_cast<usize>(source_workers);
  // Earlier completed TRAIN artifacts used parallel VM but serial IC. Absence
  // means precisely that original execution path, not unknown numerical policy.
  auto expected=method_recipe(source_cfg,recipe.contains("research_ic_workers"));
  expected["role_manifest_sha256"]["train"]=cfg.train_sha256;
  if (recipe.at("role_manifest_sha256").contains("validation"))
    expected["role_manifest_sha256"]["validation"]=cfg.validation_sha256;
  if (recipe!=expected)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN method/statistical settings/role pins differ");
  const auto& rows=artifact.at("candidates");
  if (!rows.is_array() || rows.size()!=lib.candidates.size())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN candidate count");
  const auto score_dates=train.metadata.at("score_end").get<u64>()-train.metadata.at("score_begin").get<u64>();
  const auto max_dates=score_dates>22?score_dates-22:0;
  std::vector<int> signs; signs.reserve(rows.size());
  for (usize k=0;k<rows.size();++k) {
    const auto& row=rows[k]; const auto& c=lib.candidates[k];
    if (row.at("id")!=c.id || row.at("family")!=c.family || row.at("dsl_sha256")!=c.dsl_sha ||
        row.at("orientation_horizon")!=21 || row.at("composition_selection")!="all-fixed-candidates-no-screen-selection" ||
        row.at("fit_status")!="noisy-TRAIN-sample-orientation-not-significance" ||
        !row.at("sign").is_number_integer() || !row.at("sample_orientation_sign").is_number_integer())
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN candidate recipe");
    const auto& horizons=row.at("ic").at("horizons");
    if (!horizons.is_array() || horizons.size()!=3 || horizons[0].at("horizon")!=5 ||
        horizons[1].at("horizon")!=21 || horizons[2].at("horizon")!=63)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN horizons");
    const auto& estimate=horizons[1].at("rank");
    if (!estimate.at("valid_dates").is_number_integer())
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN date count type");
    const auto dates=estimate.at("valid_dates").get<i64>();
    if (dates<0 || static_cast<u64>(dates)>max_dates || row.at("orientation_dates")!=dates ||
        (dates==0 && !estimate.at("mean").is_null()) || (dates>0 && !estimate.at("mean").is_number()))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN mature observations");
    const auto mean=dates>0?estimate.at("mean").get<f64>():0;
    if (!std::isfinite(mean) || std::abs(mean)>1.000000000001)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN rank mean");
    const int sign=mean>0?1:(mean<0?-1:0);
    if (row.at("sign")!=sign || row.at("sample_orientation_sign")!=sign ||
        row.at("orientation_defined")!=(sign!=0))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN sign differs from recorded sample");
    signs.push_back(sign);
  }
  return co::Ok(FrozenTrain{std::move(artifact),std::move(recipe),std::move(signs),recipe_sha});
}
co::Status write_json(const std::filesystem::path& path,const Json& j) {
  std::ofstream out(path,std::ios::binary); if (!out) return co::Err(co::ErrorCode::IoError,"IC runner: JSON output");
  out<<j.dump(2)<<'\n'; out.close();
  return out?co::Ok():co::Status(co::Err(co::ErrorCode::IoError,"IC runner: JSON final close"));
}
bool composition_id(std::string_view s) {
  return !s.empty() && s.size()<=64 && std::all_of(s.begin(),s.end(),[](char c) {
    return (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_';
  });
}
co::Result<Library> library(const IcRunnerConfig& cfg) {
  ATX_TRY(auto j,pinned_json(cfg.library_path,cfg.library_sha256));
  if (j.at("schema")!="atx.dsl-ic-library/v1")
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: library schema");
  Library out; out.id=j.at("id").get<std::string>();
  std::set<std::string> fields,families,ids,expressions,used_families;
  for (const auto& field:j.at("fields")) fields.insert(field.at("name").get<std::string>());
  if (fields!=std::set<std::string>{"close","raw_close","volume"})
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: declared field contract");
  for (const auto& family:j.at("families"))
    if (!families.insert(family.at("id").get<std::string>()).second)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: duplicate family");
  if (out.id.empty() || out.id.size()>128 || families.empty() || families.size()>32 ||
      j.at("candidates").empty() || j.at("candidates").size()>256)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: bounded library size");
  const al::Library operators;
  for (const auto& row:j.at("candidates")) {
    Candidate c; c.id=row.at("id").get<std::string>(); c.family=row.at("family").get<std::string>();
    const auto dsl=row.at("dsl").get<std::string>();
    if (!composition_id(c.id) || !composition_id(c.family) ||
        !ids.insert(c.id).second || !expressions.insert(dsl).second || !families.contains(c.family) ||
        row.at("sign_policy")!="train-rank-ic21" || row.at("horizons")!=Json::array({5,21,63}) ||
        dsl.empty() || dsl.size()>4096)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate recipe/identity");
    // IDs are also CSV fields; reject separators rather than publish ambiguous rows.
    if (c.id.find_first_of(",\r\n\"")!=std::string::npos)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: unsafe candidate CSV identifier");
    ATX_TRY(auto ast,al::parse_expr(dsl,operators)); ATX_TRY(auto analysis,al::analyze(ast));
    ATX_TRY(c.program,al::compile(ast,analysis)); ATX_TRY(c.dsl_sha,co::sha256_hex(dsl));
    if (c.program.roots.size()!=1 || c.program.num_slots>64)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: one bounded DSL root required");
    for (const auto& field:c.program.fields) if (!fields.contains(field))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: undeclared DSL field");
    out.max_slots=std::max(out.max_slots,static_cast<usize>(c.program.num_slots));
    out.lookback=std::max(out.lookback,static_cast<usize>(c.program.required_lookback));
    used_families.insert(c.family); out.candidates.push_back(std::move(c));
  }
  if (families!=used_families) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: empty declared family");
  return co::Ok(std::move(out));
}
co::Result<Role> admit(const IcRunnerConfig& cfg,const Library& lib,std::string path,
                      std::string pin,std::string name,bool enforce_budget=true) {
  ATX_TRY(auto j,pinned_json(path,pin));
  const auto d=j.at("dates").get<u64>(),n=j.at("instruments").get<u64>();
  const auto begin=j.at("score_begin").get<u64>(),end=j.at("score_end").get<u64>();
  if (!d || d>4096 || !n || n>20000 || end!=d || begin>=end || begin<383 ||
      lib.lookback>begin-63)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: role shape/warmup/maturity");
  const auto cells=d*n,score_dates=end-begin;
  ATX_TRY(auto composition,ic_composition_working_bytes(static_cast<usize>(d),
      static_cast<usize>(n),lib.candidates.size()));
  Budget b{std::numeric_limits<u64>::max(),0};
  // One role26, guard4, effective+VM masks2, returned signal8, VM scratch32;
  // twice maximum compiled slot payload bounds grow-before-release. No surfaces,
  // execution context, per-candidate retained signals, or book position arrays.
  if (!b.add(1,32ULL<<20) || !b.add(cells,72+16*lib.max_slots) || !b.add(composition,1) ||
      !b.add(d,512) || !b.add(n,512))
    return co::Err(co::ErrorCode::Unavailable,"IC runner: combined role/VM/composition memory budget");
  for (u64 h:{5ULL,21ULL,63ULL}) {
    const auto mature=score_dates>h+1?score_dates-h-1:0;
    if (!b.add(mature*n,16) || !b.add(mature,32))
      return co::Err(co::ErrorCode::Unavailable,"IC runner: combined IC label/rank/scratch budget");
  }
  // No second VM or label cache: only worker-local Cs/TS scratch. Allow 2x
  // vector growth within1024B/name and64B/date; add64B/name for IC row buffers;
  // separately reserve8MiB stack
  // address/commit envelope plus64KiB runtime slack per explicit worker. This
  // conservative admission is not a measured thread-stack/RSS guarantee.
  if (cfg.workers>1 && (!b.add(cfg.workers,(8ULL<<20)+(64ULL<<10)) ||
      !b.add(cfg.workers*n,1024+64) || !b.add(cfg.workers*d,64)))
    return co::Err(co::ErrorCode::OutOfRange,"IC runner: worker scratch/stack envelope overflow");
  if (enforce_budget && b.used>cfg.max_working_bytes)
    return co::Err(co::ErrorCode::Unavailable,"IC runner: required_bytes="+std::to_string(b.used)+
        " max_compiled_slots="+std::to_string(lib.max_slots)+" exceeds configured memory budget before payload load");
  return co::Ok(Role{std::move(path),std::move(pin),std::move(name),std::move(j),b.used});
}
co::Result<std::vector<u32>> guard_for(const engine::data::StrategyRoleData& role) {
  const auto& p=role.panel; const auto d=p.dates(),n=p.instruments();
  ATX_TRY(auto close_id,p.field_id("close")); ATX_TRY(auto raw_id,p.field_id("raw_close"));
  const auto close=p.field_all(close_id),raw=p.field_all(raw_id);
  std::vector<u32> out(d*n,0);
  for (usize t=1;t<d;++t) for (usize i=0;i<n;++i) {
    const auto a=(t-1)*n+i,b=t*n+i;
    bool bad=false;
    if (p.in_universe(t-1,i) && p.in_universe(t,i) &&
        std::isfinite(close[a]) && std::isfinite(close[b]) && close[a]>0 && close[b]>0) {
      const auto r=std::log(close[b])-std::log(close[a]); bad=std::abs(r)>1.5;
      if (std::isfinite(raw[a]) && std::isfinite(raw[b]) && raw[a]>0 && raw[b]>0)
        bad=bad || std::abs(r)>std::abs(std::log(raw[b])-std::log(raw[a]))+.10;
    }
    out[b]=out[a]+static_cast<u32>(bad);
  }
  return co::Ok(std::move(out));
}
Json estimate_json(const ex::IcScreenEstimate& x,int sign) {
  const bool observed=x.valid_dates>0 && std::isfinite(x.mean);
  return {{"valid_dates",x.valid_dates},{"calendar_dates",x.calendar_dates},
      {"mean",observed?Json(x.mean):Json(nullptr)},
      {"oriented_mean",observed && sign!=0?Json(sign*x.mean):Json(nullptr)},
      {"inference_defined",x.defined},{"standard_error",x.defined?Json(x.standard_error):Json(nullptr)},
      {"upper_abs_ic",x.defined?Json(x.upper_abs_ic):Json(nullptr)},
      {"required_hac_lag",x.hac_lag}, {"segment_safeguard","heuristic-not-regime-recall-guarantee"}};
}
Json result_json(const ex::ResearchIcResult& result,int sign) {
  Json horizons=Json::array();
  for (usize k=0;k<result.active_horizons;++k) {
    const auto& h=result.screen.horizons[k]; const auto& c=result.coverage[k];
    horizons.push_back({{"horizon",h.horizon},{"pearson",estimate_json(h.pearson,sign)},
        {"rank",estimate_json(h.rank,sign)},{"coverage",{
          {"mature_dates",c.mature_dates},{"structural_tail_dates",c.structural_tail_dates},
          {"decision_eligible_pairs",c.decision_eligible_pairs},{"finite_label_pairs",c.finite_label_pairs},
          {"paired_signal_pairs",c.paired_signal_pairs},
          {"missing_signal_on_label_support",c.finite_label_pairs-c.paired_signal_pairs},
          {"missing_entry_pairs",c.missing_entry_pairs},{"missing_exit_pairs",c.missing_exit_pairs},
          {"guard_excluded_pairs",c.guard_excluded_pairs},{"invalid_price_pairs",c.invalid_price_pairs},
          {"nonfinite_return_pairs",c.nonfinite_return_pairs},
          {"endpoint_reason_counts_overlap",true}}}});
  }
  return {{"reject",result.screen.reject},{"enough_evidence",result.screen.enough_evidence},
      {"reason",ex::ic_screen_reason_name(result.screen.reason)},{"horizons",std::move(horizons)}};
}
void series(std::ofstream& out,std::string_view id,const engine::data::StrategyRoleData& role,
            const ex::ResearchIcScratch& scratch,int sign) {
  const std::array<usize,3> horizons{5,21,63};
  for (usize k=0;k<horizons.size();++k) {
    const auto p=scratch.pearson_series(k),r=scratch.rank_series(k);
    for (usize row=0;row<r.size();++row) {
      const auto d=role.score_begin+row;
      out<<id<<','<<horizons[k]<<','<<d<<','<<role.session_keys[d]<<',';
      if (std::isfinite(p[row])) out<<p[row]; out<<',';
      if (std::isfinite(r[row])) out<<r[row]; out<<',';
      if (sign!=0 && std::isfinite(r[row])) out<<sign*r[row]; out<<'\n';
    }
  }
}
co::Result<Json> score_role(const IcRunnerConfig& cfg,const Library& lib,const Role& spec,
    std::vector<int>& signs,Json& frozen,std::ostream& progress) {
  const auto started=std::chrono::steady_clock::now();
  progress<<"IC loading "<<spec.name<<" admitted_bytes="<<spec.bytes<<'\n'<<std::flush;
  ATX_TRY(auto role,engine::data::read_strategy_role(spec.path,cfg.max_working_bytes));
  const auto load_seconds=std::chrono::duration<f64>(std::chrono::steady_clock::now()-started).count();
  if (role.manifest_sha256!=spec.sha)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: role manifest changed after admission");
  ATX_TRY(auto guard,guard_for(role));
  auto ic=ex::equivalence_ic_screen_config();
  ic.horizons={5,21,63,0}; ic.min_names=cfg.min_names; ic.min_dates=cfg.min_dates;
  ic.window_begin=role.score_begin; ic.window_end=role.score_end; ic.maturity_end=role.score_end;
  ic.max_cache_bytes=cfg.max_working_bytes;
  const auto label_started=std::chrono::steady_clock::now();
  ATX_TRY(auto cache,ex::prepare_research_ic(role.panel,ic,{3,true,cfg.workers},role.decision_member,guard));
  ATX_TRY(auto scratch,ex::prepare_research_ic_scratch(cache));
  if (cache.bytes()>cfg.max_working_bytes || scratch.bytes()>cfg.max_working_bytes-cache.bytes())
    return co::Err(co::ErrorCode::Unavailable,"IC runner: actual IC cache/scratch exceeds admitted budget");
  const auto label_seconds=std::chrono::duration<f64>(std::chrono::steady_clock::now()-label_started).count();
  // Lifetime order matters: Engine borrows the pool and dies first. Candidate
  // evaluation is driven by THIS main thread; Cs and Ts jobs never nest.
  std::unique_ptr<engine::parallel::DetPool> pool;
  if (cfg.workers>1) pool=std::make_unique<engine::parallel::DetPool>(cfg.workers);
  al::Engine vm(role.panel); vm.set_eval_mode(al::EvalMode::ResearchFast);
  if (pool) { vm.set_cs_pool(pool.get()); vm.set_ts_pool(pool.get()); }
  ATX_TRY_VOID(vm.set_cross_section_mask(role.decision_member));
  ATX_TRY(auto close_id,role.panel.field_id("close")); const auto close=role.panel.field_all(close_id);
  std::vector<u8> effective=role.decision_member;
  for (usize d=0;d<role.panel.dates();++d) for (usize i=0;i<role.panel.instruments();++i) {
    const auto k=d*role.panel.instruments()+i;
    effective[k]=static_cast<u8>(effective[k] && role.panel.in_universe(d,i) && std::isfinite(close[k]) && close[k]>0);
  }
  std::vector<IcCompositionCandidate> candidates; candidates.reserve(lib.candidates.size());
  for (const auto& c:lib.candidates) candidates.push_back({c.id,c.family});
  IcCompositionConfig cc; cc.dates=role.panel.dates(); cc.instruments=role.panel.instruments();
  cc.decision_begin=role.score_begin; cc.decision_end=role.score_end; cc.max_working_bytes=cfg.max_working_bytes;
  ATX_TRY(auto composition,IcComposition::create(cc,candidates,effective));
  const auto dir=std::filesystem::path(cfg.output_directory);
  std::ofstream daily(dir/(spec.name+"_daily_ic.csv"),std::ios::binary);
  std::ofstream ledger(dir/(spec.name+"_candidates.jsonl"),std::ios::binary);
  if (!daily || !ledger) return co::Err(co::ErrorCode::IoError,"IC runner: role outputs");
  daily.imbue(std::locale::classic()); daily<<std::setprecision(17);
  daily<<"id,horizon,decision_index,session_ns,pearson,rank_ic,oriented_rank_ic\n";
  const bool train=spec.name=="train";
  if (!train && signs.size()!=lib.candidates.size())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN signs unavailable");
  Json summaries=Json::array();
  f64 total_vm_seconds=0,total_ic_seconds=0,total_composition_seconds=0;
  for (usize k=0;k<lib.candidates.size();++k) {
    const auto& candidate=lib.candidates[k];
    ledger<<Json{{"id",candidate.id},{"status","started"},{"number",k+1}}.dump()<<'\n'<<std::flush;
    if (!ledger) return co::Err(co::ErrorCode::IoError,"IC runner: candidate receipt");
    const auto candidate_started=std::chrono::steady_clock::now();
    progress<<"IC eval-start "<<spec.name<<' '<<(k+1)<<'/'<<lib.candidates.size()<<' '<<candidate.id
            <<" slots="<<candidate.program.num_slots<<'\n'<<std::flush;
    vm.reset();
    const auto vm_started=std::chrono::steady_clock::now();
    ATX_TRY(auto evaluated,vm.evaluate(candidate.program));
    const auto vm_seconds=std::chrono::duration<f64>(std::chrono::steady_clock::now()-vm_started).count();
    total_vm_seconds+=vm_seconds;
    progress<<"IC VM-complete "<<candidate.id<<" seconds="<<vm_seconds<<'\n'<<std::flush;
    if (evaluated.alphas.size()!=1) return co::Err(co::ErrorCode::Internal,"IC runner: VM root missing");
    const auto& signal=evaluated.alphas.front().values;
    const auto ic_started=std::chrono::steady_clock::now();
    ATX_TRY(auto scored,ex::evaluate_research_ic(signal,cache,scratch,pool.get()));
    const auto ic_seconds=std::chrono::duration<f64>(std::chrono::steady_clock::now()-ic_started).count();
    total_ic_seconds+=ic_seconds;
    const auto& orientation=scored.screen.horizons[1].rank;
    const bool fit=orientation.valid_dates>0 && std::isfinite(orientation.mean) && orientation.mean!=0;
    const int sample_sign=fit?(orientation.mean>0?1:-1):0;
    if (train) {
      const int sign=sample_sign; signs.push_back(sign);
      frozen.push_back({{"id",candidate.id},{"family",candidate.family},{"dsl_sha256",candidate.dsl_sha},
          {"sign",sign},{"sample_orientation_sign",sample_sign},{"orientation_defined",fit},
          {"orientation_horizon",21},{"orientation_dates",orientation.valid_dates},
          {"diagnostic_keep",!scored.screen.reject},{"composition_selection","all-fixed-candidates-no-screen-selection"},{"reason",ex::ic_screen_reason_name(scored.screen.reason)},
          {"fit_status","noisy-TRAIN-sample-orientation-not-significance"},
          {"ic",result_json(scored,sign)}});
    }
    const auto sign=signs[k];
    const auto composition_started=std::chrono::steady_clock::now();
    ATX_TRY_VOID(composition.add(k,signal,sign));
    const auto composition_seconds=std::chrono::duration<f64>(
        std::chrono::steady_clock::now()-composition_started).count();
    total_composition_seconds+=composition_seconds;
    series(daily,candidate.id,role,scratch,sign);
    auto summary=result_json(scored,sign);
    summary["id"]=candidate.id; summary["family"]=candidate.family;
    summary["frozen_train_sign"]=sign; summary["status"]="complete";
    const auto candidate_seconds=std::chrono::duration<f64>(
        std::chrono::steady_clock::now()-candidate_started).count();
    summary["wall_seconds"]=candidate_seconds;
    summary["stage_seconds"]={{"vm",vm_seconds},{"ic",ic_seconds},{"composition",composition_seconds}};
    summaries.push_back(summary); ledger<<summary.dump()<<'\n'<<std::flush;
    if (!daily || !ledger) return co::Err(co::ErrorCode::IoError,"IC runner: candidate output");
    progress<<"IC "<<spec.name<<' '<<(k+1)<<'/'<<lib.candidates.size()<<' '<<candidate.id
            <<" seconds="<<candidate_seconds<<" vm="<<vm_seconds<<" ic="<<ic_seconds
            <<" composition="<<composition_seconds<<" sign="<<sign<<" reason="<<ex::ic_screen_reason_name(scored.screen.reason)<<'\n'<<std::flush;
  }
  const auto finish_started=std::chrono::steady_clock::now();
  ATX_TRY(auto combined,composition.finish());
  total_composition_seconds+=std::chrono::duration<f64>(std::chrono::steady_clock::now()-finish_started).count();
  const auto combined_ic_started=std::chrono::steady_clock::now();
  ATX_TRY(auto combined_ic,ex::evaluate_research_ic(combined.signal,cache,scratch,pool.get()));
  total_ic_seconds+=std::chrono::duration<f64>(std::chrono::steady_clock::now()-combined_ic_started).count();
  series(daily,"__combined__",role,scratch,1);
  std::ofstream targets(dir/(spec.name+"_planned_targets.csv"),std::ios::binary);
  if (!targets) return co::Err(co::ErrorCode::IoError,"IC runner: target proxy output");
  targets.imbue(std::locale::classic()); targets<<std::setprecision(17);
  targets<<"decision_index,session_ns,planned_turnover,planned_gross,planned_net,contribution_fraction,eligible_names\n";
  for (usize d=role.score_begin;d<role.score_end;++d)
    targets<<d<<','<<role.session_keys[d]<<','<<combined.planned_turnover[d]<<','
           <<combined.planned_gross[d]<<','<<combined.planned_net[d]<<','
           <<combined.contribution_fraction[d]<<','<<combined.eligible_names[d]<<'\n';
  targets.close(); daily.close(); ledger.close();
  if (!targets || !daily || !ledger) return co::Err(co::ErrorCode::IoError,"IC runner: final output close");
  const auto seconds=std::chrono::duration<f64>(std::chrono::steady_clock::now()-started).count();
  return co::Ok(Json{{"role",spec.name},{"manifest_sha256",spec.sha},{"source_sha256",role.source_sha256},
      {"dates",role.panel.dates()},{"instruments",role.panel.instruments()},
      {"score_begin",role.score_begin},{"score_end",role.score_end}, {"wall_seconds",seconds},
      {"admitted_working_bytes",spec.bytes},{"ic_cache_bytes",cache.bytes()},{"ic_scratch_bytes",scratch.bytes()},
      {"workers",cfg.workers},{"stage_seconds",{{"load",load_seconds},{"label_preparation",label_seconds},
          {"vm",total_vm_seconds},{"ic",total_ic_seconds},{"composition",total_composition_seconds}}},
      {"candidate_evaluations",lib.candidates.size()},{"combined_evaluations",1},
      {"candidates",std::move(summaries)},{"combined_ic",result_json(combined_ic,1)},
      {"planned_target_proxy",{{"total_turnover",combined.total_planned_turnover},
          {"deployment_turnover",combined.deployment_turnover},{"deployment_date",combined.deployment_date},
          {"initial_deployment_included",true},{"actual_trades_or_costs",false}}}});
}
} // namespace
co::Status run_ic(const IcRunnerConfig& cfg,std::ostream& progress) {
  try {
    const bool validation_only=!cfg.orientations_path.empty();
    if ((!cfg.plan_only && cfg.output_directory.empty()) || cfg.max_working_bytes<(32ULL<<20) || cfg.max_working_bytes>(16ULL<<30) ||
        cfg.min_names<3 || cfg.min_dates<8 || cfg.min_dates>4096 || cfg.workers<1 || cfg.workers>4 ||
        cfg.validation_manifest.empty()!=cfg.validation_sha256.empty() ||
        cfg.orientations_path.empty()!=cfg.orientations_sha256.empty() ||
        (validation_only && cfg.validation_manifest.empty()))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: bounded config");
    ATX_TRY(auto lib,library(cfg));
    std::vector<Role> roles;
    ATX_TRY(auto train,admit(cfg,lib,cfg.train_manifest,cfg.train_sha256,"train",!validation_only)); roles.push_back(std::move(train));
    if (!cfg.validation_manifest.empty()) {
      ATX_TRY(auto val,admit(cfg,lib,cfg.validation_manifest,cfg.validation_sha256,"validation"));
      if (roles.front().metadata.at("score_end_ns").get<i64>()>val.metadata.at("score_start_ns").get<i64>())
        return co::Err(co::ErrorCode::InvalidArgument,"IC runner: overlapping/nonchronological roles");
      roles.push_back(std::move(val));
    }
    FrozenTrain recovered;
    if (validation_only) {
      ATX_TRY(recovered,frozen_train(cfg,lib,roles.front()));
      roles.erase(roles.begin()); // TRAIN metadata checked, payload never opened.
    }
    if (cfg.plan_only) {
      Json plan{{"mode","metadata-only-no-payload"},{"candidates",lib.candidates.size()},
          {"max_compiled_slots",lib.max_slots},{"required_lookback",lib.lookback},{"workers",cfg.workers},
          {"library_sha256",cfg.library_sha256},{"roles",Json::array()}};
      for (const auto& role:roles) plan["roles"].push_back({{"role",role.name},
          {"manifest_sha256",role.sha},{"required_bytes",role.bytes}});
      if (validation_only) {
        plan["run_mode"]="validation-only-frozen-TRAIN";
        plan["train_recipe_sha256"]=recovered.recipe_sha;
        plan["orientations_artifact_sha256"]=cfg.orientations_sha256;
      }
      progress<<plan.dump(2)<<'\n'; return co::Ok();
    }
    auto recipe=method_recipe(cfg);
    for (const auto& role:roles) recipe["role_manifest_sha256"][role.name]=role.sha;
    if (validation_only) {
      recipe["role_manifest_sha256"]["train"]=cfg.train_sha256;
      recipe["run_mode"]="validation-only-frozen-TRAIN";
      recipe["train_recipe_sha256"]=recovered.recipe_sha;
      recipe["orientations_artifact_sha256"]=cfg.orientations_sha256;
      recipe["frozen_train_recipe"]=recovered.recipe;
    }
    ATX_TRY(auto recipe_sha,co::sha256_hex(recipe.dump()));
    std::error_code ec;
    if (!std::filesystem::create_directory(cfg.output_directory,ec))
      return co::Err(co::ErrorCode::AlreadyExists,"IC runner: output directory must be new; "+ec.message());
    const auto dir=std::filesystem::path(cfg.output_directory);
    ATX_TRY_VOID(write_json(dir/"recipe.json",recipe));
    Json report{{"status","running"},{"recipe_sha256",recipe_sha},{"roles",Json::array()},
        {"train_candidates_planned",validation_only?usize{0}:lib.candidates.size()},{"full_book_evaluations",0}};
    std::vector<int> signs; Json orientations=Json::array();
    if (validation_only) {
      signs=std::move(recovered.signs);
      report["run_mode"]="validation-only-frozen-TRAIN";
      report["train_manifest_sha256"]=cfg.train_sha256;
      report["train_recipe_sha256"]=recovered.recipe_sha;
      report["orientations_artifact_sha256"]=cfg.orientations_sha256;
      // A receipt of the verified source, not a newly fitted orientation file.
      ATX_TRY_VOID(write_json(dir/"frozen_train_receipt.json",{{"source_artifact_sha256",cfg.orientations_sha256},
          {"train_recipe_sha256",recovered.recipe_sha},{"artifact",recovered.artifact}}));
    }
    ATX_TRY_VOID(write_json(dir/"summary.json",report));
    for (const auto& role:roles) {
      auto scored=score_role(cfg,lib,role,signs,orientations,progress);
      if (!scored) {
        report["status"]="failed"; report["error"]=scored.error().to_string();
        ATX_TRY_VOID(write_json(dir/"summary.json",report)); return co::Err(scored.error());
      }
      report["roles"].push_back(std::move(*scored));
      if (role.name=="train") {
        Json fitted{{"schema","atx.dsl-ic-orientations/v1"},{"recipe_sha256",recipe_sha},
            {"library_sha256",cfg.library_sha256},{"train_manifest_sha256",cfg.train_sha256},
            {"candidates",orientations}};
        ATX_TRY(auto sha,co::sha256_hex(fitted.dump())); report["orientation_recipe_sha256"]=sha;
        ATX_TRY_VOID(write_json(dir/"orientations.json",fitted));
        ATX_TRY(auto artifact_sha,co::sha256_file((dir/"orientations.json").string()));
        report["orientations_artifact_sha256"]=artifact_sha;
      }
      ATX_TRY_VOID(write_json(dir/"summary.json",report));
    }
    report["status"]="complete";
    return write_json(dir/"summary.json",report);
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::Unavailable,"IC runner: allocation within admitted envelope failed");
  } catch (const std::exception& e) {
    return co::Err(co::ErrorCode::InvalidArgument,std::string("IC runner: ")+e.what());
  }
}
int dispatch_ic(int argc,char** argv,std::ostream& out,std::ostream& err) {
  IcRunnerConfig cfg;
  try {
    for (int i=1;i<argc;++i) {
      const std::string key=argv[i];
      if (key=="--plan-only") { cfg.plan_only=true; continue; }
      if (key=="--help") {
        out<<"equity-strategy-ic --library JSON --library-sha256 SHA --train MANIFEST --train-sha256 SHA --output NEWDIR "
               "[--validation MANIFEST --validation-sha256 SHA --max-memory-mib N --min-names N --min-dates N --workers 1..4 --plan-only] [--orientations TRAIN_ARTIFACT --orientations-sha256 SHA]\n";
        return 0;
      }
      if (++i>=argc) throw std::invalid_argument("missing option value");
      const std::string value=argv[i];
      const auto integer=[&]()->u64 {
        if (value.empty() || value.front()=='-') throw std::invalid_argument("unsigned integer required");
        usize used{}; const auto v=std::stoull(value,&used);
        if (used!=value.size()) throw std::invalid_argument("invalid integer"); return v;
      };
      if (key=="--library") cfg.library_path=value;
      else if (key=="--library-sha256") cfg.library_sha256=value;
      else if (key=="--train") cfg.train_manifest=value;
      else if (key=="--train-sha256") cfg.train_sha256=value;
      else if (key=="--validation") cfg.validation_manifest=value;
      else if (key=="--validation-sha256") cfg.validation_sha256=value;
      else if (key=="--orientations") cfg.orientations_path=value;
      else if (key=="--orientations-sha256") cfg.orientations_sha256=value;
      else if (key=="--output") cfg.output_directory=value;
      else if (key=="--max-memory-mib") { const auto n=integer(); if (n>16384) throw std::invalid_argument("memory limit"); cfg.max_working_bytes=n<<20; }
      else if (key=="--min-names") cfg.min_names=static_cast<usize>(integer());
      else if (key=="--min-dates") cfg.min_dates=static_cast<usize>(integer());
      else if (key=="--workers") cfg.workers=static_cast<usize>(integer());
      else throw std::invalid_argument("unknown option: "+key);
    }
    const auto result=run_ic(cfg,out);
    if (!result) { err<<result.error().to_string()<<'\n'; return 1; }
    return 0;
  } catch (const std::exception& e) { err<<e.what()<<'\n'; return 2; }
}
} // namespace atx::impl::strategy
