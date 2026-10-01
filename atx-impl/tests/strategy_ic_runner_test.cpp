#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/bytecode.hpp"
#include "strategy_ic_runner.hpp"

namespace {
using namespace atx;
using Json = nlohmann::json;
constexpr usize D = 480, N = 8;
constexpr i64 day = 86'400'000'000'000LL;
struct Directory {
  std::filesystem::path path;
  Directory() {
    static std::atomic<unsigned> sequence{};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned a = 0; a < 32; ++a) {
      auto candidate = std::filesystem::temp_directory_path() /
          ("atx-strategy-ic-runner-" + std::to_string(stamp) + "-" + std::to_string(sequence.fetch_add(1)));
      if (std::filesystem::create_directory(candidate)) { path = std::move(candidate); break; }
    }
  }
  ~Directory() { if (!path.empty()) { std::error_code ec; std::filesystem::remove_all(path, ec); } }
};
template<class T> bool payload(const std::filesystem::path& dir, Json& files, const char* name, const std::vector<T>& data) {
  std::ofstream f(dir / name, std::ios::binary); const auto bytes = std::as_bytes(std::span(data));
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); f.close();
  if (!f) return false;
  auto sha = core::sha256_file((dir / name).string()); if (!sha) return false;
  files[name] = {{"bytes", bytes.size()}, {"sha256", *sha}}; return true;
}
bool json_file(const std::filesystem::path& path, const Json& j, std::string& sha) {
  const auto text = j.dump(2) + "\n";
  std::ofstream out(path, std::ios::binary); out << text; out.close();
  auto digest = core::sha256_hex(text); if (!out || !digest) return false; sha = *digest; return true;
}
// `holes`: cells (d*N+i) where the role is not present (its price fields NaN).
bool role(const std::filesystem::path& dir, i64 first_day, std::string& sha, f64 direction=1,
          const std::vector<usize>& holes={}) {
  if (!std::filesystem::create_directory(dir)) return false;
  std::vector<i64> sessions(D); std::vector<u8> member(D * N, 0), present(D * N, 1);
  std::vector<f64> price(D * N), volume(D * N);
  for (usize d = 0; d < D; ++d) {
    sessions[d] = (first_day + static_cast<i64>(d)) * day;
    for (usize i = 0; i < N; ++i) {
      const auto drift=direction*(static_cast<f64>(i)-3.5)*.00002;
      price[d*N+i]=100*std::exp(drift*static_cast<f64>(d));
      volume[d*N+i]=1e8*static_cast<f64>(i+1);
      member[d * N + i] = static_cast<u8>(d >= 63);
    }
  }
  for (const auto k : holes) {
    present[k] = 0; price[k] = std::numeric_limits<f64>::quiet_NaN(); volume[k] = price[k];
  }
  Json files;
  if (!payload(dir, files, "sessions.i64", sessions) || !payload(dir, files, "ids.u64", std::vector<u64>{10,20,30,40,50,60,70,80}) ||
      !payload(dir, files, "member.u8", member) || !payload(dir, files, "present.u8", present) ||
      !payload(dir, files, "close.f64", price) || !payload(dir, files, "raw_close.f64", price) ||
      !payload(dir, files, "volume.f64", volume)) return false;
  const Json membership{{"rule", "research-prior63-usd-adv-topn-v1"}, {"top_n", N},
      {"lookback_sessions",63}, {"lag_sessions",1}, {"min_raw_price_exclusive",5}, {"min_adv_exclusive",5000000},
      {"ties","securityID-ascending"}, {"missing","complete-prior-calendar-window-required"}, {"common_stock_verified",false}};
  return json_file(dir / "manifest.json", {{"schema","atx.recent-research-role/v1"}, {"status","complete"},
      {"instrument_namespace","spiderrock.securityID"}, {"dates",D}, {"instruments",N}, {"score_begin",383}, {"score_end",D},
      {"score_start_ns",sessions[383]}, {"score_end_ns",sessions.back()+day}, {"source_sha256",std::string(64,'a')},
      {"membership_recipe",membership.dump()}, {"clock_recipe","modeled-session+22h-mark+23h-decision-v1"},
      {"close_basis","f64(raw-f32-close)*f64-cumulReturnFactor"}, {"volume_basis","raw-share-volume"},
      {"common_stock_verified",false}, {"historical_vintage_verified",false},
      {"declared_output_bytes",D*N*26+D*8+N*8}, {"files",files}}, sha);
}
bool fixture(Directory& dir,atx::impl::strategy::IcRunnerConfig& cfg) {
  if (dir.path.empty()) return false;
  cfg.library_path=(dir.path/"library.json").string();
  cfg.train_manifest=(dir.path/"train"/"manifest.json").string();
  cfg.validation_manifest=(dir.path/"validation"/"manifest.json").string();
  cfg.output_directory=(dir.path/"output").string(); cfg.min_names=3; cfg.min_dates=8;
  cfg.max_working_bytes=64ULL<<20;
  if (!role(dir.path/"train",17683,cfg.train_sha256) ||
      !role(dir.path/"validation",18200,cfg.validation_sha256,-1)) return false;
  return json_file(cfg.library_path,{{"schema","atx.dsl-ic-library/v1"},{"id","synthetic-ic-two"},
      {"fields",Json::array({{{"name","close"}},{{"name","raw_close"}},{{"name","volume"}}})},
      {"families",Json::array({{{"id","fixed_volume"}}})},
      {"candidates",Json::array({
          {{"id","volume_level"},{"family","fixed_volume"},{"dsl","volume"},
           {"sign_policy","train-rank-ic21"},{"horizons",{5,21,63}}},
          {{"id","volume_rank"},{"family","fixed_volume"},{"dsl","rank(volume)"},
           {"sign_policy","train-rank-ic21"},{"horizons",{5,21,63}}}})}},cfg.library_sha256);
}
Json read_json(const std::filesystem::path& path) { std::ifstream in(path); return Json::parse(in); }
template<class T> bool read_payload(const std::filesystem::path& path,std::vector<T>& data) {
  std::ifstream in(path,std::ios::binary);
  in.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size()*sizeof(T)));
  return static_cast<bool>(in) && in.peek()==std::char_traits<char>::eof();
}
TEST(StrategyIcRunner, FreezesTrainSignsAndReportsCombinedIcWithoutBookReturns) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  std::ostringstream progress; auto status=atx::impl::strategy::run_ic(cfg,progress);
  ASSERT_TRUE(status) << status.error().to_string();
  const auto summary=read_json(dir.path/"output"/"summary.json");
  EXPECT_EQ(summary.at("status"),"complete"); EXPECT_EQ(summary.at("full_book_evaluations"),0);
  ASSERT_EQ(summary.at("roles").size(),2U);
  const auto orientations=read_json(dir.path/"output"/"orientations.json");
  for (const auto& row:orientations.at("candidates")) EXPECT_EQ(row.at("sign"),1);
  const auto& train=summary.at("roles").at(0); const auto& val=summary.at("roles").at(1);
  EXPECT_EQ(train.at("candidate_evaluations"),2); EXPECT_EQ(val.at("candidate_evaluations"),2);
  EXPECT_GT(train.at("combined_ic").at("horizons").at(1).at("rank").at("mean").get<f64>(),.99);
  EXPECT_LT(val.at("combined_ic").at("horizons").at(1).at("rank").at("mean").get<f64>(),-.99);
  for (const auto& row:val.at("candidates")) EXPECT_EQ(row.at("frozen_train_sign"),1);
  const auto& long_h=train.at("combined_ic").at("horizons").at(2);
  EXPECT_EQ(long_h.at("coverage").at("mature_dates"),33);
  EXPECT_EQ(long_h.at("coverage").at("structural_tail_dates"),64);
  EXPECT_FALSE(long_h.at("rank").at("inference_defined").get<bool>());
  EXPECT_FALSE(train.at("combined_ic").at("reject").get<bool>());
  EXPECT_FALSE(train.at("planned_target_proxy").at("actual_trades_or_costs").get<bool>());
  EXPECT_NEAR(train.at("planned_target_proxy").at("deployment_turnover").get<f64>(),.25,1e-14);
  auto sha=core::sha256_file((dir.path/"output"/"orientations.json").string()); ASSERT_TRUE(sha);
  EXPECT_EQ(summary.at("orientations_artifact_sha256"),*sha);
  EXPECT_NE(progress.str().find("IC eval-start"),std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(dir.path/"output"/"train_combined.json"));
}
TEST(StrategyIcRunner, PlanOnlyPinsMetadataAndNeverLoadsPayload) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.plan_only=true;
  // Exact metadata pins remain intact; a payload is absent to prove no reader call.
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  std::ostringstream progress; EXPECT_TRUE(atx::impl::strategy::run_ic(cfg,progress));
  EXPECT_FALSE(std::filesystem::exists(dir.path/"output"));
  const auto plan=Json::parse(progress.str()); EXPECT_EQ(plan.at("candidate_count"),2);
  EXPECT_EQ(plan.at("candidates").size(),2U);
  EXPECT_GE(plan.at("max_compiled_slots").get<usize>(),1U);
  cfg.train_sha256=std::string(64,'0');
  EXPECT_FALSE(atx::impl::strategy::run_ic(cfg,progress));
}
TEST(StrategyIcRunner, InsufficientHorizonProducesNullRatherThanZeroMean) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  // Keep format geometry, move score start to leave only ten decision sessions.
  for (const auto& path:{cfg.train_manifest,cfg.validation_manifest}) {
    auto j=read_json(path); j["score_begin"]=470;
    j["score_start_ns"]=j.at("score_end_ns").get<i64>()-10*day;
    std::string pin; ASSERT_TRUE(json_file(path,j,pin));
    if (path==cfg.train_manifest) cfg.train_sha256=pin; else cfg.validation_sha256=pin;
  }
  std::ostringstream progress; auto status=atx::impl::strategy::run_ic(cfg,progress);
  ASSERT_TRUE(status) << status.error().to_string();
  const auto summary=read_json(dir.path/"output"/"summary.json");
  const auto& horizons=summary.at("roles").at(0).at("combined_ic").at("horizons");
  EXPECT_TRUE(horizons.at(1).at("rank").at("mean").is_null());
  EXPECT_TRUE(horizons.at(2).at("rank").at("mean").is_null());
  const auto orientations=read_json(dir.path/"output"/"orientations.json");
  for (const auto& row:orientations.at("candidates")) EXPECT_EQ(row.at("sign"),0);
}
TEST(StrategyIcRunner, SharedVmWorkersPreserveSignalsOrientationsAndPlannedTargets) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  std::ostringstream serial_progress;
  auto status=atx::impl::strategy::run_ic(cfg,serial_progress);
  ASSERT_TRUE(status) << status.error().to_string();
  cfg.workers=2; cfg.output_directory=(dir.path/"parallel").string();
  std::ostringstream parallel_progress;
  status=atx::impl::strategy::run_ic(cfg,parallel_progress);
  ASSERT_TRUE(status) << status.error().to_string();
  const auto serial=read_json(dir.path/"output"/"summary.json");
  const auto parallel=read_json(dir.path/"parallel"/"summary.json");
  EXPECT_NE(serial.at("recipe_sha256"),parallel.at("recipe_sha256"));
  auto parallel_recipe=read_json(dir.path/"parallel"/"recipe.json");
  EXPECT_EQ(parallel_recipe.at("vm_workers"),2);
  parallel_recipe.erase("vm_workers");
  EXPECT_EQ(parallel_recipe.at("research_ic_workers"),2);
  parallel_recipe.erase("research_ic_workers");
  EXPECT_EQ(read_json(dir.path/"output"/"recipe.json"),parallel_recipe);
  EXPECT_EQ(read_json(dir.path/"output"/"orientations.json").at("candidates"),
            read_json(dir.path/"parallel"/"orientations.json").at("candidates"));
  ASSERT_EQ(serial.at("roles").size(),parallel.at("roles").size());
  for (usize r=0;r<serial.at("roles").size();++r) {
    auto a=serial.at("roles").at(r); auto b=parallel.at("roles").at(r);
    EXPECT_EQ(a.at("workers"),1); EXPECT_EQ(b.at("workers"),2);
    EXPECT_GT(b.at("admitted_working_bytes").get<u64>(),a.at("admitted_working_bytes").get<u64>());
    for (const auto* key:{"load","label_preparation","vm","ic","composition"})
      EXPECT_GE(b.at("stage_seconds").at(key).get<f64>(),0);
    for (auto* role_result:{&a,&b}) {
      for (const auto* key:{"wall_seconds","stage_seconds","workers","admitted_working_bytes","ic_scratch_bytes"})
        role_result->erase(key);
      for (auto& candidate:role_result->at("candidates")) {
        candidate.erase("wall_seconds"); candidate.erase("stage_seconds");
      }
    }
    EXPECT_EQ(a,b); // Full candidate/combined IC, coverage and proxy diagnostics.
  }
  // Daily rank() uses the shared CS pool here; compare the emitted f64 values
  // and full calendar as bytes, not merely sign or rounded summary agreement.
  for (const auto* name:{"train_daily_ic.csv","validation_daily_ic.csv",
                         "train_planned_targets.csv","validation_planned_targets.csv"}) {
    auto a=core::sha256_file((dir.path/"output"/name).string()); ASSERT_TRUE(a);
    auto b=core::sha256_file((dir.path/"parallel"/name).string()); ASSERT_TRUE(b);
    EXPECT_EQ(*a,*b) << name;
  }
  EXPECT_NE(parallel_progress.str().find("IC VM-complete"),std::string::npos);
  EXPECT_NE(parallel_progress.str().find(" composition="),std::string::npos);
}
TEST(StrategyIcRunner, WorkerBoundsAndAdditionalMemoryAreAdmittedBeforePayload) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  cfg.plan_only=true;
  std::ostringstream serial_plan; ASSERT_TRUE(atx::impl::strategy::run_ic(cfg,serial_plan));
  cfg.workers=2;
  std::ostringstream parallel_plan; ASSERT_TRUE(atx::impl::strategy::run_ic(cfg,parallel_plan));
  const auto serial=Json::parse(serial_plan.str()); const auto parallel=Json::parse(parallel_plan.str());
  EXPECT_EQ(parallel.at("workers"),2);
  EXPECT_GT(parallel.at("roles").at(0).at("required_bytes").get<u64>(),
            serial.at("roles").at(0).at("required_bytes").get<u64>());
  // Invalid explicit counts refuse in configuration preflight, before the
  // deliberately missing payload or output-directory creation can be reached
  // (the bound is 16 since platform v8 B-2).
  cfg.plan_only=false;
  for (const usize workers:{usize{0},usize{17}}) {
    cfg.workers=workers; std::ostringstream progress;
    auto status=atx::impl::strategy::run_ic(cfg,progress);
    ASSERT_FALSE(status); EXPECT_NE(status.error().to_string().find("bounded config"),std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(dir.path/"output")); EXPECT_TRUE(progress.str().empty());
  }
}
TEST(StrategyIcRunner, FrozenTrainValidationMatchesUninterruptedWithoutTrainPayload) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  std::ostringstream progress; auto status=atx::impl::strategy::run_ic(cfg,progress);
  ASSERT_TRUE(status) << status.error().to_string();
  const auto original=read_json(dir.path/"output"/"summary.json");
  cfg.orientations_path=(dir.path/"output"/"orientations.json").string();
  auto pin=core::sha256_file(cfg.orientations_path); ASSERT_TRUE(pin); cfg.orientations_sha256=*pin;
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  cfg.output_directory=(dir.path/"resumed").string(); cfg.workers=2; cfg.save_combined=true;
  std::ostringstream resumed_progress; status=atx::impl::strategy::run_ic(cfg,resumed_progress);
  ASSERT_TRUE(status) << status.error().to_string();
  const auto resumed=read_json(dir.path/"resumed"/"summary.json");
  EXPECT_EQ(resumed.at("status"),"complete"); EXPECT_EQ(resumed.at("train_candidates_planned"),0);
  EXPECT_EQ(resumed.at("train_recipe_sha256"),original.at("recipe_sha256"));
  EXPECT_EQ(resumed.at("orientations_artifact_sha256"),cfg.orientations_sha256);
  ASSERT_EQ(resumed.at("roles").size(),1U);
  auto expected=original.at("roles").at(1); auto actual=resumed.at("roles").at(0);
  ASSERT_TRUE(actual.contains("combined_artifact")); actual.erase("combined_artifact");
  for (auto* row:{&expected,&actual}) {
    for (const auto* key:{"wall_seconds","stage_seconds","workers","admitted_working_bytes","ic_scratch_bytes"}) row->erase(key);
    for (auto& candidate:row->at("candidates")) {
      candidate.erase("wall_seconds"); candidate.erase("stage_seconds");
    }
  }
  EXPECT_EQ(actual,expected);
  for (const auto* name:{"validation_daily_ic.csv","validation_planned_targets.csv"}) {
    auto a=core::sha256_file((dir.path/"output"/name).string()); ASSERT_TRUE(a);
    auto b=core::sha256_file((dir.path/"resumed"/name).string()); ASSERT_TRUE(b); EXPECT_EQ(*a,*b);
  }
  EXPECT_FALSE(std::filesystem::exists(dir.path/"resumed"/"train_daily_ic.csv"));
  EXPECT_FALSE(std::filesystem::exists(dir.path/"resumed"/"orientations.json"));
  EXPECT_EQ(resumed_progress.str().find("IC loading train"),std::string::npos);
  const auto receipt=read_json(dir.path/"resumed"/"frozen_train_receipt.json");
  EXPECT_EQ(receipt.at("artifact"),read_json(cfg.orientations_path));
  EXPECT_EQ(read_json(dir.path/"resumed"/"recipe.json").at("frozen_train_recipe"),
            read_json(dir.path/"output"/"recipe.json"));
  EXPECT_EQ(read_json(dir.path/"resumed"/"validation_combined.json").at("orientations_artifact_sha256"),*pin);
  auto still_pinned=core::sha256_file(cfg.orientations_path); ASSERT_TRUE(still_pinned); EXPECT_EQ(*still_pinned,*pin);
}
TEST(StrategyIcRunner, FrozenTrainIdentityMethodAndSignsRefuseBeforeValidationPayload) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  std::ostringstream progress; auto status=atx::impl::strategy::run_ic(cfg,progress);
  ASSERT_TRUE(status) << status.error().to_string();
  const auto artifact=read_json(dir.path/"output"/"orientations.json");
  const auto recipe=read_json(dir.path/"output"/"recipe.json");
  ASSERT_TRUE(std::filesystem::create_directory(dir.path/"source"));
  std::string unused; ASSERT_TRUE(json_file(dir.path/"source"/"recipe.json",recipe,unused));
  cfg.orientations_path=(dir.path/"source"/"orientations.json").string();
  cfg.output_directory=(dir.path/"resumed").string();
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  ASSERT_TRUE(std::filesystem::remove(dir.path/"validation"/"close.f64"));
  for (int mutation=0;mutation<6;++mutation) {
    auto bad=artifact;
    if (mutation==0) bad["candidates"][0]["sign"]=-1;
    if (mutation==1) bad["candidates"][0]["dsl_sha256"]=std::string(64,'0');
    if (mutation==2) bad["candidates"][0]["id"]="different_id";
    if (mutation==3) bad["train_manifest_sha256"]=std::string(64,'0');
    if (mutation==4) bad["recipe_sha256"]=std::string(64,'0');
    if (mutation==5) bad["candidates"][0]["orientation_dates"]=100000;
    ASSERT_TRUE(json_file(cfg.orientations_path,bad,cfg.orientations_sha256));
    std::ostringstream attempt; status=atx::impl::strategy::run_ic(cfg,attempt);
    ASSERT_FALSE(status) << mutation;
    EXPECT_NE(status.error().to_string().find("frozen TRAIN"),std::string::npos) << mutation;
    EXPECT_TRUE(attempt.str().empty()); EXPECT_FALSE(std::filesystem::exists(dir.path/"resumed"));
  }
  // Even a consistently rehashed artifact cannot authorize a different method.
  auto wrong_recipe=recipe; wrong_recipe["orientation"]="validation-fit";
  ASSERT_TRUE(json_file(dir.path/"source"/"recipe.json",wrong_recipe,unused));
  auto digest=core::sha256_hex(wrong_recipe.dump()); ASSERT_TRUE(digest);
  auto repinned=artifact; repinned["recipe_sha256"]=*digest;
  ASSERT_TRUE(json_file(cfg.orientations_path,repinned,cfg.orientations_sha256));
  std::ostringstream attempt; status=atx::impl::strategy::run_ic(cfg,attempt);
  ASSERT_FALSE(status); EXPECT_NE(status.error().to_string().find("method/statistical"),std::string::npos);
  EXPECT_TRUE(attempt.str().empty()); EXPECT_FALSE(std::filesystem::exists(dir.path/"resumed"));
}
TEST(StrategyIcRunner, SavesExactPreTargetBlendSupportAndPinnedAxesOnlyWhenRequested) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.save_combined=true;
  std::ostringstream progress; auto status=atx::impl::strategy::run_ic(cfg,progress);
  ASSERT_TRUE(status) << status.error().to_string();
  const auto output=dir.path/"output"; const auto summary=read_json(output/"summary.json");
  const auto orientations=read_json(output/"orientations.json");
  auto orientation_hash=core::sha256_hex(orientations.at("candidates").dump()); ASSERT_TRUE(orientation_hash);
  EXPECT_EQ(read_json(output/"recipe.json").at("saved_combined"),"date-major-f64-with-explicit-support-v1");
  for (const auto& role_result:summary.at("roles")) {
    const auto name=role_result.at("role").get<std::string>(); const auto prefix=name+"_combined";
    const auto path=output/(prefix+".json"); const auto manifest=read_json(path);
    auto manifest_hash=core::sha256_file(path.string()); ASSERT_TRUE(manifest_hash);
    EXPECT_EQ(role_result.at("combined_artifact").at("manifest_sha256"),*manifest_hash);
    EXPECT_EQ(manifest.at("schema"),"atx.dsl-combined-signal/v1");
    EXPECT_EQ(manifest.at("dates"),D); EXPECT_EQ(manifest.at("instruments"),N);
    EXPECT_EQ(manifest.at("score_begin"),383); EXPECT_EQ(manifest.at("score_end"),D);
    EXPECT_EQ(manifest.at("role_manifest_sha256"),name=="train"?cfg.train_sha256:cfg.validation_sha256);
    EXPECT_EQ(manifest.at("library_sha256"),cfg.library_sha256);
    EXPECT_EQ(manifest.at("run_recipe_sha256"),summary.at("recipe_sha256"));
    EXPECT_EQ(manifest.at("orientation_candidates_sha256"),*orientation_hash);
    EXPECT_EQ(manifest.at("finite_cells"),(D-63)*N); EXPECT_EQ(manifest.at("member_cells"),(D-63)*N);
    for (auto it=manifest.at("files").begin();it!=manifest.at("files").end();++it) {
      const auto file=output/it.key(); auto hash=core::sha256_file(file.string()); ASSERT_TRUE(hash);
      EXPECT_EQ(it.value().at("sha256"),*hash);
      EXPECT_EQ(it.value().at("bytes"),std::filesystem::file_size(file));
    }
    std::vector<f64> signal(D*N); std::vector<u8> member(D*N),finite(D*N);
    std::vector<i64> sessions(D); std::vector<u64> ids(N);
    ASSERT_TRUE(read_payload(output/(prefix+".f64"),signal));
    ASSERT_TRUE(read_payload(output/(prefix+"_member.u8"),member));
    ASSERT_TRUE(read_payload(output/(prefix+"_finite.u8"),finite));
    ASSERT_TRUE(read_payload(output/(prefix+"_sessions.i64"),sessions));
    ASSERT_TRUE(read_payload(output/(prefix+"_ids.u64"),ids));
    const i64 first=name=="train"?17683:18200;
    for (usize d=0;d<D;++d) {
      ASSERT_EQ(sessions[d],(first+static_cast<i64>(d))*day);
      for (usize i=0;i<N;++i) {
        const auto k=d*N+i; ASSERT_EQ(member[k],static_cast<u8>(d>=63)); ASSERT_EQ(finite[k],member[k]);
        if (d<63) ASSERT_TRUE(std::isnan(signal[k]));
        // Two equally weighted monotone volume candidates have the same
        // centered rank. This scalar oracle is independent of saved-buffer code.
        else ASSERT_DOUBLE_EQ(signal[k],static_cast<f64>(i)/static_cast<f64>(N-1)-.5);
      }
    }
    for (usize i=0;i<N;++i) EXPECT_EQ(ids[i],10*(i+1));
  }
}
TEST(StrategyIcRunner, AscendingVmArenaDemandMatchesRetainedMaximumArena) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  auto lib=read_json(cfg.library_path);
  lib["candidates"].push_back({{"id","volume_compound"},{"family","fixed_volume"},
      {"dsl","rank((volume + raw_close) * (volume - raw_close))"},
      {"sign_policy","train-rank-ic21"},{"horizons",{5,21,63}}});
  ASSERT_TRUE(json_file(cfg.library_path,lib,cfg.library_sha256));
  cfg.save_combined=true;
  std::ostringstream ascending_progress; auto status=atx::impl::strategy::run_ic(cfg,ascending_progress);
  ASSERT_TRUE(status) << status.error().to_string();
  // The same expressions with the largest arena first provide the retained
  // maximum-capacity reference. All three have identical cross-sectional ranks
  // in this fixture, so their fixed1/3 blend also preserves accumulation bits.
  lib["candidates"]=Json::array({lib["candidates"][2],lib["candidates"][0],lib["candidates"][1]});
  ASSERT_TRUE(json_file(cfg.library_path,lib,cfg.library_sha256));
  cfg.output_directory=(dir.path/"maximum_first").string();
  std::ostringstream maximum_progress; status=atx::impl::strategy::run_ic(cfg,maximum_progress);
  ASSERT_TRUE(status) << status.error().to_string();
  const auto arena_count=[](const std::string& text) {
    usize count=0,pos=0;
    while ((pos=text.find("IC VM-arena",pos))!=std::string::npos) { ++count; ++pos; }
    return count;
  };
  EXPECT_GT(arena_count(ascending_progress.str()),2U); // At least one actual growth per role.
  EXPECT_EQ(arena_count(maximum_progress.str()),2U); // One fresh maximum arena per role.
  const auto a=read_json(dir.path/"output"/"summary.json");
  const auto b=read_json(dir.path/"maximum_first"/"summary.json");
  for (usize r=0;r<2;++r) {
    const auto& x=a.at("roles").at(r); const auto& y=b.at("roles").at(r);
    EXPECT_EQ(x.at("combined_ic"),y.at("combined_ic"));
    EXPECT_EQ(x.at("planned_target_proxy"),y.at("planned_target_proxy"));
    Json xm=Json::object(),ym=Json::object();
    for (auto row:x.at("candidates")) {
      row.erase("wall_seconds"); row.erase("stage_seconds"); xm[row.at("id").get<std::string>()]=row;
    }
    for (auto row:y.at("candidates")) {
      row.erase("wall_seconds"); row.erase("stage_seconds"); ym[row.at("id").get<std::string>()]=row;
    }
    EXPECT_EQ(xm,ym);
    const auto role=x.at("role").get<std::string>();
    for (const auto& name:{role+"_combined.f64",role+"_planned_targets.csv"}) {
      auto first=core::sha256_file((dir.path/"output"/name).string()); ASSERT_TRUE(first);
      auto second=core::sha256_file((dir.path/"maximum_first"/name).string()); ASSERT_TRUE(second);
      EXPECT_EQ(*first,*second);
    }
  }
}
// ---- Candidate signal cache and pinned composition weights (T1) ----
std::string file_sha(const std::filesystem::path& path) {
  auto sha=core::sha256_file(path.string()); return sha?*sha:std::string{};
}
std::vector<char> file_bytes(const std::filesystem::path& path) {
  std::ifstream in(path,std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
}
bool write_bytes(const std::filesystem::path& path,const std::vector<char>& bytes) {
  std::ofstream out(path,std::ios::binary|std::ios::trunc);
  out.write(bytes.data(),static_cast<std::streamsize>(bytes.size())); out.close();
  return static_cast<bool>(out);
}
bool text_file(const std::filesystem::path& path,const std::string& text,std::string& sha) {
  std::ofstream out(path,std::ios::binary|std::ios::trunc); out<<text; out.close();
  auto digest=core::sha256_hex(text); if (!out || !digest) return false; sha=*digest; return true;
}
struct Run { bool ok{}; std::string error,log; };
Run run_named(const Directory& dir,atx::impl::strategy::IcRunnerConfig& cfg,const std::string& name) {
  cfg.output_directory=(dir.path/name).string(); std::ostringstream progress;
  const auto status=atx::impl::strategy::run_ic(cfg,progress);
  return {static_cast<bool>(status),status?std::string{}:status.error().to_string(),progress.str()};
}
// Drops wall-clock, resource and cache bookkeeping only; every IC, sign, coverage
// and planned-target diagnostic stays in the comparison.
Json stable_role(Json result,bool keep_artifact=true) {
  for (const auto* key:{"wall_seconds","stage_seconds","workers","admitted_working_bytes",
                        "ic_scratch_bytes","candidate_cache","hash_seconds","verify_bytes"})
    result.erase(key);
  if (!keep_artifact) result.erase("combined_artifact");
  for (auto& candidate:result.at("candidates")) {
    candidate.erase("wall_seconds"); candidate.erase("stage_seconds"); candidate.erase("signal_cache");
    candidate.erase("ic_result_cache");
    candidate.erase("composition_weight"); candidate.erase("composition_sign");
  }
  return result;
}
// TRAIN-bound weights file (train_manifest_sha256 == --train-sha256); optional signs
// and fitter-style provenance (validation-only runs need its orientations_sha256).
bool pin_weights(const Directory& dir,atx::impl::strategy::IcRunnerConfig& cfg,const Json& weights,
                 const Json& signs=Json(),const std::string& name="weights.json",const Json& provenance=Json()) {
  cfg.composition_weights_path=(dir.path/name).string();
  Json file{{"schema","atx.dsl-composition-weights/v1"},{"library_sha256",cfg.library_sha256},
      {"train_manifest_sha256",cfg.train_sha256},{"weights",weights}};
  if (!signs.is_null()) file["signs"]=signs;
  if (!provenance.is_null()) file["provenance"]=provenance;
  return json_file(cfg.composition_weights_path,file,cfg.composition_weights_sha256);
}
// The VM-identity-scoped cache root a run used (DIR itself for the legacy identity).
std::filesystem::path cache_root(const std::filesystem::path& summary) {
  return std::filesystem::path(read_json(summary).at("roles").at(0).at("candidate_cache").at("directory")
      .get<std::string>()).parent_path();
}
// The v2 (content-keyed) file stem of a candidate: <id>.<first 16 hex of SHA256(dsl)>.
std::string v2_stem(const std::string& id,const std::string& dsl) {
  auto sha=core::sha256_hex(dsl); return sha?id+"."+sha->substr(0,16):std::string{};
}
// The fixture library's base candidates.
std::string level_stem() { return v2_stem("volume_level","volume"); }
std::string rank_stem() { return v2_stem("volume_rank","rank(volume)"); }
// The entry that served or stored `id` for `role_name` in a run, from its summary's
// candidate_cache.entries (either layout).
struct CacheEntryPaths { std::filesystem::path sidecar,payload; std::string layout; };
CacheEntryPaths cache_entry(const std::filesystem::path& summary,const std::string& role_name,
                            const std::string& id) {
  const auto j=read_json(summary);
  for (const auto& role_result:j.at("roles")) {
    if (role_result.at("role")!=role_name) continue;
    for (const auto& e:role_result.at("candidate_cache").at("entries"))
      if (e.at("id")==id)
        return {e.at("sidecar").get<std::string>(),e.at("payload").get<std::string>(),e.at("layout").get<std::string>()};
  }
  ADD_FAILURE() << "no cache entry for " << id << " in role " << role_name << " of " << summary;
  return {};
}
// Rewrites a v2 entry as the v1 runner wrote it -- <v1_dir>/<id>.{f64,json},
// schema v1, no content-key records -- and removes the v2 files. `fields_sha`
// (field entries only) is the fields manifest the v1 layout keyed it under.
bool to_v1(const CacheEntryPaths& v2,const std::filesystem::path& v1_dir,const std::string& id,
           const std::string& fields_sha={}) {
  auto sidecar=read_json(v2.sidecar);
  const auto fields=sidecar.at("field_payload_sha256");
  sidecar["schema"]="atx.dsl-candidate-signal/v1"; sidecar["payload"]=id+".f64";
  for (const auto* key:{"field_payload_sha256","signal_key_sha256","fields_manifest_sha256"}) sidecar.erase(key);
  if (!fields_sha.empty()) {
    Json names=Json::array();
    for (auto it=fields.begin();it!=fields.end();++it) names.push_back(it.key());
    sidecar["fields_manifest_sha256"]=fields_sha; sidecar["research_fields"]=names;
  }
  std::error_code ec; std::filesystem::create_directories(v1_dir,ec); std::string unused;
  return !ec && std::filesystem::copy_file(v2.payload,v1_dir/(id+".f64")) &&
      json_file(v1_dir/(id+".json"),sidecar,unused) && std::filesystem::remove(v2.sidecar) &&
      std::filesystem::remove(v2.payload);
}
std::string weights_text(const std::string& schema,const std::string& library,const std::string& weights) {
  return "{\"schema\":\""+schema+"\",\"library_sha256\":\""+library+"\",\"weights\":"+weights+"}";
}
// Valid equal weights bound to `train`, plus raw `extra` members (e.g. signs).
std::string bound_text(const std::string& library,const std::string& train,const std::string& extra) {
  return "{\"schema\":\"atx.dsl-composition-weights/v1\",\"library_sha256\":\""+library+
      "\",\"train_manifest_sha256\":\""+train+"\",\"weights\":{\"volume_level\":0.5,\"volume_rank\":0.5}"+extra+"}";
}
// IC series, planned targets, fitted orientations and the saved exact blend,
// its support masks and its manifest: everything pinned as bit-identical.
std::vector<std::string> exact_outputs() {
  std::vector<std::string> out{"recipe.json","orientations.json"};
  for (const std::string role_name:{"train","validation"})
    for (const auto* suffix:{"_daily_ic.csv","_planned_targets.csv","_combined.f64",
                             "_combined_member.u8","_combined_finite.u8","_combined.json"})
      out.push_back(role_name+suffix);
  return out;
}
TEST(StrategyIcRunner, CandidateCacheColdAndWarmRunsReproduceUncachedOutputsExactly) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.save_combined=true;
  const auto plain=run_named(dir,cfg,"plain"); ASSERT_TRUE(plain.ok) << plain.error;
  cfg.candidate_cache_directory=(dir.path/"c").string();
  const auto cold=run_named(dir,cfg,"cold"); ASSERT_TRUE(cold.ok) << cold.error;
  const auto warm=run_named(dir,cfg,"warm"); ASSERT_TRUE(warm.ok) << warm.error;
  EXPECT_EQ(cold.log.find("IC cache-hit"),std::string::npos);
  EXPECT_NE(cold.log.find("IC cache-write volume_level role=train"),std::string::npos);
  for (const std::string id:{"volume_level","volume_rank"})
    for (const std::string role_name:{"train","validation"})
      EXPECT_NE(warm.log.find("IC cache-hit "+id+" role="+role_name),std::string::npos) << id << role_name;
  // A fully warm run neither constructs nor runs the VM.
  EXPECT_EQ(warm.log.find("IC VM-"),std::string::npos);
  const auto reference=read_json(dir.path/"plain"/"summary.json");
  EXPECT_FALSE(reference.at("roles").at(0).contains("candidate_cache"));
  for (const std::string name:{"cold","warm"}) {
    const bool is_warm=name=="warm";
    const auto summary=read_json(dir.path/name/"summary.json");
    EXPECT_EQ(summary.at("recipe_sha256"),reference.at("recipe_sha256")) << name;
    EXPECT_EQ(summary.at("orientations_artifact_sha256"),reference.at("orientations_artifact_sha256")) << name;
    ASSERT_EQ(summary.at("roles").size(),2U);
    for (usize r=0;r<2;++r) {
      const auto& result=summary.at("roles").at(r);
      EXPECT_EQ(result.at("candidate_cache").at("hits"),is_warm?2:0) << name;
      EXPECT_EQ(result.at("candidate_cache").at("misses"),is_warm?0:2) << name;
      EXPECT_GE(result.at("stage_seconds").at("cache_load").get<f64>(),0);
      EXPECT_GE(result.at("stage_seconds").at("cache_write").get<f64>(),0);
      for (const auto& row:result.at("candidates")) EXPECT_EQ(row.at("signal_cache"),is_warm?"hit":"miss");
      // T15: a warm pass also serves every IC result from the IC-result cache.
      EXPECT_EQ(result.at("candidate_cache").at("ic_results").at("hits"),is_warm?2:0) << name;
      EXPECT_EQ(result.at("candidate_cache").at("ic_results").at("misses"),is_warm?0:2) << name;
      for (const auto& row:result.at("candidates")) EXPECT_EQ(row.at("ic_result_cache"),is_warm?"hit":"miss");
      EXPECT_EQ(stable_role(result),stable_role(reference.at("roles").at(r))) << name << r;
    }
    for (const auto& file:exact_outputs()) {
      const auto expected=file_sha(dir.path/"plain"/file); ASSERT_FALSE(expected.empty()) << file;
      EXPECT_EQ(file_sha(dir.path/name/file),expected) << name << ' ' << file;
    }
  }
  // Each committed entry: identity, raw geometry, canonical NaN and streamed hash.
  // Beside the four signal files sits exactly one IC-result subdirectory (T15).
  const auto root=cache_root(dir.path/"cold"/"summary.json");
  for (const auto& pin:{cfg.train_sha256,cfg.validation_sha256}) {
    const auto base=root/pin; usize files=0,subdirectories=0;
    for (const auto& entry:std::filesystem::directory_iterator(base)) {
      if (entry.is_directory()) { ++subdirectories; continue; }
      ++files; EXPECT_NE(entry.path().extension().string(),".partial");
    }
    EXPECT_EQ(files,4U); EXPECT_EQ(subdirectories,1U);
    for (const std::string id:{"volume_level","volume_rank"}) {
      // Base candidates: v2 entries <id>.<dsl16>.{f64,json} in the role directory.
      const auto stem=id=="volume_level"?level_stem():rank_stem();
      const auto sidecar=read_json(base/(stem+".json"));
      EXPECT_EQ(sidecar.at("schema"),"atx.dsl-candidate-signal/v2");
      EXPECT_EQ(sidecar.at("candidate_id"),id); EXPECT_EQ(sidecar.at("role_manifest_sha256"),pin);
      EXPECT_EQ(sidecar.at("library_sha256"),cfg.library_sha256);
      EXPECT_EQ(sidecar.at("dates"),D); EXPECT_EQ(sidecar.at("instruments"),N);
      EXPECT_EQ(sidecar.at("bytes"),D*N*sizeof(f64));
      EXPECT_EQ(sidecar.at("payload"),stem+".f64");
      EXPECT_EQ(sidecar.at("field_payload_sha256"),Json::object());
      EXPECT_FALSE(sidecar.contains("fields_manifest_sha256"));
      auto dsl=core::sha256_hex(id=="volume_level"?"volume":"rank(volume)"); ASSERT_TRUE(dsl);
      EXPECT_EQ(sidecar.at("dsl_sha256"),*dsl);
      EXPECT_EQ(sidecar.at("payload_sha256"),file_sha(base/(stem+".f64")));
      EXPECT_EQ(cache_entry(dir.path/"cold"/"summary.json",pin==cfg.train_sha256?"train":"validation",id)
                    .sidecar.string(),(base/(stem+".json")).string());
      std::vector<f64> signal(D*N); ASSERT_TRUE(read_payload(base/(stem+".f64"),signal));
      usize finite=0;
      for (const auto v:signal) {
        if (std::isfinite(v)) { ++finite; continue; }
        EXPECT_EQ(std::bit_cast<u64>(v),std::bit_cast<u64>(std::numeric_limits<f64>::quiet_NaN()));
      }
      EXPECT_GE(finite,(D-63)*N);
    }
  }
}
TEST(StrategyIcRunner, CandidateCacheResumesStoppedRunAndAdoptsOnlyIdenticalOrphans) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.save_combined=true; cfg.candidate_cache_directory=(dir.path/"c").string();
  const auto cold=run_named(dir,cfg,"cold"); ASSERT_TRUE(cold.ok) << cold.error;
  const auto root=cache_root(dir.path/"cold"/"summary.json");
  const auto train_dir=root/cfg.train_sha256,validation_dir=root/cfg.validation_sha256;
  // A guard stop after TRAIN candidate 1 of 2 leaves only its committed entry.
  ASSERT_TRUE(std::filesystem::remove(train_dir/(rank_stem()+".json")));
  ASSERT_TRUE(std::filesystem::remove(train_dir/(rank_stem()+".f64")));
  // A stop between the payload and sidecar commits leaves an orphan payload.
  ASSERT_TRUE(std::filesystem::remove(validation_dir/(level_stem()+".json")));
  const auto orphan=validation_dir/(level_stem()+".f64"); const auto orphan_sha=file_sha(orphan);
  const auto orphan_time=std::filesystem::last_write_time(orphan);
  cfg.plan_only=true; std::ostringstream plan_log;
  auto status=atx::impl::strategy::run_ic(cfg,plan_log); ASSERT_TRUE(status) << status.error().to_string();
  const auto plan=Json::parse(plan_log.str());
  ASSERT_EQ(plan.at("candidate_cache").size(),2U);
  for (const auto& role_plan:plan.at("candidate_cache")) EXPECT_EQ(role_plan.at("ready_entries"),1);
  cfg.plan_only=false;
  const auto resumed=run_named(dir,cfg,"resumed"); ASSERT_TRUE(resumed.ok) << resumed.error;
  for (const auto* line:{"IC cache-hit volume_level role=train","IC cache-miss volume_rank role=train",
                         "IC cache-miss volume_level role=validation","IC cache-hit volume_rank role=validation"})
    EXPECT_NE(resumed.log.find(line),std::string::npos) << line;
  EXPECT_EQ(file_sha(orphan),orphan_sha);
  EXPECT_TRUE(std::filesystem::last_write_time(orphan)==orphan_time); // Adopted, not rewritten.
  EXPECT_TRUE(std::filesystem::exists(validation_dir/(level_stem()+".json")));
  EXPECT_TRUE(std::filesystem::exists(train_dir/(rank_stem()+".f64")));
  const auto summary=read_json(dir.path/"resumed"/"summary.json");
  for (const auto& result:summary.at("roles")) EXPECT_EQ(result.at("candidate_cache").at("hits"),1);
  for (const auto& file:exact_outputs()) {
    const auto expected=file_sha(dir.path/"cold"/file); ASSERT_FALSE(expected.empty()) << file;
    EXPECT_EQ(file_sha(dir.path/"resumed"/file),expected) << file;
  }
}
TEST(StrategyIcRunner, CandidateCacheRefusesTamperedOrForeignEntriesWithoutOverwrite) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.candidate_cache_directory=(dir.path/"c").string();
  const auto cold=run_named(dir,cfg,"cold"); ASSERT_TRUE(cold.ok) << cold.error;
  const auto root=cache_root(dir.path/"cold"/"summary.json");
  const auto train_dir=root/cfg.train_sha256,validation_dir=root/cfg.validation_sha256;
  const auto expect_refusal=[&](const std::string& name,const std::string& reason) {
    const auto attempt=run_named(dir,cfg,name);
    EXPECT_FALSE(attempt.ok) << name;
    EXPECT_NE(attempt.error.find(reason),std::string::npos) << name << ": " << attempt.error;
    EXPECT_EQ(read_json(dir.path/name/"summary.json").at("status"),"failed") << name;
  };
  // One flipped payload bit: the streamed load hash refuses it and leaves it in place.
  const auto payload_path=train_dir/(level_stem()+".f64"); const auto original=file_bytes(payload_path);
  ASSERT_EQ(original.size(),D*N*sizeof(f64));
  auto tampered=original; const usize at=8*N*200;
  tampered[at]=static_cast<char>(tampered[at]^1); ASSERT_TRUE(write_bytes(payload_path,tampered));
  expect_refusal("tampered","candidate cache payload SHA256 mismatch: "+level_stem()+".f64");
  EXPECT_EQ(file_bytes(payload_path),tampered);
  tampered.resize(tampered.size()-sizeof(f64)); ASSERT_TRUE(write_bytes(payload_path,tampered));
  expect_refusal("truncated","candidate cache payload extent: "+level_stem()+".f64");
  ASSERT_TRUE(write_bytes(payload_path,original));
  // A v2 sidecar at this key's path naming another full DSL SHA256 (a 16-hex
  // prefix collision) is foreign: a loud refusal, not a recompute.
  const auto sidecar_path=train_dir/(rank_stem()+".json"); const auto sidecar=read_json(sidecar_path);
  auto foreign=sidecar; foreign["dsl_sha256"]=std::string(64,'0'); std::string unused;
  ASSERT_TRUE(json_file(sidecar_path,foreign,unused));
  expect_refusal("foreign","candidate cache entry mismatch: volume_rank");
  EXPECT_EQ(read_json(sidecar_path),foreign);
  ASSERT_TRUE(json_file(sidecar_path,sidecar,unused));
  // An uncommitted payload whose bytes differ from the fresh evaluation is never replaced.
  const auto orphan=validation_dir/(level_stem()+".f64");
  ASSERT_TRUE(std::filesystem::remove(validation_dir/(level_stem()+".json")));
  auto different=file_bytes(orphan); const usize cell=8*N*300;
  different[cell]=static_cast<char>(different[cell]^1); ASSERT_TRUE(write_bytes(orphan,different));
  expect_refusal("orphan","refusing overwrite: "+level_stem()+".f64");
  EXPECT_EQ(file_bytes(orphan),different);
  EXPECT_FALSE(std::filesystem::exists(validation_dir/(level_stem()+".json")));
}
bool two_family_library(atx::impl::strategy::IcRunnerConfig& cfg) {
  auto lib=read_json(cfg.library_path);
  lib["families"].push_back({{"id","fixed_price"}});
  lib["candidates"].push_back({{"id","volume_compound"},{"family","fixed_volume"},
      {"dsl","rank((volume + raw_close) * (volume - raw_close))"},
      {"sign_policy","train-rank-ic21"},{"horizons",{5,21,63}}});
  lib["candidates"].push_back({{"id","price_level"},{"family","fixed_price"},{"dsl","close"},
      {"sign_policy","train-rank-ic21"},{"horizons",{5,21,63}}});
  return json_file(cfg.library_path,lib,cfg.library_sha256);
}
TEST(StrategyIcRunner, PinnedDefaultCompositionWeightsReproduceDefaultBlendBytes) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  // Families of three and one: default weights 1/6 (inexact in binary) and 1/2.
  ASSERT_TRUE(two_family_library(cfg)); cfg.save_combined=true;
  const auto reference=run_named(dir,cfg,"default"); ASSERT_TRUE(reference.ok) << reference.error;
  const f64 volume=1.0/(2.0*3.0),price=1.0/(2.0*1.0);
  ASSERT_TRUE(pin_weights(dir,cfg,{{"volume_level",volume},{"volume_rank",volume},
      {"volume_compound",volume},{"price_level",price}}));
  const auto pinned=run_named(dir,cfg,"pinned"); ASSERT_TRUE(pinned.ok) << pinned.error;
  for (const std::string role_name:{"train","validation"})
    for (const auto* suffix:{"_combined.f64","_combined_member.u8","_combined_finite.u8",
                             "_planned_targets.csv","_daily_ic.csv"}) {
      const auto file=role_name+suffix; const auto expected=file_sha(dir.path/"default"/file);
      ASSERT_FALSE(expected.empty()) << file;
      EXPECT_EQ(file_sha(dir.path/"pinned"/file),expected) << file;
    }
  const auto a=read_json(dir.path/"default"/"summary.json"),b=read_json(dir.path/"pinned"/"summary.json");
  for (usize r=0;r<2;++r)
    EXPECT_EQ(stable_role(b.at("roles").at(r),false),stable_role(a.at("roles").at(r),false)) << r;
  EXPECT_EQ(read_json(dir.path/"pinned"/"orientations.json").at("candidates"),
            read_json(dir.path/"default"/"orientations.json").at("candidates"));
  // Absent weights leave every canonical record as before; pinned weights are recorded.
  EXPECT_NE(b.at("recipe_sha256"),a.at("recipe_sha256"));
  EXPECT_FALSE(a.contains("composition_weights_sha256"));
  EXPECT_EQ(b.at("composition_weights_sha256"),cfg.composition_weights_sha256);
  const auto default_recipe=read_json(dir.path/"default"/"recipe.json");
  auto pinned_recipe=read_json(dir.path/"pinned"/"recipe.json");
  EXPECT_FALSE(default_recipe.contains("composition_weights_sha256"));
  EXPECT_EQ(default_recipe.at("composition"),
      "fixed-equal-family/equal-within;centered-tied-rank;missing-or-unoriented-neutral;no-redistribution");
  EXPECT_EQ(pinned_recipe.at("composition_weights_sha256"),cfg.composition_weights_sha256);
  EXPECT_NE(pinned_recipe.at("composition"),default_recipe.at("composition"));
  pinned_recipe.erase("composition_weights_sha256"); pinned_recipe["composition"]=default_recipe.at("composition");
  EXPECT_EQ(pinned_recipe,default_recipe);
  for (const std::string role_name:{"train","validation"}) {
    const auto base=read_json(dir.path/"default"/(role_name+"_combined.json"));
    const auto manifest=read_json(dir.path/"pinned"/(role_name+"_combined.json"));
    EXPECT_FALSE(base.contains("composition_weights_sha256"));
    EXPECT_EQ(base.at("signal_semantics"),
        "exact-pre-target-composition;equal-family/equal-within;missing-or-unoriented-neutral-fixed-denominator");
    EXPECT_EQ(manifest.at("composition_weights_sha256"),cfg.composition_weights_sha256);
    EXPECT_EQ(manifest.at("signal_semantics"),
        "exact-pre-target-composition;pinned-candidate-weights;missing-or-unoriented-neutral-fixed-denominator");
    EXPECT_EQ(manifest.at("run_recipe_sha256"),b.at("recipe_sha256"));
  }
}
TEST(StrategyIcRunner, UnequalPinnedWeightsChangeBlendAndRecipeWhileZeroWeightStaysScored) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.save_combined=true;
  const auto reference=run_named(dir,cfg,"default"); ASSERT_TRUE(reference.ok) << reference.error;
  ASSERT_TRUE(pin_weights(dir,cfg,{{"volume_level",2.0},{"volume_rank",0.0}}));
  const auto weighted=run_named(dir,cfg,"weighted"); ASSERT_TRUE(weighted.ok) << weighted.error;
  const auto a=read_json(dir.path/"default"/"summary.json"),b=read_json(dir.path/"weighted"/"summary.json");
  EXPECT_NE(b.at("recipe_sha256"),a.at("recipe_sha256"));
  EXPECT_EQ(read_json(dir.path/"weighted"/"recipe.json").at("composition_weights_sha256"),
            cfg.composition_weights_sha256);
  for (usize r=0;r<2;++r) {
    // Zero weight: still evaluated for IC, so per-candidate diagnostics are unchanged.
    const auto x=stable_role(a.at("roles").at(r)),y=stable_role(b.at("roles").at(r));
    EXPECT_EQ(y.at("candidates"),x.at("candidates"));
    EXPECT_EQ(y.at("candidate_evaluations"),2);
    const auto name=y.at("role").get<std::string>();
    for (const auto* suffix:{"_combined.f64","_planned_targets.csv"})
      EXPECT_NE(file_sha(dir.path/"weighted"/(name+suffix)),file_sha(dir.path/"default"/(name+suffix)))
          << name << suffix;
    EXPECT_EQ(read_json(dir.path/"weighted"/(name+"_combined.json")).at("composition_weights_sha256"),
              cfg.composition_weights_sha256);
    std::vector<f64> signal(D*N); ASSERT_TRUE(read_payload(dir.path/"weighted"/(name+"_combined.f64"),signal));
    // Weight two on one monotone candidate; the zero-weight one contributes nothing.
    for (usize d=63;d<D;++d) for (usize i=0;i<N;++i)
      ASSERT_DOUBLE_EQ(signal[d*N+i],2*(static_cast<f64>(i)/static_cast<f64>(N-1)-.5)) << name << d << i;
  }
}
TEST(StrategyIcRunner, InvalidCompositionWeightsRefuseBeforeAnyPayloadOrOutput) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  // Payloads are absent: every refusal below must precede any role payload read.
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  ASSERT_TRUE(std::filesystem::remove(dir.path/"validation"/"close.f64"));
  ASSERT_TRUE(pin_weights(dir,cfg,{{"volume_level",.25},{"volume_rank",.75}}));
  cfg.plan_only=true; std::ostringstream plan_log;
  auto status=atx::impl::strategy::run_ic(cfg,plan_log); ASSERT_TRUE(status) << status.error().to_string();
  EXPECT_EQ(Json::parse(plan_log.str()).at("composition_weights_sha256"),cfg.composition_weights_sha256);
  const std::string schema="atx.dsl-composition-weights/v1",lib=cfg.library_sha256;
  const std::vector<std::pair<std::string,std::string>> cases{
      {weights_text(schema,lib,R"({"volume_level":0.5,"volume_rank":0.5,"other":0.1})"),"unknown candidate: other"},
      {weights_text(schema,lib,R"({"volume_level":0.5})"),"composition weight missing: volume_rank"},
      {weights_text(schema,lib,R"({"volume_level":0.5,"volume_rank":-0.5})"),"finite and >= 0: volume_rank"},
      {weights_text(schema,lib,R"({"volume_level":0.5,"volume_rank":"0.5"})"),"finite and >= 0: volume_rank"},
      {weights_text(schema,lib,R"({"volume_level":0.5,"volume_rank":1e999})"),"composition weights JSON parse"},
      {weights_text(schema,lib,R"({"volume_level":0.5,"volume_rank":0.5,"volume_rank":0.25})"),"duplicate key"},
      {weights_text(schema,std::string(64,'0'),R"({"volume_level":0.5,"volume_rank":0.5})"),"schema/library identity"},
      {weights_text("atx.dsl-composition-weights/v0",lib,R"({"volume_level":0.5,"volume_rank":0.5})"),
       "schema/library identity"},
      {weights_text(schema,lib,"[0.5,0.5]"),"schema/library identity"},
      // TRAIN binding and optional pinned signs (T1 fix round 1).
      {weights_text(schema,lib,R"({"volume_level":0.5,"volume_rank":0.5})"),
       "TRAIN binding: train_manifest_sha256 must equal --train-sha256"},
      {bound_text(lib,std::string(64,'0'),""),"TRAIN binding"},
      {bound_text(lib,cfg.train_sha256,R"(,"signs":[1,1])"),"composition signs must be an object"},
      {bound_text(lib,cfg.train_sha256,R"(,"signs":{"other":1})"),"composition sign for unknown candidate: other"},
      {bound_text(lib,cfg.train_sha256,R"(,"signs":{"volume_level":0,"volume_rank":1})"),
       "composition sign must be +1 or -1: volume_level"},
      {bound_text(lib,cfg.train_sha256,R"(,"signs":{"volume_level":1.0,"volume_rank":1})"),
       "composition sign must be +1 or -1: volume_level"},
      {bound_text(lib,cfg.train_sha256,R"(,"signs":{"volume_level":"1","volume_rank":1})"),
       "composition sign must be +1 or -1: volume_level"},
      {bound_text(lib,cfg.train_sha256,R"(,"signs":{"volume_level":1})"),
       "composition sign missing for weighted candidate: volume_rank"}};
  const auto path=dir.path/"weights.json";
  for (const bool plan_only:{true,false}) {
    cfg.plan_only=plan_only;
    for (const auto& [text,reason]:cases) {
      ASSERT_TRUE(text_file(path,text,cfg.composition_weights_sha256));
      std::ostringstream attempt; status=atx::impl::strategy::run_ic(cfg,attempt);
      ASSERT_FALSE(status) << text;
      EXPECT_NE(status.error().to_string().find(reason),std::string::npos)
          << text << " -> " << status.error().to_string();
      EXPECT_TRUE(attempt.str().empty()); EXPECT_FALSE(std::filesystem::exists(dir.path/"output"));
    }
  }
  // Hash mismatch and an unpaired option refuse the same way.
  cfg.plan_only=false;
  ASSERT_TRUE(pin_weights(dir,cfg,{{"volume_level",.5},{"volume_rank",.5}}));
  cfg.composition_weights_sha256=std::string(64,'f');
  std::ostringstream mismatch; status=atx::impl::strategy::run_ic(cfg,mismatch);
  ASSERT_FALSE(status); EXPECT_NE(status.error().to_string().find("pin differs"),std::string::npos);
  cfg.composition_weights_sha256.clear();
  std::ostringstream unpaired; status=atx::impl::strategy::run_ic(cfg,unpaired);
  ASSERT_FALSE(status); EXPECT_NE(status.error().to_string().find("bounded config"),std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(dir.path/"output"));
}
// ---- ew-theme-v6 within-theme redistribution (V6-W; fix round 1 I1 / I3) ----
// A TRAIN-bound weights file under `schema` with raw `weights` and raw `extra`
// members (signs, a theme_redistribution block).
std::string themed_text(const std::string& schema,const atx::impl::strategy::IcRunnerConfig& cfg,
                        const std::string& weights,const std::string& extra) {
  return "{\"schema\":\""+schema+"\",\"library_sha256\":\""+cfg.library_sha256+"\",\"train_manifest_sha256\":\""+
      cfg.train_sha256+"\",\"weights\":"+weights+extra+"}";
}
std::string theme_block(const std::string& themes,const std::string& rule="within-theme-v1",
                        const std::string& composition="ew-theme-v6") {
  return ",\"theme_redistribution\":{\"rule\":\""+rule+"\",\"composition\":\""+composition+"\",\"themes\":"+themes+"}";
}
const std::string weights_v1="atx.dsl-composition-weights/v1",weights_v2="atx.dsl-composition-weights/v2";
TEST(StrategyIcRunner, ThemeRedistributionRefusalsAndSchemaGatePrecedeAnyPayloadOrOutput) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  // Payloads are absent: every refusal below must precede any role payload read.
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  ASSERT_TRUE(std::filesystem::remove(dir.path/"validation"/"close.f64"));
  // The runner pairs --composition-weights with its SHA (else "bounded config"):
  // every case below rewrites this one path and re-pins its SHA.
  const auto path=dir.path/"weights.json"; cfg.composition_weights_path=path.string();
  const std::string equal=R"({"volume_level":0.5,"volume_rank":0.5})";
  const std::string both=R"({"volume_level":"liquidity","volume_rank":"liquidity"})";
  const auto plan=[&](const std::string& text) {
    if (!text_file(path,text,cfg.composition_weights_sha256)) return std::string("unwritable");
    cfg.plan_only=true; std::ostringstream log;
    const auto status=atx::impl::strategy::run_ic(cfg,log);
    if (!status) return status.error().to_string();
    return Json::parse(log.str()).at("composition_weights_sha256")==cfg.composition_weights_sha256
        ?std::string{}:std::string("plan lacks the weights pin");
  };
  // Admitted: v2 with the block (one theme, two themes, and a zero-weight candidate
  // without a theme), v1 without it.
  EXPECT_EQ(plan(themed_text(weights_v2,cfg,equal,theme_block(both))),"");
  EXPECT_EQ(plan(themed_text(weights_v2,cfg,equal,theme_block(R"({"volume_level":"a","volume_rank":"b_2"})"))),"");
  EXPECT_EQ(plan(themed_text(weights_v2,cfg,R"({"volume_level":1,"volume_rank":0})",
                             theme_block(R"({"volume_level":"a"})"))),"");
  EXPECT_EQ(plan(themed_text(weights_v1,cfg,equal,"")),"");
  const std::string shape="theme_redistribution must be {rule: within-theme-v1, composition: ew-theme-v6, "
                          "themes: {id: theme}}";
  const std::string name="theme name must match [a-z0-9_]{1,64}: ";
  const std::vector<std::pair<std::string,std::string>> cases{
      // Schema gate: v2 iff the block is present, v1 iff absent. Any other schema
      // fails the identity check, which is how a v1-only binary refuses a v2 file.
      {themed_text(weights_v2,cfg,equal,""),
       "composition weights schema atx.dsl-composition-weights/v2 requires a theme_redistribution block"},
      {themed_text(weights_v1,cfg,equal,theme_block(both)),
       "theme_redistribution requires composition weights schema atx.dsl-composition-weights/v2"},
      {themed_text("atx.dsl-composition-weights/v3",cfg,equal,theme_block(both)),"schema/library identity"},
      // Block shape: exactly {rule: within-theme-v1, composition: ew-theme-v6, themes: {...}}.
      {themed_text(weights_v2,cfg,equal,R"(,"theme_redistribution":[1])"),shape},
      {themed_text(weights_v2,cfg,equal,theme_block(both,"within-theme-v2")),shape},
      {themed_text(weights_v2,cfg,equal,theme_block(both,"within-theme-v1","ew-theme-v1")),shape},
      {themed_text(weights_v2,cfg,equal,
                   R"(,"theme_redistribution":{"rule":"within-theme-v1","composition":"ew-theme-v6"})"),shape},
      {themed_text(weights_v2,cfg,equal,theme_block(R"(["liquidity","liquidity"])")),shape},
      // Rows: known ids, [a-z0-9_]{1,64} string names, a theme for every weighted id.
      {themed_text(weights_v2,cfg,equal,theme_block(R"({"volume_level":"a","volume_rank":"a","other":"a"})")),
       "theme for unknown candidate: other"},
      {themed_text(weights_v2,cfg,equal,theme_block(R"({"volume_level":"Liquidity","volume_rank":"a"})")),
       name+"volume_level"},
      {themed_text(weights_v2,cfg,equal,theme_block(R"({"volume_level":"a-b","volume_rank":"a"})")),
       name+"volume_level"},
      {themed_text(weights_v2,cfg,equal,theme_block(R"({"volume_level":"","volume_rank":"a"})")),name+"volume_level"},
      {themed_text(weights_v2,cfg,equal,theme_block(R"({"volume_level":1,"volume_rank":"a"})")),name+"volume_level"},
      {themed_text(weights_v2,cfg,equal,
                   theme_block(R"({"volume_level":"a","volume_rank":")"+std::string(65,'a')+R"("})")),
       name+"volume_rank"},
      {themed_text(weights_v2,cfg,equal,theme_block(R"({"volume_level":"a"})")),
       "theme missing for weighted candidate: volume_rank"},
      {themed_text(weights_v2,cfg,R"({"volume_level":0,"volume_rank":0})",theme_block("{}")),
       "theme_redistribution needs 1..32 weighted themes"}};
  for (const bool plan_only:{true,false}) {
    for (const auto& [text,reason]:cases) {
      ASSERT_TRUE(text_file(path,text,cfg.composition_weights_sha256));
      cfg.plan_only=plan_only; std::ostringstream attempt;
      const auto status=atx::impl::strategy::run_ic(cfg,attempt);
      ASSERT_FALSE(status) << text;
      EXPECT_NE(status.error().to_string().find(reason),std::string::npos)
          << text << " -> " << status.error().to_string();
      EXPECT_TRUE(attempt.str().empty()); EXPECT_FALSE(std::filesystem::exists(dir.path/"output"));
    }
  }
  // 32 weighted themes are admitted, 33 refused (33 weighted candidates).
  auto lib=read_json(cfg.library_path);
  for (usize k=1;k<=31;++k)
    lib["candidates"].push_back({{"id","volume_lag_"+std::to_string(k)},{"family","fixed_volume"},
        {"dsl","delay(volume, "+std::to_string(k)+")"},{"sign_policy","train-rank-ic21"},{"horizons",{5,21,63}}});
  ASSERT_TRUE(json_file(cfg.library_path,lib,cfg.library_sha256));
  Json weights=Json::object(),themes=Json::object();
  for (const auto& row:lib.at("candidates")) {
    const auto id=row.at("id").get<std::string>();
    const auto theme="t"+std::to_string(themes.size());
    weights[id]=1.0; themes[id]=theme;
  }
  ASSERT_EQ(themes.size(),33U);
  EXPECT_NE(plan(themed_text(weights_v2,cfg,weights.dump(),theme_block(themes.dump())))
                .find("theme_redistribution needs 1..32 weighted themes"),std::string::npos);
  themes["volume_rank"]=themes.at("volume_level");
  EXPECT_EQ(plan(themed_text(weights_v2,cfg,weights.dump(),theme_block(themes.dump()))),"");
  EXPECT_FALSE(std::filesystem::exists(dir.path/"output"));
}
TEST(StrategyIcRunner, ThemeRedistributionIsRecordedInRecipeSummaryAndCombinedOnlyWhenPinned) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.save_combined=true;
  // One weighted member (volume_rank has weight 0 and needs no theme): the same
  // weights unthemed (v1), themed (v2) and themed with pinned signs (v2).
  const std::string weights=R"({"volume_level":2.0,"volume_rank":0.0})";
  const std::string block=theme_block(R"({"volume_level":"liquidity"})");
  const auto pin=[&](const std::string& file,const std::string& text) {
    cfg.composition_weights_path=(dir.path/file).string();
    return text_file(cfg.composition_weights_path,text,cfg.composition_weights_sha256);
  };
  ASSERT_TRUE(pin("plain.json",themed_text(weights_v1,cfg,weights,"")));
  const auto plain_pin=cfg.composition_weights_sha256;
  const auto plain=run_named(dir,cfg,"plain"); ASSERT_TRUE(plain.ok) << plain.error;
  ASSERT_TRUE(pin("themed.json",themed_text(weights_v2,cfg,weights,block)));
  const auto themed_pin=cfg.composition_weights_sha256;
  const auto themed=run_named(dir,cfg,"themed"); ASSERT_TRUE(themed.ok) << themed.error;
  ASSERT_TRUE(pin("signed.json",themed_text(weights_v2,cfg,weights,R"(,"signs":{"volume_level":1})"+block)));
  const auto signed_run=run_named(dir,cfg,"signed"); ASSERT_TRUE(signed_run.ok) << signed_run.error;
  // 1. recipe.json: the themed method statement and composition_redistribution.
  const std::string within="centered-tied-rank;missing-or-unoriented-mass-stays-in-theme;within-theme-v1;"
                           "theme-without-present-member-neutral";
  const auto plain_recipe=read_json(dir.path/"plain"/"recipe.json");
  auto themed_recipe=read_json(dir.path/"themed"/"recipe.json");
  const auto signed_recipe=read_json(dir.path/"signed"/"recipe.json");
  EXPECT_EQ(themed_recipe.at("composition"),"pinned-candidate-weights;TRAIN-orientation-signs;"+within);
  EXPECT_EQ(signed_recipe.at("composition"),"pinned-candidate-weights;pinned-candidate-signs;"+within);
  EXPECT_EQ(themed_recipe.at("composition_redistribution"),"within-theme-v1");
  EXPECT_EQ(signed_recipe.at("composition_redistribution"),"within-theme-v1");
  EXPECT_EQ(themed_recipe.at("composition_weights_sha256"),themed_pin);
  // Absent the block the pinned recipe is as before: no-redistribution, no new key;
  // otherwise the two recipes are identical.
  EXPECT_EQ(plain_recipe.at("composition"),"pinned-candidate-weights;TRAIN-orientation-signs;centered-tied-rank;"
                                           "missing-or-unoriented-neutral;no-redistribution");
  EXPECT_FALSE(plain_recipe.contains("composition_redistribution"));
  EXPECT_EQ(plain_recipe.at("composition_weights_sha256"),plain_pin);
  themed_recipe.erase("composition_redistribution");
  themed_recipe["composition"]=plain_recipe.at("composition");
  themed_recipe["composition_weights_sha256"]=plain_pin;
  EXPECT_EQ(themed_recipe,plain_recipe);
  // 2. summary.json composition_weights.redistribution.
  EXPECT_FALSE(read_json(dir.path/"plain"/"summary.json").at("composition_weights").contains("redistribution"));
  for (const std::string run:{"themed","signed"})
    EXPECT_EQ(read_json(dir.path/run/"summary.json").at("composition_weights").at("redistribution"),
              "within-theme-v1") << run;
  // 3. <role>_combined.json composition_redistribution; signal_semantics unchanged
  // (still one of the strings the NAV target replay admits).
  for (const std::string role_name:{"train","validation"}) {
    const auto base=read_json(dir.path/"plain"/(role_name+"_combined.json"));
    EXPECT_FALSE(base.contains("composition_redistribution")) << role_name;
    for (const std::string run:{"themed","signed"}) {
      const auto manifest=read_json(dir.path/run/(role_name+"_combined.json"));
      EXPECT_EQ(manifest.at("composition_redistribution"),"within-theme-v1") << run << ' ' << role_name;
      EXPECT_EQ(manifest.at("signal_semantics"),base.at("signal_semantics")) << run << ' ' << role_name;
      EXPECT_EQ(manifest.at("composition_weights_sha256"),
                read_json(dir.path/run/"summary.json").at("composition_weights").at("sha256")) << run;
    }
    // The single weighted member is present from d=63: its theme's mass 2 lands on it.
    std::vector<f64> signal(D*N); ASSERT_TRUE(read_payload(dir.path/"themed"/(role_name+"_combined.f64"),signal));
    for (usize d=63;d<D;++d) for (usize i=0;i<N;++i)
      ASSERT_DOUBLE_EQ(signal[d*N+i],2*(static_cast<f64>(i)/static_cast<f64>(N-1)-.5)) << role_name << d << i;
  }
}
TEST(StrategyIcRunner, ValidationOnlyResumeComposesCandidateCacheAndTrainBoundWeights) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.save_combined=true;
  const auto source=run_named(dir,cfg,"source"); ASSERT_TRUE(source.ok) << source.error;
  // The default values pinned: a weighted TRAIN source whose recipe records the pin.
  // Fit (per provenance) on the unweighted source's orientations, like the T11 fitter.
  const auto source_orientations=file_sha(dir.path/"source"/"orientations.json");
  ASSERT_TRUE(pin_weights(dir,cfg,{{"volume_level",.5},{"volume_rank",.5}},Json(),"weights.json",
      {{"orientations_sha256",source_orientations},{"fields_manifest_sha256",nullptr}}));
  const auto weights_pin=cfg.composition_weights_sha256;
  const auto weighted=run_named(dir,cfg,"weighted_source"); ASSERT_TRUE(weighted.ok) << weighted.error;
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  const auto resume_from=[&](atx::impl::strategy::IcRunnerConfig& target,const std::string& source_name) {
    target.orientations_path=(dir.path/source_name/"orientations.json").string();
    target.orientations_sha256=file_sha(target.orientations_path);
  };
  // Unweighted frozen source + TRAIN-bound weights + candidate cache: cold, then warm.
  resume_from(cfg,"source"); cfg.candidate_cache_directory=(dir.path/"c").string();
  const auto cold=run_named(dir,cfg,"cold"); ASSERT_TRUE(cold.ok) << cold.error;
  const auto warm=run_named(dir,cfg,"warm"); ASSERT_TRUE(warm.ok) << warm.error;
  EXPECT_NE(warm.log.find("IC cache-hit volume_level role=validation"),std::string::npos);
  EXPECT_EQ(warm.log.find("IC VM-"),std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(cache_root(dir.path/"cold"/"summary.json")/cfg.train_sha256));
  for (const auto* run:{&cold,&warm}) EXPECT_EQ(run->log.find("IC loading train"),std::string::npos);
  // The shipped flow (unweighted O + weights fit on O) reproduces the weighted
  // TRAIN run's validation bytes exactly, which is why a weighted frozen source
  // is never needed for validation (T14 strict ruling).
  for (const std::string name:{"cold","warm"}) {
    for (const auto* file:{"validation_combined.f64","validation_combined_finite.u8",
                           "validation_planned_targets.csv","validation_daily_ic.csv"})
      for (const std::string reference:{"source","weighted_source"}) {
        const auto expected=file_sha(dir.path/reference/file); ASSERT_FALSE(expected.empty()) << file;
        EXPECT_EQ(file_sha(dir.path/name/file),expected) << name << ' ' << reference << ' ' << file;
      }
    const auto recipe=read_json(dir.path/name/"recipe.json");
    EXPECT_EQ(recipe.at("composition_weights_sha256"),weights_pin) << name;
    EXPECT_EQ(read_json(dir.path/name/"validation_combined.json").at("composition_weights_sha256"),weights_pin);
    EXPECT_EQ(recipe.at("frozen_train_recipe"),read_json(dir.path/"source"/"recipe.json")) << name;
  }
  EXPECT_EQ(file_sha(dir.path/"cold"/"validation_combined.json"),file_sha(dir.path/"warm"/"validation_combined.json"));
  // T14 M1: how the weights were bound to TRAIN is recorded, for both run modes.
  EXPECT_EQ(read_json(dir.path/"cold"/"summary.json").at("composition_weights").at("binding"),
            "provenance-orientations-equal-frozen-TRAIN-orientations-artifact");
  EXPECT_EQ(read_json(dir.path/"weighted_source"/"summary.json").at("composition_weights").at("binding"),
            "train-manifest-sha256;TRAIN-scored-in-this-run");
  // Selection hygiene refusals, all before any payload or output.
  const auto refuse=[&](atx::impl::strategy::IcRunnerConfig attempt_cfg,const std::string& name,
                        const std::string& reason) {
    const auto attempt=run_named(dir,attempt_cfg,name);
    EXPECT_FALSE(attempt.ok) << name;
    EXPECT_NE(attempt.error.find(reason),std::string::npos) << name << ": " << attempt.error;
    EXPECT_FALSE(std::filesystem::exists(dir.path/name)) << name;
  };
  // A weighted frozen TRAIN run is never a validation source: with its own weights,
  // with none, or with others.
  const std::string weighted_source="frozen TRAIN artifact is a weighted run; validate from the unweighted TRAIN "
                                    "run whose orientations the weights were fit on";
  auto same=cfg; resume_from(same,"weighted_source"); same.candidate_cache_directory.clear();
  refuse(same,"same",weighted_source);
  auto without=same; without.composition_weights_path.clear(); without.composition_weights_sha256.clear();
  refuse(without,"without",weighted_source);
  auto other=same; ASSERT_TRUE(pin_weights(dir,other,{{"volume_level",.25},{"volume_rank",.75}},Json(),"other.json",
      {{"orientations_sha256",source_orientations}}));
  refuse(other,"other",weighted_source);
  auto foreign=cfg; resume_from(foreign,"source");
  foreign.composition_weights_path=(dir.path/"foreign.json").string();
  ASSERT_TRUE(json_file(foreign.composition_weights_path,{{"schema","atx.dsl-composition-weights/v1"},
      {"library_sha256",cfg.library_sha256},{"train_manifest_sha256",std::string(64,'0')},
      {"weights",{{"volume_level",.5},{"volume_rank",.5}}}},foreign.composition_weights_sha256));
  refuse(foreign,"foreign","composition weights TRAIN binding");
}
// ---- Pinned extra point-in-time fields (T7) ----
// Synthetic field values, date-major like the producer: NaN where not visible.
f64 si_value(usize d,usize i,f64 scale) {
  return d<63?std::numeric_limits<f64>::quiet_NaN()
             :static_cast<f64>((i+1)*(i+1))*1e6*(1+.001*static_cast<f64>(d))*scale;
}
f64 market_value(usize d) {
  return d==0?std::numeric_limits<f64>::quiet_NaN():.001*std::sin(static_cast<f64>(d));
}
// T22 group label `grp_a`: names 0-2 in group 1 and 3-6 in group 2, except name 0
// alone in group 5 every third date; name 7 is never labeled and name 6 is
// unlabeled on odd dates (NaN: no group).
f64 group_label(usize d,usize i) {
  if (i==7 || (i==6 && d%2==1)) return std::numeric_limits<f64>::quiet_NaN();
  if (i==0 && d%3==0) return 5.0;
  return i<3?1.0:2.0;
}
std::vector<f64> field_column(const std::string& name,f64 scale) {
  std::vector<f64> out(D*N);
  for (usize d=0;d<D;++d) for (usize i=0;i<N;++i)
    out[d*N+i]=name=="grp_a"?group_label(d,i):(name=="mkt_ret"?market_value(d):
        (name=="iv_atm_21d"?.3+.01*static_cast<f64>(i):si_value(d,i,scale)));
  return out;
}
// A producer-shaped atx.research-role-fields/v1 directory bound to one role's own
// manifest and axes (prepare_research_fields.py key names).
bool fields_dir(const std::filesystem::path& dir,const std::string& role_manifest,const std::string& role_sha,
                const std::vector<std::string>& names,std::string& sha,f64 scale=1) {
  if (!std::filesystem::create_directories(dir)) return false;
  const auto role_json=read_json(role_manifest); const auto& receipts=role_json.at("files");
  Json files=Json::object(),entries=Json::array();
  for (const auto& name:names) {
    const auto file=name+".f64";
    if (!payload(dir,files,file.c_str(),field_column(name,scale))) return false;
    // The producer's flags: these three are look-ahead (T6 fix c099cade).
    const bool pit=name!="is_common" && name!="mktcap_lagged" && name!="size_grp";
    entries.push_back({{"name",name},{"file",file},{"dtype","<f8"},{"layout","date-major"},{"shape",{D,N}},
        {"units","synthetic"},{"clock","synthetic"},{"sha256",files.at(file).at("sha256")},
        {"point_in_time",pit},{"non_pit_aspects",pit?Json::array():Json::array({name=="is_common"?"values":"presence"})}});
  }
  return json_file(dir/"manifest.json",{{"schema","atx.research-role-fields/v1"},{"status","complete"},
      {"role",{{"manifest_sha256",role_sha},{"sessions_sha256",receipts.at("sessions.i64").at("sha256")},
               {"ids_sha256",receipts.at("ids.u64").at("sha256")},
               {"member_sha256",receipts.at("member.u8").at("sha256")},{"dates",D},{"instruments",N}}},
      {"instrument_namespace","spiderrock.securityID"},{"fields",entries},{"files",files}},sha);
}
// Pins one fields directory per role under dir/<tag>-{train,validation}.
bool pin_fields(const Directory& dir,atx::impl::strategy::IcRunnerConfig& cfg,const std::string& tag,
                const std::vector<std::string>& names,f64 scale=1) {
  cfg.train_fields_directory=(dir.path/(tag+"-train")).string();
  cfg.validation_fields_directory=(dir.path/(tag+"-validation")).string();
  return fields_dir(cfg.train_fields_directory,cfg.train_manifest,cfg.train_sha256,names,cfg.train_fields_sha256,scale) &&
      fields_dir(cfg.validation_fields_directory,cfg.validation_manifest,cfg.validation_sha256,names,
                 cfg.validation_fields_sha256,scale);
}
void clear_fields(atx::impl::strategy::IcRunnerConfig& cfg) {
  cfg.train_fields_directory.clear(); cfg.train_fields_sha256.clear();
  cfg.validation_fields_directory.clear(); cfg.validation_fields_sha256.clear();
}
// Declares `declared` beside the price fields and adds `candidates` in one new family.
bool field_library(atx::impl::strategy::IcRunnerConfig& cfg,const std::vector<std::string>& declared,
                   const std::vector<std::pair<std::string,std::string>>& candidates) {
  auto lib=read_json(cfg.library_path);
  for (const auto& name:declared) lib["fields"].push_back({{"name",name},{"basis","synthetic upstream field"}});
  if (!candidates.empty()) lib["families"].push_back({{"id","field_family"}});
  for (const auto& [id,dsl]:candidates)
    lib["candidates"].push_back({{"id",id},{"family","field_family"},{"dsl",dsl},
        {"sign_policy","train-rank-ic21"},{"horizons",{5,21,63}}});
  return json_file(cfg.library_path,lib,cfg.library_sha256);
}
bool same_value(f64 a,f64 b) { return (std::isnan(a) && std::isnan(b)) || a==b; }
TEST(StrategyIcRunner, ExtraFieldsResolveByNameAndMatchHandComputedSignals) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  // Review M3: base candidates cached by a base-only run, in their own cache.
  auto plain_cfg=cfg; plain_cfg.candidate_cache_directory=(dir.path/"p").string();
  const auto plain=run_named(dir,plain_cfg,"plain"); ASSERT_TRUE(plain.ok) << plain.error;
  ASSERT_TRUE(field_library(cfg,{"si_shares","mkt_ret"},
      {{"si_ratio","si_shares / volume"},{"market_shift","volume + mkt_ret"}}));
  ASSERT_TRUE(pin_fields(dir,cfg,"f",{"si_shares","mkt_ret"}));
  cfg.save_combined=true; cfg.candidate_cache_directory=(dir.path/"c").string();
  const auto run=run_named(dir,cfg,"output"); ASSERT_TRUE(run.ok) << run.error;
  // Each candidate reads one extra: one column resident at a time, each loaded once.
  for (const std::string role_name:{"train","validation"}) {
    EXPECT_NE(run.log.find("IC fields-verified role="+role_name+" fields=mkt_ret,si_shares resident_capacity=1 "
                           "planned_loads=2"),std::string::npos) << role_name;
    const auto load_si=run.log.find("IC field-load role="+role_name+" field=si_shares candidate=si_ratio");
    const auto drop_si=run.log.find("IC field-release role="+role_name+" field=si_shares");
    const auto load_market=run.log.find("IC field-load role="+role_name+" field=mkt_ret candidate=market_shift");
    ASSERT_NE(load_market,std::string::npos) << role_name;
    EXPECT_LT(load_si,drop_si) << role_name; EXPECT_LT(drop_si,load_market) << role_name;
  }
  EXPECT_EQ(run.log.find("IC fields-loaded"),std::string::npos);
  // Raw VM signals (the cache stores them verbatim) against scalar oracles.
  const auto root=cache_root(dir.path/"output"/"summary.json");
  const auto output_summary=dir.path/"output"/"summary.json",plain_summary=dir.path/"plain"/"summary.json";
  for (const auto& [role_name,pin,role_pin]:{
           std::tuple{std::string("train"),cfg.train_fields_sha256,cfg.train_sha256},
           std::tuple{std::string("validation"),cfg.validation_fields_sha256,cfg.validation_sha256}}) {
    const auto ratio_entry=cache_entry(output_summary,role_name,"si_ratio");
    const auto shift_entry=cache_entry(output_summary,role_name,"market_shift");
    std::vector<f64> ratio(D*N),shift(D*N);
    ASSERT_TRUE(read_payload(ratio_entry.payload,ratio)); ASSERT_TRUE(read_payload(shift_entry.payload,shift));
    for (usize d=0;d<D;++d) for (usize i=0;i<N;++i) {
      const auto volume=1e8*static_cast<f64>(i+1);
      ASSERT_TRUE(same_value(ratio[d*N+i],si_value(d,i,1)/volume)) << d << ' ' << i;
      ASSERT_TRUE(same_value(shift[d*N+i],volume+market_value(d))) << d << ' ' << i;
    }
    // Field candidates: v2 entries under <role sha>/fp_<fk16>/, keyed on the payload
    // SHA256 of exactly the fields they read; the pinned manifest is provenance.
    const auto fields_directory=std::filesystem::path(role_name=="train"?cfg.train_fields_directory
                                                                         :cfg.validation_fields_directory);
    const auto sidecar=read_json(ratio_entry.sidecar);
    EXPECT_EQ(sidecar.at("fields_manifest_sha256"),pin); EXPECT_EQ(sidecar.at("role_manifest_sha256"),role_pin);
    EXPECT_EQ(sidecar.at("field_payload_sha256"),Json({{"si_shares",file_sha(fields_directory/"si_shares.f64")}}));
    EXPECT_EQ(read_json(shift_entry.sidecar).at("field_payload_sha256"),
              Json({{"mkt_ret",file_sha(fields_directory/"mkt_ret.f64")}}));
    EXPECT_EQ(ratio_entry.sidecar.parent_path().parent_path().string(),(root/role_pin).string());
    EXPECT_TRUE(ratio_entry.sidecar.parent_path().filename().string().starts_with("fp_"));
    EXPECT_NE(ratio_entry.sidecar.parent_path().string(),shift_entry.sidecar.parent_path().string());
    // Base-only candidates keep the role directory and carry no fields key.
    const auto level_entry=cache_entry(output_summary,role_name,"volume_level");
    EXPECT_EQ(level_entry.sidecar.parent_path().string(),(root/role_pin).string());
    EXPECT_FALSE(read_json(level_entry.sidecar).contains("fields_manifest_sha256"));
    // ...and the bytes a fields run publishes there equal a base-only run's.
    for (const std::string id:{"volume_level","volume_rank"}) {
      const auto expected=file_sha(cache_entry(plain_summary,role_name,id).payload);
      ASSERT_FALSE(expected.empty()) << id;
      EXPECT_EQ(file_sha(cache_entry(output_summary,role_name,id).payload),expected) << id;
    }
  }
  const auto summary=read_json(dir.path/"output"/"summary.json");
  const auto recipe=read_json(dir.path/"output"/"recipe.json");
  EXPECT_EQ(recipe.at("research_fields").at("loaded"),Json::array({"mkt_ret","si_shares"}));
  EXPECT_EQ(recipe.at("research_fields").at("schema"),"atx.research-role-fields/v1");
  EXPECT_EQ(recipe.at("research_fields").at("manifest_sha256"),
            Json({{"train",cfg.train_fields_sha256},{"validation",cfg.validation_fields_sha256}}));
  EXPECT_EQ(summary.at("research_fields"),recipe.at("research_fields"));
  for (usize r=0;r<2;++r) {
    const auto& role_result=summary.at("roles").at(r); const bool train=r==0;
    const auto& pin=train?cfg.train_fields_sha256:cfg.validation_fields_sha256;
    const auto& fields=role_result.at("research_fields");
    EXPECT_EQ(fields.at("manifest_sha256"),pin); EXPECT_EQ(fields.at("loaded"),Json::array({"mkt_ret","si_shares"}));
    EXPECT_EQ(fields.at("loaded_bytes"),2*D*N*sizeof(f64));
    EXPECT_EQ(fields.at("resident_capacity"),1); EXPECT_EQ(fields.at("planned_loads"),2);
    EXPECT_EQ(fields.at("field_loads"),2); EXPECT_EQ(fields.at("peak_resident_fields"),1);
    EXPECT_GE(role_result.at("stage_seconds").at("fields_load").get<f64>(),0);
    EXPECT_GE(role_result.at("stage_seconds").at("fields_verify").get<f64>(),0);
    const auto name=role_result.at("role").get<std::string>();
    EXPECT_EQ(read_json(dir.path/"output"/(name+"_combined.json")).at("research_fields_manifest_sha256"),pin);
    // Every candidate increases in i on TRAIN (sign +1); the four equal weights
    // then blend to the same centered rank as each component.
    // Named: a range-for over read_json(...).at(...) would iterate a destroyed temporary.
    const auto orientations=read_json(dir.path/"output"/"orientations.json");
    for (const auto& row:orientations.at("candidates")) EXPECT_EQ(row.at("sign"),1);
    std::vector<f64> signal(D*N); ASSERT_TRUE(read_payload(dir.path/"output"/(name+"_combined.f64"),signal));
    for (usize d=63;d<D;++d) for (usize i=0;i<N;++i)
      ASSERT_DOUBLE_EQ(signal[d*N+i],static_cast<f64>(i)/static_cast<f64>(N-1)-.5) << name << d << i;
  }
}
TEST(StrategyIcRunner, UnreferencedFieldsAreNeverOpenedOrAdmitted) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  // Declared but unreferenced iv_atm_21d must exist in the manifest; mkt_ret is not declared.
  ASSERT_TRUE(field_library(cfg,{"si_shares","iv_atm_21d"},{{"si_ratio","si_shares / volume"}}));
  ASSERT_TRUE(pin_fields(dir,cfg,"f",{"si_shares","iv_atm_21d","mkt_ret"}));
  for (const auto& fields_path:{cfg.train_fields_directory,cfg.validation_fields_directory})
    for (const auto* unused:{"iv_atm_21d.f64","mkt_ret.f64"})
      ASSERT_TRUE(std::filesystem::remove(std::filesystem::path(fields_path)/unused));
  const auto run=run_named(dir,cfg,"output"); ASSERT_TRUE(run.ok) << run.error;
  const auto summary=read_json(dir.path/"output"/"summary.json");
  for (const auto& role_result:summary.at("roles")) {
    EXPECT_EQ(role_result.at("research_fields").at("loaded"),Json::array({"si_shares"}));
    EXPECT_EQ(role_result.at("research_fields").at("files").size(),1U);
    EXPECT_EQ(role_result.at("research_fields").at("loaded_bytes"),D*N*sizeof(f64));
  }
  // Admission: one referenced field adds exactly 8B/cell plus the 1B/cell DSL-panel
  // presence mask. The comparison library has the same shape over `volume`.
  const auto required=[&](atx::impl::strategy::IcRunnerConfig plan_cfg) {
    plan_cfg.plan_only=true; std::ostringstream plan;
    const auto status=atx::impl::strategy::run_ic(plan_cfg,plan);
    EXPECT_TRUE(status) << status.error().to_string();
    return status?Json::parse(plan.str()).at("roles").at(0).at("required_bytes").get<u64>():u64{};
  };
  auto base_cfg=cfg; clear_fields(base_cfg);
  auto lib=read_json(cfg.library_path);
  lib["fields"]=Json::array({{{"name","close"}},{{"name","raw_close"}},{{"name","volume"}}});
  // Same compiled shape (two distinct loads and a divide), so slots are equal.
  lib["candidates"][2]["dsl"]="raw_close / volume";
  base_cfg.library_path=(dir.path/"base_library.json").string();
  ASSERT_TRUE(json_file(base_cfg.library_path,lib,base_cfg.library_sha256));
  EXPECT_EQ(required(cfg),required(base_cfg)+D*N*(8+1));
  // Pinned fields that no candidate references admit and load nothing.
  auto pinned_base=base_cfg; pinned_base.train_fields_directory=cfg.train_fields_directory;
  pinned_base.train_fields_sha256=cfg.train_fields_sha256;
  pinned_base.validation_fields_directory=cfg.validation_fields_directory;
  pinned_base.validation_fields_sha256=cfg.validation_fields_sha256;
  EXPECT_EQ(required(pinned_base),required(base_cfg));
}
TEST(StrategyIcRunner, FieldBindingRefusalsPrecedeRolePayloadAndOutput) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(field_library(cfg,{"si_shares"},{{"si_ratio","si_shares / volume"}}));
  ASSERT_TRUE(pin_fields(dir,cfg,"f",{"si_shares"}));
  const auto good=cfg; const auto good_library=read_json(cfg.library_path);
  // Role payloads are absent: a refusal after any role payload read would differ.
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  ASSERT_TRUE(std::filesystem::remove(dir.path/"validation"/"close.f64"));
  const auto train_fields=std::filesystem::path(cfg.train_fields_directory);
  const auto expect_refusal=[&](atx::impl::strategy::IcRunnerConfig attempt_cfg,const std::string& reason) {
    for (const bool plan_only:{true,false}) {
      attempt_cfg.plan_only=plan_only; std::ostringstream attempt;
      const auto status=atx::impl::strategy::run_ic(attempt_cfg,attempt);
      ASSERT_FALSE(status) << reason;
      EXPECT_NE(status.error().to_string().find(reason),std::string::npos)
          << reason << " -> " << status.error().to_string();
      EXPECT_TRUE(attempt.str().empty()) << reason; EXPECT_FALSE(std::filesystem::exists(dir.path/"output"));
    }
  };
  const auto with_library=[&](const Json& lib) {
    auto next=good; next.library_path=(dir.path/"variant_library.json").string();
    EXPECT_TRUE(json_file(next.library_path,lib,next.library_sha256)); return next;
  };
  // Declared field absent from the pinned manifest; undeclared DSL field.
  auto declared=good_library; declared["fields"].push_back({{"name","short_ratio"}});
  declared["candidates"].push_back({{"id","short_level"},{"family","field_family"},{"dsl","short_ratio"},
      {"sign_policy","train-rank-ic21"},{"horizons",{5,21,63}}});
  expect_refusal(with_library(declared),
      "library field 'short_ratio' is neither a role price field nor in the pinned train fields manifest");
  auto undeclared=good_library; undeclared["candidates"][2]["dsl"]="si_dtc / volume";
  expect_refusal(with_library(undeclared),"undeclared DSL field: si_dtc");
  auto dotted=good_library; dotted["fields"].push_back({{"name","IndClass.sector"}});
  expect_refusal(with_library(dotted),"declared field contract: IndClass.sector");
  // No manifest for a scored role, or an unpaired option.
  auto missing=good; clear_fields(missing);
  expect_refusal(missing,"library field 'si_shares' is not a role price field and no --train-fields manifest is pinned");
  auto no_validation=good; no_validation.validation_fields_directory.clear(); no_validation.validation_fields_sha256.clear();
  expect_refusal(no_validation,"no --validation-fields manifest is pinned");
  auto unpaired=good; unpaired.train_fields_sha256.clear();
  expect_refusal(unpaired,"bounded config");
  // Wrong role binding: the validation manifest pinned as TRAIN's.
  auto swapped=good; swapped.train_fields_directory=good.validation_fields_directory;
  swapped.train_fields_sha256=good.validation_fields_sha256;
  expect_refusal(swapped,"train fields manifest role binding differs");
  auto wrong_pin=good; wrong_pin.train_fields_sha256=std::string(64,'0');
  expect_refusal(wrong_pin,"external metadata pin differs");
  // Consistently re-pinned manifests that lie about axes or entries.
  const auto manifest=read_json(train_fields/"manifest.json");
  const auto repinned=[&](const Json& lie) {
    auto next=good; EXPECT_TRUE(json_file(train_fields/"manifest.json",lie,next.train_fields_sha256)); return next;
  };
  auto sessions=manifest; sessions["role"]["sessions_sha256"]=std::string(64,'0');
  expect_refusal(repinned(sessions),"train fields manifest role binding differs");
  auto shape=manifest; shape["fields"][0]["shape"]=Json::array({D,N+1});
  expect_refusal(repinned(shape),"train fields manifest entry: si_shares");
  auto schema=manifest; schema["schema"]="atx.research-role-fields/v0";
  expect_refusal(repinned(schema),"train fields manifest schema");
  std::string unused; ASSERT_TRUE(json_file(train_fields/"manifest.json",manifest,unused));
  ASSERT_EQ(unused,good.train_fields_sha256);
  // A truncated referenced payload is an extent refusal even in plan-only mode.
  const auto payload_path=train_fields/"si_shares.f64"; const auto original=file_bytes(payload_path);
  auto truncated=original; truncated.resize(truncated.size()-sizeof(f64));
  ASSERT_TRUE(write_bytes(payload_path,truncated));
  expect_refusal(good,"research field payload extent: si_shares.f64");
  // A same-size tampered payload passes metadata-only planning, then refuses on
  // its streamed hash before the (deleted) role payload is ever opened.
  auto flipped=original; flipped[8*N*100]=static_cast<char>(flipped[8*N*100]^1);
  ASSERT_TRUE(write_bytes(payload_path,flipped));
  auto plan_cfg=good; plan_cfg.plan_only=true; std::ostringstream plan;
  auto status=atx::impl::strategy::run_ic(plan_cfg,plan); ASSERT_TRUE(status) << status.error().to_string();
  auto run_cfg=good; const auto attempt=run_named(dir,run_cfg,"tampered");
  EXPECT_FALSE(attempt.ok);
  EXPECT_NE(attempt.error.find("research field payload SHA256 mismatch: si_shares.f64"),std::string::npos) << attempt.error;
  EXPECT_EQ(read_json(dir.path/"tampered"/"summary.json").at("status"),"failed");
  EXPECT_EQ(file_bytes(payload_path),flipped);
}
// ---- T22: `grp_`-prefixed research fields are DSL group classifiers ----
enum class GroupOp { Rank, Neutralize, Mean, Zscore };
// Scalar oracle for one raw VM cell of `op(volume, grp_a)`. From date 63 every name
// is a decision member with finite volume, so all are valid; a name's group is the
// valid names sharing its finite label, in ascending index. A NaN label has no
// group: that cell stays NaN and its volume enters no group statistic.
f64 group_expected(GroupOp op,usize d,usize i) {
  constexpr f64 missing=std::numeric_limits<f64>::quiet_NaN();
  const auto volume=[](usize j) { return 1e8*static_cast<f64>(j+1); };
  const f64 label=group_label(d,i);
  if (d<63 || std::isnan(label)) return missing;
  std::vector<usize> members;
  for (usize j=0;j<N;++j) if (group_label(d,j)==label) members.push_back(j);
  f64 sum=0; for (const auto j:members) sum+=volume(j);
  const auto count=static_cast<f64>(members.size()); const f64 mean=sum/count;
  switch (op) {
  case GroupOp::Rank: { // volume rises with the index: the rank is the position in the group
    const auto at=static_cast<f64>(std::find(members.begin(),members.end(),i)-members.begin());
    return members.size()==1?.5:at/(count-1);
  }
  case GroupOp::Neutralize: return volume(i)-mean;
  case GroupOp::Mean: return mean;
  case GroupOp::Zscore: {
    if (members.size()<2) return missing;
    f64 squares=0; for (const auto j:members) squares+=(volume(j)-mean)*(volume(j)-mean);
    return (volume(i)-mean)/std::sqrt(squares/(count-1));
  }
  }
  return missing;
}
bool near_value(f64 a,f64 b) {
  return (std::isnan(a) && std::isnan(b)) || std::abs(a-b)<=1e-12*std::max(1.0,std::abs(b));
}
TEST(StrategyIcRunner, GrpFieldsBindAsGroupClassifiersAndExcludeNaNLabels) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  const std::vector<std::pair<std::string,GroupOp>> ops{{"grp_rank",GroupOp::Rank},
      {"grp_neutralize",GroupOp::Neutralize},{"grp_mean",GroupOp::Mean},{"grp_zscore",GroupOp::Zscore}};
  ASSERT_TRUE(field_library(cfg,{"grp_a"},{{"grp_rank","group_rank(volume, grp_a)"},
      {"grp_neutralize","group_neutralize(volume, grp_a)"},{"grp_mean","group_mean(volume, grp_a)"},
      {"grp_zscore","group_zscore(volume, grp_a)"}}));
  ASSERT_TRUE(pin_fields(dir,cfg,"f",{"grp_a"}));
  cfg.candidate_cache_directory=(dir.path/"c").string();
  const auto run=run_named(dir,cfg,"output"); ASSERT_TRUE(run.ok) << run.error;
  // One resident column serves all four group candidates.
  for (const std::string role_name:{"train","validation"})
    EXPECT_NE(run.log.find("IC fields-verified role="+role_name+" fields=grp_a resident_capacity=1 "
                           "planned_loads=1"),std::string::npos) << role_name;
  // Raw VM signals (the cache stores them verbatim) against the scalar oracle.
  for (const std::string role_name:{"train","validation"}) {
    for (const auto& [id,op]:ops) {
      const auto entry=cache_entry(dir.path/"output"/"summary.json",role_name,id);
      std::vector<f64> signal(D*N); ASSERT_TRUE(read_payload(entry.payload,signal)) << id;
      for (usize d=0;d<D;++d) for (usize i=0;i<N;++i)
        ASSERT_TRUE(near_value(signal[d*N+i],group_expected(op,d,i)))
            << id << ' ' << d << ' ' << i << ": " << signal[d*N+i] << " vs " << group_expected(op,d,i);
      const auto recorded=read_json(entry.sidecar).at("field_payload_sha256");
      EXPECT_EQ(recorded.size(),1U) << id; EXPECT_TRUE(recorded.contains("grp_a")) << id;
    }
  }
  // Used as a number, a grp_ field is refused when the library compiles.
  auto numeric=cfg; auto lib=read_json(cfg.library_path);
  lib["candidates"][2]["dsl"]="rank(grp_a)";
  numeric.library_path=(dir.path/"numeric_library.json").string();
  ASSERT_TRUE(json_file(numeric.library_path,lib,numeric.library_sha256));
  numeric.plan_only=true; std::ostringstream plan;
  const auto refused=atx::impl::strategy::run_ic(numeric,plan); ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().to_string().find("got a Group classifier"),std::string::npos)
      << refused.error().to_string();
}
TEST(StrategyIcRunner, AbsentFieldOptionsLeaveRecipeAndOutputsUnchanged) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.save_combined=true;
  const auto plain=run_named(dir,cfg,"plain"); ASSERT_TRUE(plain.ok) << plain.error;
  // The canonical recipe key set is exactly the pre-fields one.
  const auto recipe=read_json(dir.path/"plain"/"recipe.json");
  std::set<std::string> keys; for (auto it=recipe.begin();it!=recipe.end();++it) keys.insert(it.key());
  EXPECT_EQ(keys,(std::set<std::string>{"schema","library_sha256","horizons","active_horizons",
      "require_endpoint_presence","execution_delay","min_names","min_dates","screen_rule","practical_abs_ic",
      "confidence_multiplier","max_working_bytes","vm","labels","guard","orientation","composition",
      "planned_targets","scope","saved_combined","role_manifest_sha256"}));
  const auto summary=read_json(dir.path/"plain"/"summary.json");
  EXPECT_FALSE(summary.contains("research_fields"));
  for (const auto& role_result:summary.at("roles")) {
    EXPECT_FALSE(role_result.contains("research_fields"));
    EXPECT_FALSE(role_result.at("stage_seconds").contains("fields_load"));
    const auto name=role_result.at("role").get<std::string>();
    EXPECT_FALSE(read_json(dir.path/"plain"/(name+"_combined.json")).contains("research_fields_manifest_sha256"));
  }
  for (const auto* line:{"IC fields-verified","IC field-load","IC field-release"})
    EXPECT_EQ(plain.log.find(line),std::string::npos) << line;
  auto plan_cfg=cfg; plan_cfg.plan_only=true; std::ostringstream plan;
  ASSERT_TRUE(atx::impl::strategy::run_ic(plan_cfg,plan)); EXPECT_FALSE(Json::parse(plan.str()).contains("research_fields"));
  // Pinned but unreferenced fields: identical numerics; only the pin records differ.
  ASSERT_TRUE(pin_fields(dir,cfg,"f",{"si_shares"}));
  const auto pinned=run_named(dir,cfg,"pinned"); ASSERT_TRUE(pinned.ok) << pinned.error;
  for (const auto& file:exact_outputs()) {
    if (file=="recipe.json" || file=="orientations.json" || file.ends_with("_combined.json")) continue;
    EXPECT_EQ(file_sha(dir.path/"pinned"/file),file_sha(dir.path/"plain"/file)) << file;
  }
  EXPECT_EQ(read_json(dir.path/"pinned"/"orientations.json").at("candidates"),
            read_json(dir.path/"plain"/"orientations.json").at("candidates"));
  auto pinned_recipe=read_json(dir.path/"pinned"/"recipe.json");
  EXPECT_EQ(pinned_recipe.at("research_fields").at("loaded"),Json::array());
  pinned_recipe.erase("research_fields"); EXPECT_EQ(pinned_recipe,recipe);
  const auto a=read_json(dir.path/"plain"/"summary.json"),b=read_json(dir.path/"pinned"/"summary.json");
  for (usize r=0;r<2;++r) {
    auto y=b.at("roles").at(r); EXPECT_EQ(y.at("research_fields").at("loaded"),Json::array());
    y.erase("research_fields");
    EXPECT_EQ(stable_role(y,false),stable_role(a.at("roles").at(r),false)) << r;
  }
}
// Platform v7 L1: a field candidate is keyed by the payload SHA256 of exactly the
// fields its DSL reads, not by the fields manifest. A new manifest that only adds a
// field misses nothing; one changed payload misses only the candidates reading it.
TEST(StrategyIcRunner, CandidateCacheKeyChangesOnlyForCandidatesReadingAChangedFieldPayload) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.candidate_cache_directory=(dir.path/"c").string();
  ASSERT_TRUE(field_library(cfg,{"si_shares","mkt_ret"},
      {{"si_ratio","si_shares / volume"},{"market_shift","volume + mkt_ret"}}));
  // Manifest a: si_shares (scale 1) and mkt_ret.
  ASSERT_TRUE(pin_fields(dir,cfg,"a",{"si_shares","mkt_ret"}));
  const auto a=run_named(dir,cfg,"run_a"); ASSERT_TRUE(a.ok) << a.error;
  EXPECT_EQ(a.log.find("IC cache-hit"),std::string::npos);
  const auto a_summary=dir.path/"run_a"/"summary.json";
  // Manifest b: the same two payloads plus an unreferenced field -- another manifest
  // SHA256, the same content keys: every candidate hits and no column is loaded.
  ASSERT_TRUE(pin_fields(dir,cfg,"b",{"si_shares","mkt_ret","iv_atm_21d"}));
  const auto b=run_named(dir,cfg,"run_b"); ASSERT_TRUE(b.ok) << b.error;
  EXPECT_EQ(b.log.find("IC cache-miss"),std::string::npos);
  EXPECT_EQ(b.log.find("IC field-load"),std::string::npos);
  EXPECT_EQ(b.log.find("IC VM-"),std::string::npos);
  for (const std::string role_name:{"train","validation"})
    for (const std::string id:{"volume_level","volume_rank","si_ratio","market_shift"})
      EXPECT_EQ(cache_entry(dir.path/"run_b"/"summary.json",role_name,id).sidecar.string(),
                cache_entry(a_summary,role_name,id).sidecar.string()) << role_name << ' ' << id;
  // Manifest c: si_shares rescaled (a new payload), mkt_ret unchanged. Only si_ratio
  // reads it, so only si_ratio misses; it lands in its own fp_ directory and the
  // stale entry is left untouched.
  const auto stale=cache_entry(a_summary,"train","si_ratio"); const auto stale_sha=file_sha(stale.payload);
  ASSERT_TRUE(pin_fields(dir,cfg,"c",{"si_shares","mkt_ret"},2));
  const auto c=run_named(dir,cfg,"run_c"); ASSERT_TRUE(c.ok) << c.error;
  for (const std::string role_name:{"train","validation"}) {
    for (const auto* id:{"volume_level","volume_rank","market_shift"})
      EXPECT_NE(c.log.find("IC cache-hit "+std::string(id)+" role="+role_name+" layout=v2"),std::string::npos)
          << role_name << ' ' << id;
    EXPECT_NE(c.log.find("IC cache-miss si_ratio role="+role_name),std::string::npos) << role_name;
    EXPECT_EQ(c.log.find("IC field-load role="+role_name+" field=mkt_ret"),std::string::npos) << role_name;
  }
  const auto c_summary=dir.path/"run_c"/"summary.json";
  const auto fresh=cache_entry(c_summary,"train","si_ratio");
  EXPECT_NE(fresh.sidecar.parent_path().string(),stale.sidecar.parent_path().string());
  EXPECT_EQ(fresh.sidecar.filename().string(),stale.sidecar.filename().string());
  EXPECT_EQ(file_sha(stale.payload),stale_sha);
  const auto c_roles=read_json(c_summary); // named: see ExtraFieldsResolveByName...
  for (const auto& role_result:c_roles.at("roles")) {
    EXPECT_EQ(role_result.at("candidate_cache").at("hits"),3);
    EXPECT_EQ(role_result.at("candidate_cache").at("misses"),1);
    EXPECT_EQ(role_result.at("candidate_cache").at("vm_evaluations"),1);
  }
  // Content key, exactly as specified (strategy_ic_runner.cpp signal_key_text).
  const auto summary=read_json(c_summary); const auto& train_cache=summary.at("roles").at(0).at("candidate_cache");
  const auto sidecar=read_json(fresh.sidecar);
  auto dsl=core::sha256_hex("si_shares / volume"); ASSERT_TRUE(dsl);
  const auto si_sha=file_sha(std::filesystem::path(cfg.train_fields_directory)/"si_shares.f64");
  const std::string key_text="atx.dsl-candidate-signal-key/v2\nvm_identity="+
      train_cache.at("vm_identity").get<std::string>()+
      "\neval_mode=ResearchFast;full-historical-asof-member-mask"
      "\nlayout=date-major-little-endian-f64;non-finite-stored-as-quiet-NaN"
      "\nrole_manifest_sha256="+cfg.train_sha256+"\ndates="+std::to_string(D)+"\ninstruments="+std::to_string(N)+
      "\ndsl_sha256="+*dsl+"\nfield=si_shares:"+si_sha+"\n";
  auto key=core::sha256_hex(key_text); ASSERT_TRUE(key);
  EXPECT_EQ(sidecar.at("signal_key_sha256"),*key);
  auto field_key=core::sha256_hex("field=si_shares:"+si_sha+"\n"); ASSERT_TRUE(field_key);
  EXPECT_EQ(fresh.sidecar.parent_path().filename().string(),"fp_"+field_key->substr(0,16));
  EXPECT_EQ(fresh.sidecar.filename().string(),"si_ratio."+dsl->substr(0,16)+".json");
  EXPECT_EQ(sidecar.at("field_payload_sha256"),Json({{"si_shares",si_sha}}));
  EXPECT_EQ(sidecar.at("fields_manifest_sha256"),cfg.train_fields_sha256);
  // The summary names every candidate's entry with its key parts.
  ASSERT_EQ(train_cache.at("entries").size(),4U);
  for (const auto& entry:train_cache.at("entries"))
    if (entry.at("id")=="si_ratio") {
      EXPECT_EQ(entry.at("signal_key_sha256"),*key); EXPECT_EQ(entry.at("layout"),"v2");
      EXPECT_EQ(entry.at("payload_sha256"),file_sha(fresh.payload));
    }
  // An entry copied into another key's directory is refused, never served.
  ASSERT_TRUE(std::filesystem::remove(fresh.sidecar)); ASSERT_TRUE(std::filesystem::remove(fresh.payload));
  ASSERT_TRUE(std::filesystem::copy_file(stale.sidecar,fresh.sidecar));
  ASSERT_TRUE(std::filesystem::copy_file(stale.payload,fresh.payload));
  const auto refused=run_named(dir,cfg,"foreign");
  EXPECT_FALSE(refused.ok);
  EXPECT_NE(refused.error.find("candidate cache entry mismatch: si_ratio"),std::string::npos) << refused.error;
}
// A changed DSL under an existing id is a new entry, never a refusal: in v2 the
// DSL SHA256 names the file, and a v1 entry recording another DSL is ignored.
TEST(StrategyIcRunner, CandidateCacheChangedDslUnderSameIdIsANewEntry) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.candidate_cache_directory=(dir.path/"c").string();
  const auto first=run_named(dir,cfg,"first"); ASSERT_TRUE(first.ok) << first.error;
  const auto old_rank=cache_entry(dir.path/"first"/"summary.json","train","volume_rank");
  // The same id with another DSL: a miss and a second file beside the first.
  auto lib=read_json(cfg.library_path); lib["candidates"][1]["dsl"]="rank(raw_close)";
  ASSERT_TRUE(json_file(cfg.library_path,lib,cfg.library_sha256));
  const auto second=run_named(dir,cfg,"second"); ASSERT_TRUE(second.ok) << second.error;
  EXPECT_NE(second.log.find("IC cache-hit volume_level role=train"),std::string::npos);
  EXPECT_NE(second.log.find("IC cache-miss volume_rank role=train"),std::string::npos);
  const auto new_rank=cache_entry(dir.path/"second"/"summary.json","train","volume_rank");
  EXPECT_EQ(new_rank.sidecar.filename().string(),v2_stem("volume_rank","rank(raw_close)")+".json");
  EXPECT_TRUE(std::filesystem::exists(old_rank.sidecar));
  // A v1 entry of the old DSL where the v1 layout keeps it: ignored, not refused.
  ASSERT_TRUE(std::filesystem::remove(new_rank.sidecar)); ASSERT_TRUE(std::filesystem::remove(new_rank.payload));
  ASSERT_TRUE(to_v1(old_rank,old_rank.sidecar.parent_path(),"volume_rank"));
  const auto third=run_named(dir,cfg,"third"); ASSERT_TRUE(third.ok) << third.error;
  EXPECT_NE(third.log.find("IC cache-miss volume_rank role=train"),std::string::npos);
  EXPECT_TRUE(std::filesystem::exists(old_rank.sidecar.parent_path()/"volume_rank.json"));
  for (const auto* file:{"orientations.json","train_daily_ic.csv","validation_daily_ic.csv"})
    EXPECT_EQ(file_sha(dir.path/"third"/file),file_sha(dir.path/"second"/file)) << file;
}
// The v1 layout stays readable in place: a base entry under <role sha>/<id>.json,
// and a field entry under <fields manifest sha>/<id>.json -- served when a known
// manifest (the pinned one, or one named by --cache-legacy-fields) gives each field
// the candidate reads the pinned payload SHA256. v1 hits reproduce the outputs.
TEST(StrategyIcRunner, CandidateCacheReadsV1EntriesInPlaceThroughKnownManifests) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.save_combined=true; cfg.candidate_cache_directory=(dir.path/"c").string();
  ASSERT_TRUE(field_library(cfg,{"si_shares","mkt_ret"},
      {{"si_ratio","si_shares / volume"},{"market_shift","volume + mkt_ret"}}));
  ASSERT_TRUE(pin_fields(dir,cfg,"old",{"si_shares","mkt_ret"}));
  const auto old_cfg=cfg;
  const auto cold=run_named(dir,cfg,"cold"); ASSERT_TRUE(cold.ok) << cold.error;
  // Rewrite every entry as the v1 runner laid it out (fields entries keyed by the
  // "old" manifest), drop the IC results, and forget the v2 files.
  const auto cold_summary=dir.path/"cold"/"summary.json"; const auto root=cache_root(cold_summary);
  for (const auto& [role_name,role_pin,fields_pin]:{
           std::tuple{std::string("train"),cfg.train_sha256,cfg.train_fields_sha256},
           std::tuple{std::string("validation"),cfg.validation_sha256,cfg.validation_fields_sha256}}) {
    for (const std::string id:{"volume_level","volume_rank"})
      ASSERT_TRUE(to_v1(cache_entry(cold_summary,role_name,id),root/role_pin,id));
    for (const std::string id:{"si_ratio","market_shift"})
      ASSERT_TRUE(to_v1(cache_entry(cold_summary,role_name,id),root/fields_pin,id,fields_pin));
  }
  for (const auto& top:std::filesystem::directory_iterator(root))
    for (const auto& sub:std::filesystem::directory_iterator(top.path()))
      if (sub.is_directory()) std::filesystem::remove_all(sub.path());
  // Under the pinned "old" manifest: the v1 key itself, so every entry hits in place.
  const auto same=run_named(dir,cfg,"same"); ASSERT_TRUE(same.ok) << same.error;
  EXPECT_EQ(same.log.find("IC cache-miss"),std::string::npos);
  EXPECT_NE(same.log.find("IC cache-hit si_ratio role=train layout=v1"),std::string::npos);
  EXPECT_NE(same.log.find("IC cache-preflight role=train ready=4/4 legacy=4"),std::string::npos);
  // A newer manifest ("new": the same payloads plus one field). Without naming the
  // old manifest, only the base entries are reachable; the field candidates miss.
  ASSERT_TRUE(pin_fields(dir,cfg,"new",{"si_shares","mkt_ret","iv_atm_21d"}));
  auto unnamed=cfg; unnamed.candidate_cache_directory=(dir.path/"c_copy").string();
  std::filesystem::copy(dir.path/"c",dir.path/"c_copy",std::filesystem::copy_options::recursive);
  const auto without=run_named(dir,unnamed,"without"); ASSERT_TRUE(without.ok) << without.error;
  EXPECT_NE(without.log.find("IC cache-hit volume_level role=train layout=v1"),std::string::npos);
  EXPECT_NE(without.log.find("IC cache-miss si_ratio role=train"),std::string::npos);
  // Naming the old fields directories: all four hit in place, no VM, no column.
  cfg.candidate_cache_legacy_fields={old_cfg.train_fields_directory,old_cfg.validation_fields_directory};
  const auto named=run_named(dir,cfg,"named"); ASSERT_TRUE(named.ok) << named.error;
  EXPECT_EQ(named.log.find("IC cache-miss"),std::string::npos);
  EXPECT_EQ(named.log.find("IC VM-"),std::string::npos);
  EXPECT_EQ(named.log.find("IC field-load"),std::string::npos);
  const auto named_summary=read_json(dir.path/"named"/"summary.json");
  for (const auto& role_result:named_summary.at("roles")) {
    EXPECT_EQ(role_result.at("candidate_cache").at("hits"),4);
    EXPECT_EQ(role_result.at("candidate_cache").at("legacy_hits"),4);
    EXPECT_EQ(role_result.at("research_fields").at("field_loads"),0);
  }
  EXPECT_EQ(cache_entry(dir.path/"named"/"summary.json","train","si_ratio").sidecar.string(),
            (root/old_cfg.train_fields_sha256/"si_ratio.json").string());
  // A manifest whose payload differs for a field the candidate reads never serves it.
  auto rescaled=cfg; ASSERT_TRUE(pin_fields(dir,rescaled,"rescaled",{"si_shares","mkt_ret"},2));
  rescaled.candidate_cache_legacy_fields=cfg.candidate_cache_legacy_fields;
  const auto changed=run_named(dir,rescaled,"changed"); ASSERT_TRUE(changed.ok) << changed.error;
  EXPECT_NE(changed.log.find("IC cache-miss si_ratio role=train"),std::string::npos);
  EXPECT_NE(changed.log.find("IC cache-hit market_shift role=train layout=v1"),std::string::npos);
  // v1 hits, v1 IC results and uncached runs agree byte for byte.
  auto plain=cfg; plain.candidate_cache_directory.clear(); plain.candidate_cache_legacy_fields.clear();
  const auto uncached=run_named(dir,plain,"uncached"); ASSERT_TRUE(uncached.ok) << uncached.error;
  for (const std::string name:{"same","named"})
    for (const auto& file:exact_outputs()) {
      if (file=="recipe.json" || file=="orientations.json" || file.ends_with("_combined.json")) continue;
      EXPECT_EQ(file_sha(dir.path/name/file),file_sha(dir.path/"uncached"/file)) << name << ' ' << file;
    }
  // Option hygiene: legacy manifests need the cache; a malformed one refuses up front.
  auto orphan_option=plain; orphan_option.candidate_cache_legacy_fields={old_cfg.train_fields_directory};
  const auto no_cache=run_named(dir,orphan_option,"no_cache"); EXPECT_FALSE(no_cache.ok);
  EXPECT_NE(no_cache.error.find("bounded config"),std::string::npos) << no_cache.error;
  auto bad=cfg; bad.candidate_cache_legacy_fields={(dir.path/"train").string()};
  const auto refused=run_named(dir,bad,"bad"); EXPECT_FALSE(refused.ok);
  EXPECT_NE(refused.error.find("--cache-legacy-fields"),std::string::npos) << refused.error;
  EXPECT_FALSE(std::filesystem::exists(dir.path/"bad"));
}
// Windows without long-path opt-in caps a path at 259 characters. The deepest
// committed file of the v2 layout is a field candidate's IC result,
// ROOT/<role>/fp_<16>/ic1_<16>/<id>.<dsl16>.json. With the cache directory padded
// so that path is exactly 255 characters, every write still lands: a transient
// partial is never deeper than the file it publishes (the former
// ".<final name>.<nonce>.partial" was ~26 characters deeper and failed to open).
TEST(StrategyIcRunner, CandidateCacheWritesFitWithinTheDeepestCommittedPath) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(field_library(cfg,{"si_shares","mkt_ret"},
      {{"si_ratio","si_shares / volume"},{"market_shift","volume + mkt_ret"}}));
  ASSERT_TRUE(pin_fields(dir,cfg,"f",{"si_shares","mkt_ret"}));
  // A shallow run shows the layout below the cache directory (identity level,
  // fp_ and ic1_ names) for this build.
  auto shallow=cfg; shallow.candidate_cache_directory=(dir.path/"c").string();
  const auto probe=run_named(dir,shallow,"probe"); ASSERT_TRUE(probe.ok) << probe.error;
  const auto probe_summary=dir.path/"probe"/"summary.json";
  const auto ic_subdirectory=read_json(probe_summary).at("roles").at(0).at("candidate_cache")
      .at("ic_results").at("subdirectory").get<std::string>();
  const auto deepest=[&ic_subdirectory](const std::filesystem::path& summary) {
    const auto sidecar=cache_entry(summary,"train","market_shift").sidecar;
    return sidecar.parent_path()/ic_subdirectory/sidecar.filename();
  };
  const usize below=deepest(probe_summary).string().size()-shallow.candidate_cache_directory.size();
  constexpr usize budget=255;
  const usize parent=dir.path.string().size()+1;
  if (parent+1+below>budget) GTEST_SKIP() << "temp directory too deep for the path budget: " << dir.path;
  cfg.candidate_cache_directory=(dir.path/std::string(budget-below-parent,'d')).string();
  const auto cold=run_named(dir,cfg,"cold"); ASSERT_TRUE(cold.ok) << cold.error;
  const auto ic_result=deepest(dir.path/"cold"/"summary.json");
  EXPECT_EQ(ic_result.string().size(),budget) << ic_result;
  EXPECT_TRUE(std::filesystem::exists(ic_result)) << ic_result;
  // Warm at that depth: every signal and IC result hits; no partial is left behind.
  const auto warm=run_named(dir,cfg,"warm"); ASSERT_TRUE(warm.ok) << warm.error;
  EXPECT_EQ(warm.log.find("IC cache-miss"),std::string::npos);
  const auto warm_summary=read_json(dir.path/"warm"/"summary.json");
  for (const auto& role_result:warm_summary.at("roles")) {
    EXPECT_EQ(role_result.at("candidate_cache").at("hits"),4);
    EXPECT_EQ(role_result.at("candidate_cache").at("ic_results").at("hits"),4);
  }
  for (const auto& entry:std::filesystem::recursive_directory_iterator(cfg.candidate_cache_directory))
    EXPECT_FALSE(entry.path().filename().string().ends_with(".partial")) << entry.path();
}
// --cache-report: metadata only, no output directory; hits and misses per role,
// and every entry under ROOT that no candidate resolves to, with its bytes.
TEST(StrategyIcRunner, CacheReportAccountsHitsMissesAndUnreferencedEntries) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.candidate_cache_directory=(dir.path/"c").string();
  const auto cold=run_named(dir,cfg,"cold"); ASSERT_TRUE(cold.ok) << cold.error;
  const auto cold_summary=dir.path/"cold"/"summary.json";
  const auto report_for=[&](atx::impl::strategy::IcRunnerConfig report_cfg) {
    report_cfg.cache_report=true; report_cfg.output_directory.clear(); std::ostringstream out;
    const auto status=atx::impl::strategy::run_ic(report_cfg,out);
    EXPECT_TRUE(status) << status.error().to_string();
    return status?Json::parse(out.str()):Json();
  };
  const auto entry_bytes=[&](const std::string& role_name,const std::string& id) {
    const auto entry=cache_entry(cold_summary,role_name,id);
    u64 total=std::filesystem::file_size(entry.sidecar)+std::filesystem::file_size(entry.payload);
    for (const auto& sub:std::filesystem::directory_iterator(entry.sidecar.parent_path()))
      if (sub.is_directory() && std::filesystem::exists(sub.path()/entry.sidecar.filename()))
        total+=std::filesystem::file_size(sub.path()/entry.sidecar.filename());
    return total;
  };
  // Everything the library needs is present: all hits, nothing unreferenced.
  const auto full=report_for(cfg);
  EXPECT_EQ(full.at("mode"),"cache-report");
  ASSERT_EQ(full.at("roles").size(),2U);
  for (const auto& role_report:full.at("roles")) {
    const auto role_name=role_report.at("role").get<std::string>();
    EXPECT_EQ(role_report.at("hits"),2); EXPECT_EQ(role_report.at("misses"),0);
    EXPECT_EQ(role_report.at("hit_bytes"),entry_bytes(role_name,"volume_level")+entry_bytes(role_name,"volume_rank"));
  }
  EXPECT_EQ(full.at("unreferenced").at("entries"),0); EXPECT_EQ(full.at("unreferenced").at("bytes"),0);
  EXPECT_EQ(full.at("orphans").at("files"),0);
  // A library without volume_rank: its two entries are unreferenced, with bytes.
  auto smaller=cfg; auto lib=read_json(cfg.library_path); lib["candidates"].erase(1);
  smaller.library_path=(dir.path/"smaller_library.json").string();
  ASSERT_TRUE(json_file(smaller.library_path,lib,smaller.library_sha256));
  // ...and a library with a new DSL under volume_level: a miss.
  auto changed=cfg; lib=read_json(cfg.library_path); lib["candidates"][0]["dsl"]="volume + raw_close";
  changed.library_path=(dir.path/"changed_library.json").string();
  ASSERT_TRUE(json_file(changed.library_path,lib,changed.library_sha256));
  const auto partial=cache_root(cold_summary)/cfg.train_sha256/".stray.f64.0123456789abcdef.partial";
  ASSERT_TRUE(write_bytes(partial,std::vector<char>(24,'x')));
  const auto report=report_for(smaller);
  u64 expected_bytes=0; usize expected_entries=0;
  for (const std::string role_name:{"train","validation"}) { expected_bytes+=entry_bytes(role_name,"volume_rank"); ++expected_entries; }
  EXPECT_EQ(report.at("unreferenced").at("entries"),expected_entries);
  EXPECT_EQ(report.at("unreferenced").at("bytes"),expected_bytes);
  for (const auto& row:report.at("unreferenced").at("list")) EXPECT_EQ(row.at("candidate_id"),"volume_rank");
  EXPECT_EQ(report.at("orphans").at("files"),1); EXPECT_EQ(report.at("orphans").at("bytes"),24);
  const auto miss=report_for(changed);
  for (const auto& role_report:miss.at("roles")) {
    EXPECT_EQ(role_report.at("hits"),1); EXPECT_EQ(role_report.at("misses"),1);
    EXPECT_EQ(role_report.at("entries").at(0).at("status"),"miss");
    EXPECT_EQ(role_report.at("entries").at(1).at("layout"),"v2");
  }
  EXPECT_EQ(miss.at("unreferenced").at("entries"),2);
  // Metadata only: nothing written, no output directory; needs the cache.
  EXPECT_FALSE(std::filesystem::exists(dir.path/"output"));
  auto uncached=cfg; uncached.candidate_cache_directory.clear(); uncached.cache_report=true;
  std::ostringstream ignored; EXPECT_FALSE(atx::impl::strategy::run_ic(uncached,ignored));
  auto both=cfg; both.cache_report=true; both.plan_only=true;
  EXPECT_FALSE(atx::impl::strategy::run_ic(both,ignored));
}
TEST(StrategyIcRunner, LibraryDeclaringMktRetRunsOnlyWhenFieldsSupplyIt) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(field_library(cfg,{"mkt_ret"},{{"market_shift","volume + mkt_ret"}}));
  const auto refuse=[&](const std::string& reason) {
    auto attempt=cfg; attempt.plan_only=true; std::ostringstream log;
    const auto status=atx::impl::strategy::run_ic(attempt,log); ASSERT_FALSE(status);
    EXPECT_NE(status.error().to_string().find(reason),std::string::npos) << status.error().to_string();
  };
  refuse("library field 'mkt_ret' is not a role price field and no --train-fields manifest is pinned");
  ASSERT_TRUE(pin_fields(dir,cfg,"si_only",{"si_shares"}));
  refuse("library field 'mkt_ret' is neither a role price field nor in the pinned train fields manifest");
  ASSERT_TRUE(pin_fields(dir,cfg,"market",{"mkt_ret","si_shares"}));
  const auto run=run_named(dir,cfg,"output"); ASSERT_TRUE(run.ok) << run.error;
  const auto summary=read_json(dir.path/"output"/"summary.json");
  for (const auto& role_result:summary.at("roles"))
    EXPECT_EQ(role_result.at("research_fields").at("loaded"),Json::array({"mkt_ret"}));
}
TEST(StrategyIcRunner, RealV2LibraryDeclaringMktRetPlansOnlyWithPinnedFields) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  const auto source=std::filesystem::path{ATX_IMPL_TESTS_DIR}.parent_path()/"strategies"/"price_volume_ic96_v2.json";
  const auto bytes=file_bytes(source); ASSERT_FALSE(bytes.empty()) << source;
  const std::string text(bytes.begin(),bytes.end()); const auto library=Json::parse(text);
  std::set<std::string> declared;
  for (const auto& field:library.at("fields")) declared.insert(field.at("name").get<std::string>());
  ASSERT_EQ(declared,(std::set<std::string>{"close","raw_close","volume","mkt_ret"}));
  cfg.library_path=source.string(); auto digest=core::sha256_hex(text); ASSERT_TRUE(digest); cfg.library_sha256=*digest;
  // Metadata-only: widen the warmup so the library's longest lookback fits, then
  // bind fields to the re-pinned roles. No payload is read in plan-only mode.
  for (const auto& path:{cfg.train_manifest,cfg.validation_manifest}) {
    auto j=read_json(path); j["score_begin"]=450;
    j["score_start_ns"]=j.at("score_end_ns").get<i64>()-30*day;
    std::string pin; ASSERT_TRUE(json_file(path,j,pin));
    if (path==cfg.train_manifest) cfg.train_sha256=pin; else cfg.validation_sha256=pin;
  }
  cfg.plan_only=true;
  std::ostringstream unpinned; auto status=atx::impl::strategy::run_ic(cfg,unpinned); ASSERT_FALSE(status);
  EXPECT_NE(status.error().to_string().find("library field 'mkt_ret' is not a role price field"),std::string::npos)
      << status.error().to_string();
  ASSERT_TRUE(pin_fields(dir,cfg,"f",{"mkt_ret"}));
  std::ostringstream plan; status=atx::impl::strategy::run_ic(cfg,plan);
  ASSERT_TRUE(status) << status.error().to_string();
  const auto result=Json::parse(plan.str());
  EXPECT_EQ(result.at("candidate_count"),library.at("candidates").size());
  EXPECT_EQ(result.at("research_fields").at("loaded"),Json::array({"mkt_ret"}));
  EXPECT_EQ(result.at("research_fields").at("roles").size(),2U);
}
// Contract K1 (platform v8 B-3): --plan-only prints one `candidates` row per library
// member, in library order, with exactly {id, dsl_sha256, num_slots, required_lookback,
// extra_fields, node_count}; the numbers are the compiled program's (checked here
// against an independent compile of the same DSL) and no payload is opened.
TEST(StrategyIcRunner, PlanOnlyPrintsCandidateRows) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(field_library(cfg,{"si_shares"},{{"si_mean","ts_mean(si_shares, 5) / volume"}}));
  ASSERT_TRUE(pin_fields(dir,cfg,"f",{"si_shares"}));
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  cfg.plan_only=true; std::ostringstream plan_log;
  const auto status=atx::impl::strategy::run_ic(cfg,plan_log); ASSERT_TRUE(status) << status.error().to_string();
  EXPECT_FALSE(std::filesystem::exists(dir.path/"output"));
  const auto plan=Json::parse(plan_log.str());
  EXPECT_EQ(plan.at("candidate_count"),3);
  const auto& rows=plan.at("candidates"); ASSERT_TRUE(rows.is_array()); ASSERT_EQ(rows.size(),3U);
  const std::vector<std::tuple<std::string,std::string,Json>> expected{
      {"volume_level","volume",Json::array()},{"volume_rank","rank(volume)",Json::array()},
      {"si_mean","ts_mean(si_shares, 5) / volume",Json::array({"si_shares"})}};
  const engine::alpha::Library operators;
  u64 max_slots=0,max_lookback=0;
  for (usize k=0;k<expected.size();++k) {
    const auto& [id,dsl,extra]=expected[k]; const auto& row=rows.at(k);
    std::set<std::string> keys;
    for (auto it=row.begin();it!=row.end();++it) keys.insert(it.key());
    EXPECT_EQ(keys,(std::set<std::string>{"id","dsl_sha256","num_slots","required_lookback","extra_fields",
                                          "node_count"})) << id;
    EXPECT_EQ(row.at("id"),id);
    auto dsl_sha=core::sha256_hex(dsl); ASSERT_TRUE(dsl_sha); EXPECT_EQ(row.at("dsl_sha256"),*dsl_sha) << id;
    EXPECT_EQ(row.at("extra_fields"),extra) << id;
    auto ast=engine::alpha::parse_expr(dsl,operators); ASSERT_TRUE(ast) << id;
    auto analysis=engine::alpha::analyze(*ast); ASSERT_TRUE(analysis) << id;
    auto program=engine::alpha::compile(*ast,*analysis); ASSERT_TRUE(program) << id;
    const auto slots=static_cast<u64>(program->num_slots),lookback=static_cast<u64>(program->required_lookback);
    EXPECT_EQ(row.at("num_slots").get<u64>(),slots) << id;
    EXPECT_EQ(row.at("required_lookback").get<u64>(),lookback) << id;
    EXPECT_EQ(row.at("node_count").get<u64>(),static_cast<u64>(program->unique_nodes)) << id;
    max_slots=std::max(max_slots,slots); max_lookback=std::max(max_lookback,lookback);
  }
  // A lone field load is one DAG node, reads no extra field and needs no history.
  EXPECT_EQ(rows.at(0).at("node_count"),1); EXPECT_EQ(rows.at(0).at("required_lookback"),0);
  EXPECT_GT(rows.at(2).at("required_lookback").get<u64>(),0U);
  // The library maxima the runner charges are the rows' maxima.
  EXPECT_EQ(plan.at("max_compiled_slots").get<u64>(),max_slots);
  EXPECT_EQ(plan.at("required_lookback").get<u64>(),max_lookback);
}
TEST(StrategyIcRunner, FrozenTrainResumeBindsResearchFieldPins) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(field_library(cfg,{"si_shares"},{{"si_ratio","si_shares / volume"}}));
  ASSERT_TRUE(pin_fields(dir,cfg,"f",{"si_shares"}));
  cfg.save_combined=true;
  const auto source=run_named(dir,cfg,"source"); ASSERT_TRUE(source.ok) << source.error;
  auto other=cfg; ASSERT_TRUE(pin_fields(dir,other,"g",{"si_shares"},2));
  cfg.orientations_path=(dir.path/"source"/"orientations.json").string();
  cfg.orientations_sha256=file_sha(cfg.orientations_path);
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  // TRAIN's fields are not needed (its payload is never opened); validation's are.
  auto resumed_cfg=cfg; resumed_cfg.train_fields_directory.clear(); resumed_cfg.train_fields_sha256.clear();
  const auto resumed=run_named(dir,resumed_cfg,"resumed"); ASSERT_TRUE(resumed.ok) << resumed.error;
  EXPECT_EQ(resumed.log.find("IC loading train"),std::string::npos);
  for (const auto* file:{"validation_combined.f64","validation_planned_targets.csv","validation_daily_ic.csv"})
    EXPECT_EQ(file_sha(dir.path/"resumed"/file),file_sha(dir.path/"source"/file)) << file;
  EXPECT_EQ(read_json(dir.path/"resumed"/"recipe.json").at("frozen_train_recipe").at("research_fields"),
            read_json(dir.path/"source"/"recipe.json").at("research_fields"));
  const auto refuse=[&](atx::impl::strategy::IcRunnerConfig attempt_cfg,const std::string& name,const std::string& reason) {
    const auto attempt=run_named(dir,attempt_cfg,name);
    EXPECT_FALSE(attempt.ok) << name;
    EXPECT_NE(attempt.error.find(reason),std::string::npos) << name << ": " << attempt.error;
    EXPECT_FALSE(std::filesystem::exists(dir.path/name)) << name;
  };
  auto moved=resumed_cfg; moved.validation_fields_directory=other.validation_fields_directory;
  moved.validation_fields_sha256=other.validation_fields_sha256;
  refuse(moved,"moved","frozen TRAIN method/statistical settings/role pins differ");
  auto train_moved=cfg; train_moved.train_fields_directory=other.train_fields_directory;
  train_moved.train_fields_sha256=other.train_fields_sha256;
  refuse(train_moved,"train_moved","frozen TRAIN research fields pin differs");
  auto no_validation=resumed_cfg; no_validation.validation_fields_directory.clear();
  no_validation.validation_fields_sha256.clear();
  refuse(no_validation,"no_validation","no --validation-fields manifest is pinned");
}
TEST(StrategyIcRunner, NonPointInTimeFieldsAndUnflaggedManifestsAreRefused) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(pin_fields(dir,cfg,"f",{"si_shares","is_common","mktcap_lagged"}));
  // Look-ahead entries in the manifest are harmless while no library declares them.
  ASSERT_TRUE(field_library(cfg,{"si_shares"},{{"si_ratio","si_shares / volume"}}));
  const auto good=cfg; const auto good_library=read_json(cfg.library_path);
  auto plan=good; plan.plan_only=true; std::ostringstream plan_log;
  auto status=atx::impl::strategy::run_ic(plan,plan_log); ASSERT_TRUE(status) << status.error().to_string();
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  const auto refuse=[&](atx::impl::strategy::IcRunnerConfig attempt_cfg,const std::string& reason) {
    for (const bool plan_only:{true,false}) {
      attempt_cfg.plan_only=plan_only; std::ostringstream log;
      const auto result=atx::impl::strategy::run_ic(attempt_cfg,log);
      ASSERT_FALSE(result) << reason;
      EXPECT_NE(result.error().to_string().find(reason),std::string::npos) << result.error().to_string();
      EXPECT_TRUE(log.str().empty()); EXPECT_FALSE(std::filesystem::exists(dir.path/"output"));
    }
  };
  const auto with_library=[&](Json lib) {
    auto next=good; next.library_path=(dir.path/"variant_library.json").string();
    EXPECT_TRUE(json_file(next.library_path,lib,next.library_sha256)); return next;
  };
  // Referenced (loaded) or merely declared, a non-PIT field refuses; no override.
  auto used=good_library; used["fields"].push_back({{"name","is_common"}});
  used["candidates"][2]["dsl"]="is_common * volume";
  refuse(with_library(used),"library field 'is_common' is not point-in-time in the pinned train fields manifest "
                            "(non_pit_aspects: values); refusing look-ahead");
  auto declared=good_library; declared["fields"].push_back({{"name","mktcap_lagged"}});
  refuse(with_library(declared),"library field 'mktcap_lagged' is not point-in-time");
  // A pre-flag manifest (old -fields-v1 layout) or a contradictory pair refuses
  // the whole manifest, even for a field no candidate uses.
  const auto manifest_path=std::filesystem::path(good.train_fields_directory)/"manifest.json";
  const auto manifest=read_json(manifest_path);
  const auto repinned=[&](const Json& lie) {
    auto next=good; EXPECT_TRUE(json_file(manifest_path,lie,next.train_fields_sha256)); return next;
  };
  auto unflagged=manifest;
  for (auto& entry:unflagged["fields"]) { entry.erase("point_in_time"); entry.erase("non_pit_aspects"); }
  refuse(repinned(unflagged),"train fields manifest entry si_shares lacks point_in_time/non_pit_aspects flags");
  auto contradictory=manifest;
  for (auto& entry:contradictory["fields"]) if (entry.at("name")=="is_common") entry["point_in_time"]=true;
  refuse(repinned(contradictory),"train fields manifest entry is_common point_in_time contradicts non_pit_aspects");
}
// ---- T1 fix round 1: pinned signs, VM identity, worker parity ----
TEST(StrategyIcRunner, PinnedSignOppositeToIcOrientationFlipsOnlyThatBlendContribution) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.save_combined=true;
  ASSERT_TRUE(pin_weights(dir,cfg,{{"volume_level",2.0},{"volume_rank",0.0}},Json(),"unsigned.json"));
  const auto unsigned_run=run_named(dir,cfg,"unsigned"); ASSERT_TRUE(unsigned_run.ok) << unsigned_run.error;
  // TRAIN IC orients volume_level +1; the pinned sign is -1. volume_rank has
  // weight 0 and no sign, which is admitted.
  ASSERT_TRUE(pin_weights(dir,cfg,{{"volume_level",2.0},{"volume_rank",0.0}},{{"volume_level",-1}},"signed.json"));
  const auto signed_run=run_named(dir,cfg,"signed"); ASSERT_TRUE(signed_run.ok) << signed_run.error;
  const auto recipe=read_json(dir.path/"signed"/"recipe.json");
  EXPECT_EQ(recipe.at("composition"),"pinned-candidate-weights;pinned-candidate-signs;centered-tied-rank;"
                                     "missing-or-unoriented-neutral;no-redistribution");
  EXPECT_NE(read_json(dir.path/"unsigned"/"recipe.json").at("composition"),recipe.at("composition"));
  const auto summary=read_json(dir.path/"signed"/"summary.json");
  EXPECT_EQ(summary.at("composition_signs"),"pinned-candidate-signs");
  EXPECT_FALSE(read_json(dir.path/"unsigned"/"summary.json").contains("composition_signs"));
  // The TRAIN orientation artifact is untouched: it still records the IC sign.
  const auto orientations=read_json(dir.path/"signed"/"orientations.json");
  for (const auto& row:orientations.at("candidates")) EXPECT_EQ(row.at("sign"),1);
  for (usize r=0;r<2;++r) {
    const auto& role_result=summary.at("roles").at(r); const auto name=role_result.at("role").get<std::string>();
    const auto& level=role_result.at("candidates").at(0); const auto& rank=role_result.at("candidates").at(1);
    EXPECT_EQ(level.at("frozen_train_sign"),1); EXPECT_EQ(level.at("composition_sign"),-1);
    EXPECT_EQ(level.at("composition_weight"),2.0);
    EXPECT_EQ(rank.at("composition_sign"),0); EXPECT_EQ(rank.at("composition_weight"),0.0);
    EXPECT_EQ(read_json(dir.path/"signed"/(name+"_combined.json")).at("composition_signs"),"pinned-candidate-signs");
    // Per-candidate IC diagnostics keep the TRAIN orientation (only the blend flips).
    EXPECT_EQ(stable_role(role_result).at("candidates"),
              stable_role(read_json(dir.path/"unsigned"/"summary.json").at("roles").at(r)).at("candidates"));
    std::vector<f64> flipped(D*N),reference(D*N);
    ASSERT_TRUE(read_payload(dir.path/"signed"/(name+"_combined.f64"),flipped));
    ASSERT_TRUE(read_payload(dir.path/"unsigned"/(name+"_combined.f64"),reference));
    for (usize d=63;d<D;++d) for (usize i=0;i<N;++i) {
      const auto k=d*N+i;
      ASSERT_DOUBLE_EQ(reference[k],2*(static_cast<f64>(i)/static_cast<f64>(N-1)-.5)) << name << d << i;
      ASSERT_EQ(flipped[k],-reference[k]) << name << d << i;
    }
  }
  // A pinned sign equal to the IC orientation reproduces the unsigned blend bytes.
  ASSERT_TRUE(pin_weights(dir,cfg,{{"volume_level",2.0},{"volume_rank",0.0}},{{"volume_level",1}},"same.json"));
  const auto same=run_named(dir,cfg,"same"); ASSERT_TRUE(same.ok) << same.error;
  for (const std::string role_name:{"train","validation"})
    EXPECT_EQ(file_sha(dir.path/"same"/(role_name+"_combined.f64")),
              file_sha(dir.path/"unsigned"/(role_name+"_combined.f64"))) << role_name;
}
TEST(StrategyIcRunner, CandidateCacheIsScopedByVmIdentityAndRefusesForeignSidecars) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.candidate_cache_directory=(dir.path/"c").string();
  const auto cold=run_named(dir,cfg,"cold"); ASSERT_TRUE(cold.ok) << cold.error;
  const auto root=cache_root(dir.path/"cold"/"summary.json");
  const auto sidecar_path=root/cfg.train_sha256/(level_stem()+".json"); const auto sidecar=read_json(sidecar_path);
  const auto identity=sidecar.at("vm_identity").get<std::string>();
  EXPECT_TRUE(identity.starts_with("dslvm1_")) << identity;
  EXPECT_EQ(read_json(dir.path/"cold"/"summary.json").at("roles").at(0).at("candidate_cache").at("vm_identity"),identity);
  // Only the verified legacy identity keeps DIR itself as its root.
  const bool legacy=identity=="dslvm1_clang18.1";
  EXPECT_EQ(root.string(),(legacy?dir.path/"c":dir.path/"c"/identity).string());
  // Entries under any other identity's root are never read: a clean miss.
  auto other_cfg=cfg; other_cfg.candidate_cache_directory=(dir.path/"c2").string();
  for (const auto& pin:{cfg.train_sha256,cfg.validation_sha256}) {
    for (const auto& elsewhere:{dir.path/"c2"/"dslvm999_other"/pin,dir.path/"c2"/"dslvm1_other_flavor"/pin}) {
      ASSERT_TRUE(std::filesystem::create_directories(elsewhere));
      std::filesystem::copy(root/pin,elsewhere);
    }
  }
  const auto other=run_named(dir,other_cfg,"other"); ASSERT_TRUE(other.ok) << other.error;
  EXPECT_EQ(other.log.find("IC cache-hit"),std::string::npos);
  const auto other_summary=read_json(dir.path/"other"/"summary.json");
  for (const auto& role_result:other_summary.at("roles"))
    EXPECT_EQ(role_result.at("candidate_cache").at("hits"),0);
  // In our own root, a sidecar recording another identity is refused, never served.
  const auto attempt=[&](const Json& edited,const std::string& name) {
    std::string unused; EXPECT_TRUE(json_file(sidecar_path,edited,unused));
    return run_named(dir,cfg,name);
  };
  auto foreign=sidecar; foreign["vm_identity"]="dslvm999_other";
  auto run=attempt(foreign,"foreign");
  EXPECT_FALSE(run.ok); EXPECT_NE(run.error.find("candidate cache entry mismatch: volume_level"),std::string::npos);
  // A v2 sidecar always records its identity: a keyless one is refused.
  auto keyless=sidecar; keyless.erase("vm_identity");
  run=attempt(keyless,"keyless_v2");
  EXPECT_FALSE(run.ok); EXPECT_NE(run.error.find("candidate cache entry mismatch: volume_level"),std::string::npos);
  // A keyless (pre-identity) v1 sidecar is accepted only as a verified legacy entry.
  { std::string unused; ASSERT_TRUE(json_file(sidecar_path,sidecar,unused)); }
  ASSERT_TRUE(to_v1({sidecar_path,root/cfg.train_sha256/(level_stem()+".f64"),"v2"},root/cfg.train_sha256,
                    "volume_level"));
  const auto v1_path=root/cfg.train_sha256/"volume_level.json"; const auto v1=read_json(v1_path);
  const auto attempt_v1=[&](const Json& edited,const std::string& name) {
    std::string unused; EXPECT_TRUE(json_file(v1_path,edited,unused));
    return run_named(dir,cfg,name);
  };
  auto keyless_v1=v1; keyless_v1.erase("vm_identity"); keyless_v1["engine_git_sha"]="0123456789abcdef";
  run=attempt_v1(keyless_v1,"keyless_unknown_engine");
  EXPECT_FALSE(run.ok); EXPECT_NE(run.error.find("candidate cache entry mismatch: volume_level"),std::string::npos);
  keyless_v1["engine_git_sha"]="429cbe43d275a49ad3cae89dfa8aa591846a2e4f";
  run=attempt_v1(keyless_v1,"keyless_legacy_engine");
  EXPECT_EQ(run.ok,legacy) << run.error;
  if (legacy) EXPECT_NE(run.log.find("IC cache-hit volume_level role=train layout=v1"),std::string::npos);
}
TEST(StrategyIcRunner, CandidateCachePayloadBytesAreIdenticalAcrossVmWorkers) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.candidate_cache_directory=(dir.path/"serial").string();
  const auto serial=run_named(dir,cfg,"serial_run"); ASSERT_TRUE(serial.ok) << serial.error;
  cfg.workers=2; cfg.candidate_cache_directory=(dir.path/"parallel").string();
  const auto parallel=run_named(dir,cfg,"parallel_run"); ASSERT_TRUE(parallel.ok) << parallel.error;
  // vm_workers is recorded, not matched, because raw VM bytes do not depend on it.
  const auto a=dir.path/"serial_run"/"summary.json",b=dir.path/"parallel_run"/"summary.json";
  for (const std::string role_name:{"train","validation"})
    for (const std::string id:{"volume_level","volume_rank"}) {
      const auto x=cache_entry(a,role_name,id),y=cache_entry(b,role_name,id);
      const auto expected=file_sha(x.payload); ASSERT_FALSE(expected.empty()) << id;
      EXPECT_EQ(file_sha(y.payload),expected) << id;
      EXPECT_EQ(read_json(x.sidecar).at("payload_sha256"),read_json(y.sidecar).at("payload_sha256"));
      // Same key, so the same file name in both caches.
      EXPECT_EQ(x.sidecar.filename().string(),y.sidecar.filename().string());
      EXPECT_EQ(read_json(y.sidecar).at("vm_workers"),2);
    }
}
// ---- T7 fix round 2 ----
// Each `lines` entry must occur in `log`, in this order.
void expect_in_order(const std::string& log,const std::vector<std::string>& lines) {
  usize from=0;
  for (const auto& line:lines) {
    const auto at=log.find(line,from);
    ASSERT_NE(at,std::string::npos) << "missing or out of order: " << line;
    from=at+line.size();
  }
}
// Review I1: an extra column is resident only between the candidates that read it
// (Belady over library order), evicted and reloaded when that lowers the peak; the
// raw signals, the blend and every output are unchanged by the schedule.
TEST(StrategyIcRunner, ExtraFieldsResideOnlyWhileReferencedAndReloadExactly) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  const std::pair<std::string,std::string> base_mid{"base_mid","rank(raw_close / volume)"};
  // Base-only reference for base_mid, which the fields run evaluates while si_shares
  // is resident, i.e. on the borrowed DSL panel (review M3).
  auto plain_cfg=cfg; plain_cfg.library_path=(dir.path/"plain_library.json").string();
  ASSERT_TRUE(std::filesystem::copy_file(cfg.library_path,plain_cfg.library_path));
  ASSERT_TRUE(field_library(plain_cfg,{},{base_mid}));
  plain_cfg.candidate_cache_directory=(dir.path/"p").string();
  const auto plain=run_named(dir,plain_cfg,"plain"); ASSERT_TRUE(plain.ok) << plain.error;
  ASSERT_TRUE(field_library(cfg,{"si_shares","mkt_ret","iv_atm_21d"},
      {{"si_market","si_shares + mkt_ret"},{"iv_level","iv_atm_21d"},base_mid,
       {"si_ratio","si_shares / volume"},{"market_shift","volume + mkt_ret"}}));
  ASSERT_TRUE(pin_fields(dir,cfg,"f",{"si_shares","mkt_ret","iv_atm_21d"}));
  cfg.save_combined=true;
  // Hand schedule (extras iv_atm_21d, mkt_ret, si_shares; candidates volume_level,
  // volume_rank, si_market{mkt,si}, iv_level{iv}, base_mid{}, si_ratio{si},
  // market_shift{mkt}): capacity 2; loads mkt+si, iv evicting mkt (next use
  // farthest), then mkt again = 4 loads, never more than 2 resident.
  auto plan_cfg=cfg; plan_cfg.plan_only=true; std::ostringstream plan;
  const auto planned=atx::impl::strategy::run_ic(plan_cfg,plan); ASSERT_TRUE(planned) << planned.error().to_string();
  const auto plan_json=Json::parse(plan.str()).at("research_fields");
  EXPECT_EQ(plan_json.at("resident_capacity"),2); EXPECT_EQ(plan_json.at("planned_loads"),4);
  const auto uncached=run_named(dir,cfg,"uncached"); ASSERT_TRUE(uncached.ok) << uncached.error;
  cfg.candidate_cache_directory=(dir.path/"c").string();
  const auto cold=run_named(dir,cfg,"cold"); ASSERT_TRUE(cold.ok) << cold.error;
  // Releases are logged when candidate k starts (after its eval-start line).
  for (const auto* run:{&uncached,&cold})
    for (const std::string r:{"train","validation"})
      expect_in_order(run->log,{
          "IC fields-verified role="+r+" fields=iv_atm_21d,mkt_ret,si_shares resident_capacity=2 planned_loads=4",
          "IC eval-start "+r+" 3/7 si_market",
          "IC field-load role="+r+" field=mkt_ret candidate=si_market",
          "IC field-load role="+r+" field=si_shares candidate=si_market",
          "IC eval-start "+r+" 4/7 iv_level",
          "IC field-release role="+r+" field=mkt_ret\n",
          "IC field-load role="+r+" field=iv_atm_21d candidate=iv_level",
          "IC eval-start "+r+" 5/7 base_mid",
          "IC field-release role="+r+" field=iv_atm_21d\n",
          "IC eval-start "+r+" 6/7 si_ratio",
          "IC eval-start "+r+" 7/7 market_shift",
          "IC field-release role="+r+" field=si_shares\n",
          "IC field-load role="+r+" field=mkt_ret candidate=market_shift"});
  // Each field file is hashed once per process: the mkt_ret reload trusts its first
  // verification (unchanged size and modification time).
  for (const std::string r:{"train","validation"}) {
    EXPECT_NE(uncached.log.find("IC field-load role="+r+" field=mkt_ret candidate=si_market"),std::string::npos);
    const auto reload=uncached.log.find("IC field-load role="+r+" field=mkt_ret candidate=market_shift");
    ASSERT_NE(reload,std::string::npos) << r;
    const auto line=uncached.log.substr(reload,uncached.log.find('\n',reload)-reload);
    EXPECT_NE(line.find(" hashed=0"),std::string::npos) << line;
  }
  const auto uncached_summary=read_json(dir.path/"uncached"/"summary.json");
  for (const auto& role_result:uncached_summary.at("roles"))
    EXPECT_EQ(role_result.at("verify_bytes"),3*D*N*sizeof(f64)) << role_result.at("role");
  const auto cold_summary=dir.path/"cold"/"summary.json",plain_summary=dir.path/"plain"/"summary.json";
  for (const std::string role_name:{"train","validation"}) {
    std::vector<f64> si_market(D*N),iv_level(D*N),si_ratio(D*N),market_shift(D*N);
    ASSERT_TRUE(read_payload(cache_entry(cold_summary,role_name,"si_market").payload,si_market));
    ASSERT_TRUE(read_payload(cache_entry(cold_summary,role_name,"iv_level").payload,iv_level));
    ASSERT_TRUE(read_payload(cache_entry(cold_summary,role_name,"si_ratio").payload,si_ratio));
    ASSERT_TRUE(read_payload(cache_entry(cold_summary,role_name,"market_shift").payload,market_shift));
    for (usize d=0;d<D;++d) for (usize i=0;i<N;++i) {
      const auto k=d*N+i; const auto volume=1e8*static_cast<f64>(i+1);
      ASSERT_TRUE(same_value(si_market[k],si_value(d,i,1)+market_value(d))) << d << ' ' << i;
      ASSERT_TRUE(same_value(iv_level[k],.3+.01*static_cast<f64>(i))) << d << ' ' << i;
      ASSERT_TRUE(same_value(si_ratio[k],si_value(d,i,1)/volume)) << d << ' ' << i;
      ASSERT_TRUE(same_value(market_shift[k],volume+market_value(d))) << d << ' ' << i;
    }
    // A base candidate evaluated on the DSL panel publishes the base-only bytes.
    const auto expected=file_sha(cache_entry(plain_summary,role_name,"base_mid").payload);
    ASSERT_FALSE(expected.empty());
    const auto base_mid=cache_entry(cold_summary,role_name,"base_mid");
    EXPECT_EQ(file_sha(base_mid.payload),expected);
    EXPECT_FALSE(read_json(base_mid.sidecar).contains("fields_manifest_sha256"));
  }
  // Warm: every signal is a hit, so no column is ever loaded.
  const auto warm=run_named(dir,cfg,"warm"); ASSERT_TRUE(warm.ok) << warm.error;
  EXPECT_EQ(warm.log.find("IC field-load"),std::string::npos);
  EXPECT_EQ(warm.log.find("IC VM-"),std::string::npos);
  for (const std::string name:{"uncached","cold","warm"}) {
    const auto summary=read_json(dir.path/name/"summary.json");
    for (const auto& role_result:summary.at("roles")) {
      const auto& fields=role_result.at("research_fields");
      const bool hits=name=="warm";
      EXPECT_EQ(fields.at("resident_capacity"),2) << name;
      EXPECT_EQ(fields.at("field_loads"),hits?0:4) << name;
      EXPECT_EQ(fields.at("peak_resident_fields"),hits?0:2) << name;
      EXPECT_EQ(fields.at("loaded_bytes"),(hits?0:4)*D*N*sizeof(f64)) << name;
    }
    if (name=="uncached") continue;
    for (const auto& file:exact_outputs())
      EXPECT_EQ(file_sha(dir.path/name/file),file_sha(dir.path/"uncached"/file)) << name << ' ' << file;
  }
}
// Review M2: LoadField NaNs an extra wherever the role is not present, even where
// the field file holds a finite value.
TEST(StrategyIcRunner, ExtraFieldsAreMaskedByRolePresence) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  const std::vector<usize> holes{100*N+2,400*N+5};
  ASSERT_GT(std::filesystem::remove_all(dir.path/"train"),0U);
  ASSERT_TRUE(role(dir.path/"train",17683,cfg.train_sha256,1,holes));
  ASSERT_TRUE(field_library(cfg,{"si_shares"},{{"si_level","si_shares"}}));
  ASSERT_TRUE(pin_fields(dir,cfg,"f",{"si_shares"}));
  std::vector<f64> file(D*N);
  ASSERT_TRUE(read_payload(std::filesystem::path(cfg.train_fields_directory)/"si_shares.f64",file));
  for (const auto k:holes) ASSERT_TRUE(std::isfinite(file[k])) << k;
  cfg.candidate_cache_directory=(dir.path/"c").string();
  const auto run=run_named(dir,cfg,"output"); ASSERT_TRUE(run.ok) << run.error;
  std::vector<f64> level(D*N);
  ASSERT_TRUE(read_payload(cache_entry(dir.path/"output"/"summary.json","train","si_level").payload,level));
  for (usize k=0;k<D*N;++k) {
    const bool hole=std::find(holes.begin(),holes.end(),k)!=holes.end();
    ASSERT_TRUE(same_value(level[k],hole?std::numeric_limits<f64>::quiet_NaN():file[k])) << k;
  }
}
// Review M5: a declared field must mean the same thing in both roles' manifests;
// role-specific keys (coverage, counts) may differ.
TEST(StrategyIcRunner, FieldDefinitionsMustMatchAcrossRoles) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(field_library(cfg,{"si_shares"},{{"si_ratio","si_shares / volume"}}));
  ASSERT_TRUE(pin_fields(dir,cfg,"f",{"si_shares"}));
  const auto path=std::filesystem::path(cfg.validation_fields_directory)/"manifest.json";
  const auto manifest=read_json(path);
  const auto plan_status=[&](const Json& edited) {
    auto attempt=cfg; attempt.plan_only=true;
    EXPECT_TRUE(json_file(path,edited,attempt.validation_fields_sha256));
    std::ostringstream log; return atx::impl::strategy::run_ic(attempt,log);
  };
  for (const auto* key:{"units","clock"}) {
    auto changed=manifest; changed["fields"][0][key]="synthetic-v2";
    const auto status=plan_status(changed); ASSERT_FALSE(status) << key;
    EXPECT_NE(status.error().to_string().find("research field 'si_shares' definition differs between the train "
                                              "and validation fields manifests"),std::string::npos)
        << key << ": " << status.error().to_string();
  }
  auto bounds=manifest; bounds["fields"][0]["plausibility"]={{"min",0},{"max",1e12},{"rule","synthetic"}};
  EXPECT_FALSE(plan_status(bounds));
  auto coverage=manifest; coverage["fields"][0]["coverage"]=.5; coverage["fields"][0]["nonfinite_count"]=7;
  const auto accepted=plan_status(coverage); EXPECT_TRUE(accepted) << accepted.error().to_string();
}
// Coordinator 2026-09-27: the option names a directory; a manifest file path
// refuses by name instead of as a missing DIR/manifest.json.
TEST(StrategyIcRunner, FieldsOptionMustNameTheDirectory) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(field_library(cfg,{"si_shares"},{{"si_ratio","si_shares / volume"}}));
  ASSERT_TRUE(pin_fields(dir,cfg,"f",{"si_shares"}));
  cfg.plan_only=true;
  auto file=cfg; file.train_fields_directory=(std::filesystem::path(cfg.train_fields_directory)/"manifest.json").string();
  std::ostringstream log; const auto status=atx::impl::strategy::run_ic(file,log); ASSERT_FALSE(status);
  EXPECT_NE(status.error().to_string().find("--train-fields must name the fields directory"),std::string::npos)
      << status.error().to_string();
  std::ostringstream ok_log; const auto ok=atx::impl::strategy::run_ic(cfg,ok_log); EXPECT_TRUE(ok) << ok.error().to_string();
}
// T1 review M2/M6: a present but unusable sidecar refuses before the role payload
// loads; the cache path and candidate ids are checked before anything else.
TEST(StrategyIcRunner, CandidateCachePreflightRefusesBeforeRolePayload) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.candidate_cache_directory=(dir.path/"c").string();
  const auto cold=run_named(dir,cfg,"cold"); ASSERT_TRUE(cold.ok) << cold.error;
  const auto root=cache_root(dir.path/"cold"/"summary.json");
  // The LAST candidate's sidecar is corrupt: refused before any candidate runs.
  const auto sidecar=root/cfg.train_sha256/(rank_stem()+".json"); const auto original=file_bytes(sidecar);
  ASSERT_TRUE(write_bytes(sidecar,std::vector<char>{'{'}));
  const auto corrupt=run_named(dir,cfg,"corrupt"); EXPECT_FALSE(corrupt.ok);
  EXPECT_NE(corrupt.error.find("candidate cache sidecar JSON: volume_rank"),std::string::npos) << corrupt.error;
  EXPECT_NE(corrupt.log.find("IC loading train"),std::string::npos);
  EXPECT_EQ(corrupt.log.find("IC eval-start"),std::string::npos);
  EXPECT_EQ(file_bytes(sidecar),std::vector<char>{'{'});
  ASSERT_TRUE(write_bytes(sidecar,original));
  // A payload of the wrong extent is refused up front too, by the same name as on load.
  const auto payload_path=root/cfg.train_sha256/(rank_stem()+".f64"); const auto bytes=file_bytes(payload_path);
  ASSERT_TRUE(write_bytes(payload_path,std::vector<char>(bytes.begin(),bytes.end()-8)));
  const auto short_run=run_named(dir,cfg,"short"); EXPECT_FALSE(short_run.ok);
  EXPECT_NE(short_run.error.find("candidate cache payload extent: "+rank_stem()+".f64"),std::string::npos)
      << short_run.error;
  EXPECT_EQ(short_run.log.find("IC eval-start"),std::string::npos);
  ASSERT_TRUE(write_bytes(payload_path,bytes));
  const auto restored=run_named(dir,cfg,"restored"); ASSERT_TRUE(restored.ok) << restored.error;
  EXPECT_NE(restored.log.find("IC cache-preflight role=train ready=2/2"),std::string::npos);
  // cache_preflight: a file where the cache directory belongs; a device-name id.
  const auto refuse=[&](atx::impl::strategy::IcRunnerConfig attempt,const std::string& name,const std::string& reason) {
    const auto run=run_named(dir,attempt,name); EXPECT_FALSE(run.ok) << name;
    EXPECT_NE(run.error.find(reason),std::string::npos) << name << ": " << run.error;
    EXPECT_FALSE(std::filesystem::exists(dir.path/name)) << name;
  };
  auto as_file=cfg; as_file.candidate_cache_directory=(dir.path/"cache_file").string();
  ASSERT_TRUE(write_bytes(as_file.candidate_cache_directory,std::vector<char>{'x'}));
  refuse(as_file,"as_file","candidate cache path must be a directory");
  auto device=cfg; auto lib=read_json(cfg.library_path);
  lib["candidates"].push_back({{"id","con"},{"family","fixed_volume"},{"dsl","rank(raw_close)"},
      {"sign_policy","train-rank-ic21"},{"horizons",{5,21,63}}});
  device.library_path=(dir.path/"device_library.json").string();
  ASSERT_TRUE(json_file(device.library_path,lib,device.library_sha256));
  refuse(device,"device","candidate id unsafe as cache file name: con");
}
// Pinned-source tripwire shared by the signal (VM) and IC-result caches: the digest
// of the listed sources, CRLF->LF normalized, and their atx/engine include closure.
void expect_sources_pinned(const atx::impl::strategy::IcCacheVmIdentity& id,const std::string& prefix,
                           const std::string& list,const std::string& remedy) {
  EXPECT_EQ(id.identity.rfind(prefix+std::to_string(id.semantics_version)+"_",0),0U) << id.identity;
  const auto repo=std::filesystem::path{ATX_IMPL_TESTS_DIR}.parent_path().parent_path();
  const std::set<std::string> listed(id.sources.begin(),id.sources.end());
  ASSERT_EQ(listed.size(),id.sources.size());
  std::string material;
  for (const auto& rel:id.sources) {
    const auto raw=file_bytes(repo/rel); ASSERT_FALSE(raw.empty()) << rel;
    std::string text; text.reserve(raw.size());
    for (usize k=0;k<raw.size();++k) if (!(raw[k]=='\r' && k+1<raw.size() && raw[k+1]=='\n')) text.push_back(raw[k]);
    material+=rel+'\n'+std::to_string(text.size())+'\n'+text;
    // Closure: every atx/engine header a listed file includes is itself listed.
    std::istringstream lines(text);
    for (std::string line;std::getline(lines,line);) {
      const auto at=line.find("#include \"atx/engine/");
      if (at==std::string::npos) continue;
      const auto begin=at+10,end=line.find('"',begin);
      ASSERT_NE(end,std::string::npos) << rel << ": " << line;
      const auto header="atx-engine/include/"+line.substr(begin,end-begin);
      EXPECT_TRUE(listed.count(header)) << rel << " includes unlisted " << header
          << " (add it to " << list << "). " << remedy;
    }
  }
  const auto digest=core::sha256_hex(material); ASSERT_TRUE(digest);
  EXPECT_EQ(*digest,id.sources_sha256) << remedy;
}
// Review I2: the candidate cache is keyed by dsl_vm_semantics_version, so any edit
// to the engine sources that evaluate a DSL signal must be a conscious decision.
TEST(StrategyIcRunner, VmSourcesPinnedToSemanticsVersion) {
  expect_sources_pinned(atx::impl::strategy::ic_cache_vm_identity(),"dslvm","dsl_vm_sources",
      "VM sources changed: bump dsl_vm_semantics_version and update the pinned hash");
}
// T15: the IC-result cache is keyed by ic_result_semantics_version; an edit to the
// engine IC scoring sources must likewise be a conscious bump-or-repin decision.
TEST(StrategyIcRunner, IcSourcesPinnedToSemanticsVersion) {
  const auto id=atx::impl::strategy::ic_result_cache_identity();
  EXPECT_NE(id.identity.find("_simd"),std::string::npos) << id.identity;
  expect_sources_pinned(id,"dslic","ic_result_sources",
      "IC scoring sources changed: bump ic_result_semantics_version and update the pinned hash");
}
// ---- T14: validation-only runs bind weights and field definitions to frozen TRAIN ----
// Review M1 (root ruling): against an unweighted frozen TRAIN artifact, pinned weights
// must name that artifact as the orientations they were fit on, and TRAIN's fields pin.
TEST(StrategyIcRunner, ValidationWeightsMustNameTheFrozenTrainOrientations) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  const auto source=run_named(dir,cfg,"source"); ASSERT_TRUE(source.ok) << source.error;
  const Json weights{{"volume_level",.75},{"volume_rank",.25}};
  // The bypass the strict ruling closes: a fit naming foreign orientations, first
  // scored in a weighted TRAIN-only run, then validated against that artifact.
  auto laundered=cfg; laundered.validation_manifest.clear(); laundered.validation_sha256.clear();
  ASSERT_TRUE(pin_weights(dir,laundered,weights,Json(),"laundered.json",{{"orientations_sha256",std::string(64,'0')}}));
  const auto weighted_train=run_named(dir,laundered,"weighted_train"); ASSERT_TRUE(weighted_train.ok) << weighted_train.error;
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  laundered.validation_manifest=cfg.validation_manifest; laundered.validation_sha256=cfg.validation_sha256;
  laundered.orientations_path=(dir.path/"weighted_train"/"orientations.json").string();
  laundered.orientations_sha256=file_sha(laundered.orientations_path);
  for (const bool plan_only:{true,false}) {
    laundered.plan_only=plan_only; const auto run=run_named(dir,laundered,"laundered");
    EXPECT_FALSE(run.ok);
    EXPECT_NE(run.error.find("frozen TRAIN artifact is a weighted run; validate from the unweighted TRAIN run "
                             "whose orientations the weights were fit on"),std::string::npos) << run.error;
    EXPECT_FALSE(std::filesystem::exists(dir.path/"laundered"));
  }
  cfg.orientations_path=(dir.path/"source"/"orientations.json").string();
  cfg.orientations_sha256=file_sha(cfg.orientations_path);
  const auto refuse=[&](const Json& provenance,const std::string& name,const std::string& reason) {
    auto attempt=cfg; ASSERT_TRUE(pin_weights(dir,attempt,weights,Json(),name+".json",provenance));
    for (const bool plan_only:{true,false}) {
      attempt.plan_only=plan_only; const auto run=run_named(dir,attempt,name);
      EXPECT_FALSE(run.ok) << name;
      EXPECT_NE(run.error.find(reason),std::string::npos) << name << ": " << run.error;
      EXPECT_TRUE(run.log.empty()) << name; EXPECT_FALSE(std::filesystem::exists(dir.path/name)) << name;
    }
  };
  const std::string unnamed="validation-only composition weights need provenance.orientations_sha256 naming "
                            "the frozen TRAIN orientations they were fit on";
  refuse(Json(),"absent",unnamed);
  refuse(Json::array(),"not_object",unnamed);
  refuse({{"orientations_sha256","abc"}},"malformed",unnamed);
  refuse({{"orientations_sha256",std::string(64,'0')}},"other_fit",
         "provenance.orientations_sha256 must equal the frozen TRAIN --orientations-sha256");
  refuse({{"orientations_sha256",cfg.orientations_sha256},{"fields_manifest_sha256",std::string(64,'a')}},
         "fields_named","provenance.fields_manifest_sha256 must equal the frozen TRAIN fields manifest pin");
  // Bound to this artifact: plans and runs, and the summary records how.
  auto good=cfg; ASSERT_TRUE(pin_weights(dir,good,weights,Json(),"good.json",
      {{"orientations_sha256",cfg.orientations_sha256},{"fields_manifest_sha256",nullptr}}));
  const Json expected{{"sha256",good.composition_weights_sha256},{"train_manifest_sha256",cfg.train_sha256},
      {"signs","TRAIN-orientation-signs"},{"provenance_orientations_sha256",cfg.orientations_sha256},
      {"provenance_fields_manifest_sha256",nullptr},
      {"binding","provenance-orientations-equal-frozen-TRAIN-orientations-artifact"}};
  auto plan_cfg=good; plan_cfg.plan_only=true; std::ostringstream plan;
  const auto planned=atx::impl::strategy::run_ic(plan_cfg,plan); ASSERT_TRUE(planned) << planned.error().to_string();
  EXPECT_EQ(Json::parse(plan.str()).at("composition_weights"),expected);
  const auto run=run_named(dir,good,"good"); ASSERT_TRUE(run.ok) << run.error;
  const auto summary=read_json(dir.path/"good"/"summary.json");
  EXPECT_EQ(summary.at("composition_weights"),expected);
  EXPECT_EQ(summary.at("composition_weights_sha256"),good.composition_weights_sha256);
  EXPECT_FALSE(summary.contains("research_field_definitions_checked_against"));
  // Pinned signs are recorded as such.
  auto signed_cfg=cfg; ASSERT_TRUE(pin_weights(dir,signed_cfg,weights,{{"volume_level",1},{"volume_rank",-1}},
      "signed.json",{{"orientations_sha256",cfg.orientations_sha256}}));
  const auto signed_run=run_named(dir,signed_cfg,"signed"); ASSERT_TRUE(signed_run.ok) << signed_run.error;
  const auto signed_record=read_json(dir.path/"signed"/"summary.json").at("composition_weights");
  EXPECT_EQ(signed_record.at("signs"),"pinned-candidate-signs");
  EXPECT_EQ(signed_record.at("provenance_fields_manifest_sha256"),nullptr);
}
// Review N3: without --train-fields, a validation-only run checks the validation
// field definitions against those the frozen TRAIN artifact records.
TEST(StrategyIcRunner, ValidationOnlyChecksFieldDefinitionsAgainstFrozenTrain) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(field_library(cfg,{"si_shares"},{{"si_ratio","si_shares / volume"}}));
  ASSERT_TRUE(pin_fields(dir,cfg,"f",{"si_shares"}));
  // A TRAIN-only source: its recipe pins no validation manifest, so the validation
  // fields manifest is bound only at validation time.
  auto train_only=cfg; train_only.validation_manifest.clear(); train_only.validation_sha256.clear();
  train_only.validation_fields_directory.clear(); train_only.validation_fields_sha256.clear();
  const auto source=run_named(dir,train_only,"source"); ASSERT_TRUE(source.ok) << source.error;
  const auto artifact=read_json(dir.path/"source"/"orientations.json");
  const auto train_manifest=read_json(std::filesystem::path(cfg.train_fields_directory)/"manifest.json");
  EXPECT_EQ(artifact.at("research_fields").at("manifest_sha256"),cfg.train_fields_sha256);
  EXPECT_EQ(artifact.at("research_fields").at("definitions").at("si_shares").at("units"),
            train_manifest.at("fields").at(0).at("units"));
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  auto resume=cfg; resume.train_fields_directory.clear(); resume.train_fields_sha256.clear();
  resume.orientations_path=(dir.path/"source"/"orientations.json").string();
  resume.orientations_sha256=file_sha(resume.orientations_path);
  const auto ok=run_named(dir,resume,"ok"); ASSERT_TRUE(ok.ok) << ok.error;
  EXPECT_EQ(read_json(dir.path/"ok"/"summary.json").at("research_field_definitions_checked_against"),
            "frozen-TRAIN-artifact");
  const auto refuse=[&](atx::impl::strategy::IcRunnerConfig attempt,const std::string& name,const std::string& reason) {
    for (const bool plan_only:{true,false}) {
      attempt.plan_only=plan_only; const auto run=run_named(dir,attempt,name);
      EXPECT_FALSE(run.ok) << name;
      EXPECT_NE(run.error.find(reason),std::string::npos) << name << ": " << run.error;
      EXPECT_FALSE(std::filesystem::exists(dir.path/name)) << name;
    }
  };
  // A validation manifest whose definition differs is refused with no TRAIN fields
  // option; with it, the live TRAIN manifest refuses first (M5).
  const auto validation_path=std::filesystem::path(cfg.validation_fields_directory)/"manifest.json";
  const auto validation_manifest=read_json(validation_path);
  auto changed=validation_manifest; changed["fields"][0]["units"]="synthetic-v2";
  auto mismatch=resume; ASSERT_TRUE(json_file(validation_path,changed,mismatch.validation_fields_sha256));
  refuse(mismatch,"mismatch","research field 'si_shares' definition differs between the frozen TRAIN artifact "
                             "and the validation fields manifest");
  auto both=mismatch; both.train_fields_directory=cfg.train_fields_directory;
  both.train_fields_sha256=cfg.train_fields_sha256;
  refuse(both,"both","research field 'si_shares' definition differs between the train and validation fields manifests");
  std::string restored; ASSERT_TRUE(json_file(validation_path,validation_manifest,restored));
  ASSERT_EQ(restored,resume.validation_fields_sha256);
  // An artifact without the record (written before T14) needs --train-fields; a
  // record naming another TRAIN manifest than its recipe is refused.
  const auto legacy_dir=dir.path/"legacy_source";
  ASSERT_TRUE(std::filesystem::create_directory(legacy_dir));
  ASSERT_TRUE(std::filesystem::copy_file(dir.path/"source"/"recipe.json",legacy_dir/"recipe.json"));
  auto legacy=resume; legacy.orientations_path=(legacy_dir/"orientations.json").string();
  auto legacy_artifact=artifact; legacy_artifact.erase("research_fields");
  ASSERT_TRUE(json_file(legacy.orientations_path,legacy_artifact,legacy.orientations_sha256));
  refuse(legacy,"legacy","frozen TRAIN artifact records no research field definitions; pass --train-fields");
  auto legacy_bound=legacy; legacy_bound.train_fields_directory=cfg.train_fields_directory;
  legacy_bound.train_fields_sha256=cfg.train_fields_sha256;
  const auto bound=run_named(dir,legacy_bound,"legacy_bound"); ASSERT_TRUE(bound.ok) << bound.error;
  EXPECT_EQ(read_json(dir.path/"legacy_bound"/"summary.json").at("research_field_definitions_checked_against"),
            "train-fields-manifest");
  auto lying_artifact=artifact; lying_artifact["research_fields"]["manifest_sha256"]=std::string(64,'0');
  auto lying=resume; lying.orientations_path=(legacy_dir/"orientations.json").string();
  ASSERT_TRUE(json_file(lying.orientations_path,lying_artifact,lying.orientations_sha256));
  refuse(lying,"lying","frozen TRAIN artifact research field record differs from its recipe pin");
  // A record without an object of definitions, or missing a declared field.
  const auto repinned_record=[&](const Json& definitions,const std::string& name,const std::string& reason) {
    auto edited=artifact; edited["research_fields"]["definitions"]=definitions;
    auto attempt=resume; attempt.orientations_path=(legacy_dir/"orientations.json").string();
    ASSERT_TRUE(json_file(attempt.orientations_path,edited,attempt.orientations_sha256));
    refuse(attempt,name,reason);
  };
  repinned_record(Json::array(),"definitions_array","frozen TRAIN artifact research field record differs from its recipe pin");
  repinned_record(Json::object(),"definitions_missing",
                  "research field 'si_shares' definition differs between the frozen TRAIN artifact");
  // M1 with fields: the weights' provenance must name TRAIN's fields pin.
  const Json weights{{"volume_level",.25},{"volume_rank",.25},{"si_ratio",.5}};
  auto weighted=resume; ASSERT_TRUE(pin_weights(dir,weighted,weights,Json(),"fields_weights.json",
      {{"orientations_sha256",resume.orientations_sha256},{"fields_manifest_sha256",cfg.train_fields_sha256}}));
  auto weighted_plan=weighted; weighted_plan.plan_only=true; std::ostringstream plan;
  const auto planned=atx::impl::strategy::run_ic(weighted_plan,plan); ASSERT_TRUE(planned) << planned.error().to_string();
  EXPECT_EQ(Json::parse(plan.str()).at("composition_weights").at("provenance_fields_manifest_sha256"),
            cfg.train_fields_sha256);
  auto unfielded=resume; ASSERT_TRUE(pin_weights(dir,unfielded,weights,Json(),"unfielded_weights.json",
      {{"orientations_sha256",resume.orientations_sha256},{"fields_manifest_sha256",nullptr}}));
  refuse(unfielded,"unfielded","provenance.fields_manifest_sha256 must equal the frozen TRAIN fields manifest pin");
}
// ---- T15: IC-result cache and pooled composition ----
// IC-result hits reproduce every IC diagnostic and output byte of an uncached run,
// with pooled workers (parallel IC rows, pooled composition) and with windows too
// short to score (undefined NaN estimates, empty daily series). Pooled composition
// reproduces the serial blend, series and planned-target bytes.
TEST(StrategyIcRunner, IcResultCacheHitsReproduceUncachedOutputsExactly) {
  for (const bool short_window:{false,true}) {
    SCOPED_TRACE(short_window?"short window":"full window");
    Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
    if (short_window) {
      for (const auto& path:{cfg.train_manifest,cfg.validation_manifest}) {
        auto j=read_json(path); j["score_begin"]=470;
        j["score_start_ns"]=j.at("score_end_ns").get<i64>()-10*day;
        std::string pin; ASSERT_TRUE(json_file(path,j,pin));
        if (path==cfg.train_manifest) cfg.train_sha256=pin; else cfg.validation_sha256=pin;
      }
    }
    cfg.save_combined=!short_window;
    std::vector<std::string> outputs{"recipe.json","orientations.json"};
    for (const std::string role_name:{"train","validation"}) {
      outputs.push_back(role_name+"_daily_ic.csv"); outputs.push_back(role_name+"_planned_targets.csv");
      if (cfg.save_combined)
        for (const auto* suffix:{"_combined.f64","_combined_member.u8","_combined_finite.u8","_combined.json"})
          outputs.push_back(role_name+suffix);
    }
    const auto serial=run_named(dir,cfg,"serial"); ASSERT_TRUE(serial.ok) << serial.error;
    cfg.workers=2;
    const auto plain=run_named(dir,cfg,"plain"); ASSERT_TRUE(plain.ok) << plain.error;
    cfg.candidate_cache_directory=(dir.path/"c").string();
    const auto cold=run_named(dir,cfg,"cold"); ASSERT_TRUE(cold.ok) << cold.error;
    const auto warm=run_named(dir,cfg,"warm"); ASSERT_TRUE(warm.ok) << warm.error;
    EXPECT_NE(cold.log.find(" ic_result=miss"),std::string::npos);
    EXPECT_EQ(cold.log.find(" ic_result=hit"),std::string::npos);
    EXPECT_NE(warm.log.find(" ic_result=hit"),std::string::npos);
    EXPECT_EQ(warm.log.find(" ic_result=miss"),std::string::npos);
    const auto reference=read_json(dir.path/"plain"/"summary.json");
    for (const std::string name:{"cold","warm"}) {
      const auto summary=read_json(dir.path/name/"summary.json");
      ASSERT_EQ(summary.at("roles").size(),2U);
      for (usize r=0;r<2;++r) {
        const auto& result=summary.at("roles").at(r);
        EXPECT_EQ(result.at("candidate_cache").at("ic_results").at("hits"),name=="warm"?2:0) << name;
        EXPECT_EQ(stable_role(result),stable_role(reference.at("roles").at(r))) << name << r;
      }
      for (const auto& file:outputs) {
        const auto expected=file_sha(dir.path/"plain"/file); ASSERT_FALSE(expected.empty()) << file;
        EXPECT_EQ(file_sha(dir.path/name/file),expected) << name << ' ' << file;
      }
    }
    // Only the recipe-bound JSON files record the worker count.
    for (const auto& file:outputs) {
      if (file.ends_with(".json")) continue;
      EXPECT_EQ(file_sha(dir.path/"plain"/file),file_sha(dir.path/"serial"/file)) << file;
    }
  }
}
// A present IC-result entry is served only when intact and naming exactly this
// candidate, signal bytes and scope key; anything else refuses loudly and is left in
// place. A deleted entry is rescored byte-identically; another IC setting keys
// another directory (a clean miss).
TEST(StrategyIcRunner, IcResultCacheRefusesTamperedOrForeignEntriesAndKeysSettings) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.candidate_cache_directory=(dir.path/"c").string();
  const auto cold=run_named(dir,cfg,"cold"); ASSERT_TRUE(cold.ok) << cold.error;
  const auto ic_results=read_json(dir.path/"cold"/"summary.json").at("roles").at(0)
      .at("candidate_cache").at("ic_results");
  const auto subdirectory=ic_results.at("subdirectory").get<std::string>();
  EXPECT_EQ(subdirectory.rfind("ic1_",0),0U) << subdirectory;
  EXPECT_EQ(subdirectory.size(),20U) << subdirectory;
  EXPECT_EQ(ic_results.at("identity"),atx::impl::strategy::ic_result_cache_identity().identity);
  EXPECT_EQ(ic_results.at("key").at("role_manifest_sha256"),cfg.train_sha256);
  const auto root=cache_root(dir.path/"cold"/"summary.json");
  const auto entry_path=root/cfg.train_sha256/subdirectory/(level_stem()+".json");
  const auto original=file_bytes(entry_path); ASSERT_FALSE(original.empty());
  const auto entry=read_json(entry_path);
  EXPECT_EQ(entry.at("schema"),"atx.dsl-candidate-ic/v1");
  EXPECT_EQ(entry.at("record").at("candidate_id"),"volume_level");
  EXPECT_EQ(entry.at("record").at("signal_payload_sha256"),
            read_json(root/cfg.train_sha256/(level_stem()+".json")).at("payload_sha256"));
  EXPECT_EQ(entry.at("record").at("key"),ic_results.at("key"));
  const auto record_sha=[](const Json& j) {
    auto sha=core::sha256_hex(j.at("record").dump()); return sha?*sha:std::string{};
  };
  EXPECT_EQ(entry.at("record_sha256"),record_sha(entry));
  const auto expect_refusal=[&](const Json& edited,const std::string& name,const std::string& reason) {
    std::string unused; ASSERT_TRUE(json_file(entry_path,edited,unused));
    const auto attempt=run_named(dir,cfg,name);
    EXPECT_FALSE(attempt.ok) << name;
    EXPECT_NE(attempt.error.find(reason),std::string::npos) << name << ": " << attempt.error;
    EXPECT_EQ(read_json(dir.path/name/"summary.json").at("status"),"failed") << name;
    EXPECT_EQ(read_json(entry_path),edited) << name; // refused in place, never overwritten
  };
  // One altered series digit under the old record hash: integrity refusal.
  auto altered=entry;
  auto& digits=altered["record"]["result"]["rank_series"][1].get_ref<std::string&>();
  ASSERT_FALSE(digits.empty()); digits.back()=digits.back()=='0'?'1':'0';
  expect_refusal(altered,"altered","candidate IC cache entry integrity: volume_level");
  // Self-consistent records naming other signal bytes or another key: mismatch.
  auto foreign=entry; foreign["record"]["signal_payload_sha256"]=std::string(64,'0');
  foreign["record_sha256"]=record_sha(foreign);
  expect_refusal(foreign,"foreign","candidate IC cache entry mismatch: volume_level");
  auto rekeyed=entry; rekeyed["record"]["key"]["min_names"]=4; rekeyed["record_sha256"]=record_sha(rekeyed);
  expect_refusal(rekeyed,"rekeyed","candidate IC cache entry mismatch: volume_level");
  // A self-consistent record with a wrong-length series: malformed.
  auto truncated=entry; truncated["record"]["result"]["pearson_series"][0]=std::string(16,'0');
  truncated["record_sha256"]=record_sha(truncated);
  expect_refusal(truncated,"truncated","candidate IC cache entry malformed: volume_level");
  ASSERT_TRUE(write_bytes(entry_path,std::vector<char>{'{'}));
  const auto corrupt=run_named(dir,cfg,"corrupt"); EXPECT_FALSE(corrupt.ok);
  EXPECT_NE(corrupt.error.find("candidate IC cache entry malformed: volume_level"),std::string::npos)
      << corrupt.error;
  // Restored: served again and untouched. Deleted: rescored and republished with
  // the same bytes. Outputs never change.
  ASSERT_TRUE(write_bytes(entry_path,original));
  const auto warm=run_named(dir,cfg,"warm"); ASSERT_TRUE(warm.ok) << warm.error;
  EXPECT_EQ(warm.log.find(" ic_result=miss"),std::string::npos);
  EXPECT_EQ(file_bytes(entry_path),original);
  ASSERT_TRUE(std::filesystem::remove(entry_path));
  const auto rescored=run_named(dir,cfg,"rescored"); ASSERT_TRUE(rescored.ok) << rescored.error;
  EXPECT_NE(rescored.log.find(" cache=hit ic_result=miss"),std::string::npos);
  EXPECT_EQ(file_bytes(entry_path),original);
  for (const std::string name:{"warm","rescored"})
    for (const auto* file:{"orientations.json","train_daily_ic.csv","validation_daily_ic.csv",
                           "train_planned_targets.csv","validation_planned_targets.csv"})
      EXPECT_EQ(file_sha(dir.path/name/file),file_sha(dir.path/"cold"/file)) << name << ' ' << file;
  // Another IC setting: its own directory, a clean miss that matches an uncached run.
  auto other=cfg; other.min_dates=16;
  const auto changed=run_named(dir,other,"changed"); ASSERT_TRUE(changed.ok) << changed.error;
  const auto changed_results=read_json(dir.path/"changed"/"summary.json").at("roles").at(0)
      .at("candidate_cache").at("ic_results");
  EXPECT_NE(changed_results.at("subdirectory"),subdirectory);
  EXPECT_EQ(changed_results.at("hits"),0);
  EXPECT_EQ(changed_results.at("key").at("min_dates"),16);
  other.candidate_cache_directory.clear();
  const auto uncached=run_named(dir,other,"changed_uncached"); ASSERT_TRUE(uncached.ok) << uncached.error;
  for (const auto* file:{"orientations.json","train_daily_ic.csv","validation_daily_ic.csv"})
    EXPECT_EQ(file_sha(dir.path/"changed"/file),file_sha(dir.path/"changed_uncached"/file)) << file;
}
// ---- Platform v8 B-1: --no-composition screening pass ----
// A daily IC CSV without its `__combined__` rows: the member rows, in file order.
std::string member_rows(const std::filesystem::path& path) {
  std::ifstream in(path,std::ios::binary); std::string out;
  for (std::string line;std::getline(in,line);)
    if (!line.starts_with("__combined__,")) out+=line+'\n';
  return out;
}
std::string text_of(const std::filesystem::path& path) {
  const auto bytes=file_bytes(path); return std::string(bytes.begin(),bytes.end());
}
TEST(NoComposition, SkipsBlendAndCombinedRows) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.save_combined=true;
  const auto full=run_named(dir,cfg,"full"); ASSERT_TRUE(full.ok) << full.error;
  auto screen=cfg; screen.no_composition=true;
  const auto run=run_named(dir,screen,"screen"); ASSERT_TRUE(run.ok) << run.error;
  const auto summary=read_json(dir.path/"screen"/"summary.json");
  const auto full_summary=read_json(dir.path/"full"/"summary.json");
  EXPECT_EQ(summary.at("status"),"complete"); EXPECT_EQ(summary.at("composition"),"skipped");
  EXPECT_FALSE(full_summary.contains("composition"));
  // Not a method input: the recipe (which still records --save-combined) is unchanged.
  EXPECT_EQ(file_sha(dir.path/"screen"/"recipe.json"),file_sha(dir.path/"full"/"recipe.json"));
  EXPECT_EQ(summary.at("recipe_sha256"),full_summary.at("recipe_sha256"));
  ASSERT_EQ(summary.at("roles").size(),2U);
  for (const auto& role_result:summary.at("roles")) {
    const auto name=role_result.at("role").get<std::string>();
    EXPECT_EQ(role_result.at("combined_evaluations"),0) << name;
    EXPECT_EQ(role_result.at("candidate_evaluations"),2) << name;
    for (const auto* key:{"combined_ic","planned_target_proxy","combined_artifact"})
      EXPECT_FALSE(role_result.contains(key)) << name << ' ' << key;
    EXPECT_FALSE(role_result.at("stage_seconds").contains("composition")) << name;
    for (const auto& candidate:role_result.at("candidates"))
      EXPECT_FALSE(candidate.at("stage_seconds").contains("composition")) << name;
    EXPECT_EQ(text_of(dir.path/"screen"/(name+"_daily_ic.csv")).find("__combined__"),std::string::npos) << name;
    for (const std::string suffix:{"_planned_targets.csv","_combined.json","_combined.f64","_combined_member.u8",
                                   "_combined_finite.u8","_combined_sessions.i64","_combined_ids.u64"})
      EXPECT_FALSE(std::filesystem::exists(dir.path/"screen"/(name+suffix))) << name << suffix;
  }
  // The same config without the flag does blend, so each omission above is the flag's.
  EXPECT_TRUE(full_summary.at("roles").at(0).contains("combined_ic"));
  EXPECT_TRUE(std::filesystem::exists(dir.path/"full"/"train_combined.json"));
  // Admission drops the composition plane.
  EXPECT_LT(summary.at("roles").at(0).at("admitted_working_bytes").get<u64>(),
            full_summary.at("roles").at(0).at("admitted_working_bytes").get<u64>());
  // The plan reports the skip. Pinned weights shape only the blend: they refuse with
  // the flag before any payload or output, in plan-only mode too.
  auto plan_cfg=screen; plan_cfg.plan_only=true; std::ostringstream plan;
  const auto planned=atx::impl::strategy::run_ic(plan_cfg,plan); ASSERT_TRUE(planned) << planned.error().to_string();
  EXPECT_EQ(Json::parse(plan.str()).at("composition"),"skipped");
  auto weighted=screen; ASSERT_TRUE(pin_weights(dir,weighted,{{"volume_level",.5},{"volume_rank",.5}}));
  for (const bool plan_only:{true,false}) {
    weighted.plan_only=plan_only; const auto refused=run_named(dir,weighted,"weighted");
    EXPECT_FALSE(refused.ok);
    EXPECT_NE(refused.error.find("--no-composition builds no blend"),std::string::npos) << refused.error;
    EXPECT_TRUE(refused.log.empty()); EXPECT_FALSE(std::filesystem::exists(dir.path/"weighted"));
  }
}
TEST(NoComposition, MemberRowsByteIdenticalToDefault) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  // A field candidate as well, so the field residency path runs under the flag.
  ASSERT_TRUE(field_library(cfg,{"si_shares"},{{"si_ratio","si_shares / volume"}}));
  ASSERT_TRUE(pin_fields(dir,cfg,"f",{"si_shares"}));
  cfg.workers=2; cfg.save_combined=true;
  const auto full=run_named(dir,cfg,"full"); ASSERT_TRUE(full.ok) << full.error;
  auto screen=cfg; screen.no_composition=true;
  const auto run=run_named(dir,screen,"screen"); ASSERT_TRUE(run.ok) << run.error;
  for (const auto* file:{"recipe.json","orientations.json"})
    EXPECT_EQ(file_sha(dir.path/"screen"/file),file_sha(dir.path/"full"/file)) << file;
  for (const std::string name:{"train","validation"})
    EXPECT_EQ(text_of(dir.path/"screen"/(name+"_daily_ic.csv")),member_rows(dir.path/"full"/(name+"_daily_ic.csv")))
        << name;
  // Every candidate row (IC, coverage, signs) and the field accounting are equal;
  // only timings, resources and the blend's own keys differ.
  const auto a=read_json(dir.path/"full"/"summary.json"),b=read_json(dir.path/"screen"/"summary.json");
  ASSERT_EQ(a.at("roles").size(),b.at("roles").size());
  for (usize r=0;r<a.at("roles").size();++r) {
    auto x=stable_role(a.at("roles").at(r),false),y=stable_role(b.at("roles").at(r),false);
    for (auto* role_result:{&x,&y})
      for (const auto* key:{"combined_ic","planned_target_proxy","combined_evaluations"}) role_result->erase(key);
    EXPECT_EQ(x,y) << r;
  }
}
// With the cache on, a candidate whose signal and IC result are both cached is not
// loaded: every payload is tampered at equal size (a load would refuse on its
// SHA256), yet the screening pass succeeds with the cold run's member outputs.
TEST(NoComposition, HitWithIcResultIsNotLoaded) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.candidate_cache_directory=(dir.path/"c").string();
  const auto cold=run_named(dir,cfg,"cold"); ASSERT_TRUE(cold.ok) << cold.error;
  const auto cold_summary=dir.path/"cold"/"summary.json";
  for (const std::string name:{"train","validation"})
    for (const std::string id:{"volume_level","volume_rank"}) {
      const auto payload=cache_entry(cold_summary,name,id).payload;
      auto bytes=file_bytes(payload); ASSERT_EQ(bytes.size(),D*N*sizeof(f64)) << id;
      bytes[8*N*100]=static_cast<char>(bytes[8*N*100]^1); ASSERT_TRUE(write_bytes(payload,bytes));
    }
  auto screen=cfg; screen.no_composition=true;
  const auto run=run_named(dir,screen,"screen"); ASSERT_TRUE(run.ok) << run.error;
  EXPECT_NE(run.log.find("IC cache-hit volume_rank role=validation layout=v2 payload=not-loaded ic_result=hit"),
            std::string::npos) << run.log;
  EXPECT_EQ(run.log.find("IC cache-miss"),std::string::npos);
  EXPECT_EQ(run.log.find("VM-complete"),std::string::npos);
  const auto summary=read_json(dir.path/"screen"/"summary.json");
  for (const auto& role_result:summary.at("roles")) {
    EXPECT_EQ(role_result.at("verify_bytes"),0);
    EXPECT_EQ(role_result.at("stage_seconds").at("cache_load"),0.0);
    EXPECT_EQ(role_result.at("candidate_cache").at("hits"),2);
    EXPECT_EQ(role_result.at("candidate_cache").at("ic_results").at("hits"),2);
    EXPECT_EQ(role_result.at("candidate_cache").at("entries").size(),2U);
  }
  EXPECT_EQ(file_sha(dir.path/"screen"/"orientations.json"),file_sha(dir.path/"cold"/"orientations.json"));
  for (const std::string name:{"train","validation"})
    EXPECT_EQ(text_of(dir.path/"screen"/(name+"_daily_ic.csv")),member_rows(dir.path/"cold"/(name+"_daily_ic.csv")))
        << name;
  // The default pass needs the bytes for its blend: it loads them and refuses.
  const auto blended=run_named(dir,cfg,"blended"); EXPECT_FALSE(blended.ok);
  EXPECT_NE(blended.error.find("candidate cache payload SHA256 mismatch"),std::string::npos) << blended.error;
  // Without its IC result the screening pass must score, so that candidate loads too.
  const auto level=cache_entry(cold_summary,"train","volume_level").sidecar;
  const auto subdirectory=read_json(cold_summary).at("roles").at(0).at("candidate_cache").at("ic_results")
      .at("subdirectory").get<std::string>();
  ASSERT_TRUE(std::filesystem::remove(level.parent_path()/subdirectory/level.filename()));
  const auto rescore=run_named(dir,screen,"rescore"); EXPECT_FALSE(rescore.ok);
  EXPECT_NE(rescore.error.find("candidate cache payload SHA256 mismatch: "+level_stem()+".f64"),std::string::npos)
      << rescore.error;
}
// Review focus 4: two roles (here two score windows over the same sessions) share
// one cache root. An entry of one role is a miss for the other, never a hit, and
// the other role's outputs equal an uncached run's.
TEST(StrategyIcRunner, CacheMissOnRoleChange) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.validation_manifest.clear(); cfg.validation_sha256.clear();
  cfg.candidate_cache_directory=(dir.path/"c").string();
  const auto first=run_named(dir,cfg,"first"); ASSERT_TRUE(first.ok) << first.error;
  const auto other=dir.path/"train_later";
  std::filesystem::copy(dir.path/"train",other,std::filesystem::copy_options::recursive);
  auto manifest=read_json(other/"manifest.json"); manifest["score_begin"]=400;
  manifest["score_start_ns"]=manifest.at("score_end_ns").get<i64>()-80*day;
  auto moved=cfg; moved.train_manifest=(other/"manifest.json").string();
  ASSERT_TRUE(json_file(moved.train_manifest,manifest,moved.train_sha256));
  ASSERT_NE(moved.train_sha256,cfg.train_sha256);
  const auto second=run_named(dir,moved,"second"); ASSERT_TRUE(second.ok) << second.error;
  EXPECT_EQ(second.log.find("IC cache-hit"),std::string::npos);
  const auto cache=read_json(dir.path/"second"/"summary.json").at("roles").at(0).at("candidate_cache");
  EXPECT_EQ(cache.at("hits"),0); EXPECT_EQ(cache.at("misses"),2);
  EXPECT_EQ(cache.at("ic_results").at("hits"),0);
  for (const auto& entry:cache.at("entries"))
    EXPECT_NE(entry.at("sidecar").get<std::string>().find(moved.train_sha256),std::string::npos);
  auto uncached=moved; uncached.candidate_cache_directory.clear();
  const auto plain=run_named(dir,uncached,"plain"); ASSERT_TRUE(plain.ok) << plain.error;
  for (const auto* file:{"orientations.json","train_daily_ic.csv","train_planned_targets.csv"})
    EXPECT_EQ(file_sha(dir.path/"second"/file),file_sha(dir.path/"plain"/file)) << file;
  // The first role's entries are untouched and still serve it.
  const auto again=run_named(dir,cfg,"again"); ASSERT_TRUE(again.ok) << again.error;
  EXPECT_EQ(read_json(dir.path/"again"/"summary.json").at("roles").at(0).at("candidate_cache").at("hits"),2);
}
// ---- Platform v8 B-2: field caps (manifest rows 1,024; referenced fields 256) and workers 16 ----
// Pads a fields directory's manifest to `rows` rows with unreferenced entries shaped
// like its first row (pad_0000, ...; payload SHAs no file backs: never opened) and re-pins it.
// `definition_bytes` > 0 gives each added row a definition of that many characters.
bool pad_manifest(const std::string& directory,usize rows,std::string& pin,usize definition_bytes=0) {
  const auto path=std::filesystem::path(directory)/"manifest.json";
  auto manifest=read_json(path); const auto first=manifest.at("fields").at(0);
  const std::string sha(64,'a');
  while (manifest.at("fields").size()<rows) {
    const auto name="pad_"+std::to_string(10000+manifest.at("fields").size()).substr(1);
    auto row=first; row["name"]=name; row["file"]=name+".f64"; row["sha256"]=sha;
    if (definition_bytes>0) row["definition"]=std::string(definition_bytes,'d');
    manifest["fields"].push_back(row);
    manifest["files"][name+".f64"]={{"bytes",D*N*sizeof(f64)},{"sha256",sha}};
  }
  return json_file(path,manifest,pin);
}
// `count` extra fields x000.. read ten per candidate (sums), in one new family.
bool wide_library(atx::impl::strategy::IcRunnerConfig& cfg,const std::string& prefix,usize count,
                  std::vector<std::string>& names) {
  names.clear(); std::vector<std::pair<std::string,std::string>> candidates;
  for (usize k=0;k<count;++k) names.push_back(prefix+std::to_string(1000+k).substr(1));
  for (usize first=0;first<count;first+=10) {
    std::string dsl;
    for (usize k=first;k<std::min(count,first+10);++k) dsl+=(k>first?" + ":"")+names[k];
    candidates.emplace_back("wide"+std::to_string(first/10),dsl);
  }
  return field_library(cfg,names,candidates);
}
TEST(FieldCaps, Admits200RowManifestWith40Referenced) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  std::vector<std::string> names; ASSERT_TRUE(wide_library(cfg,"f",40,names));
  ASSERT_TRUE(pin_fields(dir,cfg,"f",names));
  ASSERT_TRUE(pad_manifest(cfg.train_fields_directory,200,cfg.train_fields_sha256));
  ASSERT_TRUE(pad_manifest(cfg.validation_fields_directory,200,cfg.validation_fields_sha256));
  const auto run=run_named(dir,cfg,"output"); ASSERT_TRUE(run.ok) << run.error;
  const auto summary=read_json(dir.path/"output"/"summary.json");
  for (const auto& role_result:summary.at("roles")) {
    const auto& fields=role_result.at("research_fields");
    EXPECT_EQ(fields.at("loaded").size(),40U);
    EXPECT_EQ(fields.at("resident_capacity"),10); EXPECT_EQ(fields.at("planned_loads"),40);
    EXPECT_EQ(fields.at("field_loads"),40); EXPECT_EQ(fields.at("peak_resident_fields"),10);
  }
  // The row cap is 1,024: a 1,024-row manifest plans, a 1,025-row one refuses before any payload.
  auto plan_cfg=cfg; plan_cfg.plan_only=true;
  ASSERT_TRUE(pad_manifest(plan_cfg.train_fields_directory,1024,plan_cfg.train_fields_sha256));
  std::ostringstream plan; const auto admitted=atx::impl::strategy::run_ic(plan_cfg,plan);
  EXPECT_TRUE(admitted) << admitted.error().to_string();
  ASSERT_TRUE(pad_manifest(plan_cfg.train_fields_directory,1025,plan_cfg.train_fields_sha256));
  std::ostringstream refused_log; const auto refused=atx::impl::strategy::run_ic(plan_cfg,refused_log);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().to_string().find("train fields manifest field list (1..1024 rows)"),std::string::npos)
      << refused.error().to_string();
  EXPECT_TRUE(refused_log.str().empty());
}
// Review B-4: the fields manifest has its own byte bound (ic_fields_manifest_max_bytes,
// 16 MiB). 100 rows as wide as the widest published fields-v9 row (sv_ratio126, 12.3 KB)
// weigh over the 1 MiB metadata bound and still plan; one past 16 MiB refuses naming both
// bounds, before any payload or output. Other metadata files keep 1 MiB.
TEST(FieldCaps, AdmitsA100RowManifestOfPublishedRowWidth) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(field_library(cfg,{"si_shares"},{{"si_ratio","si_shares / volume"}}));
  ASSERT_TRUE(pin_fields(dir,cfg,"f",{"si_shares"}));
  ASSERT_TRUE(pad_manifest(cfg.train_fields_directory,100,cfg.train_fields_sha256,12300));
  const auto manifest=std::filesystem::path(cfg.train_fields_directory)/"manifest.json";
  EXPECT_EQ(read_json(manifest).at("fields").size(),100U);
  EXPECT_GT(std::filesystem::file_size(manifest),1ULL<<20);
  EXPECT_LT(std::filesystem::file_size(manifest),atx::impl::strategy::ic_fields_manifest_max_bytes);
  cfg.plan_only=true;
  std::ostringstream plan; const auto admitted=atx::impl::strategy::run_ic(cfg,plan);
  ASSERT_TRUE(admitted) << admitted.error().to_string();
  const auto roles=Json::parse(plan.str()).at("research_fields").at("roles");
  ASSERT_EQ(roles.size(),2U);
  EXPECT_EQ(roles.at(0).at("manifest_sha256"),cfg.train_fields_sha256);
  // One more row carrying 16 MiB of definition text crosses the bound.
  ASSERT_TRUE(pad_manifest(cfg.train_fields_directory,101,cfg.train_fields_sha256,
                           static_cast<usize>(atx::impl::strategy::ic_fields_manifest_max_bytes)));
  std::ostringstream refused_log; const auto refused=atx::impl::strategy::run_ic(cfg,refused_log);
  ASSERT_FALSE(refused);
  const auto message=refused.error().to_string();
  EXPECT_NE(message.find("over 16 MiB"),std::string::npos) << message;
  EXPECT_NE(message.find("train fields manifest bounds: 16 MiB, 1..1024 rows"),std::string::npos) << message;
  EXPECT_TRUE(refused_log.str().empty()); EXPECT_FALSE(std::filesystem::exists(dir.path/"output"));
}
// 256 referenced fields (bits 0..255 of the FieldMask) score end to end; a 257th refuses
// when the library compiles, before any manifest, role or output.
TEST(FieldCaps, RefusesLibraryReferencing257Fields) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  cfg.validation_manifest.clear(); cfg.validation_sha256.clear();
  const auto base_library=read_json(cfg.library_path); const auto base_sha=cfg.library_sha256;
  std::vector<std::string> names; ASSERT_TRUE(wide_library(cfg,"x",257,names));
  auto over=cfg; over.plan_only=true; std::ostringstream log;
  const auto status=atx::impl::strategy::run_ic(over,log); ASSERT_FALSE(status);
  EXPECT_NE(status.error().to_string().find("at most 256 extra fields (the library references 257)"),
            std::string::npos) << status.error().to_string();
  EXPECT_TRUE(log.str().empty());
  const auto run_over=run_named(dir,cfg,"over"); EXPECT_FALSE(run_over.ok);
  EXPECT_FALSE(std::filesystem::exists(dir.path/"over"));
  std::string restored; ASSERT_TRUE(json_file(cfg.library_path,base_library,restored)); ASSERT_EQ(restored,base_sha);
  cfg.library_sha256=base_sha;
  ASSERT_TRUE(wide_library(cfg,"x",256,names));
  cfg.train_fields_directory=(dir.path/"wide-train").string();
  ASSERT_TRUE(fields_dir(cfg.train_fields_directory,cfg.train_manifest,cfg.train_sha256,names,cfg.train_fields_sha256));
  const auto run=run_named(dir,cfg,"output"); ASSERT_TRUE(run.ok) << run.error;
  const auto summary=read_json(dir.path/"output"/"summary.json");
  const auto& fields=summary.at("roles").at(0).at("research_fields");
  EXPECT_EQ(fields.at("loaded").size(),256U); EXPECT_EQ(fields.at("resident_capacity"),10);
  EXPECT_EQ(fields.at("planned_loads"),256); EXPECT_EQ(fields.at("field_loads"),256);
  EXPECT_EQ(summary.at("roles").at(0).at("candidate_evaluations"),2+26);
  // The last candidate reads bits 250..255: it was loaded and scored like the first.
  EXPECT_NE(run.log.find("IC field-load role=train field=x255 candidate=wide25"),std::string::npos);
}
// The v7.1 library's field plan, the only structure the mask change touches, is the
// one the receipted v7.1 u pass printed (mega-v71-train-u-run1: resident_capacity=6
// planned_loads=54 over 40 referenced fields).
TEST(FieldCaps, V71FieldPlanUnchanged) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  const auto source=std::filesystem::path{ATX_IMPL_TESTS_DIR}.parent_path()/"strategies"/"fund_industry_ic_v71.json";
  const auto bytes=file_bytes(source); ASSERT_FALSE(bytes.empty()) << source;
  const std::string text(bytes.begin(),bytes.end()); const auto library=Json::parse(text);
  cfg.library_path=source.string(); auto digest=core::sha256_hex(text); ASSERT_TRUE(digest); cfg.library_sha256=*digest;
  // Metadata only: widen the warm-up so the library's longest lookback fits.
  for (const auto& path:{cfg.train_manifest,cfg.validation_manifest}) {
    auto j=read_json(path); j["score_begin"]=450;
    j["score_start_ns"]=j.at("score_end_ns").get<i64>()-30*day;
    std::string pin; ASSERT_TRUE(json_file(path,j,pin));
    if (path==cfg.train_manifest) cfg.train_sha256=pin; else cfg.validation_sha256=pin;
  }
  std::vector<std::string> extras;
  for (const auto& field:library.at("fields")) {
    const auto name=field.at("name").get<std::string>();
    if (name!="close" && name!="raw_close" && name!="volume") extras.push_back(name);
  }
  ASSERT_EQ(extras.size(),40U);
  ASSERT_TRUE(pin_fields(dir,cfg,"f",extras));
  cfg.plan_only=true; std::ostringstream plan;
  const auto status=atx::impl::strategy::run_ic(cfg,plan); ASSERT_TRUE(status) << status.error().to_string();
  const auto fields=Json::parse(plan.str()).at("research_fields");
  EXPECT_EQ(fields.at("loaded").size(),40U);
  EXPECT_EQ(fields.at("resident_capacity"),6); EXPECT_EQ(fields.at("planned_loads"),54);
}
// Workers 4, 8 and 16 give the same bytes: daily IC, planned targets, the saved blend
// and its masks, orientations and every role statistic; only the recipe's two worker
// keys (and timings/resources) differ.
TEST(Workers, OutputsByteIdenticalAt4And8And16) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(field_library(cfg,{"si_shares"},{{"si_ratio","si_shares / volume"}}));
  ASSERT_TRUE(pin_fields(dir,cfg,"f",{"si_shares"}));
  cfg.save_combined=true; cfg.max_working_bytes=512ULL<<20;
  const auto out=[&](usize workers) { return dir.path/("w"+std::to_string(workers)); };
  for (const usize workers:{usize{4},usize{8},usize{16}}) {
    cfg.workers=workers;
    const auto run=run_named(dir,cfg,"w"+std::to_string(workers)); ASSERT_TRUE(run.ok) << workers << ": " << run.error;
  }
  const auto recipe=[&](usize workers) {
    auto j=read_json(out(workers)/"recipe.json");
    EXPECT_EQ(j.at("vm_workers"),workers); EXPECT_EQ(j.at("research_ic_workers"),workers);
    j.erase("vm_workers"); j.erase("research_ic_workers"); return j;
  };
  const auto roles=[&](usize workers) {
    const auto summary=read_json(out(workers)/"summary.json"); Json stable=Json::array();
    for (const auto& role_result:summary.at("roles")) stable.push_back(stable_role(role_result,false));
    return stable;
  };
  std::vector<std::string> files{"train_daily_ic.csv","validation_daily_ic.csv","train_planned_targets.csv",
                                 "validation_planned_targets.csv"};
  for (const std::string role_name:{"train","validation"})
    for (const auto* suffix:{"_combined.f64","_combined_member.u8","_combined_finite.u8"})
      files.push_back(role_name+suffix);
  const auto reference_recipe=recipe(4); const auto reference_roles=roles(4);
  const auto reference_signs=read_json(out(4)/"orientations.json").at("candidates");
  for (const usize workers:{usize{8},usize{16}}) {
    EXPECT_EQ(recipe(workers),reference_recipe) << workers;
    EXPECT_EQ(roles(workers),reference_roles) << workers;
    EXPECT_EQ(read_json(out(workers)/"orientations.json").at("candidates"),reference_signs) << workers;
    for (const auto& file:files) {
      const auto expected=file_sha(out(4)/file); ASSERT_FALSE(expected.empty()) << file;
      EXPECT_EQ(file_sha(out(workers)/file),expected) << workers << ' ' << file;
    }
  }
}
// Review focus 3: --plan-only reports each role's required bytes; the per-worker
// envelope is as coded, so 16 workers need exactly 12 envelopes more than 4; below
// the requirement the run refuses before any payload, naming the bytes. OD-2: the
// CLI admits --max-memory-mib 2560 (bound 16,384) and --workers 16 (bound 16).
TEST(StrategyIcRunner, AdmissionReportsRequiredBytes) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  ASSERT_TRUE(std::filesystem::remove(dir.path/"validation"/"close.f64"));
  cfg.max_working_bytes=512ULL<<20;
  const auto plan=[&](atx::impl::strategy::IcRunnerConfig plan_cfg) {
    plan_cfg.plan_only=true; std::ostringstream log;
    const auto status=atx::impl::strategy::run_ic(plan_cfg,log);
    EXPECT_TRUE(status) << status.error().to_string();
    return status?Json::parse(log.str()):Json();
  };
  cfg.workers=4; const auto four=plan(cfg);
  cfg.workers=16; const auto sixteen=plan(cfg);
  ASSERT_FALSE(four.is_null()); ASSERT_FALSE(sixteen.is_null());
  EXPECT_EQ(sixteen.at("workers"),16); EXPECT_EQ(sixteen.at("max_working_bytes"),512ULL<<20);
  const u64 envelope=(8ULL<<20)+(64ULL<<10)+N*(1024+64+16)+D*64;
  ASSERT_EQ(sixteen.at("roles").size(),2U);
  for (usize r=0;r<2;++r)
    EXPECT_EQ(sixteen.at("roles").at(r).at("required_bytes").get<u64>()-
              four.at("roles").at(r).at("required_bytes").get<u64>(),12*envelope) << r;
  const auto required=sixteen.at("roles").at(0).at("required_bytes").get<u64>();
  auto tight=cfg; tight.max_working_bytes=required-1;
  for (const bool plan_only:{true,false}) {
    tight.plan_only=plan_only; const auto run=run_named(dir,tight,"tight");
    EXPECT_FALSE(run.ok);
    EXPECT_NE(run.error.find("required_bytes="+std::to_string(required)),std::string::npos) << run.error;
    EXPECT_TRUE(run.log.empty()); EXPECT_FALSE(std::filesystem::exists(dir.path/"tight"));
  }
  const auto cli=[&](const std::string& mib,const std::string& workers,std::string& out) {
    std::vector<std::string> args{"atx-equity-strategy-ic","--library",cfg.library_path,"--library-sha256",
        cfg.library_sha256,"--train",cfg.train_manifest,"--train-sha256",cfg.train_sha256,"--validation",
        cfg.validation_manifest,"--validation-sha256",cfg.validation_sha256,"--min-names","3","--min-dates","8",
        "--workers",workers,"--max-memory-mib",mib,"--plan-only"};
    std::vector<char*> argv; for (auto& arg:args) argv.push_back(arg.data());
    std::ostringstream stdout_log,stderr_log;
    const int code=atx::impl::strategy::dispatch_ic(static_cast<int>(argv.size()),argv.data(),stdout_log,stderr_log);
    out=stdout_log.str(); return code;
  };
  std::string printed;
  ASSERT_EQ(cli("2560","16",printed),0);
  const auto admitted=Json::parse(printed);
  EXPECT_EQ(admitted.at("max_working_bytes"),2560ULL<<20); EXPECT_EQ(admitted.at("workers"),16);
  EXPECT_EQ(cli("16385","16",printed),2); // the CLI's memory bound
  EXPECT_EQ(cli("2560","17",printed),1);  // bounded config
}
// ---- Platform v8 R-1: composition ew-theme-std-v1 (theme_standardise block) ----
std::string std_block(const std::string& themes,bool rerank,const std::string& rule="ew-theme-std-v1") {
  return ",\"theme_standardise\":{\"rule\":\""+rule+"\",\"rerank\":"+(rerank?"true":"false")+",\"themes\":"+themes+"}";
}
// The `__combined__` rows of a daily IC CSV, in file order.
std::string combined_rows(const std::filesystem::path& path) {
  std::ifstream in(path,std::ios::binary); std::string out;
  for (std::string line;std::getline(in,line);)
    if (line.starts_with("__combined__,")) out+=line+'\n';
  return out;
}
// The fixture library plus volume_vee, whose rank is V-shaped in the instrument (the
// fixture's volume rises with it), so a theme of volume_level and volume_vee has a
// composite that is not itself a rank grid: re-ranking it must change the blend.
bool vee_library(atx::impl::strategy::IcRunnerConfig& cfg) {
  auto lib=read_json(cfg.library_path);
  lib["candidates"].push_back({{"id","volume_vee"},{"family","fixed_volume"},{"dsl","abs(volume - 450000000)"},
      {"sign_policy","train-rank-ic21"},{"horizons",{5,21,63}}});
  return json_file(cfg.library_path,lib,cfg.library_sha256);
}
// R-1 identity (brief step 3): the rule with its re-rank off (the file's rerank false)
// and its member cap off (the fitter's side: here the ew-theme-v1 weights themselves)
// reproduces the ew-theme-v1 blend byte for byte -- saved signal, planned targets and
// __combined__ IC rows; only the weights pin and the summary's standardise record
// differ. With rerank true the blend changes and the rule is recorded.
TEST(CompositionV8, IdentityWithReRankAndCapOffIsEwThemeV1) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(vee_library(cfg)); cfg.save_combined=true;
  // ew-theme-v1 over one theme of two admitted members: 1 / (T n) = .5 each; volume_rank not admitted.
  const std::string weights=R"({"volume_level":0.5,"volume_rank":0.0,"volume_vee":0.5})";
  const std::string signs=R"(,"signs":{"volume_level":1,"volume_vee":1})";
  const std::string themes=R"({"volume_level":"liquidity","volume_vee":"liquidity"})";
  const auto pin=[&](const std::string& file,const std::string& text) {
    cfg.composition_weights_path=(dir.path/file).string();
    return text_file(cfg.composition_weights_path,text,cfg.composition_weights_sha256);
  };
  ASSERT_TRUE(pin("v1.json",themed_text(weights_v1,cfg,weights,signs)));
  const auto v1=run_named(dir,cfg,"v1"); ASSERT_TRUE(v1.ok) << v1.error;
  ASSERT_TRUE(pin("off.json",themed_text(weights_v2,cfg,weights,signs+std_block(themes,false))));
  const auto off=run_named(dir,cfg,"off"); ASSERT_TRUE(off.ok) << off.error;
  ASSERT_TRUE(pin("on.json",themed_text(weights_v2,cfg,weights,signs+std_block(themes,true))));
  const auto on=run_named(dir,cfg,"on"); ASSERT_TRUE(on.ok) << on.error;
  for (const std::string role_name:{"train","validation"}) {
    SCOPED_TRACE(role_name);
    for (const auto* suffix:{"_combined.f64","_combined_member.u8","_combined_finite.u8","_planned_targets.csv"}) {
      const auto expected=file_sha(dir.path/"v1"/(role_name+suffix)); ASSERT_FALSE(expected.empty()) << suffix;
      EXPECT_EQ(file_sha(dir.path/"off"/(role_name+suffix)),expected) << suffix;
    }
    const auto daily=role_name+"_daily_ic.csv";
    EXPECT_EQ(combined_rows(dir.path/"off"/daily),combined_rows(dir.path/"v1"/daily));
    EXPECT_FALSE(combined_rows(dir.path/"v1"/daily).empty());
    // Member IC rows never see the composition.
    EXPECT_EQ(member_rows(dir.path/"on"/daily),member_rows(dir.path/"v1"/daily));
    EXPECT_NE(file_sha(dir.path/"on"/(role_name+"_combined.f64")),file_sha(dir.path/"v1"/(role_name+"_combined.f64")));
    const auto v1_manifest=read_json(dir.path/"v1"/(role_name+"_combined.json"));
    EXPECT_FALSE(read_json(dir.path/"off"/(role_name+"_combined.json")).contains("composition_standardise"));
    const auto on_manifest=read_json(dir.path/"on"/(role_name+"_combined.json"));
    EXPECT_EQ(on_manifest.at("composition_standardise"),"ew-theme-std-v1");
    EXPECT_FALSE(on_manifest.contains("composition_redistribution"));
    EXPECT_EQ(on_manifest.at("signal_semantics"),v1_manifest.at("signal_semantics")); // still admissible to replay
  }
  // recipe.json: rerank off is the pinned method (only the weights pin differs); on is the rule.
  auto v1_recipe=read_json(dir.path/"v1"/"recipe.json"),off_recipe=read_json(dir.path/"off"/"recipe.json");
  const auto on_recipe=read_json(dir.path/"on"/"recipe.json");
  EXPECT_FALSE(off_recipe.contains("composition_standardise"));
  off_recipe.erase("composition_weights_sha256"); v1_recipe.erase("composition_weights_sha256");
  EXPECT_EQ(off_recipe,v1_recipe);
  EXPECT_EQ(on_recipe.at("composition_standardise"),"ew-theme-std-v1");
  EXPECT_EQ(on_recipe.at("composition"),"pinned-candidate-weights;pinned-candidate-signs;centered-tied-rank;"
      "theme-weighted-rank-sum-missing-neutral;theme-rerank-centered-tied-over-names-with-a-present-member;"
      "theme-weight-sum-of-member-weights");
  // summary.json composition_weights.standardise names the block, rerank-off included.
  EXPECT_FALSE(read_json(dir.path/"v1"/"summary.json").at("composition_weights").contains("standardise"));
  EXPECT_EQ(read_json(dir.path/"off"/"summary.json").at("composition_weights").at("standardise"),
            "ew-theme-std-v1;rerank-off");
  EXPECT_EQ(read_json(dir.path/"on"/"summary.json").at("composition_weights").at("standardise"),"ew-theme-std-v1");
}
TEST(CompositionV8, ThemeStandardiseRefusalsPrecedeAnyPayloadOrOutput) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  // Payloads are absent: every refusal below must precede any role payload read.
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  ASSERT_TRUE(std::filesystem::remove(dir.path/"validation"/"close.f64"));
  const auto path=dir.path/"weights.json"; cfg.composition_weights_path=path.string();
  const std::string equal=R"({"volume_level":0.5,"volume_rank":0.5})";
  const std::string both=R"({"volume_level":"liquidity","volume_rank":"liquidity"})";
  const auto plan=[&](const std::string& text,Json& printed) {
    if (!text_file(path,text,cfg.composition_weights_sha256)) return std::string("unwritable");
    cfg.plan_only=true; std::ostringstream log;
    const auto status=atx::impl::strategy::run_ic(cfg,log);
    if (!status) return status.error().to_string();
    printed=Json::parse(log.str()); return std::string{};
  };
  // Admitted: rerank on and off, and a zero-weight candidate without a theme. The
  // standardised composition admits one f64 plane per theme; rerank off admits none.
  Json on,off,sparse;
  ASSERT_EQ(plan(themed_text(weights_v2,cfg,equal,std_block(both,true)),on),"");
  ASSERT_EQ(plan(themed_text(weights_v2,cfg,equal,std_block(both,false)),off),"");
  ASSERT_EQ(plan(themed_text(weights_v2,cfg,R"({"volume_level":1,"volume_rank":0})",
                             std_block(R"({"volume_level":"a"})",true)),sparse),"");
  for (usize r=0;r<2;++r)
    EXPECT_EQ(on.at("roles").at(r).at("required_bytes").get<u64>()-off.at("roles").at(r).at("required_bytes").get<u64>(),
              D*N*8U) << r;
  const std::string shape="theme_standardise must be {rule: ew-theme-std-v1, rerank: true|false, themes: {id: theme}}";
  const std::string block_without_rerank=R"(,"theme_standardise":{"rule":"ew-theme-std-v1","themes":)"+both+"}";
  const std::string numeric_rerank=R"(,"theme_standardise":{"rule":"ew-theme-std-v1","rerank":1,"themes":)"+both+"}";
  const std::vector<std::pair<std::string,std::string>> cases{
      {themed_text(weights_v1,cfg,equal,std_block(both,true)),
       "theme_standardise requires composition weights schema atx.dsl-composition-weights/v2"},
      {themed_text(weights_v1,cfg,equal,std_block(both,false)),
       "theme_standardise requires composition weights schema atx.dsl-composition-weights/v2"},
      {themed_text(weights_v2,cfg,equal,theme_block(both)+std_block(both,true)),
       "theme_redistribution and theme_standardise are exclusive"},
      {themed_text(weights_v2,cfg,equal,""),"requires a theme_redistribution block or a theme_standardise block"},
      {themed_text(weights_v2,cfg,equal,std_block(both,true,"ew-theme-std-v2")),shape},
      {themed_text(weights_v2,cfg,equal,block_without_rerank),shape},
      {themed_text(weights_v2,cfg,equal,numeric_rerank),shape},
      {themed_text(weights_v2,cfg,equal,R"(,"theme_standardise":[1])"),shape},
      // The themes map is checked with rerank off too.
      {themed_text(weights_v2,cfg,equal,std_block(R"({"volume_level":"a"})",false)),
       "theme missing for weighted candidate: volume_rank"},
      {themed_text(weights_v2,cfg,equal,std_block(R"({"volume_level":"a","volume_rank":"a","other":"a"})",true)),
       "theme for unknown candidate: other"},
      {themed_text(weights_v2,cfg,equal,std_block(R"({"volume_level":"A","volume_rank":"a"})",true)),
       "theme name must match [a-z0-9_]{1,64}: volume_level"},
      {themed_text(weights_v2,cfg,R"({"volume_level":0,"volume_rank":0})",std_block("{}",true)),
       "theme_standardise needs 1..32 weighted themes"}};
  for (const bool plan_only:{true,false}) {
    for (const auto& [text,reason]:cases) {
      ASSERT_TRUE(text_file(path,text,cfg.composition_weights_sha256));
      cfg.plan_only=plan_only; std::ostringstream attempt;
      const auto status=atx::impl::strategy::run_ic(cfg,attempt);
      ASSERT_FALSE(status) << text;
      EXPECT_NE(status.error().to_string().find(reason),std::string::npos)
          << text << " -> " << status.error().to_string();
      EXPECT_TRUE(attempt.str().empty()); EXPECT_FALSE(std::filesystem::exists(dir.path/"output"));
    }
  }
}
// ---- Platform v8 R-11: composition theme-resid-v1 (theme_residualise block) ----
std::string resid_block(const std::string& order,const std::string& rule="theme-resid-v1") {
  return ",\"theme_residualise\":{\"rule\":\""+rule+"\",\"order\":"+order+"}";
}
// Two themes of the vee library: a = volume_level + volume_vee (a composite that is no rank
// grid), b = volume_rank. Order [a, b] re-ranks b's residual on a, order [b, a] a's on b, so
// both blends differ from ew-theme-std-v1's and from each other (the block's order is the
// rule's, not the library's); member IC rows never see the composition; recipe, combined
// manifest and summary record the rule. With one theme the rule is ew-theme-std-v1 byte for
// byte: the first theme in order is its standardised composite.
TEST(ThemeResidRunner, ResidualisesInTheBlockOrderAndRecordsTheRule) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(vee_library(cfg)); cfg.save_combined=true;
  const std::string weights=R"({"volume_level":0.25,"volume_rank":0.5,"volume_vee":0.25})";
  const std::string signs=R"(,"signs":{"volume_level":1,"volume_rank":1,"volume_vee":1})";
  const std::string themes=R"({"volume_level":"a","volume_rank":"b","volume_vee":"a"})";
  const auto pin=[&](const std::string& file,const std::string& text) {
    cfg.composition_weights_path=(dir.path/file).string();
    return text_file(cfg.composition_weights_path,text,cfg.composition_weights_sha256);
  };
  ASSERT_TRUE(pin("std.json",themed_text(weights_v2,cfg,weights,signs+std_block(themes,true))));
  const auto plain=run_named(dir,cfg,"std"); ASSERT_TRUE(plain.ok) << plain.error;
  ASSERT_TRUE(pin("ab.json",themed_text(weights_v2,cfg,weights,signs+std_block(themes,true)+resid_block(R"(["a","b"])"))));
  const auto ab=run_named(dir,cfg,"ab"); ASSERT_TRUE(ab.ok) << ab.error;
  ASSERT_TRUE(pin("ba.json",themed_text(weights_v2,cfg,weights,signs+std_block(themes,true)+resid_block(R"(["b","a"])"))));
  const auto ba=run_named(dir,cfg,"ba"); ASSERT_TRUE(ba.ok) << ba.error;
  for (const std::string role_name:{"train","validation"}) {
    SCOPED_TRACE(role_name);
    const auto combined=role_name+"_combined.f64";
    ASSERT_FALSE(file_sha(dir.path/"std"/combined).empty());
    EXPECT_NE(file_sha(dir.path/"ab"/combined),file_sha(dir.path/"std"/combined));
    EXPECT_NE(file_sha(dir.path/"ba"/combined),file_sha(dir.path/"std"/combined));
    EXPECT_NE(file_sha(dir.path/"ab"/combined),file_sha(dir.path/"ba"/combined));
    const auto daily=role_name+"_daily_ic.csv";
    EXPECT_EQ(member_rows(dir.path/"ab"/daily),member_rows(dir.path/"std"/daily));
    const auto manifest=read_json(dir.path/"ab"/(role_name+"_combined.json"));
    EXPECT_EQ(manifest.at("composition_residualise"),"theme-resid-v1");
    EXPECT_EQ(manifest.at("composition_standardise"),"ew-theme-std-v1");
    EXPECT_FALSE(read_json(dir.path/"std"/(role_name+"_combined.json")).contains("composition_residualise"));
  }
  auto ab_recipe=read_json(dir.path/"ab"/"recipe.json"),std_recipe=read_json(dir.path/"std"/"recipe.json");
  EXPECT_EQ(ab_recipe.at("composition_residualise"),"theme-resid-v1");
  EXPECT_FALSE(std_recipe.contains("composition_residualise"));
  ab_recipe.erase("composition_residualise"); ab_recipe.erase("composition_weights_sha256");
  std_recipe.erase("composition_weights_sha256");
  EXPECT_EQ(ab_recipe,std_recipe); // every other method statement is ew-theme-std-v1's
  EXPECT_EQ(read_json(dir.path/"ab"/"summary.json").at("composition_weights").at("residualise"),"theme-resid-v1");
  EXPECT_FALSE(read_json(dir.path/"std"/"summary.json").at("composition_weights").contains("residualise"));
  const std::string one=R"({"volume_level":"a","volume_rank":"a","volume_vee":"a"})";
  ASSERT_TRUE(pin("one-std.json",themed_text(weights_v2,cfg,weights,signs+std_block(one,true))));
  const auto one_std=run_named(dir,cfg,"one-std"); ASSERT_TRUE(one_std.ok) << one_std.error;
  ASSERT_TRUE(pin("one-resid.json",themed_text(weights_v2,cfg,weights,signs+std_block(one,true)+resid_block(R"(["a"])"))));
  const auto one_resid=run_named(dir,cfg,"one-resid"); ASSERT_TRUE(one_resid.ok) << one_resid.error;
  for (const std::string role_name:{"train","validation"})
    for (const auto* suffix:{"_combined.f64","_combined_finite.u8","_planned_targets.csv"})
      EXPECT_EQ(file_sha(dir.path/"one-resid"/(role_name+suffix)),file_sha(dir.path/"one-std"/(role_name+suffix)))
          << role_name << suffix;
}
TEST(ThemeResidRunner, BlockRefusalsPrecedeAnyPayloadOrOutput) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  // Payloads are absent: every refusal below must precede any role payload read.
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  ASSERT_TRUE(std::filesystem::remove(dir.path/"validation"/"close.f64"));
  const auto path=dir.path/"weights.json"; cfg.composition_weights_path=path.string();
  const std::string equal=R"({"volume_level":0.5,"volume_rank":0.5})";
  const std::string two=R"({"volume_level":"a","volume_rank":"b"})";
  const auto plan=[&](const std::string& text,Json& printed) {
    if (!text_file(path,text,cfg.composition_weights_sha256)) return std::string("unwritable");
    cfg.plan_only=true; std::ostringstream log;
    const auto status=atx::impl::strategy::run_ic(cfg,log);
    if (!status) return status.error().to_string();
    printed=Json::parse(log.str()); return std::string{};
  };
  // Admitted: the block adds the regression scratch, N x (8 x themes + 8) B per role.
  Json with,without;
  ASSERT_EQ(plan(themed_text(weights_v2,cfg,equal,std_block(two,true)+resid_block(R"(["b","a"])")),with),"");
  ASSERT_EQ(plan(themed_text(weights_v2,cfg,equal,std_block(two,true)),without),"");
  for (usize r=0;r<2;++r)
    EXPECT_EQ(with.at("roles").at(r).at("required_bytes").get<u64>()-
              without.at("roles").at(r).at("required_bytes").get<u64>(),N*(8U*2U+8U)) << r;
  EXPECT_EQ(with.at("composition_weights").at("residualise"),"theme-resid-v1");
  EXPECT_FALSE(without.at("composition_weights").contains("residualise"));
  const std::string shape="theme_residualise must be {rule: theme-resid-v1, order: [theme, ...]}";
  const std::string needs="theme_residualise needs a theme_standardise block with rerank true";
  const std::string order="theme_residualise order must name each weighted theme of theme_standardise exactly once";
  const auto std_on=std_block(two,true);
  const std::vector<std::pair<std::string,std::string>> cases{
      {themed_text(weights_v2,cfg,equal,resid_block(R"(["a","b"])")),needs},
      {themed_text(weights_v2,cfg,equal,std_block(two,false)+resid_block(R"(["a","b"])")),needs},
      {themed_text(weights_v2,cfg,equal,theme_block(two)+resid_block(R"(["a","b"])")),needs},
      {themed_text(weights_v1,cfg,equal,std_on+resid_block(R"(["a","b"])")),
       "theme_standardise requires composition weights schema atx.dsl-composition-weights/v2"},
      {themed_text(weights_v2,cfg,equal,std_on+resid_block(R"(["a","b"])","theme-resid-v2")),shape},
      {themed_text(weights_v2,cfg,equal,std_on+R"(,"theme_residualise":{"rule":"theme-resid-v1"})"),shape},
      {themed_text(weights_v2,cfg,equal,std_on+R"(,"theme_residualise":{"rule":"theme-resid-v1","order":"a,b"})"),shape},
      {themed_text(weights_v2,cfg,equal,std_on+R"(,"theme_residualise":[1])"),shape},
      {themed_text(weights_v2,cfg,equal,std_on+resid_block(R"(["a"])")),order},
      {themed_text(weights_v2,cfg,equal,std_on+resid_block(R"(["a","b","c"])")),order},
      {themed_text(weights_v2,cfg,equal,std_on+resid_block(R"(["a","a"])")),order+" (repeated: a)"},
      {themed_text(weights_v2,cfg,equal,std_on+resid_block(R"(["a","c"])")),order+" (not a weighted theme: c)"},
      {themed_text(weights_v2,cfg,equal,std_on+resid_block(R"(["a",1])")),order+" (an entry is not a string)"}};
  for (const bool plan_only:{true,false}) {
    for (const auto& [text,reason]:cases) {
      ASSERT_TRUE(text_file(path,text,cfg.composition_weights_sha256));
      cfg.plan_only=plan_only; std::ostringstream attempt;
      const auto status=atx::impl::strategy::run_ic(cfg,attempt);
      ASSERT_FALSE(status) << text;
      EXPECT_NE(status.error().to_string().find(reason),std::string::npos)
          << text << " -> " << status.error().to_string();
      EXPECT_TRUE(attempt.str().empty()); EXPECT_FALSE(std::filesystem::exists(dir.path/"output"));
    }
  }
}
// Review B-3 (Ruling E-10): a role built with --delisting-returns is refused as a signal
// role at admission, train or validation, before any payload or output; a role whose
// delisting block only marks terminations (returns_applied false) is admitted.
TEST(StrategyIcRunner, DelistingReturnsRoleIsRefusedBeforeAnyPayloadOrOutput) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  ASSERT_TRUE(std::filesystem::remove(dir.path/"validation"/"close.f64"));
  const auto declare=[](const std::string& manifest,bool applied,std::string& sha) {
    auto j=read_json(manifest);
    j["universe"]={{"id","linked-operating-v1"},{"delisting",{{"returns_applied",applied}}}};
    return json_file(manifest,j,sha);
  };
  ASSERT_TRUE(declare(cfg.train_manifest,false,cfg.train_sha256));
  ASSERT_TRUE(declare(cfg.validation_manifest,false,cfg.validation_sha256));
  cfg.plan_only=true;
  std::ostringstream admitted;
  const auto marked=atx::impl::strategy::run_ic(cfg,admitted);
  ASSERT_TRUE(marked) << marked.error().to_string();
  for (const bool train:{true,false}) {
    auto role_cfg=cfg;
    const auto& manifest=train?role_cfg.train_manifest:role_cfg.validation_manifest;
    ASSERT_TRUE(declare(manifest,true,train?role_cfg.train_sha256:role_cfg.validation_sha256));
    for (const bool plan_only:{true,false}) {
      role_cfg.plan_only=plan_only; std::ostringstream attempt;
      const auto status=atx::impl::strategy::run_ic(role_cfg,attempt);
      ASSERT_FALSE(status);
      const auto message=status.error().to_string();
      EXPECT_NE(message.find(manifest),std::string::npos) << message;
      EXPECT_NE(message.find("universe.delisting.returns_applied true"),std::string::npos) << message;
      EXPECT_TRUE(attempt.str().empty()); EXPECT_FALSE(std::filesystem::exists(dir.path/"output"));
    }
    ASSERT_TRUE(declare(manifest,false,train?role_cfg.train_sha256:role_cfg.validation_sha256));
  }
}
// ---- Platform v8 R-10: composition ic-shrink-v1 (the theme_standardise rule table) ----
// Six candidates in two themes (T = 2, cap 1/4): liquidity {volume_level .004, volume_vee .002,
// volume_lag_1 .003} (mean .003, shares 7/18, 5/18, 6/18) and size {volume_rank .001,
// volume_lag_2 .003, volume_lag_3 -.004} (mean 0, volume_lag_3 floored, shares 1/4, 3/4, 0).
// w = share / 2; volume_lag_2's 3/8 is capped at 1/4 and its 1/8 goes to the liquidity members
// (x 5/4): {35/144, 25/144, 30/144 | 1/8, 1/4, 0}, the hand derivation the runner verifies.
bool shrink_library(atx::impl::strategy::IcRunnerConfig& cfg) {
  auto lib=read_json(cfg.library_path);
  lib["candidates"].push_back({{"id","volume_vee"},{"family","fixed_volume"},{"dsl","abs(volume - 450000000)"},
      {"sign_policy","train-rank-ic21"},{"horizons",{5,21,63}}});
  for (usize k=1;k<=3;++k)
    lib["candidates"].push_back({{"id","volume_lag_"+std::to_string(k)},{"family","fixed_volume"},
        {"dsl","delay(volume, "+std::to_string(k)+")"},{"sign_policy","train-rank-ic21"},{"horizons",{5,21,63}}});
  return json_file(cfg.library_path,lib,cfg.library_sha256);
}
// ic-shrink-aim-v1 (fix round 1, Ruling E-44) adds the parent's aim gains {.5, 1, 1, .5 | .2, .9}
// (liquidity {level, vee, lag_1} | size {rank, lag_2, lag_3}): share x g = {7/36, 10/36, 6/36 |
// 1/4, 3/20, 0}, w = share x g / (2 x theme sum) = {7/46, 10/46, 6/46 | 5/16, 3/16, 0};
// volume_rank's 5/16 is capped at 1/4 and its 1/16 goes to the liquidity members (x 9/8):
// {63/368, 45/184, 27/184 | 1/4, 3/16, 0}.
struct ShrinkMember { const char* id; const char* theme; f64 ic; f64 weight; f64 gain; f64 aim_weight; };
const std::vector<ShrinkMember> shrink_members{
    {"volume_level","liquidity",.004,35.0/144,.5,63.0/368},{"volume_rank","size",.001,1.0/8,1.0,1.0/4},
    {"volume_vee","liquidity",.002,25.0/144,1.0,45.0/184},{"volume_lag_1","liquidity",.003,30.0/144,.5,27.0/184},
    {"volume_lag_2","size",.003,1.0/4,.2,3.0/16},{"volume_lag_3","size",-.004,0.0,.9,0.0}};
