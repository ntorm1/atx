#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <span>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
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
bool role(const std::filesystem::path& dir, i64 first_day, std::string& sha, f64 direction=1) {
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
  const auto plan=Json::parse(progress.str()); EXPECT_EQ(plan.at("candidates"),2);
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
  // deliberately missing payload or output-directory creation can be reached.
  cfg.plan_only=false;
  for (const usize workers:{usize{0},usize{5}}) {
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
} // namespace
