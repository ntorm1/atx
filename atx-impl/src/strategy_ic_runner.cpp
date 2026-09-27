#include "strategy_ic_runner.hpp"
#include "strategy_ic_composition.hpp"
#include "build_provenance.hpp"
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
constexpr f64 quiet_nan=std::numeric_limits<f64>::quiet_NaN();
constexpr const char* vm_eval_mode="ResearchFast;full-historical-asof-member-mask";
constexpr const char* cache_schema="atx.dsl-candidate-signal/v1";
constexpr const char* cache_layout="date-major-little-endian-f64;non-finite-stored-as-quiet-NaN";
constexpr const char* weights_schema="atx.dsl-composition-weights/v1";
constexpr usize io_chunk=1U<<20;
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
co::Result<std::string> pinned_text(const std::string& path,const std::string& pin) {
  if (!hash_valid(pin)) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: external SHA256 required");
  ATX_TRY(auto text,metadata_text(path));
  ATX_TRY(auto actual,co::sha256_hex(text));
  if (actual!=pin) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: external metadata pin differs");
  return co::Ok(std::move(text));
}
co::Result<Json> pinned_json(const std::string& path,const std::string& pin) {
  ATX_TRY(auto text,pinned_text(path,pin));
  return co::Ok(Json::parse(text));
}
Json method_recipe(const IcRunnerConfig& cfg,bool parallel_ic=true) {
  Json recipe{{"schema","atx.dsl-fast-ic/v1"},{"library_sha256",cfg.library_sha256},
      {"horizons",{5,21,63}},{"active_horizons",3},{"require_endpoint_presence",true},
      {"execution_delay",1},{"min_names",cfg.min_names},{"min_dates",cfg.min_dates},
      {"screen_rule","equivalence-v3"},{"practical_abs_ic",.002},{"confidence_multiplier",3.5},
      {"max_working_bytes",cfg.max_working_bytes},{"vm",vm_eval_mode},
      {"labels","close[d+1+h]/close[d+1]-1;strict-positive-observed-endpoints;role-maturity"},
      {"guard","observed-adjacent-log1.5;adjusted-log-vs-raw+.10;no-missing-zero-fill"},
      {"orientation","TRAIN21h-nonzero-sample-rank-mean;undefined=0;screen-diagnostic-only;freeze-before-validation"},
      {"composition","fixed-equal-family/equal-within;centered-tied-rank;missing-or-unoriented-neutral;no-redistribution"},
      {"planned_targets","final-rank-neutral-gross1;cadence5;fraction.25;no-drift;offcycle-membership-exit-zero;deployment-included"},
      {"scope","IC-and-planned-weight-change-only;no-costs-trades-NAV-Sharpe-or-holdout"}};
  if (cfg.workers!=1) recipe["vm_workers"]=cfg.workers;
  if (parallel_ic && cfg.workers!=1) recipe["research_ic_workers"]=cfg.workers;
  if (cfg.save_combined) recipe["saved_combined"]="date-major-f64-with-explicit-support-v1";
  // Absent weights leave the recipe byte-identical; pinned weights change both
  // the method statement and the canonical hash. The candidate cache is not a
  // method input: a verified hit is the exact VM output it replaces.
  if (!cfg.composition_weights_sha256.empty()) {
    recipe["composition"]="pinned-candidate-weights;TRAIN-orientation-signs;centered-tied-rank;"
        "missing-or-unoriented-neutral;no-redistribution";
    recipe["composition_weights_sha256"]=cfg.composition_weights_sha256;
  }
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
  const bool source_weighted=recipe.contains("composition_weights_sha256");
  if (source_weighted && (!recipe.at("composition_weights_sha256").is_string() ||
      !hash_valid(recipe.at("composition_weights_sha256").get<std::string>())))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN composition weights pin");
  auto source_cfg=cfg; source_cfg.max_working_bytes=static_cast<u64>(source_bytes);
  source_cfg.workers=static_cast<usize>(source_workers);
  source_cfg.save_combined=recipe.contains("saved_combined");
  // Signs never depend on composition weights, so the frozen source may have
  // used different (or no) pinned weights; its recipe still must match exactly.
  source_cfg.composition_weights_sha256=
      source_weighted?recipe.at("composition_weights_sha256").get<std::string>():std::string{};
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
    const Json& orientations,const std::string& recipe_sha,const std::string& orientation_pin) {
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
  const auto name=prefix+".json"; ATX_TRY_VOID(write_json(dir/name,manifest));
  ATX_TRY(auto pin,co::sha256_file((dir/name).string()));
  return co::Ok(Json{{"manifest",name},{"manifest_sha256",pin},{"orientation_candidates_sha256",orientation_sha}});
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
  // One maximum compiled slot payload: the runner destroys an undersized Engine
  // before creating its replacement. Output SignalSet has its own8B/cell above.
  // That returned-signal8 is also the single reused candidate buffer: a cache
  // hit loads into it, and it is released before any VM evaluation allocates.
  // Cache I/O and hashing stream it in place in 1MiB slices: no second copy.
  // The fresh Engine's initial1x1 pool is covered by the fixed slack. No surfaces,
  // execution context, per-candidate retained signals, or book position arrays.
  if (!b.add(1,32ULL<<20) || !b.add(cells,72+8*lib.max_slots) || !b.add(composition,1) ||
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
// Duplicate object keys would silently keep the last weight; refuse them instead.
// Malformed text and numeric overflow (e.g. 1e999) are parse refusals here.
co::Result<Json> unique_key_json(const std::string& text) {
  std::vector<std::set<std::string>> open; bool duplicate=false;
  auto j=Json::parse(text,[&](int,Json::parse_event_t event,Json& value) {
    if (event==Json::parse_event_t::object_start) open.emplace_back();
    else if (event==Json::parse_event_t::object_end && !open.empty()) open.pop_back();
    else if (event==Json::parse_event_t::key && !open.empty() &&
             !open.back().insert(value.get<std::string>()).second) duplicate=true;
    return true;
  },false);
  if (j.is_discarded()) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition weights JSON parse");
  if (duplicate) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition weights duplicate key");
  return co::Ok(std::move(j));
}
// Empty result = default equal family/within-family weights. Runs before any
// role payload load (including under --plan-only); every refusal is loud.
co::Result<std::vector<f64>> composition_weights(const IcRunnerConfig& cfg,const Library& lib) {
  std::vector<f64> weights;
  if (cfg.composition_weights_path.empty()) return co::Ok(std::move(weights));
  ATX_TRY(auto text,pinned_text(cfg.composition_weights_path,cfg.composition_weights_sha256));
  ATX_TRY(auto j,unique_key_json(text));
  if (!j.is_object() || !j.contains("schema") || j.at("schema")!=weights_schema ||
      !j.contains("library_sha256") || j.at("library_sha256")!=cfg.library_sha256 ||
      !j.contains("weights") || !j.at("weights").is_object())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition weights schema/library identity");
  const auto& rows=j.at("weights");
  std::set<std::string> ids;
  for (const auto& c:lib.candidates) ids.insert(c.id);
  for (auto it=rows.begin();it!=rows.end();++it)
    if (!ids.contains(it.key()))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition weight for unknown candidate: "+it.key());
  weights.reserve(lib.candidates.size());
  for (const auto& c:lib.candidates) {
    const auto it=rows.find(c.id);
    if (it==rows.end())
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition weight missing: "+c.id);
    const f64 w=it->is_number()?it->get<f64>():quiet_nan;
    if (!std::isfinite(w) || w<0)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition weight must be finite and >= 0: "+c.id);
    weights.push_back(w);
  }
  return co::Ok(std::move(weights));
}
// ---- Candidate signal cache: DIR/<role-manifest-sha256>/<id>.{f64,json} ----
// Identity is (DSL sha, role manifest sha, geometry) plus a streamed payload
// SHA256. Library, role name, source and engine identity are recorded only, so a
// grown library reuses unchanged candidates. A changed VM needs a fresh DIR.
struct CacheKey { std::filesystem::path dir; std::string role_sha; u64 dates{},instruments{}; };
std::string hex(const std::array<std::byte,32>& bytes) {
  constexpr char digits[]="0123456789abcdef"; std::string out(64,'0');
  for (usize i=0;i<bytes.size();++i) {
    const auto b=std::to_integer<unsigned>(bytes[i]);
    out[2*i]=digits[b>>4]; out[2*i+1]=digits[b&15];
  }
  return out;
}
// Candidate IDs are [a-z0-9_]{1,64}; only these basenames are Windows devices.
bool device_name(std::string_view id) {
  for (const std::string_view reserved:{"con","prn","aux","nul"}) if (id==reserved) return true;
  return id.size()==4 && (id.starts_with("com") || id.starts_with("lpt")) && id[3]>='0' && id[3]<='9';
}
co::Status cache_preflight(const IcRunnerConfig& cfg,const Library& lib) {
  if (cfg.candidate_cache_directory.empty()) return co::Ok();
  if constexpr (std::endian::native!=std::endian::little)
    return co::Err(co::ErrorCode::Unavailable,"IC runner: candidate cache requires little-endian host");
  for (const auto& c:lib.candidates) if (device_name(c.id))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate id unsafe as cache file name: "+c.id);
  std::error_code ec; const auto root=std::filesystem::path(cfg.candidate_cache_directory);
  const bool present=std::filesystem::exists(root,ec);
  if (ec || (present && !std::filesystem::is_directory(root,ec)) || ec)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate cache path must be a directory");
  return co::Ok();
}
// A present sidecar must describe exactly this candidate on this role geometry;
// anything else is a loud refusal, never a silent recompute over foreign bytes.
co::Result<std::string> cached_payload_sha(const Json& j,const CacheKey& key,const Candidate& c) {
  const auto text=[&](const char* k) {
    return j.contains(k) && j.at(k).is_string()?j.at(k).get<std::string>():std::string{};
  };
  const auto count=[&](const char* k) {
    return j.contains(k) && j.at(k).is_number_unsigned()?j.at(k).get<u64>():~u64{0};
  };
  auto sha=text("payload_sha256");
  if (!j.is_object() || text("schema")!=cache_schema || text("candidate_id")!=c.id ||
      text("dsl_sha256")!=c.dsl_sha || text("role_manifest_sha256")!=key.role_sha ||
      text("eval_mode")!=vm_eval_mode || text("layout")!=cache_layout || count("dates")!=key.dates ||
      count("instruments")!=key.instruments || count("bytes")!=key.dates*key.instruments*sizeof(f64) ||
      !hash_valid(sha))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate cache entry mismatch: "+c.id);
  return co::Ok(std::move(sha));
}
// nullopt: no sidecar (evaluate and write). Present-but-inconsistent: error.
co::Result<std::optional<std::string>> cache_lookup(const CacheKey& key,const Candidate& c) {
  const auto path=key.dir/(c.id+".json"); std::error_code ec;
  const bool present=std::filesystem::exists(path,ec);
  if (ec) return co::Err(co::ErrorCode::IoError,"IC runner: candidate cache probe: "+c.id);
  if (!present) return co::Ok(std::optional<std::string>{});
  ATX_TRY(auto text,metadata_text(path.string()));
  const auto j=Json::parse(text,nullptr,false);
  if (j.is_discarded())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate cache sidecar JSON: "+c.id);
  ATX_TRY(auto sha,cached_payload_sha(j,key,c));
  return co::Ok(std::optional<std::string>(std::move(sha)));
}
// Reuses `out` (one candidate payload at a time); hashes each chunk as it lands.
co::Status cache_load(const std::filesystem::path& path,const std::string& expected,usize cells,
                      std::vector<f64>& out) {
  const auto bytes=static_cast<u64>(cells)*sizeof(f64); const auto name=path.filename().string();
  std::ifstream in(path,std::ios::binary|std::ios::ate);
  if (!in || in.tellg()<0 || static_cast<u64>(in.tellg())!=bytes)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate cache payload extent: "+name);
  in.seekg(0); out.resize(cells);
  const auto destination=std::as_writable_bytes(std::span(out)); co::Sha256 digest;
  for (usize offset=0;offset<destination.size();) {
    const auto chunk=destination.subspan(offset,std::min(io_chunk,destination.size()-offset));
    // SAFETY: char accesses the object representation of trivially copyable f64 storage.
    in.read(reinterpret_cast<char*>(chunk.data()),static_cast<std::streamsize>(chunk.size()));
    if (!in) return co::Err(co::ErrorCode::IoError,"IC runner: candidate cache payload truncated: "+name);
    ATX_TRY_VOID(digest.update(chunk)); offset+=chunk.size();
  }
  if (in.peek()!=std::char_traits<char>::eof())
    return co::Err(co::ErrorCode::IoError,"IC runner: candidate cache payload changed extent: "+name);
  ATX_TRY(auto actual,digest.finalize());
  if (hex(actual)!=expected)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate cache payload SHA256 mismatch: "+name);
  return co::Ok();
}
// Unique per attempt so a concurrent or killed writer never shares a partial.
struct PartialFile {
  std::filesystem::path path;
  explicit PartialFile(const std::filesystem::path& final) {
    std::random_device entropy;
    const auto nonce=((static_cast<u64>(entropy())<<32)^static_cast<u64>(entropy()))^
        static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
    // Short fixed-width tag: cache paths already carry a 64-hex role directory.
    constexpr char digits[]="0123456789abcdef"; std::string tag(16,'0');
    for (usize i=0;i<tag.size();++i) tag[i]=digits[(nonce>>(4*i))&15U];
    path=final.parent_path()/("."+final.filename().string()+"."+tag+".partial");
  }
  PartialFile(const PartialFile&)=delete;
  PartialFile& operator=(const PartialFile&)=delete;
  PartialFile(PartialFile&&)=delete;
  PartialFile& operator=(PartialFile&&)=delete;
  // Published bytes live on under the final hard link. A partial that cannot be
  // removed is an inert dot-file no lookup ever reads, so cleanup is best effort.
  ~PartialFile() { std::error_code ec; std::filesystem::remove(path,ec); }
};
// Atomic no-replace publication in one directory; true iff `final` already existed.
co::Result<bool> publish_new(const std::filesystem::path& partial,const std::filesystem::path& final) {
  std::error_code ec; std::filesystem::create_hard_link(partial,final,ec);
  if (!ec) return co::Ok(false);
  std::error_code probe;
  if (std::filesystem::exists(final,probe) && !probe) return co::Ok(true);
  return co::Err(co::ErrorCode::IoError,
      "IC runner: candidate cache publish "+final.filename().string()+": "+ec.message());
}
co::Result<std::string> write_partial(const PartialFile& partial,std::span<const std::byte> bytes) {
  std::ofstream out(partial.path,std::ios::binary); co::Sha256 digest;
  if (!out) return co::Err(co::ErrorCode::IoError,"IC runner: candidate cache partial output");
  for (usize offset=0;offset<bytes.size();) {
    const auto chunk=bytes.subspan(offset,std::min(io_chunk,bytes.size()-offset));
    out.write(reinterpret_cast<const char*>(chunk.data()),static_cast<std::streamsize>(chunk.size()));
    if (!out) return co::Err(co::ErrorCode::IoError,"IC runner: candidate cache partial write");
    ATX_TRY_VOID(digest.update(chunk)); offset+=chunk.size();
  }
  out.close(); if (!out) return co::Err(co::ErrorCode::IoError,"IC runner: candidate cache partial close");
  ATX_TRY(auto sha,digest.finalize());
  return co::Ok(hex(sha));
}
// Never overwrites: an existing payload (a run stopped between the payload and
// sidecar publications, or a concurrent writer) is adopted only if byte-identical.
co::Result<std::string> cache_store_payload(const std::filesystem::path& final,
                                            std::span<const std::byte> bytes) {
  std::error_code ec; const bool present=std::filesystem::exists(final,ec);
  if (ec) return co::Err(co::ErrorCode::IoError,"IC runner: candidate cache probe: "+final.filename().string());
  std::string sha;
  if (!present) {
    const PartialFile partial(final);
    ATX_TRY(sha,write_partial(partial,bytes));
    ATX_TRY(auto existed,publish_new(partial.path,final));
    if (!existed) return co::Ok(std::move(sha));
  } else {
    ATX_TRY(sha,co::sha256_hex(bytes));
  }
  ATX_TRY(auto existing,co::sha256_file(final.string()));
  if (existing!=sha)
    return co::Err(co::ErrorCode::AlreadyExists,"IC runner: candidate cache payload exists with "
        "different bytes; refusing overwrite: "+final.filename().string());
  return co::Ok(std::move(sha));
}
// Canonicalizes non-finite cells to quiet NaN IN PLACE, so this run consumes
// exactly the bytes it stores (cold == warm by construction). Downstream IC rows
// and composition admit a cell only via std::isfinite, so no-cache results are
// unchanged. The sidecar is published last: it is the entry's commit record.
co::Status cache_store(const CacheKey& key,const IcRunnerConfig& cfg,const Role& spec,
    const engine::data::StrategyRoleData& role,const Candidate& c,std::span<f64> signal) {
  if (signal.size()!=key.dates*key.instruments)
    return co::Err(co::ErrorCode::Internal,"IC runner: candidate cache geometry");
  for (auto& v:signal) if (!std::isfinite(v)) v=quiet_nan;
  ATX_TRY(auto sha,cache_store_payload(key.dir/(c.id+".f64"),std::as_bytes(signal)));
  const Json sidecar{{"schema",cache_schema},{"candidate_id",c.id},{"family",c.family},
      {"dsl_sha256",c.dsl_sha},{"library_sha256",cfg.library_sha256},
      {"role_manifest_sha256",key.role_sha},{"role",spec.name},{"source_sha256",role.source_sha256},
      {"dates",key.dates},{"instruments",key.instruments},{"bytes",signal.size()*sizeof(f64)},
      {"layout",cache_layout},{"payload",c.id+".f64"},{"payload_sha256",sha},
      {"semantics","raw-unoriented-pre-composition-single-VM-root"},{"eval_mode",vm_eval_mode},
      {"vm_workers",cfg.workers},{"engine_git_sha",std::string(build_engine_git_sha())}};
  const auto text=sidecar.dump(2)+"\n";
  const auto final=key.dir/(c.id+".json");
  const PartialFile partial(final);
  ATX_TRY_VOID(write_partial(partial,std::as_bytes(std::span(text.data(),text.size()))));
  ATX_TRY(auto existed,publish_new(partial.path,final));
  if (existed) {
    // A concurrent writer committed first: accept only the same identity and bytes.
    ATX_TRY(auto committed,cache_lookup(key,c));
    if (!committed || *committed!=sha)
      return co::Err(co::ErrorCode::AlreadyExists,
          "IC runner: candidate cache sidecar raced with different bytes: "+c.id);
  }
  return co::Ok();
}
// Metadata-only readiness for --plan-only: sidecars are identity-checked and
// payload extents stat'ed; payload hashes are verified only when loaded.
co::Result<Json> cache_plan(const IcRunnerConfig& cfg,const Library& lib,const Role& role) {
  const CacheKey key{std::filesystem::path(cfg.candidate_cache_directory)/role.sha,role.sha,
      role.metadata.at("dates").get<u64>(),role.metadata.at("instruments").get<u64>()};
  usize ready=0;
  for (const auto& c:lib.candidates) {
    ATX_TRY(auto sha,cache_lookup(key,c));
    if (!sha) continue;
    std::error_code ec; const auto size=std::filesystem::file_size(key.dir/(c.id+".f64"),ec);
    if (ec || size!=key.dates*key.instruments*sizeof(f64))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate cache payload extent: "+c.id);
    ++ready;
  }
  return co::Ok(Json{{"role",role.name},{"directory",key.dir.string()},{"ready_entries",ready},
      {"candidates",lib.candidates.size()}});
}
void release(std::vector<f64>& buffer) noexcept { std::vector<f64>().swap(buffer); }
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
struct SignalTiming { f64 vm{},cache_load{},cache_write{}; bool hit{}; };
// Leaves the candidate's raw (unoriented) signal in `buffer`: a verified cache
// hit, or a VM evaluation that is then committed to the cache when enabled.
co::Result<SignalTiming> candidate_signal(const IcRunnerConfig& cfg,const Role& spec,
    const engine::data::StrategyRoleData& role,const Candidate& candidate,
    const std::optional<CacheKey>& signal_cache,engine::parallel::DetPool* pool,
    std::unique_ptr<al::Engine>& vm,std::vector<f64>& buffer,std::ostream& progress) {
  using steady=std::chrono::steady_clock;
  const auto since=[](steady::time_point from) { return std::chrono::duration<f64>(steady::now()-from).count(); };
  SignalTiming out;
  if (signal_cache) {
    const auto load_started=steady::now();
    ATX_TRY(auto sha,cache_lookup(*signal_cache,candidate));
    if (sha) {
      ATX_TRY_VOID(cache_load(signal_cache->dir/(candidate.id+".f64"),*sha,
          role.panel.dates()*role.panel.instruments(),buffer));
      out.cache_load=since(load_started); out.hit=true;
      progress<<"IC cache-hit "<<candidate.id<<" role="<<spec.name<<" seconds="<<out.cache_load<<'\n'<<std::flush;
      return co::Ok(out);
    }
    out.cache_load=since(load_started);
    progress<<"IC cache-miss "<<candidate.id<<" role="<<spec.name<<'\n'<<std::flush;
  }
  // At most one full-panel candidate signal exists: drop the previous one
  // before any VM arena growth or evaluation allocates.
  release(buffer);
  const auto vm_started=steady::now();
  if (!vm || candidate.program.num_slots>vm->pool_capacity()) {
    const auto previous_slots=vm?vm->pool_capacity():0;
    // The preceding candidate's signal is already released. Free the old
    // full-panel arena BEFORE Engine::evaluate allocates a larger one;
    // Engine's own ensure_pool otherwise retains both during construction.
    vm.reset();
    vm=std::make_unique<al::Engine>(role.panel);
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
    ATX_TRY_VOID(cache_store(*signal_cache,cfg,spec,role,candidate,buffer));
    out.cache_write=since(write_started);
    progress<<"IC cache-write "<<candidate.id<<" role="<<spec.name<<" seconds="<<out.cache_write<<'\n'<<std::flush;
  }
  return co::Ok(out);
}
co::Result<Json> score_role(const IcRunnerConfig& cfg,const Library& lib,const Role& spec,
    std::span<const f64> weights,std::vector<int>& signs,Json& frozen,const std::string& recipe_sha,
    const std::string& orientation_pin,std::ostream& progress) {
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
  std::unique_ptr<al::Engine> vm;
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
  ATX_TRY(auto composition,IcComposition::create(cc,candidates,effective,weights));
  std::optional<CacheKey> signal_cache;
  if (!cfg.candidate_cache_directory.empty()) {
    signal_cache=CacheKey{std::filesystem::path(cfg.candidate_cache_directory)/spec.sha,spec.sha,
        role.panel.dates(),role.panel.instruments()};
    std::error_code ec; std::filesystem::create_directories(signal_cache->dir,ec);
    if (ec) return co::Err(co::ErrorCode::IoError,"IC runner: candidate cache directory: "+ec.message());
  }
  std::vector<f64> signal_buffer; usize cache_hits=0;
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
    ATX_TRY(auto acquired,candidate_signal(cfg,spec,role,candidate,signal_cache,pool.get(),vm,
        signal_buffer,progress));
    const std::span<const f64> signal(signal_buffer);
    const auto vm_seconds=acquired.vm; total_vm_seconds+=vm_seconds;
    total_cache_load_seconds+=acquired.cache_load; total_cache_write_seconds+=acquired.cache_write;
    cache_hits+=acquired.hit?1U:0U;
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
    if (signal_cache) {
      summary["stage_seconds"]["cache_load"]=acquired.cache_load;
      summary["stage_seconds"]["cache_write"]=acquired.cache_write;
      summary["signal_cache"]=acquired.hit?"hit":"miss";
    }
    summaries.push_back(summary); ledger<<summary.dump()<<'\n'<<std::flush;
    if (!daily || !ledger) return co::Err(co::ErrorCode::IoError,"IC runner: candidate output");
    progress<<"IC "<<spec.name<<' '<<(k+1)<<'/'<<lib.candidates.size()<<' '<<candidate.id
            <<" seconds="<<candidate_seconds<<" vm="<<vm_seconds<<" ic="<<ic_seconds
            <<" composition="<<composition_seconds<<" sign="<<sign<<" reason="<<ex::ic_screen_reason_name(scored.screen.reason);
    if (signal_cache) progress<<" cache="<<(acquired.hit?"hit":"miss");
    progress<<'\n'<<std::flush;
  }
  // Composition owns its accumulated blend; it does not borrow VM slots or any
  // discarded candidate output. Combined IC/save need only the shared pool.
  vm.reset(); release(signal_buffer);
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
  Json saved; f64 save_seconds=0;
  if (cfg.save_combined) {
    const auto save_started=std::chrono::steady_clock::now();
    ATX_TRY(saved,save_combined_artifact(cfg,spec,role,combined.signal,effective,frozen,recipe_sha,orientation_pin));
    save_seconds=std::chrono::duration<f64>(std::chrono::steady_clock::now()-save_started).count();
  }
  const auto seconds=std::chrono::duration<f64>(std::chrono::steady_clock::now()-started).count();
  Json result{{"role",spec.name},{"manifest_sha256",spec.sha},{"source_sha256",role.source_sha256},
      {"dates",role.panel.dates()},{"instruments",role.panel.instruments()},
      {"score_begin",role.score_begin},{"score_end",role.score_end}, {"wall_seconds",seconds},
      {"admitted_working_bytes",spec.bytes},{"ic_cache_bytes",cache.bytes()},{"ic_scratch_bytes",scratch.bytes()},
      {"workers",cfg.workers},{"stage_seconds",{{"load",load_seconds},{"label_preparation",label_seconds},
          {"vm",total_vm_seconds},{"ic",total_ic_seconds},{"composition",total_composition_seconds}}},
      {"candidate_evaluations",lib.candidates.size()},{"combined_evaluations",1},
      {"candidates",std::move(summaries)},{"combined_ic",result_json(combined_ic,1)},
      {"planned_target_proxy",{{"total_turnover",combined.total_planned_turnover},
          {"deployment_turnover",combined.deployment_turnover},{"deployment_date",combined.deployment_date},
          {"initial_deployment_included",true},{"actual_trades_or_costs",false}}}};
  if (cfg.save_combined) {
    result["combined_artifact"]=std::move(saved);
    result["stage_seconds"]["save_combined"]=save_seconds;
  }
  if (signal_cache) {
    result["stage_seconds"]["cache_load"]=total_cache_load_seconds;
    result["stage_seconds"]["cache_write"]=total_cache_write_seconds;
    result["candidate_cache"]={{"directory",signal_cache->dir.string()},{"hits",cache_hits},
        {"misses",lib.candidates.size()-cache_hits},{"vm_evaluations",lib.candidates.size()-cache_hits}};
  }
  return co::Ok(std::move(result));
}
} // namespace
co::Status run_ic(const IcRunnerConfig& cfg,std::ostream& progress) {
  try {
    const bool validation_only=!cfg.orientations_path.empty();
    if ((!cfg.plan_only && cfg.output_directory.empty()) || cfg.max_working_bytes<(32ULL<<20) || cfg.max_working_bytes>(16ULL<<30) ||
        cfg.min_names<3 || cfg.min_dates<8 || cfg.min_dates>4096 || cfg.workers<1 || cfg.workers>4 ||
        cfg.validation_manifest.empty()!=cfg.validation_sha256.empty() ||
        cfg.orientations_path.empty()!=cfg.orientations_sha256.empty() ||
        cfg.composition_weights_path.empty()!=cfg.composition_weights_sha256.empty() ||
        (validation_only && cfg.validation_manifest.empty()))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: bounded config");
    ATX_TRY(auto lib,library(cfg));
    // Both new options are fully validated here, before any role payload.
    ATX_TRY(const auto weights,composition_weights(cfg,lib));
    ATX_TRY_VOID(cache_preflight(cfg,lib));
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
      if (!cfg.composition_weights_sha256.empty()) plan["composition_weights_sha256"]=cfg.composition_weights_sha256;
      if (!cfg.candidate_cache_directory.empty()) {
        plan["candidate_cache"]=Json::array();
        for (const auto& role:roles) {
          ATX_TRY(auto entry,cache_plan(cfg,lib,role)); plan["candidate_cache"].push_back(std::move(entry));
        }
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
    if (!cfg.composition_weights_sha256.empty()) report["composition_weights_sha256"]=cfg.composition_weights_sha256;
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
      auto scored=score_role(cfg,lib,role,weights,signs,orientations,recipe_sha,
          report.value("orientations_artifact_sha256",std::string{}),progress);
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
      if (key=="--save-combined") { cfg.save_combined=true; continue; }
      if (key=="--help") {
        out<<"equity-strategy-ic --library JSON --library-sha256 SHA --train MANIFEST --train-sha256 SHA --output NEWDIR "
               "[--validation MANIFEST --validation-sha256 SHA --max-memory-mib N --min-names N --min-dates N --workers 1..4 --plan-only --save-combined] [--orientations TRAIN_ARTIFACT --orientations-sha256 SHA] "
               "[--candidate-cache DIR] [--composition-weights JSON --composition-weights-sha256 SHA]\n";
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
      else if (key=="--composition-weights") cfg.composition_weights_path=value;
      else if (key=="--composition-weights-sha256") cfg.composition_weights_sha256=value;
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
