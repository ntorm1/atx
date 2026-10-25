#pragma once

#include "atx/vol/api/fitting/quote_slice.hpp"
#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace quote_bench_acceptance {

// Tool-local owner. Every allocating capture operation is called inside the cold
// interval. Probe is forbidden until publication and the external CPU/work
// snapshots have both completed. No method fits or reconstructs a surface.
class RetainedRun {
public:
  using Identity = std::array<std::string, 3>;
  void begin(std::size_t snapshot_capacity);
  void note_fit_invocation() noexcept;
  [[nodiscard]] std::size_t attempt(const Identity &identity,
                                    std::span<const std::uint64_t> members);
  void retain(std::size_t index, std::vector<atx::vol::QuoteSurfaceSlice> &&fitted);
  void failed(std::size_t index, std::string reason);
  void unsupported(std::span<const std::uint64_t> members);
  void published();
  void snapshots_complete();
  [[nodiscard]] nlohmann::json audit(nlohmann::json &payload);
  void release(nlohmann::json &marker);

private:
  enum class Phase { Empty, Capturing, Published, Snapshotted, Audited, Released };
  struct Snapshot {
    Identity identity;
    std::vector<std::uint64_t> members;
    std::vector<atx::vol::QuoteSurfaceSlice> fitted;
    std::string failure;
    bool completed{false};
  };
  void require(Phase expected) const;
  Phase phase_{Phase::Empty};
  std::vector<Snapshot> snapshots_;
  std::vector<std::uint64_t> unsupported_;
  std::size_t fit_invocations_{0};
  std::size_t before_audit_invocations_{0};
  bool invocation_overflow_{false};
};
} // namespace quote_bench_acceptance
