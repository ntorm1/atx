// RiskStream — a fixed book over a moving market: every state's frame is evaluated
// exactly once and is the shifted side of one explain and the base of the next.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "atx/vol/api/risk/pnl_explain.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

struct RiskStream::Impl {
  Impl(const RiskBook &b, RiskKernel k, RiskNeeds n, ExplainOptions o)
      : kernel{std::move(k)}, needs{n}, opts{o} {
    Result<PreparedRiskBook> p = PreparedRiskBook::create(b);
    if (p.has_value()) {
      prepared = std::move(*p);
      ws.reserve(prepared);
    } else {
      prepare_status = Err(p.error());
    }
  }

  PreparedRiskBook prepared;
  Status prepare_status{Ok()};
  RiskKernel kernel;
  RiskNeeds needs;
  ExplainOptions opts;
  RiskWorkspace ws;
  std::array<RiskFrame, 2> frames; // ping-pong: base and shifted of the last step
  std::size_t current{0};
  ExplainResult result;
  const MarketState *previous_state{nullptr}; // borrowed; see the header's LIFETIME note
  bool started{false};
  // Set when the last step saw a rate move: the NEXT evaluation materializes rho, so
  // the step after it can explain a rate move by rho instead of unexplained. The step
  // that first sees the move (and the one after, whose base frame was evaluated
  // before the move was known) may report rate_move_without_rho.
  bool rho_next{false};
  std::uint64_t n_evaluations{0};
  std::uint64_t n_explains{0};
};

RiskStream::RiskStream(const RiskBook &book, RiskKernel kernel, RiskNeeds needs,
                       ExplainOptions opts)
    : impl_{std::make_unique<Impl>(book, std::move(kernel), needs, opts)} {}
RiskStream::~RiskStream() = default;
RiskStream::RiskStream(RiskStream &&) noexcept = default;
RiskStream &RiskStream::operator=(RiskStream &&) noexcept = default;

Status RiskStream::start(const MarketState &s0) {
  if (!impl_) {
    return Err(ErrorCode::InvalidArgument, "RiskStream: moved-from stream");
  }
  Impl &m = *impl_;
  if (!m.prepare_status.has_value()) {
    return m.prepare_status;
  }
  m.current = 0;
  m.rho_next = false;
  if (Status st = m.kernel.evaluate(m.prepared, s0, m.needs, m.frames[0], m.ws); !st.has_value()) {
    m.started = false;
    return st;
  }
  ++m.n_evaluations;
  m.previous_state = &s0;
  m.started = true;
  return Ok();
}

Result<const ExplainResult *> RiskStream::step(const MarketState &s1) {
  if (!impl_) {
    return Err(ErrorCode::InvalidArgument, "RiskStream: moved-from stream");
  }
  Impl &m = *impl_;
  if (!m.started || m.previous_state == nullptr) {
    return Err(ErrorCode::InvalidArgument, "RiskStream::step before a successful start");
  }
  RiskNeeds needs = m.needs;
  needs.rho = needs.rho || m.rho_next;
  const std::size_t next = 1u - m.current;
  if (Status st = m.kernel.evaluate(m.prepared, s1, needs, m.frames[next], m.ws);
      !st.has_value()) {
    return Err(st.error());
  }
  ++m.n_evaluations;
  if (Status st = explain_into(m.prepared, *m.previous_state, m.frames[m.current], s1,
                               m.frames[next], m.opts, m.result);
      !st.has_value()) {
    return Err(st.error());
  }
  ++m.n_explains;
  bool rate_moved = false;
  for (std::size_t i = 0; i < m.result.size() && !rate_moved; ++i) {
    rate_moved = m.result.status[i] == RiskLaneStatus::Ok && m.result.d_rate[i] != 0.0;
  }
  m.rho_next = rate_moved;
  m.current = next;
  m.previous_state = &s1;
  return &m.result;
}

const PreparedRiskBook &RiskStream::prepared() const noexcept { return impl_->prepared; }
const RiskFrame &RiskStream::current() const noexcept { return impl_->frames[impl_->current]; }
const RiskFrame &RiskStream::previous() const noexcept {
  return impl_->frames[1u - impl_->current];
}
std::uint64_t RiskStream::n_evaluations() const noexcept {
  return impl_ ? impl_->n_evaluations : 0u;
}
std::uint64_t RiskStream::n_explains() const noexcept { return impl_ ? impl_->n_explains : 0u; }

} // namespace atx::vol
