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
// ew-theme-v6 files (V6-W fix round 1 I1): v2 iff a theme_redistribution block is
// present, v1 iff absent, so a binary predating within-theme-v1 refuses them loudly.
constexpr const char* weights_schema_v2="atx.dsl-composition-weights/v2";
// ew-theme-v6 (v4-prereg v6 revision V6-W): the only admitted theme_redistribution block.
constexpr const char* theme_redistribution_rule="within-theme-v1";
constexpr const char* theme_redistribution_composition="ew-theme-v6";
constexpr const char* fields_schema="atx.research-role-fields/v1";
constexpr const char* fields_semantics="extra-date-major-f64-columns-resolved-by-name;NaN-where-not-visible;"
    "role-presence-mask;decision-member-mask-unchanged";
constexpr std::array<std::string_view,3> base_fields{"close","raw_close","volume"};
constexpr usize io_chunk=1U<<20;
// ---- Candidate-cache VM identity -------------------------------------------
// BUMP dsl_vm_semantics_version with any change that can alter one evaluated bit
// of a DSL signal: parse/analyze/compile (atx-engine alpha parser/typecheck/dag/
// bytecode/fusion), the VM or its kernels (alpha/vm.hpp, cs_ops, ts_ops, ts_*,
// state_ops), or the member-mask/eval-mode contract used here. The value keys the
// cache directory, so a bump is a clean miss + recompute, never a stale hit.
constexpr int dsl_vm_semantics_version=1;
// Tripwire for the bump above (test StrategyIcRunner.VmSourcesPinnedToSemanticsVersion):
// every engine source that parses, compiles or evaluates a DSL signal here -- the
// runner TU's include closure under atx/engine minus IC scoring (factory/), plus
// the TUs those headers declare -- is hashed and compared with the pin below.
// Digest: SHA-256 over, per listed path in order, "<path>\n<bytes>\n<text>", the
// text CRLF->LF normalized (bytes = its normalized length). On a mismatch, decide:
// a semantic change bumps dsl_vm_semantics_version (clean cache miss); either way
// the pin is re-set. The test also fails if a listed file includes an unlisted
// atx/engine header, so the list cannot silently fall behind the closure.
constexpr std::array<std::string_view,29> dsl_vm_sources{
    "atx-engine/include/atx/engine/alpha/bytecode.hpp",
    "atx-engine/include/atx/engine/alpha/cs_ops.hpp",
    "atx-engine/include/atx/engine/alpha/cs_radix.hpp",
    "atx-engine/include/atx/engine/alpha/dag.hpp",
    "atx-engine/include/atx/engine/alpha/fusion.hpp",
    "atx-engine/include/atx/engine/alpha/fwd.hpp",
    "atx-engine/include/atx/engine/alpha/lexer.hpp",
    "atx-engine/include/atx/engine/alpha/panel.hpp",
    "atx-engine/include/atx/engine/alpha/parser.hpp",
    "atx-engine/include/atx/engine/alpha/registry.hpp",
    "atx-engine/include/atx/engine/alpha/state_ops.hpp",
    "atx-engine/include/atx/engine/alpha/subtree_cache.hpp",
    "atx-engine/include/atx/engine/alpha/ts_ops.hpp",
    "atx-engine/include/atx/engine/alpha/ts_order_stat.hpp",
    "atx-engine/include/atx/engine/alpha/ts_sliding.hpp",
    "atx-engine/include/atx/engine/alpha/typecheck.hpp",
    "atx-engine/include/atx/engine/alpha/vm.hpp",
    "atx-engine/include/atx/engine/parallel/det_pool.hpp",
    "atx-engine/include/atx/engine/parallel/fwd.hpp",
    "atx-engine/include/atx/engine/data/strategy_data.hpp",
    "atx-engine/src/alpha/bytecode.cpp",
    "atx-engine/src/alpha/dag.cpp",
    "atx-engine/src/alpha/lexer.cpp",
    "atx-engine/src/alpha/panel.cpp",
    "atx-engine/src/alpha/parser.cpp",
    "atx-engine/src/alpha/registry.cpp",
    "atx-engine/src/alpha/subtree_cache.cpp",
    "atx-engine/src/alpha/typecheck.cpp",
    "atx-engine/src/data/strategy_data.cpp"};
constexpr std::string_view dsl_vm_sources_sha256=
    "18693b1880103c7ff7ddf1b59fc35d42b0be3efba381a3fc85884e6ae900e340";
// FP-relevant build flavor of this TU, which instantiates the header-only VM:
// compiler major.minor and FMA/AVX2/fast-math. Patch-level compiler updates are
// assumed not to change strict-FP results. clang-cl defines both __clang__ and
// _MSC_VER; the clang branch wins.
#define ATX_IC_STRINGIZE2(x) #x
#define ATX_IC_STRINGIZE(x) ATX_IC_STRINGIZE2(x)
#if defined(__clang__)
constexpr std::string_view vm_compiler=
    "clang" ATX_IC_STRINGIZE(__clang_major__) "." ATX_IC_STRINGIZE(__clang_minor__);
#elif defined(_MSC_VER)
constexpr std::string_view vm_compiler="msvc" ATX_IC_STRINGIZE(_MSC_VER);
#else
constexpr std::string_view vm_compiler{}; // unknown: --candidate-cache refuses
#endif
#undef ATX_IC_STRINGIZE
#undef ATX_IC_STRINGIZE2
constexpr std::string_view vm_fp_flavor=""
#if defined(__FMA__)
    "_fma"
#endif
#if defined(__AVX2__)
    "_avx2"
#endif
#if defined(__FAST_MATH__)
    "_fastmath"
#endif
    ;
// Entries written before this identity existed sit directly under DIR/<sha>/ with
// no vm_identity key. They came from engine builds 429cbe43/6d85ac2a (clang-cl
// 18.1.8, dev preset, no /arch) and no alpha, parallel or core source changed
// between those commits and this key, so exactly this identity keeps that layout
// and accepts keyless sidecars recorded by exactly those builds. Every other
// identity lives under DIR/<identity>/ and must match its sidecar.
constexpr std::string_view legacy_vm_identity="dslvm1_clang18.1";
constexpr std::array<std::string_view,2> legacy_engine_shas{
    "429cbe43d275a49ad3cae89dfa8aa591846a2e4f","6d85ac2a8b7aca6f28cea0e651cdcfc55d77aa29"};
std::string vm_identity() {
  return "dslvm"+std::to_string(dsl_vm_semantics_version)+"_"+std::string(vm_compiler)+std::string(vm_fp_flavor);
}
// ---- IC-result cache identity (T15) -----------------------------------------
// BUMP ic_result_semantics_version with any change that can alter one bit of a
// candidate's IC result or daily IC series for the same signal bytes: the engine
// IC scoring sources pinned below, or how score_role configures IC. Every IC
// configuration field, the decision-membership span and the return guard are
// also keyed by value/content, so the bump covers only code, never inputs.
constexpr int ic_result_semantics_version=1;
constexpr const char* ic_cache_schema="atx.dsl-candidate-ic/v1";
constexpr const char* ic_price_field="close";
// Tripwire for the bump above (test StrategyIcRunner.IcSourcesPinnedToSemanticsVersion,
// same digest recipe as dsl_vm_sources): the IC scoring TU and its atx/engine include
// closure, plus the TUs those headers declare.
constexpr std::array<std::string_view,10> ic_result_sources{
    "atx-engine/include/atx/engine/alpha/fwd.hpp",
    "atx-engine/include/atx/engine/alpha/panel.hpp",
    "atx-engine/include/atx/engine/eval/hac.hpp",
    "atx-engine/include/atx/engine/factory/ic_research.hpp",
    "atx-engine/include/atx/engine/factory/ic_screen.hpp",
    "atx-engine/include/atx/engine/factory/ic_screen_config.hpp",
    "atx-engine/include/atx/engine/parallel/det_pool.hpp",
    "atx-engine/include/atx/engine/parallel/fwd.hpp",
    "atx-engine/src/alpha/panel.cpp",
    "atx-engine/src/factory/ic_screen.cpp"};
constexpr std::string_view ic_result_sources_sha256=
    "e3e6f2d6a9c99ee93b7cfd0538be41fd73ae536975b7ec12cfa788f31f4116f7";
