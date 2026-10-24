#pragma once

// Offline export only. The numerical observer owns fixed values; JSON creation
// and file output occur after the fit and the capture scope have both ended.
#include "pricing/american_iv_candidate_capture.hpp"

#include <nlohmann/json.hpp>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace quote_bench_candidate_capture {
using Json = nlohmann::json;
using namespace atx::vol;

inline Json real(double value) {
  std::ostringstream bits;
  bits << std::hex << std::setw(16) << std::setfill('0') << std::bit_cast<std::uint64_t>(value);
  return {{"value", std::isfinite(value) ? Json(value) : Json(nullptr)}, {"bits", bits.str()}};
}
inline Json optional_real(const std::optional<double> &value) {
  return value ? real(*value) : Json(nullptr);
}
inline Json options(const std::optional<AlOpts> &value) {
  if (!value) return nullptr;
  return {{"n_collocation", value->n_collocation}, {"n_quadrature", value->n_quadrature},
      {"n_quad_price", value->n_quad_price}, {"max_newton_iter", value->max_newton_iter},
      {"tol", real(value->tol)}, {"profile", static_cast<unsigned>(value->profile)}};
}
inline Json floor_state(const IvPriceSeedFloorSnapshot &value) {
  Json slots = Json::array();
  for (const auto &slot : value.slots) {
    slots.push_back({{"prepared", slot.prepared}, {"anchor", real(slot.anchor)},
        {"T", real(slot.T)}, {"r", real(slot.r)}, {"q", real(slot.q)},
        {"side", static_cast<unsigned>(slot.side)}, {"options", options(slot.opts)}});
  }
  return {{"preparations", value.preparations}, {"hits", value.hits}, {"slots", slots}};
}
inline Json trace(const research::IvPriceCandidateTrace &value) {
  const auto &a = value.request;
  const auto &o = value.options;
  const auto &s = value.stats;
  Json phases = Json::array();
  for (std::size_t i = 0; i < value.phases.size(); ++i) {
    const auto &p = value.phases[i];
    phases.push_back({{"phase", i}, {"visited", p.visited}, {"complete", p.complete},
        {"stopped_early", p.stopped_early}, {"outcome", static_cast<unsigned>(p.outcome)},
        {"sigma", optional_real(p.sigma)}, {"seed_sigma", optional_real(p.seed_sigma)},
        {"error", p.error ? Json(static_cast<unsigned>(*p.error)) : Json(nullptr)},
        {"typed_status", p.typed_status ? Json(static_cast<unsigned>(*p.typed_status)) : Json(nullptr)},
        {"options", options(p.al_opts)}, {"elapsed_ns", p.elapsed_ns}, {"work", p.work},
        {"floor_preparations", p.floor_preparations}, {"floor_hits", p.floor_hits}});
  }
  return {{"complete", value.complete}, {"floor_cache_present", value.floor_cache_present},
      {"request", {{"price", real(a.price)}, {"S", real(a.S)}, {"K", real(a.K)},
          {"T", real(a.T)}, {"r", real(a.r)}, {"q", real(a.q)}, {"budget", real(a.budget)},
          {"side", static_cast<unsigned>(a.side)}}},
      {"options", {{"method", static_cast<unsigned>(o.method)}, {"tol", real(o.tol)},
          {"max_iter", o.max_iter}, {"al_options", options(o.al_opts)},
          {"has_correction", o.has_correction}, {"warm_start", real(o.warm_start)},
          {"price_tol", real(o.price_tol)}, {"refuse_non_monotone", o.refuse_non_monotone},
          {"strategy", static_cast<unsigned>(o.strategy)},
          {"seed_profile", o.seed_profile ? Json(static_cast<unsigned>(*o.seed_profile)) : Json(nullptr)}}},
      {"kernel", static_cast<unsigned>(value.kernel)}, {"outcome", static_cast<unsigned>(value.outcome)},
      {"sigma", optional_real(value.sigma)},
      {"stats", {{"stopped_early", s.stopped_early}, {"accepted_early", s.accepted_early},
          {"retried_full_search", s.retried_full_search}, {"screen_attempted", s.screen_attempted},
          {"screen_stopped_early", s.screen_stopped_early}, {"screen_accepted", s.screen_accepted}}},
      {"phases", phases}, {"elapsed_ns", value.elapsed_ns}, {"work", value.work},
      {"floor_preparations", value.floor_preparations}, {"floor_hits", value.floor_hits},
      {"floor_before", floor_state(value.floor_before)}, {"floor_after", floor_state(value.floor_after)}};
}
inline Json report(const research::IvCandidateCapture &capture, std::size_t record_capacity,
                   std::size_t cache_capacity) {
  Json records = Json::array(), caches = Json::array();
  for (const auto &record : capture.records())
    records.push_back({{"sequence", record.sequence}, {"cache_id", record.cache_id},
                       {"trace", trace(record.trace)}});
  for (const auto &cache : capture.cache_lifetimes())
    caches.push_back({{"id", cache.id}, {"address", cache.address}, {"closed", cache.closed},
        {"created_before_call", cache.created_before_call}, {"closed_after_calls", cache.closed_after_calls},
        {"final_state", floor_state(cache.final_state)}});
  return {{"schema", "atx-price-candidate-capture-v1"}, {"timing_qualifying", false},
      {"capture_enabled", research::iv_candidate_capture_enabled()}, {"valid", capture.valid()},
      {"attempted_calls", capture.attempted_calls()}, {"record_capacity", record_capacity},
      {"cache_capacity", cache_capacity}, {"record_overflow", capture.record_overflow()},
      {"cache_overflow", capture.cache_overflow()}, {"records", records}, {"caches", caches},
      {"phase_order", {"ScreenProposal", "ScreenProof", "FastProposal", "FastProof", "CompleteFast", "RetryProof"}},
      {"clock_scope", "Nested observed operation clocks; outer includes observer bookkeeping. Not qualified latency."}};
}
inline void write(const std::filesystem::path &path, const Json &value) {
  if (std::filesystem::exists(path)) throw std::runtime_error("Candidate capture output already exists");
  std::ofstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("Cannot open candidate capture output");
  stream << value.dump(2) << '\n';
  stream.close();
  if (!stream) throw std::runtime_error("Candidate capture write failed");
}
} // namespace quote_bench_candidate_capture