// The members' weights file: weights, +1 signs of the weighted members, and a theme_standardise
// block of `rule` (rerank true) naming their themes; ic-shrink-v1 adds its ic_shrink inputs,
// ic-shrink-aim-v1 those with the gains. The weights are ic-shrink-v1's, or ic-shrink-aim-v1's
// for that rule or with `aim_weights`.
Json shrink_doc(const atx::impl::strategy::IcRunnerConfig& cfg,const std::string& rule,bool aim_weights=false) {
  const bool aim=aim_weights || rule=="ic-shrink-aim-v1";
  Json weights=Json::object(),signs=Json::object(),themes=Json::object(),members=Json::object();
  for (const auto& m:shrink_members) {
    const f64 w=aim?m.aim_weight:m.weight;
    weights[m.id]=w;
    members[m.id]={{"theme",m.theme},{"ic",m.ic}};
    if (aim) members[m.id]["gain"]=m.gain;
    if (w>0) { signs[m.id]=1; themes[m.id]=m.theme; }
  }
  Json block{{"rule",rule},{"rerank",true},{"themes",themes}};
  if (rule=="ic-shrink-v1" || rule=="ic-shrink-aim-v1")
    block["ic_shrink"]=Json{{"intensity",.5},{"floor",0.0},{"members",members}};
  return Json{{"schema",weights_v2},{"library_sha256",cfg.library_sha256},
      {"train_manifest_sha256",cfg.train_sha256},{"weights",weights},{"signs",signs},{"theme_standardise",block}};
}
// ic-shrink-v1 runs ew-theme-std-v1's per-date standardisation unchanged: the same weights pinned
// under either rule give the same blend, planned targets and IC rows byte for byte (the flag-
// absent identity of the rule table: an ew-theme-std-v1 file keeps its records). The recipe, the
// combined manifests and the summary name the rule; the marginal verb's reader takes the block as
// a standardised one.
TEST(CompositionV8, IcShrinkRunsTheStandardisationUnchangedAndRecordsItsRule) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(shrink_library(cfg)); cfg.save_combined=true;
  const auto pin=[&](const std::string& file,const Json& doc) {
    cfg.composition_weights_path=(dir.path/file).string();
    return text_file(cfg.composition_weights_path,doc.dump(),cfg.composition_weights_sha256);
  };
  ASSERT_TRUE(pin("std.json",shrink_doc(cfg,"ew-theme-std-v1")));
  const auto standard=run_named(dir,cfg,"std"); ASSERT_TRUE(standard.ok) << standard.error;
  ASSERT_TRUE(pin("shrink.json",shrink_doc(cfg,"ic-shrink-v1")));
  const auto shrink=run_named(dir,cfg,"shrink"); ASSERT_TRUE(shrink.ok) << shrink.error;
  for (const std::string role_name:{"train","validation"}) {
    SCOPED_TRACE(role_name);
    for (const auto* suffix:{"_combined.f64","_combined_member.u8","_combined_finite.u8","_planned_targets.csv"}) {
      const auto expected=file_sha(dir.path/"std"/(role_name+suffix)); ASSERT_FALSE(expected.empty()) << suffix;
      EXPECT_EQ(file_sha(dir.path/"shrink"/(role_name+suffix)),expected) << suffix;
    }
    const auto daily=role_name+"_daily_ic.csv";
    EXPECT_FALSE(combined_rows(dir.path/"std"/daily).empty());
    EXPECT_EQ(combined_rows(dir.path/"shrink"/daily),combined_rows(dir.path/"std"/daily));
    EXPECT_EQ(member_rows(dir.path/"shrink"/daily),member_rows(dir.path/"std"/daily));
    auto std_manifest=read_json(dir.path/"std"/(role_name+"_combined.json"));
    auto shrink_manifest=read_json(dir.path/"shrink"/(role_name+"_combined.json"));
    EXPECT_EQ(std_manifest.at("composition_standardise"),"ew-theme-std-v1");
    EXPECT_EQ(shrink_manifest.at("composition_standardise"),"ic-shrink-v1");
    // A recipe pin too: the validation manifest names its run's orientations.json, which carries
    // the run's recipe_sha256 (the train manifest's is null).
    for (const auto& [run,manifest]:{std::pair{"std",&std_manifest},std::pair{"shrink",&shrink_manifest}}) {
      const auto& pin=manifest->at("orientations_artifact_sha256");
      if (role_name=="validation") EXPECT_EQ(pin,file_sha(dir.path/run/"orientations.json")) << run;
      else EXPECT_TRUE(pin.is_null()) << run;
    }
    for (auto* manifest:{&std_manifest,&shrink_manifest})
      for (const auto* key:{"composition_standardise","composition_weights_sha256","run_recipe_sha256",
                            "orientations_artifact_sha256"})
        manifest->erase(key);
    EXPECT_EQ(shrink_manifest,std_manifest);
  }
  auto std_recipe=read_json(dir.path/"std"/"recipe.json"),shrink_recipe=read_json(dir.path/"shrink"/"recipe.json");
  EXPECT_EQ(std_recipe.at("composition_standardise"),"ew-theme-std-v1");
  EXPECT_EQ(shrink_recipe.at("composition_standardise"),"ic-shrink-v1");
  for (auto* recipe:{&std_recipe,&shrink_recipe}) {
    recipe->erase("composition_standardise"); recipe->erase("composition_weights_sha256");
  }
  EXPECT_EQ(shrink_recipe,std_recipe); // the same per-date method statement
  EXPECT_EQ(read_json(dir.path/"std"/"summary.json").at("composition_weights").at("standardise"),"ew-theme-std-v1");
  EXPECT_EQ(read_json(dir.path/"shrink"/"summary.json").at("composition_weights").at("standardise"),"ic-shrink-v1");
  const auto grouping=atx::impl::strategy::ic_weights_themes(text_of(dir.path/"shrink.json"));
  ASSERT_TRUE(grouping) << grouping.error().to_string();
  EXPECT_EQ(grouping->block,"theme_standardise"); EXPECT_TRUE(grouping->rerank);
  EXPECT_EQ(grouping->themes.size(),5U);
}
// The runner verifies an ic-shrink-v1 file against its recorded inputs before any payload or
// output: weights off the rule (beyond 1e-12), other constants, rerank off, missing or malformed
// inputs, a weighted non-member, a themes entry that is not the member's theme, an unknown rule.
TEST(CompositionV8, IcShrinkRefusalsPrecedeAnyPayloadOrOutput) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(shrink_library(cfg));
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  ASSERT_TRUE(std::filesystem::remove(dir.path/"validation"/"close.f64"));
  const auto path=dir.path/"weights.json"; cfg.composition_weights_path=path.string();
  const auto attempt=[&](const Json& doc,bool plan_only,std::ostringstream& log) {
    cfg.plan_only=plan_only;
    if (!text_file(path,doc.dump(),cfg.composition_weights_sha256)) return std::string("unwritable");
    const auto status=atx::impl::strategy::run_ic(cfg,log);
    return status?std::string{}:status.error().to_string();
  };
  const auto good=shrink_doc(cfg,"ic-shrink-v1");
  const auto change=[&](auto&& edit) { auto doc=good; edit(doc); return doc; };
  // Admitted: the rule's weights, and weights within the 1e-12 tolerance of them.
  const auto within=change([](Json& d) { d["weights"]["volume_level"]=35.0/144+5e-13; });
  for (const auto* doc:{&good,&within}) {
    std::ostringstream log; EXPECT_EQ(attempt(*doc,true,log),"");
  }
  const std::string registered="intensity and floor must be the registered 0.5 and 0";
  const std::string malformed="member volume_level needs {theme";
  const std::vector<std::pair<Json,std::string>> cases{
      {change([](Json& d) { d["weights"]["volume_level"]=35.0/144+1e-9; d["weights"]["volume_vee"]=25.0/144-1e-9; }),
       "composition weight of volume_level is"},
      {change([](Json& d) { d["theme_standardise"]["ic_shrink"]["intensity"]=.25; }),registered},
      {change([](Json& d) { d["theme_standardise"]["ic_shrink"]["floor"]=.001; }),registered},
      {change([](Json& d) { d["theme_standardise"].erase("ic_shrink"); }),"needs ic_shrink"},
      {change([](Json& d) { d["theme_standardise"]["rerank"]=false; }),
       "theme_standardise rule ic-shrink-v1 needs rerank true"},
      {change([](Json& d) { d["theme_standardise"]["ic_shrink"]["members"]=Json::object(); }),
       "ic_shrink.members must be a non-empty object"},
      {change([](Json& d) {
         d["theme_standardise"]["ic_shrink"]["members"]["other"]=Json{{"theme","size"},{"ic",.001}};
       }),"member of unknown candidate: other"},
      {change([](Json& d) { d["theme_standardise"]["ic_shrink"]["members"]["volume_level"]["ic"]="0.004"; }),malformed},
      {change([](Json& d) { d["theme_standardise"]["ic_shrink"]["members"]["volume_level"]["ic"]=nullptr; }),malformed},
      {change([](Json& d) { d["theme_standardise"]["ic_shrink"]["members"]["volume_level"]["theme"]="Liquidity"; }),
       malformed},
      // Without volume_level the rule on the other five members is feasible ({1/4 x 4, 0}); the
      // weighted volume_level is refused as a non-member before its weight is compared.
      {change([](Json& d) { d["theme_standardise"]["ic_shrink"]["members"].erase("volume_level"); }),
       "weighted candidate volume_level is not an ic_shrink member"},
      {change([](Json& d) { d["theme_standardise"]["themes"]["volume_level"]="size"; }),
       "themes.volume_level is not its ic_shrink member theme"},
      {change([](Json& d) { d["theme_standardise"]["rule"]="ic-shrink-v2"; }),
       "theme_standardise must be {rule: ew-theme-std-v1, rerank: true|false, themes: {id: theme}} or "
       "{rule: ic-shrink-v1"}};
  for (const bool plan_only:{true,false}) {
    for (const auto& [doc,reason]:cases) {
      std::ostringstream log;
      const auto error=attempt(doc,plan_only,log);
      ASSERT_FALSE(error.empty()) << doc.dump();
      EXPECT_NE(error.find(reason),std::string::npos) << doc.dump() << " -> " << error;
      EXPECT_TRUE(log.str().empty()); EXPECT_FALSE(std::filesystem::exists(dir.path/"output"));
    }
  }
}
// ic-shrink-aim-v1 (fix round 1, Ruling E-44): its weights pinned under ew-theme-std-v1 and under
// the variant give the same blend, planned targets and IC rows byte for byte; the recipe, the
// combined manifests and the summary name the variant.
TEST(CompositionV8, IcShrinkAimRunsTheStandardisationUnchangedAndRecordsItsRule) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(shrink_library(cfg)); cfg.save_combined=true;
  const auto pin=[&](const std::string& file,const Json& doc) {
    cfg.composition_weights_path=(dir.path/file).string();
    return text_file(cfg.composition_weights_path,doc.dump(),cfg.composition_weights_sha256);
  };
  ASSERT_TRUE(pin("std.json",shrink_doc(cfg,"ew-theme-std-v1",true)));
  const auto standard=run_named(dir,cfg,"std"); ASSERT_TRUE(standard.ok) << standard.error;
  ASSERT_TRUE(pin("aim.json",shrink_doc(cfg,"ic-shrink-aim-v1")));
  const auto aim=run_named(dir,cfg,"aim"); ASSERT_TRUE(aim.ok) << aim.error;
  for (const std::string role_name:{"train","validation"}) {
    SCOPED_TRACE(role_name);
    for (const auto* suffix:{"_combined.f64","_combined_member.u8","_combined_finite.u8","_planned_targets.csv"}) {
      const auto expected=file_sha(dir.path/"std"/(role_name+suffix)); ASSERT_FALSE(expected.empty()) << suffix;
      EXPECT_EQ(file_sha(dir.path/"aim"/(role_name+suffix)),expected) << suffix;
    }
    const auto daily=role_name+"_daily_ic.csv";
    EXPECT_FALSE(combined_rows(dir.path/"std"/daily).empty());
    EXPECT_EQ(combined_rows(dir.path/"aim"/daily),combined_rows(dir.path/"std"/daily));
    EXPECT_EQ(member_rows(dir.path/"aim"/daily),member_rows(dir.path/"std"/daily));
    EXPECT_EQ(read_json(dir.path/"aim"/(role_name+"_combined.json")).at("composition_standardise"),"ic-shrink-aim-v1");
  }
  EXPECT_EQ(read_json(dir.path/"aim"/"recipe.json").at("composition_standardise"),"ic-shrink-aim-v1");
  EXPECT_EQ(read_json(dir.path/"aim"/"summary.json").at("composition_weights").at("standardise"),"ic-shrink-aim-v1");
  const auto grouping=atx::impl::strategy::ic_weights_themes(text_of(dir.path/"aim.json"));
  ASSERT_TRUE(grouping) << grouping.error().to_string();
  EXPECT_EQ(grouping->block,"theme_standardise"); EXPECT_TRUE(grouping->rerank);
  EXPECT_EQ(grouping->themes.size(),5U);
}
// The runner's re-application check covers the variant: the rule with the recorded gains, before
// any payload or output. ic-shrink-v1 weights under the variant, the variant's weights under
// ic-shrink-v1, a changed, missing or malformed gain, other constants and rerank off are refused.
TEST(CompositionV8, IcShrinkAimRefusalsPrecedeAnyPayloadOrOutput) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(shrink_library(cfg));
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  ASSERT_TRUE(std::filesystem::remove(dir.path/"validation"/"close.f64"));
  const auto path=dir.path/"weights.json"; cfg.composition_weights_path=path.string();
  const auto attempt=[&](const Json& doc,bool plan_only,std::ostringstream& log) {
    cfg.plan_only=plan_only;
    if (!text_file(path,doc.dump(),cfg.composition_weights_sha256)) return std::string("unwritable");
    const auto status=atx::impl::strategy::run_ic(cfg,log);
    return status?std::string{}:status.error().to_string();
  };
  const auto good=shrink_doc(cfg,"ic-shrink-aim-v1");
  const auto change=[&](auto&& edit) { auto doc=good; edit(doc); return doc; };
  const auto within=change([](Json& d) { d["weights"]["volume_level"]=63.0/368+5e-13; });
  for (const auto* doc:{&good,&within}) {
    std::ostringstream log; EXPECT_EQ(attempt(*doc,true,log),"");
  }
  const std::string variant="IC runner: theme_standardise rule ic-shrink-aim-v1: ";
  const std::string gain_shape="member volume_rank needs {theme: [a-z0-9_]{1,64}, ic: finite number, gain: finite "
                               "number > 0}";
  auto plain=shrink_doc(cfg,"ic-shrink-v1");
  plain["theme_standardise"]["rule"]="ic-shrink-aim-v1";
  plain["theme_standardise"]["ic_shrink"]=good.at("theme_standardise").at("ic_shrink");
  const std::vector<std::pair<Json,std::string>> cases{
      {plain,variant+"composition weight of volume_level is"},
      {change([](Json& d) { d["theme_standardise"]["rule"]="ic-shrink-v1"; }),
       "IC runner: theme_standardise rule ic-shrink-v1: composition weight of volume_level is"},
      {change([](Json& d) { d["theme_standardise"]["ic_shrink"]["members"]["volume_lag_2"]["gain"]=.3; }),
       variant+"composition weight of volume_level is"},
      {change([](Json& d) { d["theme_standardise"]["ic_shrink"]["members"]["volume_rank"].erase("gain"); }),
       variant+gain_shape},
      {change([](Json& d) { d["theme_standardise"]["ic_shrink"]["members"]["volume_rank"]["gain"]=0.0; }),
       variant+gain_shape},
      {change([](Json& d) { d["theme_standardise"]["ic_shrink"]["members"]["volume_rank"]["gain"]=-1.0; }),
       variant+gain_shape},
      {change([](Json& d) { d["theme_standardise"]["ic_shrink"]["members"]["volume_rank"]["gain"]="1"; }),
       variant+gain_shape},
      {change([](Json& d) { d["theme_standardise"]["ic_shrink"]["intensity"]=.25; }),
       variant+"ic_shrink intensity and floor must be the registered 0.5 and 0"},
      {change([](Json& d) { d["theme_standardise"].erase("ic_shrink"); }),
       variant+"needs ic_shrink {intensity, floor, members: {id: {theme, ic, gain}}}"},
      {change([](Json& d) { d["theme_standardise"]["rerank"]=false; }),
       "theme_standardise rule ic-shrink-aim-v1 needs rerank true"}};
  for (const bool plan_only:{true,false}) {
    for (const auto& [doc,reason]:cases) {
      std::ostringstream log;
      const auto error=attempt(doc,plan_only,log);
      ASSERT_FALSE(error.empty()) << doc.dump();
      EXPECT_NE(error.find(reason),std::string::npos) << doc.dump() << " -> " << error;
      EXPECT_TRUE(log.str().empty()); EXPECT_FALSE(std::filesystem::exists(dir.path/"output"));
    }
  }
}
// ---- Platform v8 R-11 on an R-10 parent (finding R6B-O-1; Rulings E-44, E-45) ----
// The shrink members' file under registered theme names: liquidity -> value, size ->
// price_momentum (the same first-appearance order, so the same rule weights and verify).
Json registered_themes(Json doc) {
  const auto relabel=[](Json& theme) { theme=(theme=="liquidity")?"value":"price_momentum"; };
  auto& block=doc["theme_standardise"];
  for (auto it=block["themes"].begin();it!=block["themes"].end();++it) relabel(*it);
  if (block.contains("ic_shrink"))
    for (auto it=block["ic_shrink"]["members"].begin();it!=block["ic_shrink"]["members"].end();++it)
      relabel((*it)["theme"]);
  return doc;
}
// theme-resid-v1 rides on a rerank-true theme_standardise of every row of the rule table
// (ew-theme-std-v1, and R-10's ic-shrink-v1 / ic-shrink-aim-v1): the plan admits the block and
// records both rules, before any payload (close.f64 is absent).
TEST(ThemeResidRunner, RidesOnEveryRerankTrueRuleOfTheTable) {
  Directory dir; atx::impl::strategy::IcRunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  ASSERT_TRUE(shrink_library(cfg));
  ASSERT_TRUE(std::filesystem::remove(dir.path/"train"/"close.f64"));
  ASSERT_TRUE(std::filesystem::remove(dir.path/"validation"/"close.f64"));
  const auto path=dir.path/"weights.json"; cfg.composition_weights_path=path.string(); cfg.plan_only=true;
  for (const std::string rule:{"ew-theme-std-v1","ic-shrink-v1","ic-shrink-aim-v1"}) {
    SCOPED_TRACE(rule);
    auto doc=registered_themes(shrink_doc(cfg,rule));
    doc["theme_residualise"]=Json{{"rule","theme-resid-v1"},{"order",Json::array({"value","price_momentum"})}};
    ASSERT_TRUE(text_file(path,doc.dump(),cfg.composition_weights_sha256));
    std::ostringstream log;
    const auto status=atx::impl::strategy::run_ic(cfg,log);
    ASSERT_TRUE(status) << status.error().to_string();
    const auto record=Json::parse(log.str()).at("composition_weights");
    EXPECT_EQ(record.at("standardise"),rule);
    EXPECT_EQ(record.at("residualise"),"theme-resid-v1");
    EXPECT_FALSE(std::filesystem::exists(dir.path/"output"));
  }
}
} // namespace