// FP build flavor as for the VM, plus the IC kernel's SIMD width (its reduction
// order): ic_screen.cpp reports its own compiled xsimd batch size.
std::string ic_identity() {
  return "dslic"+std::to_string(ic_result_semantics_version)+"_"+std::string(vm_compiler)+
      std::string(vm_fp_flavor)+"_simd"+std::to_string(ex::ic_screen_simd_width());
}
// extra_fields: the sorted non-base fields this compiled program loads.
struct Candidate { std::string id,family,dsl_sha; al::Program program; std::vector<std::string> extra_fields; };
// Extra-field residency schedule (bit f = Library::extra_fields[f], <= 64 fields).
// Candidates run in library order because the blend accumulates in that order, so
// at most `capacity` columns are resident: the most extras any one candidate reads.
// needs[k] = candidate k's fields; planned[k] = the resident set while k runs, by
// Belady's MIN over library order (a field no later candidate reads is dropped;
// a load at capacity evicts the resident field read farthest ahead, lowest index
// on ties). It depends on the library alone, so admission counts `capacity`
// columns rather than the union of referenced fields.
struct FieldPlan { usize capacity{},loads{}; std::vector<u64> needs,planned; };
// declared_extra: every declared non-base field; extra_fields: the sorted union
// the compiled programs reference -- the only extra columns ever loaded.
struct Library {
  std::string id; std::vector<Candidate> candidates; usize max_slots{},lookback{};
  std::vector<std::string> declared_extra,extra_fields; FieldPlan field_plan;
};
// non_pit_aspects: the producer's list, comma-joined; empty iff point in time.
// definition: the producer's spec-level description, compared across roles.
struct FieldFile {
  std::string name; std::filesystem::path path; std::string sha; u64 bytes{}; std::string non_pit_aspects;
  Json definition;
};
// A role's pinned fields manifest (empty sha: none pinned), the referenced subset
// to load (in Library::extra_fields order), each with its manifest-pinned SHA256
// and exact extent, and every declared extra's definition.
struct RoleFields { std::string directory,sha; std::vector<FieldFile> load; std::map<std::string,Json> declared; };
struct Role { std::string path,sha,name; Json metadata; u64 bytes{}; RoleFields fields; };
struct Budget {
  u64 limit{},used{};
  bool add(u64 n,u64 width) { if (width && n>(limit-used)/width) return false; used+=n*width; return true; }
};
bool hash_valid(std::string_view value) {
  return value.size()==64 && std::all_of(value.begin(),value.end(),[](char c) {
    return (c>='0' && c<='9') || (c>='a' && c<='f');
  });
}
bool base_field(std::string_view name) {
  return std::find(base_fields.begin(),base_fields.end(),name)!=base_fields.end();
}
// Plain DSL identifier: no dots or separators, so `<name>.f64` is a safe basename.
bool field_identifier(std::string_view s) {
  return !s.empty() && s.size()<=64 && ((s.front()>='a' && s.front()<='z') || s.front()=='_') &&
      std::all_of(s.begin(),s.end(),[](char c) { return (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_'; });
}
co::Result<std::string> metadata_text(const std::string& path) {
  std::ifstream in(path,std::ios::binary|std::ios::ate);
  if (!in || in.tellg()<0)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: metadata file missing or unreadable: "+path);
  if (in.tellg()==0 || static_cast<u64>(in.tellg())>(1ULL<<20))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: metadata file empty or over 1 MiB ("+
        std::to_string(static_cast<u64>(in.tellg()))+" B): "+path);
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
Json method_recipe(const IcRunnerConfig& cfg,bool parallel_ic=true,bool pinned_signs=false,bool themed=false) {
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
    // Signs pinned in the (TRAIN-bound, hashed) weights file replace the IC
    // orientation in the blend; IC diagnostics keep the TRAIN orientation.
    recipe["composition"]=pinned_signs
        ?"pinned-candidate-weights;pinned-candidate-signs;centered-tied-rank;"
         "missing-or-unoriented-neutral;no-redistribution"
        :"pinned-candidate-weights;TRAIN-orientation-signs;centered-tied-rank;"
         "missing-or-unoriented-neutral;no-redistribution";
    // A pinned theme_redistribution block (ew-theme-v6) replaces no-redistribution;
    // absent, the recipe bytes above are unchanged.
    if (themed) {
      recipe["composition"]=std::string(pinned_signs?"pinned-candidate-weights;pinned-candidate-signs;"
                                                    :"pinned-candidate-weights;TRAIN-orientation-signs;")+
          "centered-tied-rank;missing-or-unoriented-mass-stays-in-theme;within-theme-v1;"
          "theme-without-present-member-neutral";
      recipe["composition_redistribution"]=theme_redistribution_rule;
    }
    recipe["composition_weights_sha256"]=cfg.composition_weights_sha256;
  }
  return recipe;
}
// Recorded as recipe["research_fields"] exactly when a fields manifest is pinned
// (absent: recipe bytes unchanged). `loaded` is library-derived, identical per role.
Json fields_recipe(Json pins,const Library& lib) {
  return Json{{"schema",fields_schema},{"manifest_sha256",std::move(pins)},{"loaded",lib.extra_fields},
      {"semantics",fields_semantics}};
}
Json fields_pins(const IcRunnerConfig& cfg) {
  Json pins=Json::object();
  if (!cfg.train_fields_sha256.empty()) pins["train"]=cfg.train_fields_sha256;
  if (!cfg.validation_fields_sha256.empty()) pins["validation"]=cfg.validation_fields_sha256;
  return pins;
}
bool fields_pinned(const IcRunnerConfig& cfg) {
  return !cfg.train_fields_sha256.empty() || !cfg.validation_fields_sha256.empty();
}
// A frozen TRAIN recipe records its pinned fields exactly as run_ic writes them.
// TRAIN's pin is taken from the source (its payload is never opened here) and, if
// re-supplied, must match; a validation pin must equal this run's, like role pins.
co::Status expect_frozen_fields(const IcRunnerConfig& cfg,const Library& lib,const Json& recipe,Json& expected) {
  if (!recipe.contains("research_fields")) {
    // A library declaring extras cannot have been scored on TRAIN without a pin.
    if (!lib.declared_extra.empty() || !cfg.train_fields_sha256.empty())
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN research fields pin differs");
    return co::Ok();
  }
  const auto& source=recipe.at("research_fields");
  if (!source.is_object() || !source.contains("manifest_sha256") || !source.at("manifest_sha256").is_object())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN research fields record");
  const auto& source_pins=source.at("manifest_sha256");
  Json pins=Json::object();
  if (source_pins.contains("train")) {
    const auto& train=source_pins.at("train");
    if (!train.is_string() || !hash_valid(train.get<std::string>()))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN research fields record");
    pins["train"]=train;
  }
  if (!cfg.train_fields_sha256.empty() && (!pins.contains("train") || pins.at("train")!=cfg.train_fields_sha256))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN research fields pin differs");
  if (source_pins.contains("validation")) pins["validation"]=cfg.validation_fields_sha256;
  expected["research_fields"]=fields_recipe(std::move(pins),lib);
  return co::Ok();
}
// train_fields_sha: the frozen TRAIN fields manifest pin (empty: none).
struct FrozenTrain {
  Json artifact,recipe; std::vector<int> signs; std::string recipe_sha; std::string train_fields_sha;
};
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
  // T14 ruling (strict M1): a blend frozen WITH pinned weights is never a validation
  // source. Its artifact hashes a recipe that pins the weights, whose provenance
  // names the orientations they were fit on, so that provenance can never name
  // this artifact. The shipped flow is: unweighted TRAIN run (orientations O) ->
  // fitter -> weights W (provenance names O) -> validation-only run with O + W,
  // which reproduces a weighted run's validation bytes exactly.
  if (recipe.contains("composition_weights_sha256"))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN artifact is a weighted run; validate "
        "from the unweighted TRAIN run whose orientations the weights were fit on (the weights' "
        "provenance.orientations_sha256)");
  auto source_cfg=cfg; source_cfg.max_working_bytes=static_cast<u64>(source_bytes);
  source_cfg.workers=static_cast<usize>(source_workers);
  source_cfg.save_combined=recipe.contains("saved_combined");
  source_cfg.composition_weights_sha256.clear();
  // Earlier completed TRAIN artifacts used parallel VM but serial IC. Absence
  // means precisely that original execution path, not unknown numerical policy.
  auto expected=method_recipe(source_cfg,recipe.contains("research_ic_workers"),false);
  expected["role_manifest_sha256"]["train"]=cfg.train_sha256;
  if (recipe.at("role_manifest_sha256").contains("validation"))
    expected["role_manifest_sha256"]["validation"]=cfg.validation_sha256;
  ATX_TRY_VOID(expect_frozen_fields(cfg,lib,recipe,expected));
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
  // expect_frozen_fields validated the record's shape and hash above.
  std::string train_fields_sha;
  if (recipe.contains("research_fields") && recipe.at("research_fields").at("manifest_sha256").contains("train"))
    train_fields_sha=recipe.at("research_fields").at("manifest_sha256").at("train").get<std::string>();
  return co::Ok(FrozenTrain{std::move(artifact),std::move(recipe),std::move(signs),recipe_sha,
                            std::move(train_fields_sha)});
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
// See FieldPlan. At most 64 candidates' worth of scans per field: O(n^2 f) with
// n <= 256 candidates and f <= 64 fields.
co::Result<FieldPlan> field_plan(const std::vector<Candidate>& candidates,const std::vector<std::string>& extras) {
  FieldPlan plan;
  if (extras.size()>64) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: at most 64 extra fields");
  const usize n=candidates.size(),f=extras.size();
  for (const auto& c:candidates) {
    u64 mask=0;
    for (const auto& name:c.extra_fields) {
      const auto at=std::lower_bound(extras.begin(),extras.end(),name);
      if (at==extras.end() || *at!=name) return co::Err(co::ErrorCode::Internal,"IC runner: field plan index");
      mask|=u64{1}<<static_cast<unsigned>(at-extras.begin());
    }
    plan.needs.push_back(mask); plan.capacity=std::max(plan.capacity,static_cast<usize>(std::popcount(mask)));
  }
  const auto next_use=[&](usize from,usize field) {
    for (usize t=from;t<n;++t) if ((plan.needs[t]>>field)&1U) return t;
    return n;
  };
  u64 resident=0;
  for (usize k=0;k<n;++k) {
    for (usize g=0;g<f;++g) if (((resident>>g)&1U) && next_use(k,g)==n) resident&=~(u64{1}<<g);
    for (usize g=0;g<f;++g) {
      if (!((plan.needs[k]>>g)&1U) || ((resident>>g)&1U)) continue;
      if (static_cast<usize>(std::popcount(resident))>=plan.capacity) {
        // A victim exists: fewer than `capacity` needed fields are resident yet.
        usize victim=f,farthest=0;
        for (usize h=0;h<f;++h) {
          if (!((resident>>h)&1U) || ((plan.needs[k]>>h)&1U)) continue;
          const auto t=next_use(k,h);
          if (victim==f || t>farthest) { victim=h; farthest=t; }
        }
        if (victim==f) return co::Err(co::ErrorCode::Internal,"IC runner: field plan eviction");
        resident&=~(u64{1}<<victim);
      }
      resident|=u64{1}<<g; ++plan.loads;
    }
    plan.planned.push_back(resident);
  }
  return co::Ok(std::move(plan));
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
  std::set<std::string> fields,families,ids,expressions,used_families,referenced;
  // The role's three price/volume fields plus any plain-identifier extras; each
  // extra must be present in every scored role's pinned fields manifest.
  for (const auto& field:j.at("fields")) {
    const auto name=field.at("name").get<std::string>();
    if (!field_identifier(name) || !fields.insert(name).second)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: declared field contract: "+name);
    if (!base_field(name)) out.declared_extra.push_back(name);
  }
  for (const auto base:base_fields) if (!fields.contains(std::string(base)))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: declared field contract: missing "+std::string(base));
  std::sort(out.declared_extra.begin(),out.declared_extra.end());
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
    // The compiled field dictionary is exactly what the VM resolves by name.
    for (const auto& field:c.program.fields) {
      if (!fields.contains(field))
        return co::Err(co::ErrorCode::InvalidArgument,"IC runner: undeclared DSL field: "+field);
      if (!base_field(field)) c.extra_fields.push_back(field);
    }
    std::sort(c.extra_fields.begin(),c.extra_fields.end());
    c.extra_fields.erase(std::unique(c.extra_fields.begin(),c.extra_fields.end()),c.extra_fields.end());
    referenced.insert(c.extra_fields.begin(),c.extra_fields.end());
    out.max_slots=std::max(out.max_slots,static_cast<usize>(c.program.num_slots));
    out.lookback=std::max(out.lookback,static_cast<usize>(c.program.required_lookback));
    used_families.insert(c.family); out.candidates.push_back(std::move(c));
  }
  if (families!=used_families) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: empty declared family");
  out.extra_fields.assign(referenced.begin(),referenced.end());
  ATX_TRY(out.field_plan,field_plan(out.candidates,out.extra_fields));
  return co::Ok(std::move(out));
}
// `themes`: pinned within-theme redistribution themes (0: none, admission unchanged).
co::Result<Role> admit(const IcRunnerConfig& cfg,const Library& lib,std::string path,
                      std::string pin,std::string name,bool enforce_budget=true,usize themes=0) {
  ATX_TRY(auto j,pinned_json(path,pin));
  const auto d=j.at("dates").get<u64>(),n=j.at("instruments").get<u64>();
  const auto begin=j.at("score_begin").get<u64>(),end=j.at("score_end").get<u64>();
  if (!d || d>4096 || !n || n>20000 || end!=d || begin>=end || begin<383 ||
      lib.lookback>begin-63)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: role shape/warmup/maturity");
  const auto cells=d*n,score_dates=end-begin;
  ATX_TRY(auto composition,ic_composition_working_bytes(static_cast<usize>(d),
      static_cast<usize>(n),lib.candidates.size(),themes));
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
  // Extra fields: at most field_plan.capacity columns are ever resident (8B/cell
  // each; FieldResidency enforces it), plus the 1B/cell owned presence mask of the
  // borrowed DSL panel (base columns are borrowed, never copied). Nothing is added
  // without extras, so default admission is unchanged.
  if (!lib.extra_fields.empty() && !b.add(cells,8*static_cast<u64>(lib.field_plan.capacity)+1))
    return co::Err(co::ErrorCode::Unavailable,"IC runner: research field memory budget");
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
  // Pooled composition adds one 16B/name ranked row per worker (T15).
  if (cfg.workers>1 && (!b.add(cfg.workers,(8ULL<<20)+(64ULL<<10)) ||
      !b.add(cfg.workers*n,1024+64+16) || !b.add(cfg.workers*d,64)))
    return co::Err(co::ErrorCode::OutOfRange,"IC runner: worker scratch/stack envelope overflow");
  if (enforce_budget && b.used>cfg.max_working_bytes)
    return co::Err(co::ErrorCode::Unavailable,"IC runner: required_bytes="+std::to_string(b.used)+
        " max_compiled_slots="+std::to_string(lib.max_slots)+" exceeds configured memory budget before payload load");
  return co::Ok(Role{std::move(path),std::move(pin),std::move(name),std::move(j),b.used,RoleFields{}});
}
// The producer flags every field entry with point_in_time (bool) and
// non_pit_aspects (strings, empty iff point in time). An entry without both, or
// with a contradictory pair, refuses the whole manifest: absence cannot be shown
// safe (pre-flag manifests listed look-ahead fields unmarked). Returns the aspects
// comma-joined, empty for a point-in-time field.
co::Result<std::string> non_pit_aspects(const Json& row,const std::string& where) {
  if (!row.contains("point_in_time") || !row.at("point_in_time").is_boolean() ||
      !row.contains("non_pit_aspects") || !row.at("non_pit_aspects").is_array())
    return co::Err(co::ErrorCode::InvalidArgument,where+" lacks point_in_time/non_pit_aspects flags");
  std::string aspects;
  for (const auto& aspect:row.at("non_pit_aspects")) {
    if (!aspect.is_string() || aspect.get<std::string>().empty())
      return co::Err(co::ErrorCode::InvalidArgument,where+" has a malformed non_pit_aspects entry");
    aspects+=(aspects.empty()?"":",")+aspect.get<std::string>();
  }
  if (row.at("point_in_time").get<bool>()==!aspects.empty())
    return co::Err(co::ErrorCode::InvalidArgument,where+" point_in_time contradicts non_pit_aspects");
  return co::Ok(std::move(aspects));
}
// The producer's spec-level meaning of a field, identical across roles written by
// one producer version: not its role-specific sources, coverage or counts.
Json field_definition(const Json& row) {
  Json out=Json::object();
  for (const auto* key:{"units","clock","staleness","source_columns","definition","point_in_time","non_pit_aspects"})
    out[key]=row.contains(key)?row.at(key):Json(nullptr);
  Json domain=nullptr;
  if (row.contains("plausibility") && row.at("plausibility").is_object()) {
    const auto& p=row.at("plausibility"); domain=Json::object();
    for (const auto* key:{"min","max","inclusive","rule"}) domain[key]=p.contains(key)?p.at(key):Json(nullptr);
  }
  out["plausibility"]=std::move(domain);
  return out;
}
// Review M5: with both manifests pinned, every declared extra must mean the same
// thing in TRAIN and validation (a different producer version would otherwise pass).
co::Status same_field_definitions(const Library& lib,const Role& train,const Role& validation) {
  if (train.fields.sha.empty() || validation.fields.sha.empty()) return co::Ok();
  for (const auto& name:lib.declared_extra)
    if (train.fields.declared.at(name)!=validation.fields.declared.at(name))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: research field '"+name+
          "' definition differs between the train and validation fields manifests");
  return co::Ok();
}
// Binds a pinned fields manifest to an admitted role before any payload: schema,
// role manifest pin, sessions/ids receipts and shape. Every declared extra must be
// present; referenced extras are selected for load and, for a scored role, their
// extents stat'ed. A declared extra with no pinned manifest refuses for a scored
// role; an unscored (frozen-TRAIN) role needs none.
co::Status bind_fields(const Library& lib,Role& role,const std::string& directory,const std::string& pin,
                       bool scored) {
  const auto option="--"+role.name+"-fields";
  if (directory.empty()) {
    if (scored && !lib.declared_extra.empty())
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: library field '"+lib.declared_extra.front()+
          "' is not a role price field and no "+option+" manifest is pinned");
    return co::Ok();
  }
  const auto dir=std::filesystem::path(directory);
  // The option names the fields DIRECTORY (its manifest.json is what the SHA pins);
  // a file path here would otherwise surface only as a missing DIR/manifest.json.
  if (std::error_code ec; !std::filesystem::is_directory(dir,ec))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: "+option+" must name the fields directory "
        "(the one holding manifest.json), not a file or missing path: "+directory);
  ATX_TRY(auto j,pinned_json((dir/"manifest.json").string(),pin));
  const auto d=role.metadata.at("dates").get<u64>(),n=role.metadata.at("instruments").get<u64>();
  const auto& receipts=role.metadata.at("files");
  if (!j.is_object() || j.value("schema",std::string{})!=fields_schema ||
      j.value("status",std::string{})!="complete" || !j.contains("role") || !j.at("role").is_object())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: "+role.name+" fields manifest schema");
  const auto& bound=j.at("role");
  if (bound.value("manifest_sha256",std::string{})!=role.sha ||
      receipts.at("sessions.i64").at("sha256")!=bound.value("sessions_sha256",std::string{}) ||
      receipts.at("ids.u64").at("sha256")!=bound.value("ids_sha256",std::string{}) ||
      !bound.contains("dates") || bound.at("dates")!=d || !bound.contains("instruments") ||
      bound.at("instruments")!=n)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: "+role.name+
        " fields manifest role binding differs from the pinned role manifest/axes");
  const auto& rows=j.at("fields"); const auto& files=j.at("files");
  if (!rows.is_array() || rows.empty() || rows.size()>64 || !files.is_object())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: "+role.name+" fields manifest field list");
  const auto bytes=d*n*sizeof(f64);
  std::map<std::string,FieldFile> available;
  for (const auto& row:rows) {
    const auto name=row.at("name").get<std::string>(),file=row.at("file").get<std::string>();
    const auto sha=row.at("sha256").get<std::string>();
    ATX_TRY(auto aspects,non_pit_aspects(row,"IC runner: "+role.name+" fields manifest entry "+name));
    if (!field_identifier(name) || base_field(name) || file!=name+".f64" || row.at("dtype")!="<f8" ||
        row.at("layout")!="date-major" || row.at("shape")!=Json::array({d,n}) || !hash_valid(sha) ||
        !files.contains(file) || files.at(file).at("sha256")!=sha || files.at(file).at("bytes")!=bytes ||
        !available.emplace(name,FieldFile{name,dir/file,sha,bytes,std::move(aspects),field_definition(row)}).second)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: "+role.name+" fields manifest entry: "+name);
  }
  for (const auto& name:lib.declared_extra) {
    const auto it=available.find(name);
    if (it==available.end())
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: library field '"+name+
          "' is neither a role price field nor in the pinned "+role.name+" fields manifest");
    // Look-ahead guard with no override: a non-point-in-time field is never
    // declared, so never loaded (referenced fields are a subset of declared).
    if (!it->second.non_pit_aspects.empty())
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: library field '"+name+
          "' is not point-in-time in the pinned "+role.name+" fields manifest (non_pit_aspects: "+
          it->second.non_pit_aspects+"); refusing look-ahead");
  }
  RoleFields bound_fields{directory,pin,{},{}};
  for (const auto& name:lib.declared_extra) bound_fields.declared[name]=available.at(name).definition;
  for (const auto& name:lib.extra_fields) {
    const auto& field=available.at(name);
    std::error_code ec; const auto size=std::filesystem::file_size(field.path,ec);
    if (scored && (ec || size!=field.bytes))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: research field payload extent: "+
          field.path.filename().string());
    bound_fields.load.push_back(field);
  }
  role.fields=std::move(bound_fields);
  return co::Ok();
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
// Pinned per-candidate weights in library order (empty = default equal family/
// within-family weights) and, when the file carries `signs`, the blend sign per
// candidate (+1/-1; 0 only for an unsigned zero-weight candidate). Empty signs =
// the runner's TRAIN IC orientation.
// `provenance`: the file's provenance object (null when absent), bound to a frozen
// TRAIN artifact by frozen_weights_binding in validation-only mode.
// `themes`: per-candidate theme index of an ew-theme-v6 theme_redistribution block
// (empty: none; see composition_themes), `theme_count` its number of themes.
struct PinnedWeights {
  std::vector<f64> values; std::vector<int> signs; Json provenance; std::vector<usize> themes; usize theme_count{};
};
// Optional `signs`: id -> integer +1/-1, known ids only; every candidate with a
// positive weight must carry one (the weights were fit on so-oriented returns).
co::Result<std::vector<int>> composition_signs(const Json& j,const Library& lib,std::span<const f64> weights) {
  std::vector<int> signs;
  if (!j.contains("signs")) return co::Ok(std::move(signs));
  const auto& rows=j.at("signs");
  if (!rows.is_object())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition signs must be an object");
  std::set<std::string> ids;
  for (const auto& c:lib.candidates) ids.insert(c.id);
  for (auto it=rows.begin();it!=rows.end();++it) {
    if (!ids.contains(it.key()))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition sign for unknown candidate: "+it.key());
    if (!it->is_number_integer() || (it->get<i64>()!=1 && it->get<i64>()!=-1))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition sign must be +1 or -1: "+it.key());
  }
  signs.reserve(lib.candidates.size());
  for (usize k=0;k<lib.candidates.size();++k) {
    const auto it=rows.find(lib.candidates[k].id);
    if (it==rows.end() && weights[k]>0)
      return co::Err(co::ErrorCode::InvalidArgument,
          "IC runner: composition sign missing for weighted candidate: "+lib.candidates[k].id);
    signs.push_back(it==rows.end()?0:static_cast<int>(it->get<i64>()));
  }
  return co::Ok(std::move(signs));
}
// Optional top-level `theme_redistribution` (fitter ew-theme-v6, v4-prereg v6 revision
// V6-W): exactly {"rule":"within-theme-v1","composition":"ew-theme-v6","themes":{id:
// theme}} with known ids, names [a-z0-9_]{1,64}, a theme for every positive-weight
// candidate and 1..32 themes. Indices follow first appearance in library order; a
// zero-weight candidate keeps 0 (ignored by the composition). Absent: pinned.themes
// stays empty and nothing downstream changes. The block requires schema v2 and v2
// requires the block (checked by composition_weights).
co::Status composition_themes(const Json& j,const Library& lib,PinnedWeights& pinned) {
  if (!j.contains("theme_redistribution")) return co::Ok();
  const auto& block=j.at("theme_redistribution");
  if (!block.is_object() || !block.contains("rule") || block.at("rule")!=theme_redistribution_rule ||
      !block.contains("composition") || block.at("composition")!=theme_redistribution_composition ||
      !block.contains("themes") || !block.at("themes").is_object())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_redistribution must be {rule: within-theme-v1, "
        "composition: ew-theme-v6, themes: {id: theme}}");
  const auto& rows=block.at("themes");
  std::set<std::string> ids;
  for (const auto& c:lib.candidates) ids.insert(c.id);
  const auto theme_name=[](const std::string& s) {
    return !s.empty() && s.size()<=64 && std::all_of(s.begin(),s.end(),[](char c) {
      return (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_';
    });
  };
  for (auto it=rows.begin();it!=rows.end();++it) {
    if (!ids.contains(it.key()))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme for unknown candidate: "+it.key());
    if (!it->is_string() || !theme_name(it->get<std::string>()))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme name must match [a-z0-9_]{1,64}: "+it.key());
  }
  std::vector<std::string> names;
  std::vector<usize> index(lib.candidates.size(),0);
  for (usize k=0;k<lib.candidates.size();++k) {
    if (!(pinned.values[k]>0)) continue;
    const auto it=rows.find(lib.candidates[k].id);
    if (it==rows.end())
      return co::Err(co::ErrorCode::InvalidArgument,
          "IC runner: theme missing for weighted candidate: "+lib.candidates[k].id);
    const auto name=it->get<std::string>();
    const auto at=std::find(names.begin(),names.end(),name);
    index[k]=static_cast<usize>(at-names.begin());
    if (at==names.end()) names.push_back(name);
  }
  if (names.empty() || names.size()>32)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_redistribution needs 1..32 weighted themes");
  pinned.themes=std::move(index); pinned.theme_count=names.size();
  return co::Ok();
}
// Runs before any role payload load (including under --plan-only); every refusal
// is loud. Selection hygiene: the file must name the TRAIN role manifest it was
// fitted on (top-level train_manifest_sha256 == --train-sha256; in validation-only
// mode --train-sha256 is also the frozen TRAIN artifact's). Unknown keys are allowed.
co::Result<PinnedWeights> composition_weights(const IcRunnerConfig& cfg,const Library& lib) {
  PinnedWeights pinned;
  if (cfg.composition_weights_path.empty()) return co::Ok(std::move(pinned));
  auto& weights=pinned.values;
  ATX_TRY(auto text,pinned_text(cfg.composition_weights_path,cfg.composition_weights_sha256));
  ATX_TRY(auto j,unique_key_json(text));
  if (!j.is_object() || !j.contains("schema") ||
      (j.at("schema")!=weights_schema && j.at("schema")!=weights_schema_v2) ||
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
  if (!j.contains("train_manifest_sha256") || !j.at("train_manifest_sha256").is_string() ||
      j.at("train_manifest_sha256")!=cfg.train_sha256)
    return co::Err(co::ErrorCode::InvalidArgument,
        "IC runner: composition weights TRAIN binding: train_manifest_sha256 must equal --train-sha256");
  ATX_TRY(pinned.signs,composition_signs(j,lib,weights));
  ATX_TRY_VOID(composition_themes(j,lib,pinned));
  const bool v2=j.at("schema")==weights_schema_v2;
  if (v2 && pinned.themes.empty())
    return co::Err(co::ErrorCode::InvalidArgument,
        "IC runner: composition weights schema atx.dsl-composition-weights/v2 requires a theme_redistribution block");
  if (!v2 && !pinned.themes.empty())
    return co::Err(co::ErrorCode::InvalidArgument,
        "IC runner: theme_redistribution requires composition weights schema atx.dsl-composition-weights/v2");
  if (j.contains("provenance")) pinned.provenance=j.at("provenance");
  return co::Ok(std::move(pinned));
}
// The plan/summary record of pinned weights: SHA, TRAIN binding, signs mode, the
// file's provenance pins (null when absent) and how they are tied to TRAIN.
Json weights_summary(const IcRunnerConfig& cfg,const PinnedWeights& pinned,const char* binding) {
  const auto& p=pinned.provenance;
  const auto pin=[&](const char* key) { return p.is_object() && p.contains(key)?p.at(key):Json(nullptr); };
  Json out{{"sha256",cfg.composition_weights_sha256},{"train_manifest_sha256",cfg.train_sha256},
      {"signs",pinned.signs.empty()?"TRAIN-orientation-signs":"pinned-candidate-signs"},
      {"provenance_orientations_sha256",pin("orientations_sha256")},
      {"provenance_fields_manifest_sha256",pin("fields_manifest_sha256")},{"binding",binding}};
  // ew-theme-v6 only (absent otherwise): the pinned within-theme redistribution.
  if (!pinned.themes.empty()) out["redistribution"]=theme_redistribution_rule;
  return out;
}
// Review M1 (root ruling, strict): weights applied against a frozen TRAIN artifact
// must name it as the orientations they were fit on. The T11 fitter records
// provenance.orientations_sha256 (the orientations.json file SHA it consumed) and
// provenance.fields_manifest_sha256 (the TRAIN fields pin, or null). The first must
// equal --orientations-sha256, full stop (frozen_train already refused a weighted
// frozen source, whose artifact no provenance can name); the second must equal the
// frozen TRAIN fields pin (absent or null when TRAIN pinned none).
co::Result<Json> frozen_weights_binding(const IcRunnerConfig& cfg,const PinnedWeights& pinned,
                                        const FrozenTrain& frozen) {
  const auto& p=pinned.provenance;
  if (!p.is_object() || !p.contains("orientations_sha256") || !p.at("orientations_sha256").is_string() ||
      !hash_valid(p.at("orientations_sha256").get<std::string>()))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: validation-only composition weights need "
        "provenance.orientations_sha256 naming the frozen TRAIN orientations they were fit on");
  if (p.at("orientations_sha256")!=cfg.orientations_sha256)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition weights provenance.orientations_sha256 "
        "must equal the frozen TRAIN --orientations-sha256 (weights were fit on other orientations)");
  const Json expected_fields=frozen.train_fields_sha.empty()?Json(nullptr):Json(frozen.train_fields_sha);
  const Json recorded_fields=p.contains("fields_manifest_sha256")?p.at("fields_manifest_sha256"):Json(nullptr);
  if (recorded_fields!=expected_fields)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition weights provenance.fields_manifest_sha256 "
        "must equal the frozen TRAIN fields manifest pin (null when TRAIN pinned none)");
  return co::Ok(weights_summary(cfg,pinned,"provenance-orientations-equal-frozen-TRAIN-orientations-artifact"));
}
// Review N3: validation-only runs compare the validation fields manifest with the
// frozen TRAIN definitions even without --train-fields. The TRAIN artifact records
// them (orientations.json research_fields.definitions); an older artifact without
// that record needs --train-fields, whose manifest same_field_definitions compared.
// Returns how the check was satisfied (empty: no declared extras).
co::Result<std::string> frozen_field_definitions(const Library& lib,const FrozenTrain& frozen,const Role& train,
                                                 const Role& validation) {
  if (lib.declared_extra.empty()) return co::Ok(std::string{});
  const auto& artifact=frozen.artifact;
  if (!artifact.contains("research_fields")) {
    if (train.fields.sha.empty())
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN artifact records no research field "
          "definitions; pass --train-fields/--train-fields-sha256 (the frozen TRAIN pin) so the validation "
          "field definitions can be checked");
    return co::Ok(std::string("train-fields-manifest"));
  }
  const auto& record=artifact.at("research_fields");
  if (!record.is_object() || !record.contains("manifest_sha256") || record.at("manifest_sha256")!=frozen.train_fields_sha ||
      !record.contains("definitions") || !record.at("definitions").is_object())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN artifact research field record differs "
        "from its recipe pin");
  const auto& definitions=record.at("definitions");
  for (const auto& name:lib.declared_extra)
    if (!definitions.contains(name) || definitions.at(name)!=validation.fields.declared.at(name))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: research field '"+name+
          "' definition differs between the frozen TRAIN artifact and the validation fields manifest");
  return co::Ok(std::string("frozen-TRAIN-artifact"));
}
// ---- Candidate signal cache: ROOT/<role-manifest-sha256>/<id>.{f64,json} ----
// ROOT is DIR/<vm identity>/ (DIR itself for the legacy identity), so a VM or
// build-flavor change is a clean miss; the sidecar's vm_identity must match too.
// Within ROOT the identity is (DSL sha, role manifest sha, geometry) plus a
// streamed payload SHA256. Library, role name, source and engine sha are recorded
// only, so a grown library reuses unchanged candidates. vm_workers is recorded,
// not matched: DetPool column/row parallelism is bit-identical to the serial VM
// (vm.hpp S3-3 contract), pinned on raw payload bytes by the worker-parity fixture.
// A candidate whose DSL reads any extra field lives in ROOT/<fields-manifest-sha256>/
// instead (that manifest pins exactly one role manifest) and its sidecar must name
// that manifest, so a changed field payload is a clean miss, never a stale hit.
struct CacheKey {
  std::filesystem::path dir; std::string role_sha; u64 dates{},instruments{}; std::string fields_sha,vm_identity;
};
std::filesystem::path cache_root(const IcRunnerConfig& cfg) {
  const auto root=std::filesystem::path(cfg.candidate_cache_directory); const auto identity=vm_identity();
  return identity==legacy_vm_identity?root:root/identity;
}
CacheKey cache_key(const IcRunnerConfig& cfg,const Role& role,const Candidate& c) {
  const bool fields=!c.extra_fields.empty();
  return CacheKey{cache_root(cfg)/(fields?role.fields.sha:role.sha),role.sha,
      role.metadata.at("dates").get<u64>(),role.metadata.at("instruments").get<u64>(),
      fields?role.fields.sha:std::string{},vm_identity()};
}
// Keyless sidecars predate vm_identity: only base entries recorded by the
// verified legacy builds, read under the legacy identity, are accepted.
bool legacy_entry(const Json& j,const CacheKey& key) {
  if (key.vm_identity!=legacy_vm_identity || !key.fields_sha.empty() || !j.contains("engine_git_sha") ||
      !j.at("engine_git_sha").is_string())
    return false;
  const auto engine=j.at("engine_git_sha").get<std::string>();
  return std::find(legacy_engine_shas.begin(),legacy_engine_shas.end(),engine)!=legacy_engine_shas.end();
}
// Every referenced extra field is bound for a scored role (bind_fields refuses
// otherwise); re-asserted before any key or panel is derived from it.
co::Status fields_bound(const Library& lib,const Role& role) {
  if (!lib.extra_fields.empty() && (role.fields.sha.empty() || role.fields.load.size()!=lib.extra_fields.size()))
    return co::Err(co::ErrorCode::Internal,"IC runner: research fields not bound for role "+role.name);
  return co::Ok();
}
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
  if constexpr (vm_compiler.empty())
    return co::Err(co::ErrorCode::Unavailable,"IC runner: candidate cache requires a known VM build identity");
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
  // A base entry never names a fields manifest; a field entry names exactly ours.
  const bool fields_match=key.fields_sha.empty()?!j.contains("fields_manifest_sha256")
                                                :text("fields_manifest_sha256")==key.fields_sha;
  // Foreign bytes in this identity's directory are refused, never served.
  const bool vm_match=j.contains("vm_identity")?text("vm_identity")==key.vm_identity:legacy_entry(j,key);
  if (!j.is_object() || text("schema")!=cache_schema || text("candidate_id")!=c.id ||
      text("dsl_sha256")!=c.dsl_sha || text("role_manifest_sha256")!=key.role_sha ||
      text("eval_mode")!=vm_eval_mode || text("layout")!=cache_layout || count("dates")!=key.dates ||
      count("instruments")!=key.instruments || count("bytes")!=key.dates*key.instruments*sizeof(f64) ||
      !fields_match || !vm_match || !hash_valid(sha))
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
// Reads a pinned date-major f64 payload into `out` (reused; no second copy),
// hashing each chunk as it lands. `what` names the payload kind in refusals.
co::Status load_pinned_f64(const std::filesystem::path& path,const std::string& expected,usize cells,
                           std::vector<f64>& out,std::string_view what) {
  const auto bytes=static_cast<u64>(cells)*sizeof(f64);
  const auto label=std::string("IC runner: ")+std::string(what); const auto name=path.filename().string();
  std::ifstream in(path,std::ios::binary|std::ios::ate);
  if (!in || in.tellg()<0 || static_cast<u64>(in.tellg())!=bytes)
    return co::Err(co::ErrorCode::InvalidArgument,label+" extent: "+name);
  in.seekg(0); out.resize(cells);
  const auto destination=std::as_writable_bytes(std::span(out)); co::Sha256 digest;
  for (usize offset=0;offset<destination.size();) {
    const auto chunk=destination.subspan(offset,std::min(io_chunk,destination.size()-offset));
    // SAFETY: char accesses the object representation of trivially copyable f64 storage.
    in.read(reinterpret_cast<char*>(chunk.data()),static_cast<std::streamsize>(chunk.size()));
    if (!in) return co::Err(co::ErrorCode::IoError,label+" truncated: "+name);
    ATX_TRY_VOID(digest.update(chunk)); offset+=chunk.size();
  }
  if (in.peek()!=std::char_traits<char>::eof())
    return co::Err(co::ErrorCode::IoError,label+" changed extent: "+name);
  ATX_TRY(auto actual,digest.finalize());
  if (hex(actual)!=expected)
    return co::Err(co::ErrorCode::InvalidArgument,label+" SHA256 mismatch: "+name);
  return co::Ok();
}
co::Status cache_load(const std::filesystem::path& path,const std::string& expected,usize cells,
                      std::vector<f64>& out) {
  return load_pinned_f64(path,expected,cells,out,"candidate cache payload");
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
    // SAFETY: char reads the object representation of the caller's byte span.
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
// Returns the committed payload SHA256 (the IC-result cache keys on it).
co::Result<std::string> cache_store(const CacheKey& key,const IcRunnerConfig& cfg,const Role& spec,
    const engine::data::StrategyRoleData& role,const Candidate& c,std::span<f64> signal) {
  if (signal.size()!=key.dates*key.instruments)
    return co::Err(co::ErrorCode::Internal,"IC runner: candidate cache geometry");
  for (auto& v:signal) if (!std::isfinite(v)) v=quiet_nan;
  ATX_TRY(auto sha,cache_store_payload(key.dir/(c.id+".f64"),std::as_bytes(signal)));
  Json sidecar{{"schema",cache_schema},{"candidate_id",c.id},{"family",c.family},
      {"dsl_sha256",c.dsl_sha},{"library_sha256",cfg.library_sha256},
      {"role_manifest_sha256",key.role_sha},{"role",spec.name},{"source_sha256",role.source_sha256},
      {"dates",key.dates},{"instruments",key.instruments},{"bytes",signal.size()*sizeof(f64)},
      {"layout",cache_layout},{"payload",c.id+".f64"},{"payload_sha256",sha},
      {"semantics","raw-unoriented-pre-composition-single-VM-root"},{"eval_mode",vm_eval_mode},
      {"vm_workers",cfg.workers},{"engine_git_sha",std::string(build_engine_git_sha())},
      {"vm_identity",key.vm_identity}};
  // Only field entries name a fields manifest; base entries never do.
  if (!key.fields_sha.empty()) {
    sidecar["fields_manifest_sha256"]=key.fields_sha; sidecar["research_fields"]=c.extra_fields;
  }
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
  return co::Ok(std::move(sha));
}
// Metadata-only readiness for --plan-only: sidecars are identity-checked and
// payload extents stat'ed; payload hashes are verified only when loaded.
co::Result<Json> cache_plan(const IcRunnerConfig& cfg,const Library& lib,const Role& role) {
  ATX_TRY_VOID(fields_bound(lib,role));
  usize ready=0;
  for (const auto& c:lib.candidates) {
    const auto key=cache_key(cfg,role,c);
    ATX_TRY(auto sha,cache_lookup(key,c));
    if (!sha) continue;
    std::error_code ec; const auto size=std::filesystem::file_size(key.dir/(c.id+".f64"),ec);
    if (ec || size!=key.dates*key.instruments*sizeof(f64))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate cache payload extent: "+c.id+".f64");
    ++ready;
  }
  Json plan{{"role",role.name},{"directory",(cache_root(cfg)/role.sha).string()},
      {"ready_entries",ready},{"candidates",lib.candidates.size()},{"vm_identity",vm_identity()}};
  if (!lib.extra_fields.empty()) plan["fields_directory"]=(cache_root(cfg)/role.fields.sha).string();
  return co::Ok(std::move(plan));
}
void release(std::vector<f64>& buffer) noexcept { std::vector<f64>().swap(buffer); }
// Fail fast: every referenced field file is hashed (streamed, nothing retained)
// before the role payload is opened, so a tampered field refuses first. Loads hash
// the bytes again as they land, so the VM reads exactly what was pinned.
co::Status verify_fields(const Role& spec) {
  for (const auto& field:spec.fields.load) {
    const auto name=field.path.filename().string();
    std::error_code ec; const auto size=std::filesystem::file_size(field.path,ec);
    if (ec || size!=field.bytes)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: research field payload extent: "+name);
    ATX_TRY(auto sha,co::sha256_file(field.path.string()));
    if (sha!=field.sha)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: research field payload SHA256 mismatch: "+name);
  }
  return co::Ok();
}
// The DSL panel: the role's base columns and the resident extras, all BORROWED
// (no copy of any column), resolved by name. Only the 1B/cell presence mask is
// owned, copied from the role panel, so LoadField NaNs extras exactly where it
// NaNs the base fields. Must not outlive `base` or the extra columns.
co::Result<al::Panel> dsl_panel(const al::Panel& base,std::vector<std::string> extra_names,
                                std::vector<std::span<const f64>> extra_columns) {
  const auto d=base.dates(),n=base.instruments();
  std::vector<std::string> names; std::vector<std::span<const f64>> columns;
  names.reserve(base.num_fields()+extra_names.size()); columns.reserve(base.num_fields()+extra_names.size());
  for (usize f=0;f<base.num_fields();++f) {
    names.push_back(base.field_name(f)); columns.push_back(base.field_all(static_cast<al::FieldId>(f)));
  }
  for (usize k=0;k<extra_names.size();++k) {
    names.push_back(std::move(extra_names[k])); columns.push_back(extra_columns[k]);
  }
  std::vector<u8> presence(d*n);
  for (usize t=0;t<d;++t) for (usize i=0;i<n;++i) presence[t*n+i]=static_cast<u8>(base.in_universe(t,i));
  return al::Panel::create_borrowed(d,n,std::move(names),std::move(columns),std::move(presence));
}
// Runtime side of FieldPlan for one scored role. The resident set never leaves
// planned[k]: enter(k) drops what planned[k] excludes; panel_for(k) loads only the
// fields candidate k reads, and only when k needs the VM (a cache miss), so a warm
// run loads nothing. Any resident-set change destroys the VM first (it borrows
// the panel, which borrows the columns), so a live VM always matches panel_for's
// result. Borrows `lib` and `spec`; non-copyable because the panel aliases columns_.
class FieldResidency {
public:
  FieldResidency(const Library& lib,const Role& spec,usize cells)
      : lib_{lib},spec_{spec},cells_{cells},columns_(lib.extra_fields.size()) {}
  FieldResidency(const FieldResidency&)=delete;
  FieldResidency& operator=(const FieldResidency&)=delete;
  FieldResidency(FieldResidency&&)=delete;
  FieldResidency& operator=(FieldResidency&&)=delete;
  void enter(usize k,std::unique_ptr<al::Engine>& vm,std::ostream& progress);
  [[nodiscard]] co::Result<const al::Panel*> panel_for(usize k,const al::Panel& base,const std::string& candidate,
                                                       std::unique_ptr<al::Engine>& vm,std::ostream& progress);
  void drop_all(std::unique_ptr<al::Engine>& vm) noexcept;
  [[nodiscard]] usize loads() const noexcept { return loads_; }
  [[nodiscard]] usize peak() const noexcept { return peak_; }
  [[nodiscard]] f64 seconds() const noexcept { return seconds_; }
private:
  const Library& lib_; const Role& spec_; usize cells_;
  std::vector<std::vector<f64>> columns_; // index = Library::extra_fields index; empty = not resident
  u64 resident_{};
  std::optional<al::Panel> panel_;
  usize loads_{},peak_{}; f64 seconds_{};
};
void FieldResidency::enter(usize k,std::unique_ptr<al::Engine>& vm,std::ostream& progress) {
  const u64 drop=resident_&~lib_.field_plan.planned[k];
  if (!drop) return;
  vm.reset(); panel_.reset();
  for (usize f=0;f<columns_.size();++f) if ((drop>>f)&1U) {
    release(columns_[f]);
    progress<<"IC field-release role="<<spec_.name<<" field="<<lib_.extra_fields[f]<<'\n';
  }
  resident_&=~drop; progress<<std::flush;
}
co::Result<const al::Panel*> FieldResidency::panel_for(usize k,const al::Panel& base,const std::string& candidate,
    std::unique_ptr<al::Engine>& vm,std::ostream& progress) {
  const u64 missing=lib_.field_plan.needs[k]&~resident_;
  if (missing) {
    vm.reset(); panel_.reset();
    for (usize f=0;f<columns_.size();++f) {
      if (!((missing>>f)&1U)) continue;
      const auto& field=spec_.fields.load[f]; const auto started=std::chrono::steady_clock::now();
      ATX_TRY_VOID(load_pinned_f64(field.path,field.sha,cells_,columns_[f],"research field payload"));
      if (std::any_of(columns_[f].begin(),columns_[f].end(),[](f64 v) { return std::isinf(v); }))
        return co::Err(co::ErrorCode::InvalidArgument,"IC runner: research field value is infinite: "+field.name);
      const auto seconds=std::chrono::duration<f64>(std::chrono::steady_clock::now()-started).count();
      seconds_+=seconds; ++loads_; resident_|=u64{1}<<f;
      progress<<"IC field-load role="<<spec_.name<<" field="<<field.name<<" candidate="<<candidate
              <<" seconds="<<seconds<<'\n'<<std::flush;
    }
    peak_=std::max(peak_,static_cast<usize>(std::popcount(resident_)));
  }
  const al::Panel* out=&base;
  if (resident_) {
    if (!panel_) {
      std::vector<std::string> names; std::vector<std::span<const f64>> columns;
      for (usize f=0;f<columns_.size();++f) if ((resident_>>f)&1U) {
        names.push_back(lib_.extra_fields[f]); columns.emplace_back(columns_[f]);
      }
      ATX_TRY(auto panel,dsl_panel(base,std::move(names),std::move(columns)));
      panel_.emplace(std::move(panel));
    }
    out=&*panel_;
  }
  return co::Ok(out);
}
void FieldResidency::drop_all(std::unique_ptr<al::Engine>& vm) noexcept {
  vm.reset(); panel_.reset();
  for (auto& column:columns_) release(column);
  resident_=0;
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
// The daily IC series of one evaluation per active horizon (5, 21, 63): borrowed
// from the IC scratch right after evaluate_research_ic, or from a verified
// cached record (CachedIc). Valid until the next evaluation or record reuse.
struct IcSeries { std::array<std::span<const f64>,3> pearson,rank; };
IcSeries scratch_series(const ex::ResearchIcScratch& scratch) {
  IcSeries out;
  for (usize k=0;k<out.rank.size();++k) {
    out.pearson[k]=scratch.pearson_series(k); out.rank[k]=scratch.rank_series(k);
  }
  return out;
}
// ---- IC-result cache: <signal entry dir>/ic<v>_<key16>/<id>.json (T15) ------
// One verified evaluate_research_ic result per candidate signal, so a warm pass
// skips IC scoring. An entry binds the exact signal bytes (the signal cache's
// payload SHA256), the candidate id and DSL SHA, and the scope key: the role
// manifest pin (panel close and presence), the content SHA256 of the exact
// membership and return-guard spans given to prepare_research_ic, every
// IcScreenConfig/option field that can change a bit, and ic_identity().
// max_cache_bytes and workers are not keyed: they only admit or schedule (the
// row kernels are per-date independent; HAC and classification run after the
// join), as the serial/parallel research IC fixtures pin. The directory carries
// the first 16 hex of SHA256(key.dump()) and the record the full key, so a
// prefix collision refuses instead of serving. Every f64 is stored as its IEEE
// bit pattern in hex (NaNs included) and the record is self-hashed, so a hit
// reproduces the evaluated struct and series exactly. Like the signal cache it is
// never a method input: the recipe and every output byte are unchanged by it.
// Memory: one entry at a time, at most 1 MiB of text (metadata_text) plus its
// parsed/decoded form, inside admission's fixed 32 MiB slack.
struct IcCacheScope { Json key; std::string directory; };
struct CachedIc { ex::ResearchIcResult result; std::array<std::vector<f64>,3> pearson,rank; };
IcSeries cached_series(const CachedIc& cached) {
  IcSeries out;
  for (usize k=0;k<out.rank.size();++k) { out.pearson[k]=cached.pearson[k]; out.rank[k]=cached.rank[k]; }
  return out;
}
std::string bits_hex(f64 value) {
  constexpr char digits[]="0123456789abcdef"; const auto bits=std::bit_cast<u64>(value);
  std::string out(16,'0');
  for (usize i=0;i<out.size();++i) out[i]=digits[(bits>>(60U-4U*i))&15U];
  return out;
}
// Exactly 16 lowercase hex digits, most significant first; anything else fails.
std::optional<u64> hex_bits(std::string_view text) {
  if (text.size()!=16) return std::nullopt;
  u64 bits=0;
  for (const char c:text) {
    u64 nibble=0;
    if (c>='0' && c<='9') nibble=static_cast<u64>(c-'0');
    else if (c>='a' && c<='f') nibble=static_cast<u64>(c-'a')+10U;
    else return std::nullopt;
    bits=(bits<<4U)|nibble;
  }
  return bits;
}
std::string series_hex(std::span<const f64> values) {
  std::string out; out.reserve(values.size()*16U);
  for (const auto v:values) out+=bits_hex(v);
  return out;
}
// Hashes the membership (6.5 MB) and guard (26 MB) spans once per scored role.
co::Result<IcCacheScope> ic_cache_scope(const Role& spec,const engine::data::StrategyRoleData& role,
    const ex::IcScreenConfig& ic,const ex::ResearchIcOptions& options,std::span<const u32> guard) {
  ATX_TRY(auto member_sha,co::sha256_hex(std::as_bytes(std::span<const u8>(role.decision_member))));
  ATX_TRY(auto guard_sha,co::sha256_hex(std::as_bytes(guard)));
  Json horizons=Json::array();
  for (const auto h:ic.horizons) horizons.push_back(h);
  Json key{{"semantics_version",static_cast<u64>(ic_result_semantics_version)},{"ic_identity",ic_identity()},
      {"role_manifest_sha256",spec.sha},{"dates",role.panel.dates()},{"instruments",role.panel.instruments()},
      {"rule",std::string(ex::ic_screen_rule_name(ic.rule))},{"horizons",std::move(horizons)},
      {"execution_delay",ic.execution_delay},{"window_begin",ic.window_begin},{"window_end",ic.window_end},
      {"maturity_end",ic.maturity_end},{"min_names",ic.min_names},{"min_dates",ic.min_dates},
      {"practical_abs_ic_bits",bits_hex(ic.practical_abs_ic)},
      {"confidence_multiplier_bits",bits_hex(ic.confidence_multiplier)},
      {"active_horizons",options.active_horizons},
      {"require_endpoint_presence",options.require_endpoint_presence},
      {"price_field",std::string(ic_price_field)},{"decision_member_sha256",std::move(member_sha)},
      {"return_guard_sha256",std::move(guard_sha)}};
  ATX_TRY(auto key_sha,co::sha256_hex(key.dump()));
  auto directory="ic"+std::to_string(ic_result_semantics_version)+"_"+key_sha.substr(0,16);
  return co::Ok(IcCacheScope{std::move(key),std::move(directory)});
}
Json estimate_record(const ex::IcScreenEstimate& e) {
  return {{"valid_dates",e.valid_dates},{"calendar_dates",e.calendar_dates},{"hac_lag",e.hac_lag},
      {"mean",bits_hex(e.mean)},{"standard_error",bits_hex(e.standard_error)},
      {"upper_abs_ic",bits_hex(e.upper_abs_ic)},{"max_segment_abs_ic",bits_hex(e.max_segment_abs_ic)},
      {"defined",e.defined},{"suggestive_direction",e.suggestive_direction}};
}
// Every field of the result (all four horizon/coverage slots) plus the active
// horizons' daily series; integers stay JSON integers, so dump() round-trips.
Json ic_result_record(const ex::ResearchIcResult& result,const IcSeries& daily) {
  Json horizons=Json::array(),coverage=Json::array(),pearson=Json::array(),rank=Json::array();
  for (const auto& h:result.screen.horizons)
    horizons.push_back({{"horizon",h.horizon},{"enough_evidence",h.enough_evidence},
        {"pearson",estimate_record(h.pearson)},{"rank",estimate_record(h.rank)}});
  for (const auto& c:result.coverage)
    coverage.push_back({{"mature_dates",c.mature_dates},{"structural_tail_dates",c.structural_tail_dates},
        {"decision_eligible_pairs",c.decision_eligible_pairs},{"finite_label_pairs",c.finite_label_pairs},
        {"paired_signal_pairs",c.paired_signal_pairs},{"missing_entry_pairs",c.missing_entry_pairs},
        {"missing_exit_pairs",c.missing_exit_pairs},{"guard_excluded_pairs",c.guard_excluded_pairs},
        {"invalid_price_pairs",c.invalid_price_pairs},{"nonfinite_return_pairs",c.nonfinite_return_pairs}});
  for (usize k=0;k<result.active_horizons && k<daily.rank.size();++k) {
    pearson.push_back(series_hex(daily.pearson[k])); rank.push_back(series_hex(daily.rank[k]));
  }
  return {{"active_horizons",result.active_horizons},{"reject",result.screen.reject},
      {"enough_evidence",result.screen.enough_evidence},{"reason",static_cast<u64>(result.screen.reason)},
      {"horizons",std::move(horizons)},{"coverage",std::move(coverage)},
      {"pearson_series",std::move(pearson)},{"rank_series",std::move(rank)}};
}
// Strict readers for a cached record: any absent or mistyped field clears `ok`.
struct IcRecordReader {
  bool ok{true};
  u64 count(const Json& j,const char* key) {
    if (!j.is_object() || !j.contains(key) || !j.at(key).is_number_unsigned()) { ok=false; return 0; }
    return j.at(key).get<u64>();
  }
  bool flag(const Json& j,const char* key) {
    if (!j.is_object() || !j.contains(key) || !j.at(key).is_boolean()) { ok=false; return false; }
    return j.at(key).get<bool>();
  }
  f64 real(const Json& j,const char* key) {
    if (!j.is_object() || !j.contains(key) || !j.at(key).is_string()) { ok=false; return 0; }
    const auto bits=hex_bits(j.at(key).get_ref<const std::string&>());
    if (!bits) { ok=false; return 0; }
    return std::bit_cast<f64>(*bits);
  }
  void estimate(const Json& j,ex::IcScreenEstimate& e) {
    e.valid_dates=static_cast<usize>(count(j,"valid_dates"));
    e.calendar_dates=static_cast<usize>(count(j,"calendar_dates"));
    e.hac_lag=static_cast<usize>(count(j,"hac_lag"));
    e.mean=real(j,"mean"); e.standard_error=real(j,"standard_error"); e.upper_abs_ic=real(j,"upper_abs_ic");
    e.max_segment_abs_ic=real(j,"max_segment_abs_ic");
    e.defined=flag(j,"defined"); e.suggestive_direction=flag(j,"suggestive_direction");
  }
  // Exactly `length` hex-encoded f64 words into `out`.
  void values(const Json& j,usize length,std::vector<f64>& out) {
    if (!j.is_string() || j.get_ref<const std::string&>().size()!=length*16U) { ok=false; return; }
    const std::string_view text=j.get_ref<const std::string&>();
    out.resize(length);
    for (usize i=0;i<length;++i) {
      const auto bits=hex_bits(text.substr(i*16U,16U));
      if (!bits) { ok=false; return; }
      out[i]=std::bit_cast<f64>(*bits);
    }
  }
};
// `scratch` supplies this role's series lengths; `active` the configured horizons.
std::optional<CachedIc> decode_ic_record(const Json& r,const ex::ResearchIcScratch& scratch,usize active) {
  IcRecordReader in; CachedIc out; auto& result=out.result;
  const auto sized=[&](const char* key,usize n) {
    return r.is_object() && r.contains(key) && r.at(key).is_array() && r.at(key).size()==n;
  };
  const auto recorded=in.count(r,"active_horizons"),reason=in.count(r,"reason");
  if (!in.ok || recorded!=active || active>out.rank.size() ||
      reason>static_cast<u64>(ex::IcScreenReason::PracticalNull) || !sized("horizons",4) ||
      !sized("coverage",4) || !sized("pearson_series",active) || !sized("rank_series",active))
    return std::nullopt;
  result.active_horizons=active;
  result.screen.reject=in.flag(r,"reject"); result.screen.enough_evidence=in.flag(r,"enough_evidence");
  result.screen.reason=static_cast<ex::IcScreenReason>(static_cast<u8>(reason));
  for (usize h=0;h<result.screen.horizons.size();++h) {
    const auto& row=r.at("horizons").at(h); auto& dst=result.screen.horizons[h];
    if (!row.is_object() || !row.contains("pearson") || !row.contains("rank")) return std::nullopt;
    dst.horizon=static_cast<usize>(in.count(row,"horizon"));
    dst.enough_evidence=in.flag(row,"enough_evidence");
    in.estimate(row.at("pearson"),dst.pearson); in.estimate(row.at("rank"),dst.rank);
    const auto& c=r.at("coverage").at(h); auto& cov=result.coverage[h];
    cov.mature_dates=static_cast<usize>(in.count(c,"mature_dates"));
    cov.structural_tail_dates=static_cast<usize>(in.count(c,"structural_tail_dates"));
    cov.decision_eligible_pairs=in.count(c,"decision_eligible_pairs");
    cov.finite_label_pairs=in.count(c,"finite_label_pairs");
    cov.paired_signal_pairs=in.count(c,"paired_signal_pairs");
    cov.missing_entry_pairs=in.count(c,"missing_entry_pairs");
    cov.missing_exit_pairs=in.count(c,"missing_exit_pairs");
    cov.guard_excluded_pairs=in.count(c,"guard_excluded_pairs");
    cov.invalid_price_pairs=in.count(c,"invalid_price_pairs");
    cov.nonfinite_return_pairs=in.count(c,"nonfinite_return_pairs");
  }
  for (usize k=0;k<active;++k) {
    in.values(r.at("pearson_series").at(k),scratch.pearson_series(k).size(),out.pearson[k]);
    in.values(r.at("rank_series").at(k),scratch.rank_series(k).size(),out.rank[k]);
  }
  if (!in.ok) return std::nullopt;
  return out;
}
// nullopt: no entry (score, then store). A present entry must be intact and name
// exactly this candidate, signal bytes and scope key; anything else is a loud
// refusal, never a silent rescore over foreign bytes (the signal cache's policy).
co::Result<std::optional<CachedIc>> ic_cache_lookup(const std::filesystem::path& path,
    const IcCacheScope& scope,const Candidate& c,const std::string& signal_sha,
    const ex::ResearchIcScratch& scratch,usize active) {
  std::error_code ec; const bool present=std::filesystem::exists(path,ec);
  if (ec) return co::Err(co::ErrorCode::IoError,"IC runner: candidate IC cache probe: "+c.id);
  if (!present) return co::Ok(std::optional<CachedIc>{});
  ATX_TRY(auto text,metadata_text(path.string()));
  const auto j=Json::parse(text,nullptr,false);
  if (j.is_discarded() || !j.is_object() || !j.contains("schema") || !j.at("schema").is_string() ||
      j.at("schema").get<std::string>()!=ic_cache_schema || !j.contains("record") ||
      !j.at("record").is_object() || !j.contains("record_sha256") || !j.at("record_sha256").is_string())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate IC cache entry malformed: "+c.id);
  const auto& record=j.at("record");
  ATX_TRY(auto digest,co::sha256_hex(record.dump()));
  if (digest!=j.at("record_sha256").get<std::string>())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate IC cache entry integrity: "+c.id);
  const auto text_of=[&](const char* key) {
    return record.contains(key) && record.at(key).is_string()?record.at(key).get<std::string>():std::string{};
  };
  if (text_of("candidate_id")!=c.id || text_of("dsl_sha256")!=c.dsl_sha ||
      text_of("signal_payload_sha256")!=signal_sha || !record.contains("key") ||
      record.at("key")!=scope.key || !record.contains("result"))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate IC cache entry mismatch: "+c.id);
  auto cached=decode_ic_record(record.at("result"),scratch,active);
  if (!cached)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate IC cache entry malformed: "+c.id);
  return co::Ok(std::move(cached));
}
// No-replace publication; an entry that raced in first is accepted only if its
// record is identical. `recorded` values are provenance only, outside the hash.
co::Status ic_cache_store(const std::filesystem::path& path,const IcCacheScope& scope,const Candidate& c,
    const std::string& signal_sha,const ex::ResearchIcResult& result,const IcSeries& daily,usize workers) {
  std::error_code ec; std::filesystem::create_directories(path.parent_path(),ec);
  if (ec) return co::Err(co::ErrorCode::IoError,"IC runner: candidate IC cache directory: "+ec.message());
  const Json record{{"candidate_id",c.id},{"dsl_sha256",c.dsl_sha},{"signal_payload_sha256",signal_sha},
      {"key",scope.key},{"result",ic_result_record(result,daily)}};
  ATX_TRY(auto digest,co::sha256_hex(record.dump()));
  const Json entry{{"schema",ic_cache_schema},{"record_sha256",std::move(digest)},{"record",record},
      {"recorded",{{"engine_git_sha",std::string(build_engine_git_sha())},{"ic_workers",workers}}}};
  const auto text=entry.dump()+"\n";
  const PartialFile partial(path);
  ATX_TRY_VOID(write_partial(partial,std::as_bytes(std::span(text.data(),text.size()))));
  ATX_TRY(auto existed,publish_new(partial.path,path));
  if (existed) {
    ATX_TRY(auto committed,metadata_text(path.string()));
    const auto other=Json::parse(committed,nullptr,false);
    if (other.is_discarded() || !other.is_object() || !other.contains("record") || other.at("record")!=record)
      return co::Err(co::ErrorCode::AlreadyExists,
          "IC runner: candidate IC cache entry raced with different bytes: "+c.id);
  }
  return co::Ok();
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
// payload_sha: SHA256 of the exact signal bytes in `buffer` when the cache is on
// (a verified hit's pin, or the payload just committed); empty when it is off.
struct SignalTiming { f64 vm{},cache_load{},cache_write{}; bool hit{}; std::string payload_sha; };
// Leaves the candidate's raw (unoriented) signal in `buffer`: a verified cache
// hit, or a VM evaluation that is then committed to the cache when enabled.
// Only a miss asks `fields` for the panel (loading candidate k's extras if not
// resident): the role panel, or the borrowed DSL panel over the resident extras.
// Every Engine borrows it; `fields` destroys `vm` before changing it. Null cache = off.
co::Result<SignalTiming> candidate_signal(const IcRunnerConfig& cfg,const Role& spec,
    const engine::data::StrategyRoleData& role,FieldResidency& fields,usize k,const Candidate& candidate,
    const CacheKey* signal_cache,engine::parallel::DetPool* pool,
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
      out.cache_load=since(load_started); out.hit=true; out.payload_sha=std::move(*sha);
      progress<<"IC cache-hit "<<candidate.id<<" role="<<spec.name<<" seconds="<<out.cache_load<<'\n'<<std::flush;
      return co::Ok(std::move(out));
    }
    out.cache_load=since(load_started);
    progress<<"IC cache-miss "<<candidate.id<<" role="<<spec.name<<'\n'<<std::flush;
  }
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
    ATX_TRY(out.payload_sha,cache_store(*signal_cache,cfg,spec,role,candidate,buffer));
    out.cache_write=since(write_started);
    progress<<"IC cache-write "<<candidate.id<<" role="<<spec.name<<" seconds="<<out.cache_write<<'\n'<<std::flush;
  }
  return co::Ok(std::move(out));
}
// `blend_signs`: pinned per-candidate blend signs (empty = the TRAIN IC orientation).
// `themes`: pinned within-theme redistribution themes (empty = none; ew-theme-v6).
co::Result<Json> score_role(const IcRunnerConfig& cfg,const Library& lib,const Role& spec,
    std::span<const f64> weights,std::span<const int> blend_signs,std::vector<int>& signs,Json& frozen,
    const std::string& recipe_sha,const std::string& orientation_pin,std::ostream& progress,
    std::span<const usize> themes={}) {
  const auto started=std::chrono::steady_clock::now();
  progress<<"IC loading "<<spec.name<<" admitted_bytes="<<spec.bytes<<'\n'<<std::flush;
  ATX_TRY_VOID(fields_bound(lib,spec));
  // Referenced extra fields are hashed first, so a tampered field payload refuses
  // before the role payload is opened. None referenced: nothing is read.
  ATX_TRY_VOID(verify_fields(spec));
  const auto verify_seconds=std::chrono::duration<f64>(std::chrono::steady_clock::now()-started).count();
  if (!spec.fields.load.empty()) {
    progress<<"IC fields-verified role="<<spec.name<<" fields=";
    for (usize k=0;k<spec.fields.load.size();++k) progress<<(k?",":"")<<spec.fields.load[k].name;
    progress<<" resident_capacity="<<lib.field_plan.capacity<<" planned_loads="<<lib.field_plan.loads
            <<" seconds="<<verify_seconds<<'\n'<<std::flush;
  }
  // Fail fast (T1 review M2): every existing sidecar is identity-checked and its
  // payload extent stat'ed before the role loads, so a foreign or truncated entry
  // refuses now, not hours into the loop. Payload hashes are still verified on load.
  if (!cfg.candidate_cache_directory.empty()) {
    ATX_TRY(auto ready,cache_plan(cfg,lib,spec));
    progress<<"IC cache-preflight role="<<spec.name<<" ready="<<ready.at("ready_entries").get<usize>()
            <<'/'<<lib.candidates.size()<<'\n'<<std::flush;
  }
  const auto role_started=std::chrono::steady_clock::now();
  ATX_TRY(auto role,engine::data::read_strategy_role(spec.path,cfg.max_working_bytes));
  const auto load_seconds=std::chrono::duration<f64>(std::chrono::steady_clock::now()-role_started).count();
  if (role.manifest_sha256!=spec.sha)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: role manifest changed after admission");
  // Declared after `role` and before `pool`/`vm`: borrows the former and is
  // borrowed by the latter. Absent extras, the VM reads role.panel exactly as before.
  FieldResidency fields(lib,spec,role.panel.dates()*role.panel.instruments());
  ATX_TRY(auto guard,guard_for(role));
  auto ic=ex::equivalence_ic_screen_config();
  ic.horizons={5,21,63,0}; ic.min_names=cfg.min_names; ic.min_dates=cfg.min_dates;
  ic.window_begin=role.score_begin; ic.window_end=role.score_end; ic.maturity_end=role.score_end;
  ic.max_cache_bytes=cfg.max_working_bytes;
  const ex::ResearchIcOptions ic_options{3,true,cfg.workers};
  const auto label_started=std::chrono::steady_clock::now();
  ATX_TRY(auto cache,ex::prepare_research_ic(role.panel,ic,ic_options,role.decision_member,guard,
      ic_price_field));
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
  ATX_TRY(auto composition,IcComposition::create(cc,candidates,effective,weights,themes));
  // One key per candidate (empty = cache off): its role or fields directory.
  std::vector<CacheKey> cache_keys;
  if (!cfg.candidate_cache_directory.empty()) {
    cache_keys.reserve(lib.candidates.size());
    for (const auto& c:lib.candidates) {
      cache_keys.push_back(cache_key(cfg,spec,c));
      std::error_code ec; std::filesystem::create_directories(cache_keys.back().dir,ec);
      if (ec) return co::Err(co::ErrorCode::IoError,"IC runner: candidate cache directory: "+ec.message());
    }
  }
  const bool signal_cache=!cache_keys.empty();
  // IC-result cache: on exactly when the signal cache is (its entries key on the
  // signal payload SHA256); one scope per scored role, entries per candidate dir.
  std::optional<IcCacheScope> ic_scope; usize ic_hits=0;
  if (signal_cache) {
    ATX_TRY(auto scope,ic_cache_scope(spec,role,ic,ic_options,guard));
    ic_scope.emplace(std::move(scope));
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
    fields.enter(k,vm,progress);
    ATX_TRY(auto acquired,candidate_signal(cfg,spec,role,fields,k,candidate,
        signal_cache?&cache_keys[k]:nullptr,pool.get(),vm,signal_buffer,progress));
    const std::span<const f64> signal(signal_buffer);
    const auto vm_seconds=acquired.vm; total_vm_seconds+=vm_seconds;
    total_cache_load_seconds+=acquired.cache_load; total_cache_write_seconds+=acquired.cache_write;
    cache_hits+=acquired.hit?1U:0U;
    const auto ic_started=std::chrono::steady_clock::now();
    // A verified IC-result hit replaces evaluate_research_ic; a miss scores and,
    // with the cache on, commits that result keyed on the exact signal bytes.
    std::optional<CachedIc> cached_ic; std::filesystem::path ic_entry;
    if (ic_scope) {
      if (!hash_valid(acquired.payload_sha))
        return co::Err(co::ErrorCode::Internal,
            "IC runner: signal payload SHA256 missing for IC-result cache");
      ic_entry=cache_keys[k].dir/ic_scope->directory/(candidate.id+".json");
      ATX_TRY(cached_ic,ic_cache_lookup(ic_entry,*ic_scope,candidate,acquired.payload_sha,scratch,
          ic_options.active_horizons));
    }
    ex::ResearchIcResult scored; IcSeries daily_series;
    if (cached_ic) {
      scored=cached_ic->result; daily_series=cached_series(*cached_ic); ++ic_hits;
    } else {
      ATX_TRY(scored,ex::evaluate_research_ic(signal,cache,scratch,pool.get()));
      daily_series=scratch_series(scratch);
      if (ic_scope) {
        ATX_TRY_VOID(ic_cache_store(ic_entry,*ic_scope,candidate,acquired.payload_sha,scored,daily_series,
            cfg.workers));
      }
    }
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
    // Pinned signs orient the blend only; every IC diagnostic keeps `sign`.
    const int blend_sign=blend_signs.empty()?sign:blend_signs[k];
    const auto composition_started=std::chrono::steady_clock::now();
    ATX_TRY_VOID(composition.add(k,signal,blend_sign,pool.get()));
    const auto composition_seconds=std::chrono::duration<f64>(
        std::chrono::steady_clock::now()-composition_started).count();
    total_composition_seconds+=composition_seconds;
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
    summary["stage_seconds"]={{"vm",vm_seconds},{"ic",ic_seconds},{"composition",composition_seconds}};
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
  const auto finish_started=std::chrono::steady_clock::now();
  ATX_TRY(auto combined,composition.finish());
  total_composition_seconds+=std::chrono::duration<f64>(std::chrono::steady_clock::now()-finish_started).count();
  const auto combined_ic_started=std::chrono::steady_clock::now();
  ATX_TRY(auto combined_ic,ex::evaluate_research_ic(combined.signal,cache,scratch,pool.get()));
  total_ic_seconds+=std::chrono::duration<f64>(std::chrono::steady_clock::now()-combined_ic_started).count();
  series(daily,"__combined__",role,scratch_series(scratch),1);
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
    ATX_TRY(saved,save_combined_artifact(cfg,spec,role,combined.signal,effective,frozen,recipe_sha,orientation_pin,
        !blend_signs.empty(),!themes.empty()));
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
    result["candidate_cache"]={
        {"directory",(cache_root(cfg)/spec.sha).string()},{"vm_identity",vm_identity()},
        {"hits",cache_hits},{"misses",lib.candidates.size()-cache_hits},
        {"vm_evaluations",lib.candidates.size()-cache_hits}};
    if (!lib.extra_fields.empty())
      result["candidate_cache"]["fields_directory"]=(cache_root(cfg)/spec.fields.sha).string();
    // Entries live in <each candidate's signal directory>/<subdirectory>/<id>.json.
    result["candidate_cache"]["ic_results"]={{"subdirectory",ic_scope->directory},
        {"identity",ic_identity()},{"key",ic_scope->key},{"hits",ic_hits},
        {"misses",lib.candidates.size()-ic_hits}};
  }
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
IcCacheVmIdentity ic_cache_vm_identity() {
  IcCacheVmIdentity out{dsl_vm_semantics_version,vm_identity(),{},std::string(dsl_vm_sources_sha256)};
  for (const auto path:dsl_vm_sources) out.sources.emplace_back(path);
  return out;
}
IcCacheVmIdentity ic_result_cache_identity() {
  IcCacheVmIdentity out{ic_result_semantics_version,ic_identity(),{},std::string(ic_result_sources_sha256)};
  for (const auto path:ic_result_sources) out.sources.emplace_back(path);
  return out;
}
co::Status run_ic(const IcRunnerConfig& cfg,std::ostream& progress) {
  try {
    const bool validation_only=!cfg.orientations_path.empty();
    if ((!cfg.plan_only && cfg.output_directory.empty()) || cfg.max_working_bytes<(32ULL<<20) || cfg.max_working_bytes>(16ULL<<30) ||
        cfg.min_names<3 || cfg.min_dates<8 || cfg.min_dates>4096 || cfg.workers<1 || cfg.workers>4 ||
        cfg.validation_manifest.empty()!=cfg.validation_sha256.empty() ||
        cfg.orientations_path.empty()!=cfg.orientations_sha256.empty() ||
        cfg.composition_weights_path.empty()!=cfg.composition_weights_sha256.empty() ||
        cfg.train_fields_directory.empty()!=cfg.train_fields_sha256.empty() ||
        cfg.validation_fields_directory.empty()!=cfg.validation_fields_sha256.empty() ||
        (!cfg.validation_fields_directory.empty() && cfg.validation_manifest.empty()) ||
        (validation_only && cfg.validation_manifest.empty()))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: bounded config");
    ATX_TRY(auto lib,library(cfg));
    // Both new options are fully validated here, before any role payload.
    ATX_TRY(const auto pinned,composition_weights(cfg,lib));
    const bool pinned_signs=!pinned.signs.empty();
    ATX_TRY_VOID(cache_preflight(cfg,lib));
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
      if (pinned_signs) plan["composition_signs"]="pinned-candidate-signs";
      if (!weights_record.is_null()) plan["composition_weights"]=weights_record;
      if (!definitions_check.empty()) plan["research_field_definitions_checked_against"]=definitions_check;
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
          ATX_TRY(auto entry,cache_plan(cfg,lib,role)); plan["candidate_cache"].push_back(std::move(entry));
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
      auto scored=score_role(cfg,lib,role,pinned.values,pinned.signs,signs,orientations,recipe_sha,
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
      if (key=="--save-combined") { cfg.save_combined=true; continue; }
      if (key=="--help") {
        out<<"equity-strategy-ic --library JSON --library-sha256 SHA --train MANIFEST --train-sha256 SHA --output NEWDIR "
               "[--validation MANIFEST --validation-sha256 SHA --max-memory-mib N --min-names N --min-dates N --workers 1..4 --plan-only --save-combined] [--orientations TRAIN_ARTIFACT --orientations-sha256 SHA] "
               "[--candidate-cache DIR] [--composition-weights JSON --composition-weights-sha256 SHA] "
               "[--train-fields DIR --train-fields-sha256 SHA] [--validation-fields DIR --validation-fields-sha256 SHA]\n"
               "  --*-fields: atx.research-role-fields/v1 directory bound to that role; SHA pins DIR/manifest.json.\n"
               "  --candidate-cache: entries under DIR[/<vm-identity>]/<role-or-fields-sha>/; publication does not fsync;\n"
               "    killed writes leave inert .partial files (safe to delete); a foreign/corrupt entry refuses loudly.\n"
               "    It also caches IC results: <role-or-fields-sha>/ic<v>_<key>/<id>.json, keyed on signal bytes.\n"
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
