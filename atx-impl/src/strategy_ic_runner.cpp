#include "strategy_ic_runner.hpp"
#include "strategy_ic_composition.hpp"
#include "strategy_ic_detail.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <ostream>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/data/strategy_data.hpp"
#include "atx/engine/factory/ic_research.hpp"
#include "atx/engine/parallel/det_pool.hpp"

namespace atx::impl::strategy {
// The IC runner's shared state and helpers (strategy_ic_detail.hpp).
using namespace ic_detail;
namespace {
co::Status write_json(const std::filesystem::path& path,const Json& j) {
  std::ofstream out(path,std::ios::binary); if (!out) return co::Err(co::ErrorCode::IoError,"IC runner: JSON output");
  out<<j.dump(2)<<'\n'; out.close();
  return out?co::Ok():co::Status(co::Err(co::ErrorCode::IoError,"IC runner: JSON final close"));
}
co::Result<Json> binary_receipt(const std::filesystem::path& path,u64 bytes) {
  ATX_TRY(auto sha,co::sha256_file(path.string()));
  return co::Ok(Json{{"bytes",bytes},{"sha256",sha}});
}
co::Result<Json> save_bytes(const std::filesystem::path& path,std::span<const std::byte> bytes) {
  std::ofstream out(path,std::ios::binary);
  if (!out) return co::Err(co::ErrorCode::IoError,"IC runner: combined payload output");
  constexpr usize chunk=1U<<20;
  for (usize offset=0;offset<bytes.size();) {
    const auto count=std::min(chunk,bytes.size()-offset);
    out.write(reinterpret_cast<const char*>(bytes.data()+offset),static_cast<std::streamsize>(count));
    if (!out) return co::Err(co::ErrorCode::IoError,"IC runner: combined payload write");
    offset+=count;
  }
  out.close(); if (!out) return co::Err(co::ErrorCode::IoError,"IC runner: combined payload close");
  return binary_receipt(path,bytes.size());
}
co::Result<Json> save_combined_artifact(const IcRunnerConfig& cfg,const Role& spec,
    const engine::data::StrategyRoleData& role,std::span<const f64> signal,std::span<const u8> member,
    const Json& orientations,const std::string& recipe_sha,const std::string& orientation_pin,bool pinned_signs,
    bool themed=false) {
  if constexpr (std::endian::native!=std::endian::little)
    return co::Err(co::ErrorCode::Unavailable,"IC runner: combined artifact requires little-endian host");
  const auto cells=role.panel.dates()*role.panel.instruments();
  if (signal.size()!=cells || member.size()!=cells || orientations.empty())
    return co::Err(co::ErrorCode::Internal,"IC runner: combined artifact geometry/orientations");
  const auto dir=std::filesystem::path(cfg.output_directory); const auto prefix=spec.name+"_combined";
  Json files;
  const auto store=[&](const std::string& suffix,std::span<const std::byte> bytes)->co::Status {
    const auto name=prefix+suffix; ATX_TRY(auto receipt,save_bytes(dir/name,bytes));
    files[name]=std::move(receipt); return co::Ok();
  };
  ATX_TRY_VOID(store(".f64",std::as_bytes(signal)));
  ATX_TRY_VOID(store("_member.u8",std::as_bytes(member)));
  ATX_TRY_VOID(store("_sessions.i64",std::as_bytes(std::span<const i64>(role.session_keys))));
  ATX_TRY_VOID(store("_ids.u64",std::as_bytes(std::span<const u64>(role.instrument_ids))));
  // Fixed-size scratch only; do not duplicate the dense signal or its masks.
  const auto finite_name=prefix+"_finite.u8";
  std::ofstream finite(dir/finite_name,std::ios::binary);
  if (!finite) return co::Err(co::ErrorCode::IoError,"IC runner: combined finite-mask output");
  std::array<u8,65536> chunk{}; u64 finite_cells=0,member_cells=0;
  for (usize offset=0;offset<cells;) {
    const auto count=std::min(chunk.size(),cells-offset);
    for (usize j=0;j<count;++j) {
      const auto k=offset+j;
      chunk[j]=static_cast<u8>(std::isfinite(signal[k]));
      if (member[k]>1 || (!member[k] && chunk[j]))
        return co::Err(co::ErrorCode::Internal,"IC runner: combined support invariant");
      finite_cells+=chunk[j]; member_cells+=member[k];
    }
    finite.write(reinterpret_cast<const char*>(chunk.data()),static_cast<std::streamsize>(count));
    if (!finite) return co::Err(co::ErrorCode::IoError,"IC runner: combined finite-mask write");
    offset+=count;
  }
  finite.close(); if (!finite) return co::Err(co::ErrorCode::IoError,"IC runner: combined finite-mask close");
  ATX_TRY(auto finite_file,binary_receipt(dir/finite_name,cells)); files[finite_name]=std::move(finite_file);
  ATX_TRY(auto orientation_sha,co::sha256_hex(orientations.dump()));
  const bool pinned=!cfg.composition_weights_sha256.empty();
  Json manifest{{"schema","atx.dsl-combined-signal/v1"},{"status","complete"},{"role",spec.name},
      {"layout","date-major-little-endian"},{"dates",role.panel.dates()},{"instruments",role.panel.instruments()},
      {"score_begin",role.score_begin},{"score_end",role.score_end},{"role_manifest_sha256",spec.sha},
      {"source_sha256",role.source_sha256},{"library_sha256",cfg.library_sha256},
      {"train_manifest_sha256",cfg.train_sha256},{"run_recipe_sha256",recipe_sha},
      {"orientation_candidates_sha256",orientation_sha},
      {"orientations_artifact_sha256",orientation_pin.empty()?Json(nullptr):Json(orientation_pin)},
      {"signal_semantics",pinned
          ?"exact-pre-target-composition;pinned-candidate-weights;missing-or-unoriented-neutral-fixed-denominator"
          :"exact-pre-target-composition;equal-family/equal-within;missing-or-unoriented-neutral-fixed-denominator"},
      {"member_semantics","decision-member-and-source-present-and-finite-positive-close;independent-of-component-coverage"},
      {"finite_semantics","one-iff-saved-f64-is-finite;nonmembers-NaN;zero-is-valid-neutral-signal"},
      {"axes_semantics","exact-ordered-role-sessions-and-instrument-IDs;no-static-broadcast"},
      {"role_window_required",true},{"finite_cells",finite_cells},{"member_cells",member_cells},
      {"files",std::move(files)},{"actual_trades_or_returns",false}};
  // Key absent (not null) without weights: the default manifest bytes are unchanged.
  if (pinned) manifest["composition_weights_sha256"]=cfg.composition_weights_sha256;
  // Signs from that same pinned file; signal_semantics is unchanged so replay
  // consumers still admit the blend, and this key states which signs were used.
  if (pinned_signs) manifest["composition_signs"]="pinned-candidate-signs";
  // Same precedent for ew-theme-v6: the blend redistributes a missing member's mass
  // inside its theme (per name and date); signal_semantics stays admissible to the
  // replay consumers and this key (absent otherwise) states the redistribution.
  if (themed) manifest["composition_redistribution"]=theme_redistribution_rule;
  // Likewise absent unless a fields manifest is pinned for this role.
  if (!spec.fields.sha.empty()) manifest["research_fields_manifest_sha256"]=spec.fields.sha;
  const auto name=prefix+".json"; ATX_TRY_VOID(write_json(dir/name,manifest));
  ATX_TRY(auto pin,co::sha256_file((dir/name).string()));
  return co::Ok(Json{{"manifest",name},{"manifest_sha256",pin},{"orientation_candidates_sha256",orientation_sha}});
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
            const IcSeries& daily,int sign) {
  const std::array<usize,3> horizons{5,21,63};
  for (usize k=0;k<horizons.size();++k) {
    const auto p=daily.pearson[k],r=daily.rank[k];
    for (usize row=0;row<r.size();++row) {
      const auto d=role.score_begin+row;
      out<<id<<','<<horizons[k]<<','<<d<<','<<role.session_keys[d]<<',';
      if (std::isfinite(p[row])) out<<p[row]; out<<',';
      if (std::isfinite(r[row])) out<<r[row]; out<<',';
      if (sign!=0 && std::isfinite(r[row])) out<<sign*r[row]; out<<'\n';
    }
  }
}
// entry: the cache entry now holding the exact signal bytes in `buffer` (the
// verified hit, or the v2 entry just committed); nullopt when the cache is off.
struct SignalTiming { f64 vm{},cache_load{},cache_write{}; bool hit{}; std::optional<CacheHit> entry; };
// Leaves the candidate's raw (unoriented) signal in `buffer`: a verified cache
// hit (`hit`, resolved at preflight), or a VM evaluation that is then committed
// to the cache when enabled. Only a miss asks `fields` for the panel (loading
// candidate k's extras if not resident): the role panel, or the borrowed DSL
// panel over the resident extras. Every Engine borrows it; `fields` destroys
// `vm` before changing it. Null cache key = cache off.
co::Result<SignalTiming> candidate_signal(const IcRunnerConfig& cfg,const Role& spec,
    const engine::data::StrategyRoleData& role,FieldResidency& fields,usize k,const Candidate& candidate,
    const CacheKey* signal_cache,const std::optional<CacheHit>& hit,engine::parallel::DetPool* pool,
    std::unique_ptr<al::Engine>& vm,std::vector<f64>& buffer,HashMeter& meter,std::ostream& progress) {
  using steady=std::chrono::steady_clock;
  const auto since=[](steady::time_point from) { return std::chrono::duration<f64>(steady::now()-from).count(); };
  SignalTiming out;
  if (signal_cache && hit) {
    const auto load_started=steady::now();
    ATX_TRY_VOID(cache_load(*hit,role.panel.dates()*role.panel.instruments(),buffer,meter));
    out.cache_load=since(load_started); out.hit=true; out.entry=*hit;
    progress<<"IC cache-hit "<<candidate.id<<" role="<<spec.name<<" layout="<<(hit->legacy?"v1":"v2")
            <<" seconds="<<out.cache_load<<'\n'<<std::flush;
    return co::Ok(std::move(out));
  }
  if (signal_cache) progress<<"IC cache-miss "<<candidate.id<<" role="<<spec.name<<'\n'<<std::flush;
  // At most one full-panel candidate signal exists: drop the previous one
  // before any VM arena growth or evaluation allocates.
  release(buffer);
  ATX_TRY(auto panel,fields.panel_for(k,role.panel,candidate.id,vm,progress));
  const auto vm_started=steady::now();
  if (!vm || candidate.program.num_slots>vm->pool_capacity()) {
    const auto previous_slots=vm?vm->pool_capacity():0;
    // The preceding candidate's signal is already released. Free the old
    // full-panel arena BEFORE Engine::evaluate allocates a larger one;
    // Engine's own ensure_pool otherwise retains both during construction.
    vm.reset();
    vm=std::make_unique<al::Engine>(*panel);
    vm->set_eval_mode(al::EvalMode::ResearchFast);
    if (pool) { vm->set_cs_pool(pool); vm->set_ts_pool(pool); }
    ATX_TRY_VOID(vm->set_cross_section_mask(role.decision_member));
    progress<<"IC VM-arena previous_slots="<<previous_slots
            <<" requested_slots="<<candidate.program.num_slots<<" release_before_growth=true\n"<<std::flush;
  }
  vm->reset();
  ATX_TRY(auto evaluated,vm->evaluate(candidate.program));
  out.vm=since(vm_started);
  progress<<"IC VM-complete "<<candidate.id<<" seconds="<<out.vm<<'\n'<<std::flush;
  if (evaluated.alphas.size()!=1) return co::Err(co::ErrorCode::Internal,"IC runner: VM root missing");
  buffer=std::move(evaluated.alphas.front().values);
  if (signal_cache) {
    const auto write_started=steady::now();
    ATX_TRY(auto stored,cache_store(*signal_cache,cfg,spec,role,candidate,buffer,meter));
    out.entry=std::move(stored); out.cache_write=since(write_started);
    progress<<"IC cache-write "<<candidate.id<<" role="<<spec.name<<" seconds="<<out.cache_write<<'\n'<<std::flush;
  }
  return co::Ok(std::move(out));
}
// `blend_signs`: pinned per-candidate blend signs (empty = the TRAIN IC orientation).
// `themes`: pinned within-theme redistribution themes (empty = none; ew-theme-v6).
co::Result<Json> score_role(const IcRunnerConfig& cfg,const Library& lib,const Role& spec,
    const KnownManifests& known,std::span<const f64> weights,std::span<const int> blend_signs,
    std::vector<int>& signs,Json& frozen,const std::string& recipe_sha,const std::string& orientation_pin,
    std::ostream& progress,std::span<const usize> themes={}) {
  const auto started=std::chrono::steady_clock::now();
  progress<<"IC loading "<<spec.name<<" admitted_bytes="<<spec.bytes<<'\n'<<std::flush;
  ATX_TRY_VOID(fields_bound(lib,spec));
  // Fail fast (T1 review M2): every candidate's entry is resolved once (v2, or v1
  // read in place), identity-checked and its payload extent stat'ed before the
  // role loads, so a foreign or truncated entry refuses now, not hours into the
  // loop. Payload hashes are still verified on load.
  CacheResolution cache;
  if (!cfg.candidate_cache_directory.empty()) {
    ATX_TRY(cache,cache_resolve(cfg,lib,spec,known));
    progress<<"IC cache-preflight role="<<spec.name<<" ready="<<cache.ready<<'/'<<lib.candidates.size()
            <<" legacy="<<cache.legacy<<'\n'<<std::flush;
  }
  // Referenced extra fields also fail fast: every extent is stat'ed, and the
  // payloads some cache miss will load are hashed now, once (their loads trust
  // that verification). A field only cache hits read is never opened; none
  // referenced: nothing is read.
  const auto verify_started=std::chrono::steady_clock::now();
  ATX_TRY_VOID(check_field_extents(spec));
  FieldMask needed;
  for (usize k=0;k<lib.candidates.size();++k)
    if (cache.hits.empty() || !cache.hits[k]) needed|=lib.field_plan.needs[k];
  HashMeter meter; std::vector<std::optional<FileStamp>> verified(lib.extra_fields.size());
  ATX_TRY_VOID(verify_fields(spec,needed,meter,verified));
  const auto verify_seconds=std::chrono::duration<f64>(std::chrono::steady_clock::now()-verify_started).count();
  if (!spec.fields.load.empty()) {
    progress<<"IC fields-verified role="<<spec.name<<" fields=";
    for (usize k=0;k<spec.fields.load.size();++k) progress<<(k?",":"")<<spec.fields.load[k].name;
    progress<<" resident_capacity="<<lib.field_plan.capacity<<" planned_loads="<<lib.field_plan.loads
            <<" hashed="<<needed.count()<<" seconds="<<verify_seconds<<'\n'<<std::flush;
  }
  const auto role_started=std::chrono::steady_clock::now();
  ATX_TRY(auto role,engine::data::read_strategy_role(spec.path,cfg.max_working_bytes));
  const auto load_seconds=std::chrono::duration<f64>(std::chrono::steady_clock::now()-role_started).count();
  if (role.manifest_sha256!=spec.sha)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: role manifest changed after admission");
  // Declared after `role` and before `pool`/`vm`: borrows the former and is
  // borrowed by the latter. Absent extras, the VM reads role.panel exactly as before.
  FieldResidency fields(lib,spec,role.panel.dates()*role.panel.instruments(),meter,std::move(verified));
  ATX_TRY(auto guard,guard_for(role));
  auto ic=ex::equivalence_ic_screen_config();
  ic.horizons={5,21,63,0}; ic.min_names=cfg.min_names; ic.min_dates=cfg.min_dates;
  ic.window_begin=role.score_begin; ic.window_end=role.score_end; ic.maturity_end=role.score_end;
  ic.max_cache_bytes=cfg.max_working_bytes;
  const ex::ResearchIcOptions ic_options{3,true,cfg.workers};
  const auto label_started=std::chrono::steady_clock::now();
  ATX_TRY(auto labels,ex::prepare_research_ic(role.panel,ic,ic_options,role.decision_member,guard,
      ic_price_field));
  ATX_TRY(auto scratch,ex::prepare_research_ic_scratch(labels));
  if (labels.bytes()>cfg.max_working_bytes || scratch.bytes()>cfg.max_working_bytes-labels.bytes())
    return co::Err(co::ErrorCode::Unavailable,"IC runner: actual IC cache/scratch exceeds admitted budget");
  const auto label_seconds=std::chrono::duration<f64>(std::chrono::steady_clock::now()-label_started).count();
  // Lifetime order matters: Engine borrows the pool and dies first. Candidate
  // evaluation is driven by THIS main thread; Cs and Ts jobs never nest.
  std::unique_ptr<engine::parallel::DetPool> pool;
  if (cfg.workers>1) pool=std::make_unique<engine::parallel::DetPool>(cfg.workers);
  std::unique_ptr<al::Engine> vm;
  // --no-composition (screening) builds no blend: neither the composition nor the
  // effective membership that it and the saved artifact use.
  std::vector<u8> effective; std::optional<IcComposition> composition;
  if (!cfg.no_composition) {
    ATX_TRY(auto close_id,role.panel.field_id("close")); const auto close=role.panel.field_all(close_id);
    effective=role.decision_member;
    for (usize d=0;d<role.panel.dates();++d) for (usize i=0;i<role.panel.instruments();++i) {
      const auto k=d*role.panel.instruments()+i;
      effective[k]=static_cast<u8>(effective[k] && role.panel.in_universe(d,i) && std::isfinite(close[k]) && close[k]>0);
    }
    std::vector<IcCompositionCandidate> candidates; candidates.reserve(lib.candidates.size());
    for (const auto& c:lib.candidates) candidates.push_back({c.id,c.family});
    IcCompositionConfig cc; cc.dates=role.panel.dates(); cc.instruments=role.panel.instruments();
    cc.decision_begin=role.score_begin; cc.decision_end=role.score_end; cc.max_working_bytes=cfg.max_working_bytes;
    ATX_TRY(auto created,IcComposition::create(cc,candidates,effective,weights,themes));
    composition.emplace(std::move(created));
  }
  // One key per candidate (empty = cache off); directories are created on write.
  const bool signal_cache=!cache.keys.empty();
  // IC-result cache: on exactly when the signal cache is (its entries key on the
  // signal payload SHA256); one scope per scored role, entries per candidate dir.
  std::optional<IcCacheScope> ic_scope; usize ic_hits=0;
  if (signal_cache) {
    ATX_TRY(auto scope,ic_cache_scope(spec,role,ic,ic_options,guard,meter));
    ic_scope.emplace(std::move(scope));
  }
  std::vector<f64> signal_buffer; usize cache_hits=0,legacy_hits=0; Json cache_entries=Json::array();
  f64 total_cache_load_seconds=0,total_cache_write_seconds=0;
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
    fields.enter(k,vm,progress);
    // A verified IC-result hit replaces evaluate_research_ic; a miss scores and,
    // with the cache on, commits that result keyed on the exact signal bytes.
    std::optional<CachedIc> cached_ic; std::filesystem::path ic_entry; bool ic_probed=false; f64 probe_seconds=0;
    // --no-composition: nothing but IC reads the signal, and an IC entry is keyed on
    // the payload SHA256 its sidecar records, so a signal hit whose IC result is
    // cached too is never loaded (its bytes were verified when that entry was made).
    if (cfg.no_composition && ic_scope && cache.hits[k]) {
      const auto probe_started=std::chrono::steady_clock::now();
      const auto& hit=*cache.hits[k];
      ic_entry=hit.dir/ic_scope->directory/(hit.stem+".json");
      ATX_TRY(cached_ic,ic_cache_lookup(ic_entry,*ic_scope,candidate,hit.payload_sha,scratch,
          ic_options.active_horizons));
      ic_probed=true;
      probe_seconds=std::chrono::duration<f64>(std::chrono::steady_clock::now()-probe_started).count();
    }
    SignalTiming acquired;
    if (cached_ic) {
      release(signal_buffer); // no stale signal outlives its candidate
      acquired.hit=true; acquired.entry=*cache.hits[k];
      progress<<"IC cache-hit "<<candidate.id<<" role="<<spec.name<<" layout="<<(acquired.entry->legacy?"v1":"v2")
              <<" payload=not-loaded ic_result=hit\n"<<std::flush;
    } else {
      ATX_TRY(acquired,candidate_signal(cfg,spec,role,fields,k,candidate,
          signal_cache?&cache.keys[k]:nullptr,signal_cache?cache.hits[k]:std::nullopt,pool.get(),vm,
          signal_buffer,meter,progress));
    }
    const std::span<const f64> signal(signal_buffer);
    const auto vm_seconds=acquired.vm; total_vm_seconds+=vm_seconds;
    total_cache_load_seconds+=acquired.cache_load; total_cache_write_seconds+=acquired.cache_write;
    cache_hits+=acquired.hit?1U:0U;
    if (acquired.entry) {
      const auto& entry=*acquired.entry; const auto& key=cache.keys[k];
      legacy_hits+=entry.legacy?1U:0U;
      cache_entries.push_back({{"id",candidate.id},{"layout",entry.legacy?"v1":"v2"},
          {"sidecar",(entry.dir/(entry.stem+".json")).string()},{"payload",(entry.dir/(entry.stem+".f64")).string()},
          {"payload_sha256",entry.payload_sha},{"field_payload_sha256",fields_json(key.fields)},
          {"signal_key_sha256",key.signal_key}});
    }
    const auto ic_started=std::chrono::steady_clock::now();
    if (ic_scope) {
      if (!acquired.entry || !hash_valid(acquired.entry->payload_sha))
        return co::Err(co::ErrorCode::Internal,
            "IC runner: signal payload SHA256 missing for IC-result cache");
      // Beside the signal entry that served or stored the bytes: v1 <id>.json, v2 <id>.<dsl16>.json.
      if (!ic_probed) {
        ic_entry=acquired.entry->dir/ic_scope->directory/(acquired.entry->stem+".json");
        ATX_TRY(cached_ic,ic_cache_lookup(ic_entry,*ic_scope,candidate,acquired.entry->payload_sha,scratch,
            ic_options.active_horizons));
      }
    }
    ex::ResearchIcResult scored; IcSeries daily_series;
    if (cached_ic) {
      scored=cached_ic->result; daily_series=cached_series(*cached_ic); ++ic_hits;
    } else {
      ATX_TRY(scored,ex::evaluate_research_ic(signal,labels,scratch,pool.get()));
      daily_series=scratch_series(scratch);
      if (ic_scope) {
        ATX_TRY_VOID(ic_cache_store(ic_entry,*ic_scope,candidate,acquired.entry->payload_sha,scored,
            daily_series,cfg.workers));
      }
    }
    const auto ic_seconds=probe_seconds+
        std::chrono::duration<f64>(std::chrono::steady_clock::now()-ic_started).count();
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
    // Pinned signs orient the blend only; every IC diagnostic keeps `sign`.
    const int blend_sign=blend_signs.empty()?sign:blend_signs[k];
    f64 composition_seconds=0;
    if (composition) {
      const auto composition_started=std::chrono::steady_clock::now();
      ATX_TRY_VOID(composition->add(k,signal,blend_sign,pool.get()));
      composition_seconds=std::chrono::duration<f64>(std::chrono::steady_clock::now()-composition_started).count();
      total_composition_seconds+=composition_seconds;
    }
    series(daily,candidate.id,role,daily_series,sign);
    auto summary=result_json(scored,sign);
    summary["id"]=candidate.id; summary["family"]=candidate.family;
    summary["frozen_train_sign"]=sign; summary["status"]="complete";
    // Present only with pinned weights: the applied blend weight/sign (NR input).
    if (!weights.empty()) summary["composition_weight"]=weights[k];
    if (!blend_signs.empty()) summary["composition_sign"]=blend_sign;
    const auto candidate_seconds=std::chrono::duration<f64>(
        std::chrono::steady_clock::now()-candidate_started).count();
    summary["wall_seconds"]=candidate_seconds;
    summary["stage_seconds"]={{"vm",vm_seconds},{"ic",ic_seconds}};
    if (composition) summary["stage_seconds"]["composition"]=composition_seconds;
    if (signal_cache) {
      summary["stage_seconds"]["cache_load"]=acquired.cache_load;
      summary["stage_seconds"]["cache_write"]=acquired.cache_write;
      summary["signal_cache"]=acquired.hit?"hit":"miss";
      summary["ic_result_cache"]=cached_ic?"hit":"miss";
    }
    summaries.push_back(summary); ledger<<summary.dump()<<'\n'<<std::flush;
    if (!daily || !ledger) return co::Err(co::ErrorCode::IoError,"IC runner: candidate output");
    progress<<"IC "<<spec.name<<' '<<(k+1)<<'/'<<lib.candidates.size()<<' '<<candidate.id
            <<" seconds="<<candidate_seconds<<" vm="<<vm_seconds<<" ic="<<ic_seconds
            <<" composition="<<composition_seconds<<" sign="<<sign<<" reason="<<ex::ic_screen_reason_name(scored.screen.reason);
    if (signal_cache)
      progress<<" cache="<<(acquired.hit?"hit":"miss")<<" ic_result="<<(cached_ic?"hit":"miss");
    progress<<'\n'<<std::flush;
  }
  // Composition owns its accumulated blend; it does not borrow VM slots or any
  // discarded candidate output. Combined IC/save need only the shared pool.
  // The extra columns are dropped too, VM first (it borrows them).
  fields.drop_all(vm); release(signal_buffer);
  // --no-composition stops at the member rows: no blend, `__combined__` rows,
  // planned targets or saved artifact.
  std::optional<IcCompositionResult> combined; Json combined_ic_json;
  if (composition) {
    const auto finish_started=std::chrono::steady_clock::now();
    ATX_TRY(combined,composition->finish());
    total_composition_seconds+=std::chrono::duration<f64>(std::chrono::steady_clock::now()-finish_started).count();
    const auto combined_ic_started=std::chrono::steady_clock::now();
    ATX_TRY(auto combined_ic,ex::evaluate_research_ic(combined->signal,labels,scratch,pool.get()));
    total_ic_seconds+=std::chrono::duration<f64>(std::chrono::steady_clock::now()-combined_ic_started).count();
    series(daily,"__combined__",role,scratch_series(scratch),1);
    combined_ic_json=result_json(combined_ic,1);
    std::ofstream targets(dir/(spec.name+"_planned_targets.csv"),std::ios::binary);
    if (!targets) return co::Err(co::ErrorCode::IoError,"IC runner: target proxy output");
    targets.imbue(std::locale::classic()); targets<<std::setprecision(17);
    targets<<"decision_index,session_ns,planned_turnover,planned_gross,planned_net,contribution_fraction,eligible_names\n";
    for (usize d=role.score_begin;d<role.score_end;++d)
      targets<<d<<','<<role.session_keys[d]<<','<<combined->planned_turnover[d]<<','
             <<combined->planned_gross[d]<<','<<combined->planned_net[d]<<','
             <<combined->contribution_fraction[d]<<','<<combined->eligible_names[d]<<'\n';
    targets.close();
    if (!targets) return co::Err(co::ErrorCode::IoError,"IC runner: final output close");
  }
  daily.close(); ledger.close();
  if (!daily || !ledger) return co::Err(co::ErrorCode::IoError,"IC runner: final output close");
  Json saved; f64 save_seconds=0;
  const bool save=combined && cfg.save_combined;
  if (save) {
    const auto save_started=std::chrono::steady_clock::now();
    ATX_TRY(saved,save_combined_artifact(cfg,spec,role,combined->signal,effective,frozen,recipe_sha,orientation_pin,
        !blend_signs.empty(),!themes.empty()));
    save_seconds=std::chrono::duration<f64>(std::chrono::steady_clock::now()-save_started).count();
  }
  const auto seconds=std::chrono::duration<f64>(std::chrono::steady_clock::now()-started).count();
  Json result{{"role",spec.name},{"manifest_sha256",spec.sha},{"source_sha256",role.source_sha256},
      {"dates",role.panel.dates()},{"instruments",role.panel.instruments()},
      {"score_begin",role.score_begin},{"score_end",role.score_end}, {"wall_seconds",seconds},
      {"admitted_working_bytes",spec.bytes},{"ic_cache_bytes",labels.bytes()},{"ic_scratch_bytes",scratch.bytes()},
      {"workers",cfg.workers},{"stage_seconds",{{"load",load_seconds},{"label_preparation",label_seconds},
          {"vm",total_vm_seconds},{"ic",total_ic_seconds}}},
      {"candidate_evaluations",lib.candidates.size()},{"combined_evaluations",combined?1:0},
      {"candidates",std::move(summaries)}};
  // Absent under --no-composition (the summary's top-level "composition": "skipped").
  if (combined) {
    result["stage_seconds"]["composition"]=total_composition_seconds;
    result["combined_ic"]=std::move(combined_ic_json);
    result["planned_target_proxy"]={{"total_turnover",combined->total_planned_turnover},
        {"deployment_turnover",combined->deployment_turnover},{"deployment_date",combined->deployment_date},
        {"initial_deployment_included",true},{"actual_trades_or_costs",false}};
  }
  if (save) {
    result["combined_artifact"]=std::move(saved);
    result["stage_seconds"]["save_combined"]=save_seconds;
  }
  if (signal_cache) {
    result["stage_seconds"]["cache_load"]=total_cache_load_seconds;
    result["stage_seconds"]["cache_write"]=total_cache_write_seconds;
    // `directory`/`fields_directory` are the v1 locations (kept for v1 readers);
    // `entries` names each candidate's exact entry in either layout.
    result["candidate_cache"]={
        {"directory",(cache_root(cfg)/spec.sha).string()},{"vm_identity",vm_identity()},
        {"layout",cache_schema_v2},{"hits",cache_hits},{"legacy_hits",legacy_hits},
        {"misses",lib.candidates.size()-cache_hits},{"vm_evaluations",lib.candidates.size()-cache_hits},
        {"entries",std::move(cache_entries)}};
    if (!lib.extra_fields.empty())
      result["candidate_cache"]["fields_directory"]=(cache_root(cfg)/spec.fields.sha).string();
    // Entries live beside each signal entry: <entry dir>/<subdirectory>/<entry stem>.json.
    result["candidate_cache"]["ic_results"]={{"subdirectory",ic_scope->directory},
        {"identity",ic_identity()},{"key",ic_scope->key},{"hits",ic_hits},
        {"misses",lib.candidates.size()-ic_hits}};
  }
  // SHA-256 spent by this role (field and cache payload verification, cache
  // writes, IC-result scope) and the pinned payload bytes hashed to verify them.
  result["hash_seconds"]=meter.seconds; result["verify_bytes"]=meter.verify_bytes;
  // Present exactly when a fields manifest is pinned for this role.
  if (!spec.fields.sha.empty()) {
    Json loaded=Json::array(),files=Json::object();
    for (const auto& field:spec.fields.load) {
      loaded.push_back(field.name); files[field.name]={{"bytes",field.bytes},{"sha256",field.sha}};
    }
    result["research_fields"]={{"manifest_sha256",spec.fields.sha},{"directory",spec.fields.directory},
        {"loaded",std::move(loaded)},{"files",std::move(files)},
        {"resident_capacity",lib.field_plan.capacity},{"planned_loads",lib.field_plan.loads},
        {"field_loads",fields.loads()},{"peak_resident_fields",fields.peak()},
        {"loaded_bytes",static_cast<u64>(fields.loads())*role.panel.dates()*role.panel.instruments()*sizeof(f64)}};
    result["stage_seconds"]["fields_verify"]=verify_seconds;
    result["stage_seconds"]["fields_load"]=fields.seconds();
  }
  return co::Ok(std::move(result));
}
} // namespace
co::Status run_ic(const IcRunnerConfig& cfg,std::ostream& progress) {
  try {
    const bool validation_only=!cfg.orientations_path.empty();
    const bool metadata_only=cfg.plan_only || cfg.cache_report;
    if ((!metadata_only && cfg.output_directory.empty()) || cfg.max_working_bytes<(32ULL<<20) || cfg.max_working_bytes>(16ULL<<30) ||
        cfg.min_names<3 || cfg.min_dates<8 || cfg.min_dates>4096 || cfg.workers<1 ||
        cfg.workers>ex::max_research_ic_workers ||
        cfg.validation_manifest.empty()!=cfg.validation_sha256.empty() ||
        cfg.orientations_path.empty()!=cfg.orientations_sha256.empty() ||
        cfg.composition_weights_path.empty()!=cfg.composition_weights_sha256.empty() ||
        cfg.train_fields_directory.empty()!=cfg.train_fields_sha256.empty() ||
        cfg.validation_fields_directory.empty()!=cfg.validation_fields_sha256.empty() ||
        (!cfg.validation_fields_directory.empty() && cfg.validation_manifest.empty()) ||
        (validation_only && cfg.validation_manifest.empty()) || (cfg.plan_only && cfg.cache_report) ||
        (cfg.candidate_cache_directory.empty() && (cfg.cache_report || !cfg.candidate_cache_legacy_fields.empty())))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: bounded config");
    if (cfg.no_composition && !cfg.composition_weights_path.empty())
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: --no-composition builds no blend, so "
          "--composition-weights (which only weight the blend) is refused with it");
    ATX_TRY(auto lib,library(cfg));
    // Both new options are fully validated here, before any role payload.
    ATX_TRY(const auto pinned,composition_weights(cfg,lib));
    const bool pinned_signs=!pinned.signs.empty();
    ATX_TRY_VOID(cache_preflight(cfg,lib));
    ATX_TRY(const auto known,legacy_manifests(cfg));
    std::vector<Role> roles;
    // Fields bind at admission (metadata only); an unscored frozen TRAIN needs none.
    ATX_TRY(auto train,admit(cfg,lib,cfg.train_manifest,cfg.train_sha256,"train",!validation_only,
        pinned.theme_count));
    ATX_TRY_VOID(bind_fields(lib,train,cfg.train_fields_directory,cfg.train_fields_sha256,!validation_only));
    roles.push_back(std::move(train));
    if (!cfg.validation_manifest.empty()) {
      ATX_TRY(auto val,admit(cfg,lib,cfg.validation_manifest,cfg.validation_sha256,"validation",true,
          pinned.theme_count));
      ATX_TRY_VOID(bind_fields(lib,val,cfg.validation_fields_directory,cfg.validation_fields_sha256,true));
      ATX_TRY_VOID(same_field_definitions(lib,roles.front(),val));
      if (roles.front().metadata.at("score_end_ns").get<i64>()>val.metadata.at("score_start_ns").get<i64>())
        return co::Err(co::ErrorCode::InvalidArgument,"IC runner: overlapping/nonchronological roles");
      roles.push_back(std::move(val));
    }
    FrozenTrain recovered; std::string definitions_check;
    // Summary record of the applied weights (null: none pinned).
    Json weights_record;
    if (validation_only) {
      ATX_TRY(recovered,frozen_train(cfg,lib,roles.front()));
      if (!cfg.composition_weights_path.empty()) {
        ATX_TRY(weights_record,frozen_weights_binding(cfg,pinned,recovered));
      }
      ATX_TRY(definitions_check,frozen_field_definitions(lib,recovered,roles.front(),roles.back()));
      roles.erase(roles.begin()); // TRAIN metadata checked, payload never opened.
    } else if (!cfg.composition_weights_path.empty()) {
      weights_record=weights_summary(cfg,pinned,"train-manifest-sha256;TRAIN-scored-in-this-run");
    }
    if (cfg.cache_report) {
      ATX_TRY(auto listing,cache_report(cfg,lib,roles,known));
      progress<<listing.dump(2)<<'\n'; return co::Ok();
    }
    if (cfg.plan_only) {
      // `candidates`: contract K1 rows (candidate_plan_rows); the count is candidate_count.
      Json plan{{"mode","metadata-only-no-payload"},{"candidate_count",lib.candidates.size()},
          {"candidates",candidate_plan_rows(lib)},{"max_working_bytes",cfg.max_working_bytes},
          {"max_compiled_slots",lib.max_slots},{"required_lookback",lib.lookback},{"workers",cfg.workers},
          {"library_sha256",cfg.library_sha256},{"roles",Json::array()}};
      for (const auto& role:roles) plan["roles"].push_back({{"role",role.name},
          {"manifest_sha256",role.sha},{"required_bytes",role.bytes}});
      if (validation_only) {
        plan["run_mode"]="validation-only-frozen-TRAIN";
        plan["train_recipe_sha256"]=recovered.recipe_sha;
        plan["orientations_artifact_sha256"]=cfg.orientations_sha256;
      }
      if (!cfg.composition_weights_sha256.empty()) plan["composition_weights_sha256"]=cfg.composition_weights_sha256;
      if (pinned_signs) plan["composition_signs"]="pinned-candidate-signs";
      if (!weights_record.is_null()) plan["composition_weights"]=weights_record;
      if (!definitions_check.empty()) plan["research_field_definitions_checked_against"]=definitions_check;
      if (cfg.no_composition) plan["composition"]="skipped";
      if (fields_pinned(cfg)) {
        Json bound=Json::array();
        for (const auto& role:roles) if (!role.fields.sha.empty()) {
          Json loaded=Json::array();
          for (const auto& field:role.fields.load) loaded.push_back(field.name);
          bound.push_back({{"role",role.name},{"manifest_sha256",role.fields.sha},
              {"directory",role.fields.directory},{"loaded",std::move(loaded)}});
        }
        plan["research_fields"]={{"loaded",lib.extra_fields},{"declared",lib.declared_extra},
            {"resident_capacity",lib.field_plan.capacity},{"planned_loads",lib.field_plan.loads},
            {"roles",std::move(bound)}};
      }
      if (!cfg.candidate_cache_directory.empty()) {
        plan["candidate_cache"]=Json::array();
        for (const auto& role:roles) {
          ATX_TRY(auto entry,cache_plan(cfg,lib,role,known)); plan["candidate_cache"].push_back(std::move(entry));
        }
      }
      progress<<plan.dump(2)<<'\n'; return co::Ok();
    }
    auto recipe=method_recipe(cfg,true,pinned_signs,!pinned.themes.empty());
    for (const auto& role:roles) recipe["role_manifest_sha256"][role.name]=role.sha;
    if (fields_pinned(cfg)) recipe["research_fields"]=fields_recipe(fields_pins(cfg),lib);
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
    if (!cfg.composition_weights_sha256.empty()) report["composition_weights_sha256"]=cfg.composition_weights_sha256;
    if (pinned_signs) report["composition_signs"]="pinned-candidate-signs";
    if (!weights_record.is_null()) report["composition_weights"]=weights_record;
    if (!definitions_check.empty()) report["research_field_definitions_checked_against"]=definitions_check;
    if (recipe.contains("research_fields")) report["research_fields"]=recipe.at("research_fields");
    // Not a method input (recipe unchanged): the pass scored IC and orientations only.
    if (cfg.no_composition) report["composition"]="skipped";
    std::vector<int> signs; Json orientations=Json::array();
    if (validation_only) {
      signs=std::move(recovered.signs);
      orientations=recovered.artifact.at("candidates");
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
      auto scored=score_role(cfg,lib,role,known,pinned.values,pinned.signs,signs,orientations,recipe_sha,
          report.value("orientations_artifact_sha256",std::string{}),progress,pinned.themes);
      if (!scored) {
        report["status"]="failed"; report["error"]=scored.error().to_string();
        ATX_TRY_VOID(write_json(dir/"summary.json",report)); return co::Err(scored.error());
      }
      report["roles"].push_back(std::move(*scored));
      if (role.name=="train") {
        Json fitted{{"schema","atx.dsl-ic-orientations/v1"},{"recipe_sha256",recipe_sha},
            {"library_sha256",cfg.library_sha256},{"train_manifest_sha256",cfg.train_sha256},
            {"candidates",orientations}};
        // Review N3: the TRAIN field definitions travel with the frozen artifact, so a
        // validation-only run checks its manifest against them without --train-fields.
        // Only with declared extras and a pinned TRAIN manifest (otherwise unchanged).
        if (!lib.declared_extra.empty() && !role.fields.sha.empty()) {
          Json definitions=Json::object();
          for (const auto& name:lib.declared_extra) definitions[name]=role.fields.declared.at(name);
          fitted["research_fields"]={{"manifest_sha256",role.fields.sha},{"definitions",std::move(definitions)}};
        }
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
      if (key=="--cache-report") { cfg.cache_report=true; continue; }
      if (key=="--save-combined") { cfg.save_combined=true; continue; }
      if (key=="--no-composition") { cfg.no_composition=true; continue; }
      if (key=="--help") {
        out<<"equity-strategy-ic --library JSON --library-sha256 SHA --train MANIFEST --train-sha256 SHA --output NEWDIR "
               "[--validation MANIFEST --validation-sha256 SHA --max-memory-mib N --min-names N --min-dates N --workers 1..16 --plan-only --save-combined] [--orientations TRAIN_ARTIFACT --orientations-sha256 SHA] "
               "[--candidate-cache DIR [--cache-legacy-fields DIR]... [--cache-report]] "
               "[--composition-weights JSON --composition-weights-sha256 SHA] "
               "[--train-fields DIR --train-fields-sha256 SHA] [--validation-fields DIR --validation-fields-sha256 SHA] "
               "[--no-composition]\n"
               "  verbs: marginal (marginal IC of each candidate against a saved blend, contract K6; see\n"
               "    `atx-equity-strategy-ic marginal --help`); ic (this option list, also the default).\n"
               "  --no-composition: screening pass; member IC rows and orientations only (byte-identical to a full\n"
               "    run's), no blend, __combined__ rows, planned targets or saved blend; with --candidate-cache a\n"
               "    candidate whose signal and IC result are both cached is not loaded. Refuses --composition-weights.\n"
               "  --*-fields: atx.research-role-fields/v1 directory bound to that role; SHA pins DIR/manifest.json.\n"
               "  --candidate-cache: content-keyed entries (v2) under DIR[/<vm-identity>]/<role-sha>/[fp_<fk16>/]\n"
               "    <id>.<dsl16>.{f64,json}, keyed on the DSL and the payload SHA256 of each field it reads; v1 entries\n"
               "    (<role-or-fields-sha>/<id>.{f64,json}) are read in place. Publication does not fsync; killed writes\n"
               "    leave inert .partial files (safe to delete); a foreign/corrupt entry refuses loudly. It also caches\n"
               "    IC results beside each entry: ic<v>_<key>/<stem>.json, keyed on signal bytes.\n"
               "  --cache-legacy-fields DIR (repeatable): a fields directory whose manifest keyed v1 field entries;\n"
               "    such an entry hits when every field its DSL reads has the pinned manifest's payload SHA256.\n"
               "  --cache-report: metadata only; prints per-role hits/misses and the unreferenced cache entries with\n"
               "    bytes (needs --candidate-cache; no --output).\n"
               "  --composition-weights: must carry train_manifest_sha256 (== --train-sha256); optional signs {id: +1|-1}\n"
               "    replace the IC orientation in the blend; a blend frozen with weights resumes only with the same file.\n"
               "    optional theme_redistribution {rule: within-theme-v1, composition: ew-theme-v6, themes: {id: theme}}\n"
               "    keeps a missing member's mass inside its theme per name and date; schema\n"
               "    atx.dsl-composition-weights/v2 iff that block is present, v1 iff absent.\n";
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
      else if (key=="--candidate-cache") cfg.candidate_cache_directory=value;
      else if (key=="--cache-legacy-fields") cfg.candidate_cache_legacy_fields.push_back(value);
      else if (key=="--composition-weights") cfg.composition_weights_path=value;
      else if (key=="--composition-weights-sha256") cfg.composition_weights_sha256=value;
      else if (key=="--train-fields") cfg.train_fields_directory=value;
      else if (key=="--train-fields-sha256") cfg.train_fields_sha256=value;
      else if (key=="--validation-fields") cfg.validation_fields_directory=value;
      else if (key=="--validation-fields-sha256") cfg.validation_fields_sha256=value;
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
