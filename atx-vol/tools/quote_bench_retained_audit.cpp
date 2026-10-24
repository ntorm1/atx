#include "quote_bench_retained_audit.hpp"
#include "quote_bench_acceptance.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace quote_bench_acceptance {
void RetainedRun::note_fit_invocation() noexcept {
  if (fit_invocations_ == std::numeric_limits<std::size_t>::max()) invocation_overflow_ = true;
  else ++fit_invocations_;
}

void RetainedRun::require(Phase expected) const {
  if (phase_ != expected) throw std::logic_error("Retained API audit lifecycle violation");
}

void RetainedRun::begin(std::size_t snapshot_capacity) {
  require(Phase::Empty);
  snapshots_.reserve(snapshot_capacity);
  phase_ = Phase::Capturing;
}

std::size_t RetainedRun::attempt(const Identity &identity,
                               std::span<const std::uint64_t> members) {
  require(Phase::Capturing);
  if (members.empty()) throw std::invalid_argument("Empty retained snapshot");
  std::set<std::uint64_t> unique(members.begin(), members.end());
  if (unique.size() != members.size()) throw std::invalid_argument("Duplicate retained member");
  for (const auto &snapshot : snapshots_) {
    for (const auto id : snapshot.members) {
      if (unique.contains(id)) throw std::invalid_argument("Repeated retained member");
    }
  }
  snapshots_.push_back({identity, {members.begin(), members.end()}, {}, {}, false});
  return snapshots_.size() - 1U;
}

void RetainedRun::retain(std::size_t index, std::vector<QuoteSurfaceSlice> &&fitted) {
  require(Phase::Capturing);
  auto &snapshot = snapshots_.at(index);
  if (snapshot.completed) throw std::logic_error("Repeated retained result");
  if (fitted.size() != snapshot.members.size())
    throw std::invalid_argument("Retained surface/member shape mismatch");
  snapshot.fitted = std::move(fitted);
  snapshot.completed = true;
}

void RetainedRun::failed(std::size_t index, std::string reason) {
  require(Phase::Capturing);
  auto &snapshot = snapshots_.at(index);
  if (snapshot.completed) throw std::logic_error("Repeated retained result");
  snapshot.failure = reason.empty() ? "surface_fit_failed" : std::move(reason);
  snapshot.completed = true;
}

void RetainedRun::unsupported(std::span<const std::uint64_t> members) {
  require(Phase::Capturing);
  unsupported_.insert(unsupported_.end(), members.begin(), members.end());
}

void RetainedRun::published() {
  require(Phase::Capturing);
  phase_ = Phase::Published;
}

void RetainedRun::snapshots_complete() {
  require(Phase::Published);
  before_audit_invocations_ = fit_invocations_;
  phase_ = Phase::Snapshotted;
}

nlohmann::json RetainedRun::audit(nlohmann::json &payload) {
  require(Phase::Snapshotted);
  const auto start = std::chrono::steady_clock::now();
  Json marker{{"schema", "atx-vol-post-fit-api-proof-v1"},
      {"phase", "after_cold_publication_and_fit_snapshots"},
      {"cold_clock_contract", "retained_surface_publication_v1"},
      {"same_retained_instance", true}, {"audit_completed", false}, {"failure_reason", ""},
      {"attempted_snapshot_count", snapshots_.size()}, {"retained_snapshot_count", 0U},
      {"serialized_snapshot_count", 0U}, {"attempted_member_count", 0U},
      {"retained_member_count", 0U}, {"serialized_member_count", 0U},
      {"attempted_chain_ids", Json::array()}, {"retained_chain_ids", Json::array()},
      {"serialized_chain_ids", Json::array()}, {"unsupported_chain_ids", unsupported_},
      {"missing_owner_chain_ids", Json::array()}, {"snapshot_failures", Json::array()},
      {"graded_surface_fit_invocations_before", before_audit_invocations_},
      {"retained_snapshot_capacity", snapshots_.capacity()}, {"retained_member_capacity", 0U},
      {"retained_fit_capacity", 0U}, {"retention_setup_in_cold", true},
      {"retained_owner_released_after_audit", false}, {"retained_owner_destruction_s", 0.0}};
  payload = {{"schema", "atx-vol-final-api-audit-v1"}, {"timing_qualifying", false},
      {"phase", "post_fit"}, {"representation", "certified_piecewise_linear"},
      {"snapshots", Json::array()}};
  std::size_t retained = 0, serialized = 0, member_capacity = 0, fit_capacity = 0;
  for (const auto &snapshot : snapshots_) {
    member_capacity += snapshot.members.capacity();
    fit_capacity += snapshot.fitted.capacity();
    for (const auto id : snapshot.members) marker["attempted_chain_ids"].push_back(id);
    if (!snapshot.completed || snapshot.fitted.empty()) {
      for (const auto id : snapshot.members) marker["missing_owner_chain_ids"].push_back(id);
      marker["snapshot_failures"].push_back({{"identity", snapshot.identity},
          {"reason", snapshot.failure.empty() ? "missing_owner" : snapshot.failure}});
      continue;
    }
    ++retained;
    for (const auto id : snapshot.members) marker["retained_chain_ids"].push_back(id);
    try {
      payload["snapshots"].push_back(surface(snapshot.fitted, snapshot.members));
      ++serialized;
      for (const auto id : snapshot.members) marker["serialized_chain_ids"].push_back(id);
    } catch (const std::exception &error) {
      marker["snapshot_failures"].push_back(
          {{"identity", snapshot.identity}, {"reason", error.what()}});
    }
  }
  marker["retained_snapshot_count"] = retained;
  marker["serialized_snapshot_count"] = serialized;
  marker["retained_member_capacity"] = member_capacity;
  marker["retained_fit_capacity"] = fit_capacity;
  for (const auto prefix : {"attempted", "retained", "serialized"}) {
    marker[std::string(prefix) + "_member_count"] =
        marker[std::string(prefix) + "_chain_ids"].size();
  }
  marker["graded_surface_fit_invocations_after"] = fit_invocations_;
  marker["refit_count"] = fit_invocations_ - before_audit_invocations_;
  const bool calls_match = !invocation_overflow_ &&
      before_audit_invocations_ == snapshots_.size() &&
      fit_invocations_ == before_audit_invocations_;
  const bool complete = !snapshots_.empty() && serialized == snapshots_.size() &&
                        unsupported_.empty() && calls_match;
  marker["audit_completed"] = complete;
  if (!complete) marker["failure_reason"] = "incomplete_retained_surface_evidence";
  if (!calls_match) marker["failure_reason"] = "surface_fit_invocation_mismatch";
  marker["audit_s"] = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - start).count();
  phase_ = Phase::Audited;
  return marker;
}

void RetainedRun::release(nlohmann::json &marker) {
  require(Phase::Audited);
  const auto start = std::chrono::steady_clock::now();
  {
    std::vector<Snapshot> releasing;
    releasing.swap(snapshots_);
    std::vector<std::uint64_t> unsupported;
    unsupported.swap(unsupported_);
  }
  marker["retained_owner_destruction_s"] = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - start).count();
  marker["retained_owner_released_after_audit"] = true;
  phase_ = Phase::Released;
}
} // namespace quote_bench_acceptance
