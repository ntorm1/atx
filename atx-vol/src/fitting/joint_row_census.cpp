#include "fitting/joint_row_census.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <utility>

namespace atx::vol::research {
namespace {
std::atomic<bool> active{false};
std::atomic<std::uint64_t> foreign_events{0};
thread_local JointRowObserver *local_owner = nullptr;

template <class T> bool append_extent(std::span<T> values,
    std::array<std::pair<std::uintptr_t, std::uintptr_t>, 5> &ranges, std::size_t i) noexcept {
  const auto begin = reinterpret_cast<std::uintptr_t>(values.data());
  if (values.size() > std::numeric_limits<std::uintptr_t>::max() / sizeof(T)) return false;
  const auto bytes = values.size() * sizeof(T);
  if (bytes > std::numeric_limits<std::uintptr_t>::max() - begin) return false;
  ranges[i] = {begin, begin + bytes};
  return true;
}
bool disjoint_storage(JointRowStorage s) noexcept {
  std::array<std::pair<std::uintptr_t, std::uintptr_t>, 5> ranges{};
  if (!append_extent(s.slices, ranges, 0) || !append_extent(s.nodes, ranges, 1) ||
      !append_extent(s.coordinates, ranges, 2) || !append_extent(s.variables, ranges, 3) ||
      !append_extent(s.rows, ranges, 4)) return false;
  for (std::size_t i = 0; i < ranges.size(); ++i)
    for (std::size_t j = 0; j < i; ++j)
      if (ranges[i].first != ranges[i].second && ranges[j].first != ranges[j].second &&
          ranges[i].first < ranges[j].second && ranges[j].first < ranges[i].second) return false;
  return true;
}
} // namespace

JointRowObserver::JointRowObserver(void *context, Built built, Published published) noexcept
    : context_(context), built_(built), published_(published) {}
bool JointRowObserver::valid() const noexcept {
  return finished_ && !active_ && !busy_ && error_ == JointRowCaptureError::None &&
      attempts_ == 1 && built_events_ == 1 && published_events_ == 1;
}
void JointRowObserver::refuse(JointRowCaptureError error) noexcept {
  if (error_ == JointRowCaptureError::None) error_ = error;
}
void JointRowObserver::invalidate(JointRowCaptureError error) noexcept { refuse(error); }
void JointRowObserver::attempt() noexcept {
  ++attempts_;
  if (busy_) refuse(JointRowCaptureError::Reentrant);
  else if (!active_ || attempts_ != 1) refuse(JointRowCaptureError::WrongPhase);
}
void JointRowObserver::built(const JointRowBuiltView &view) noexcept {
  ++built_events_;
  if (busy_) refuse(JointRowCaptureError::Reentrant);
  if (!active_ || attempts_ != 1 || built_events_ != 1 || published_events_ != 0 || !built_)
    refuse(JointRowCaptureError::WrongPhase);
  if (error_ != JointRowCaptureError::None) return;
  busy_ = true;
  try { refuse(built_(context_, view)); }
  catch (...) { refuse(JointRowCaptureError::CallbackException); }
  busy_ = false;
}
void JointRowObserver::published(const JointRowPublishedView &view) noexcept {
  ++published_events_;
  if (busy_) refuse(JointRowCaptureError::Reentrant);
  if (!active_ || attempts_ != 1 || built_events_ != 1 || published_events_ != 1 || !published_)
    refuse(JointRowCaptureError::WrongPhase);
  if (error_ != JointRowCaptureError::None) return;
  busy_ = true;
  try { refuse(published_(context_, view)); }
  catch (...) { refuse(JointRowCaptureError::CallbackException); }
  busy_ = false;
}
JointRowCaptureScope::JointRowCaptureScope(JointRowObserver &observer) noexcept {
  foreign_before_ = foreign_events.load();
  if (!joint_row_census_compiled()) { observer.refuse(JointRowCaptureError::Disabled); return; }
  if (observer.finished_) { observer.refuse(JointRowCaptureError::ReusedScope); return; }
  bool expected = false;
  if (!active.compare_exchange_strong(expected, true)) {
    observer.refuse(JointRowCaptureError::NestedScope);
    if (local_owner) local_owner->refuse(JointRowCaptureError::NestedScope);
    ++foreign_events;
    return;
  }
  observer.active_ = true;
  owner_ = &observer;
  local_owner = owner_;
}
JointRowCaptureScope::~JointRowCaptureScope() {
  if (!owner_) return;
  active.store(false);
  if (foreign_events.load() != foreign_before_) owner_->refuse(JointRowCaptureError::ForeignThread);
  owner_->active_ = false;
  owner_->finished_ = true;
  if (owner_->attempts_ != 1 || owner_->built_events_ != 1 || owner_->published_events_ != 1)
    owner_->refuse(JointRowCaptureError::Incomplete);
  local_owner = nullptr;
}
bool joint_row_census_compiled() noexcept {
#ifdef ATX_VOL_JOINT_ROW_CENSUS_RESEARCH
  return true;
#else
  return false;
#endif
}
JointRowObserver *joint_row_census_observer() noexcept {
  if (local_owner) return local_owner;
  if (active.load()) ++foreign_events;
  return nullptr;
}
JointRowRecorder::JointRowRecorder(JointRowStorage storage) noexcept
    : storage_(storage), observer_(this, copy_built, copy_published) {
  if (!disjoint_storage(storage_)) observer_.invalidate(JointRowCaptureError::InvalidView);
}
JointRowStorage JointRowRecorder::data() const noexcept {
  return {storage_.slices.first(slices_), storage_.nodes.first(nodes_),
      storage_.coordinates.first(coordinates_), storage_.variables.first(variables_),
      storage_.rows.first(rows_)};
}
JointRowCaptureError JointRowRecorder::copy_built(void *context, const JointRowBuiltView &v) {
  auto &self = *static_cast<JointRowRecorder *>(context);
  const auto n = v.target.size();
  if (v.slices != v.offsets.size() || !v.slice_context || !v.slice_at ||
      v.variable.size() != v.fixed.size() ||
      v.lower.size() != n || v.upper.size() != n || v.initial.size() != n ||
      !v.row_at || !v.row_context || v.iteration_budget == 0 || !(v.tolerance > 0.))
    return JointRowCaptureError::InvalidView;
  if (!v.raw.at || !v.raw.context) return JointRowCaptureError::MissingRawTargets;
  std::size_t nodes = 0;
  for (std::size_t i = 0; i < v.slices; ++i) {
    const auto slice = v.slice_at(v.slice_context, i);
    if (slice.nodes.size() > std::numeric_limits<std::size_t>::max() - nodes)
      return JointRowCaptureError::InvalidView;
    nodes += slice.nodes.size();
  }
  if (nodes > std::numeric_limits<std::size_t>::max() / 2 || v.variable.size() != 2 * nodes)
    return JointRowCaptureError::InvalidView;
  const auto storage = self.storage_;
  if (v.slices > storage.slices.size() || nodes > storage.nodes.size() ||
      v.variable.size() > storage.coordinates.size() || n > storage.variables.size() ||
      v.rows > storage.rows.size()) return JointRowCaptureError::Capacity;
  // Lengths remain zero until the complete synchronous copy succeeds.
  std::size_t offset = 0;
  for (std::size_t s = 0; s < v.slices; ++s) {
    const auto slice = v.slice_at(v.slice_context, s);
    const auto raw = v.raw.at(v.raw.context, s);
    if (raw.size() != slice.nodes.size()) return JointRowCaptureError::MissingRawTargets;
    if (slice.boxes.size() != slice.nodes.size() || v.offsets[s] != 2 * offset ||
        (!slice.terminal_prepaid.empty() && slice.terminal_prepaid.size() != slice.nodes.size()))
      return JointRowCaptureError::InvalidView;
    storage.slices[s] = {offset, slice.nodes.size(), v.offsets[s], slice.bounds,
        slice.terminal_discount.has_value(), !slice.terminal_prepaid.empty(),
        slice.terminal_discount.value_or(0.)};
    for (std::size_t i = 0; i < slice.nodes.size(); ++i) {
      const auto &node = slice.nodes[i];
      if (node.strike != raw[i].strike || !std::isfinite(raw[i].call) || !std::isfinite(raw[i].put))
        return JointRowCaptureError::MissingRawTargets;
      storage.nodes[offset+i] = {node.strike, node.call, node.put, raw[i].call, raw[i].put,
          slice.terminal_prepaid.empty() ? 0. : slice.terminal_prepaid[i], 0., 0., slice.boxes[i]};
    }
    offset += slice.nodes.size();
  }
  for (std::size_t i = 0; i < v.variable.size(); ++i) {
    if (v.variable[i] < -1 || (v.variable[i] >= 0 && static_cast<std::size_t>(v.variable[i]) >= n))
      return JointRowCaptureError::InvalidView;
    storage.coordinates[i] = {v.variable[i], v.fixed[i]};
  }
  for (std::size_t i = 0; i < n; ++i)
    storage.variables[i] = {v.lower[i], v.upper[i], v.target[i], v.initial[i], 0., 0.};
  for (std::size_t i = 0; i < v.rows; ++i) {
    const auto row = v.row_at(v.row_context, i);
    if (row.count == 0 || row.count > 4 || row.index.size() != 4 || row.coefficient.size() != 4)
      return JointRowCaptureError::InvalidView;
    if (!std::isfinite(row.rhs) || !std::isfinite(row.initial_slack))
      return JointRowCaptureError::InvalidView;
    for (std::size_t j = 0; j < row.count; ++j)
      if (row.index[j] < 0 || static_cast<std::size_t>(row.index[j]) >= n ||
          !std::isfinite(row.coefficient[j]))
        return JointRowCaptureError::InvalidView;
    auto &copy = storage.rows[i];
    std::copy_n(row.index.begin(), 4, copy.index.begin());
    std::copy_n(row.coefficient.begin(), 4, copy.coefficient.begin());
    copy.count = row.count;
    copy.rhs = row.rhs;
    copy.initial_slack = row.initial_slack;
  }
  self.slices_ = v.slices; self.nodes_ = nodes; self.coordinates_ = v.variable.size();
  self.variables_ = n; self.rows_ = v.rows; self.budget_ = v.iteration_budget;
  self.tolerance_ = v.tolerance;
  return JointRowCaptureError::None;
}
JointRowCaptureError JointRowRecorder::copy_published(void *context, const JointRowPublishedView &v) {
  auto &self = *static_cast<JointRowRecorder *>(context);
  if (v.nodes.size() != self.slices_ || v.iterate.size() != self.variables_ ||
      !v.diagnostics || !v.diagnostics->certificate_passed ||
      v.diagnostics->variables != self.variables_ || v.diagnostics->constraints != self.rows_)
    return JointRowCaptureError::InvalidView;
  const auto storage = self.storage_;
  for (std::size_t s = 0; s < self.slices_; ++s) {
    const auto &slice = storage.slices[s];
    if (v.nodes[s].size() != slice.nodes) return JointRowCaptureError::InvalidView;
    for (std::size_t i = 0; i < slice.nodes; ++i) {
      auto &node = storage.nodes[slice.node_offset+i];
      const auto &published = v.nodes[s][i];
      if (node.strike != published.strike) return JointRowCaptureError::InvalidView;
      node.published_call = published.call;
      node.published_put = published.put;
      for (std::size_t side = 0; side < 2; ++side) {
        const auto &coordinate = storage.coordinates[slice.coordinate_offset+2*i+side];
        if (coordinate.variable >= 0)
          storage.variables[static_cast<std::size_t>(coordinate.variable)].published =
              side == 0 ? published.call : published.put;
      }
    }
  }
  for (std::size_t i = 0; i < self.variables_; ++i) storage.variables[i].iterate = v.iterate[i];
  self.sweeps_ = v.sweeps;
  self.diagnostics_ = *v.diagnostics;
  self.has_profile_ = v.profile != nullptr;
  if (v.profile) self.profile_ = *v.profile;
  return JointRowCaptureError::None;
}
} // namespace atx::vol::research
