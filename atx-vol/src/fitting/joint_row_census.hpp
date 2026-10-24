#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "fitting/joint_inbox_projection.hpp"

namespace atx::vol::research {

// Every view/accessor is synchronous and expires when its callback returns.
struct JointRowView {
  std::span<const int> index;
  std::span<const double> coefficient;
  std::size_t count = 0;
  double rhs = 0.;
  double initial_slack = 0.;
};
struct JointRowRawSource {
  const void *context = nullptr;
  std::span<const AmericanPriceNode> (*at)(const void *, std::size_t) noexcept = nullptr;
};
struct JointRowSliceView {
  std::span<const AmericanPriceNode> nodes;
  std::span<const AmericanPriceBox> boxes;
  AmericanPriceBounds bounds;
  std::optional<double> terminal_discount;
  std::span<const double> terminal_prepaid;
};
struct JointRowBuiltView {
  std::size_t slices = 0;
  const void *slice_context = nullptr;
  JointRowSliceView (*slice_at)(const void *, std::size_t) noexcept = nullptr;
  std::span<const std::size_t> offsets;
  std::span<const int> variable;
  std::span<const double> fixed, lower, upper, target, initial;
  const void *row_context = nullptr;
  JointRowView (*row_at)(const void *, std::size_t) noexcept = nullptr;
  std::size_t rows = 0, iteration_budget = 0;
  double tolerance = 0.;
  JointRowRawSource raw;
};
struct JointRowPublishedView {
  std::span<const std::vector<AmericanPriceNode>> nodes;
  std::span<const double> iterate;
  std::size_t sweeps = 0;
  const detail::JointInboxDiagnostics *diagnostics = nullptr;
  const detail::JointInboxProfile *profile = nullptr;
};

enum class JointRowCaptureError {
  None, Disabled, Capacity, InvalidView, MissingRawTargets, WrongPhase,
  CallbackException, Reentrant, NestedScope, ReusedScope, ForeignThread, Incomplete,
  SetupAllocation
};

class JointRowObserver {
public:
  using Built = JointRowCaptureError (*)(void *, const JointRowBuiltView &);
  using Published = JointRowCaptureError (*)(void *, const JointRowPublishedView &);
  JointRowObserver(void *context, Built built, Published published) noexcept;
  JointRowObserver(const JointRowObserver &) = delete;
  JointRowObserver &operator=(const JointRowObserver &) = delete;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] JointRowCaptureError error() const noexcept { return error_; }
  [[nodiscard]] std::size_t attempts() const noexcept { return attempts_; }
  [[nodiscard]] std::size_t built_events() const noexcept { return built_events_; }
  [[nodiscard]] std::size_t published_events() const noexcept { return published_events_; }
  void invalidate(JointRowCaptureError error) noexcept;
  void attempt() noexcept;
  void built(const JointRowBuiltView &) noexcept;
  void published(const JointRowPublishedView &) noexcept;
private:
  friend class JointRowCaptureScope;
  void refuse(JointRowCaptureError error) noexcept;
  void *context_;
  Built built_;
  Published published_;
  std::size_t attempts_ = 0, built_events_ = 0, published_events_ = 0;
  bool busy_ = false, active_ = false, finished_ = false;
  JointRowCaptureError error_ = JointRowCaptureError::None;
};

// Single owner thread; join every worker before scope destruction. Foreign
// events never dereference owner storage. Nested/reentrant capture fails closed.
class JointRowCaptureScope {
public:
  explicit JointRowCaptureScope(JointRowObserver &) noexcept;
  ~JointRowCaptureScope();
  JointRowCaptureScope(const JointRowCaptureScope &) = delete;
  JointRowCaptureScope &operator=(const JointRowCaptureScope &) = delete;
  [[nodiscard]] bool attached() const noexcept { return owner_ != nullptr; }
private:
  JointRowObserver *owner_ = nullptr;
  std::uint64_t foreign_before_ = 0;
};
[[nodiscard]] bool joint_row_census_compiled() noexcept;
[[nodiscard]] JointRowObserver *joint_row_census_observer() noexcept;

struct JointCapturedRow {
  std::array<int, 4> index{};
  std::array<double, 4> coefficient{};
  std::size_t count = 0;
  double rhs = 0.;
  double initial_slack = 0.;
};
struct JointCapturedSlice {
  std::size_t node_offset = 0, nodes = 0, coordinate_offset = 0;
  AmericanPriceBounds bounds{};
  bool has_discount = false, has_prepaid = false;
  double discount = 0.;
};
struct JointCapturedNode {
  double strike = 0., selected_call = 0., selected_put = 0.;
  double raw_call = 0., raw_put = 0., prepaid = 0.;
  double published_call = 0., published_put = 0.;
  AmericanPriceBox box{};
};
struct JointCapturedCoordinate { int variable = -1; double fixed = 0.; };
struct JointCapturedVariable {
  double lower = 0., upper = 0., target = 0., initial = 0., iterate = 0., published = 0.;
};
struct JointRowStorage {
  std::span<JointCapturedSlice> slices;
  std::span<JointCapturedNode> nodes;
  std::span<JointCapturedCoordinate> coordinates;
  std::span<JointCapturedVariable> variables;
  std::span<JointCapturedRow> rows;
};

// Borrows disjoint storage for the owner's entire lifetime; hooks never allocate.
// Empty/short storage invalidates evidence without affecting numerical execution.
class JointRowRecorder {
public:
  explicit JointRowRecorder(JointRowStorage storage) noexcept;
  JointRowRecorder(const JointRowRecorder &) = delete;
  JointRowRecorder &operator=(const JointRowRecorder &) = delete;
  [[nodiscard]] JointRowObserver &observer() noexcept { return observer_; }
  [[nodiscard]] const JointRowObserver &observer() const noexcept { return observer_; }
  [[nodiscard]] JointRowStorage data() const noexcept;
  [[nodiscard]] JointRowStorage capacity() const noexcept { return storage_; }
  [[nodiscard]] std::size_t iteration_budget() const noexcept { return budget_; }
  [[nodiscard]] double tolerance() const noexcept { return tolerance_; }
  [[nodiscard]] std::size_t sweeps() const noexcept { return sweeps_; }
  [[nodiscard]] const detail::JointInboxDiagnostics &diagnostics() const noexcept { return diagnostics_; }
  [[nodiscard]] const detail::JointInboxProfile &profile() const noexcept { return profile_; }
  [[nodiscard]] bool has_profile() const noexcept { return has_profile_; }
private:
  static JointRowCaptureError copy_built(void *, const JointRowBuiltView &);
  static JointRowCaptureError copy_published(void *, const JointRowPublishedView &);
  JointRowStorage storage_;
  JointRowObserver observer_;
  std::size_t slices_ = 0, nodes_ = 0, coordinates_ = 0, variables_ = 0, rows_ = 0;
  std::size_t budget_ = 0, sweeps_ = 0;
  double tolerance_ = 0.;
  detail::JointInboxDiagnostics diagnostics_{};
  detail::JointInboxProfile profile_{};
  bool has_profile_ = false;
};

} // namespace atx::vol::research
