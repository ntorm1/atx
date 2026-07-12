#include "atx/vol/chain.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

#include "atx/core/error.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

std::atomic<std::uint64_t> g_next_chain_instance_id{1u};

[[nodiscard]] std::uint64_t next_chain_instance_id() noexcept {
  std::uint64_t current = g_next_chain_instance_id.load();
  while (current != std::numeric_limits<std::uint64_t>::max()) {
    if (g_next_chain_instance_id.compare_exchange_weak(current, current + 1u)) {
      return current;
    }
  }
  return 0u;
}

} // namespace

Result<OptionChain> OptionChain::from_frame(const QuoteFrame &frame, MarketEnv env) {
  OptionChain chain;
  ATX_TRY(const Uid uid, data_install(chain.u_, frame));
  // Validate the underlying resolves (install returned a live uid).
  ATX_TRY(const Underlying *under, chain.u_.get_underlying(uid));
  if (under == nullptr) {
    return Err(ErrorCode::NotFound, "OptionChain::from_frame: no underlying installed");
  }
  chain.instance_id_ = next_chain_instance_id();
  if (chain.instance_id_ == 0u) {
    return Err(ErrorCode::OutOfRange, "OptionChain::from_frame: chain instance id exhausted");
  }
  chain.uid_ = uid;
  chain.expiry_quote_revisions_.assign(under->chains.size(), 0u);
  chain.env_ = std::move(env);
  if (!(chain.env_.spot > 0.0)) {
    chain.env_.spot = frame.spot; // fall back to the frame's spot
  }
  if (chain.env_.now_ns == 0) {
    chain.env_.now_ns = frame.snapshot_ts_ns;
  }
  // Representative pipeline rate: the env's rate at the front listed expiry
  // (chains are installed ascending in T). == flat_rate for a flat env.
  double front_T = 0.0;
  if (!under->chains.empty()) {
    front_T = under->chains.front().T;
  }
  chain.r_repr_ = (front_T > 0.0) ? chain.env_.rate_at(front_T) : chain.env_.flat_rate;
  return Ok(std::move(chain));
}

Result<OptionChain> OptionChain::from_frame(const QuoteFrame &frame, double r, double spot) {
  MarketEnv env = MarketEnv::flat(spot, r, frame.snapshot_ts_ns);
  // A flat env with spot == 0 falls back to frame.spot inside the env overload.
  return from_frame(frame, std::move(env));
}

const Underlying &OptionChain::underlying() const {
  // uid_ is valid for the chain's lifetime (set at from_frame); the deref is
  // safe. get_underlying is O(1) and the deque address is stable.
  return *u_.get_underlying(uid_).value();
}

std::size_t OptionChain::size() const noexcept {
  const auto under = u_.get_underlying(uid_);
  if (!under.has_value()) {
    return 0u;
  }
  std::size_t n = 0u;
  for (const Chain &c : (*under.value()).chains) {
    n += 2u * c.n_strikes(); // call + put per strike
  }
  return n;
}

std::vector<OptionId> OptionChain::ids() const {
  std::vector<OptionId> out;
  const auto under = u_.get_underlying(uid_);
  if (!under.has_value()) {
    return out;
  }
  const Underlying &U = *under.value();
  out.reserve(size());
  for (const Chain &c : U.chains) {
    const std::uint16_t ns = static_cast<std::uint16_t>(c.n_strikes());
    for (std::uint16_t si = 0; si < ns; ++si) {
      out.push_back(make_contract_id(uid_, c.expiry_id, si, Side::Call));
      out.push_back(make_contract_id(uid_, c.expiry_id, si, Side::Put));
    }
  }
  return out;
}

ChainSnapshot OptionChain::snapshot() const {
  ChainSnapshot out;
  const auto under = u_.get_underlying(uid_);
  if (!under.has_value()) {
    return out;
  }
  const Underlying &U = *under.value();
  const std::size_t n = size();
  out.ids.reserve(n);
  out.T.reserve(n);
  out.strike.reserve(n);
  out.bid.reserve(n);
  out.ask.reserve(n);
  out.mid.reserve(n);
  out.side.reserve(n);
  for (const Chain &c : U.chains) {
    const std::uint16_t ns = static_cast<std::uint16_t>(c.n_strikes());
    for (std::uint16_t si = 0; si < ns; ++si) {
      for (const Side side : {Side::Call, Side::Put}) {
        const std::size_t idx = chain_index(si, side);
        out.ids.push_back(make_contract_id(uid_, c.expiry_id, si, side));
        out.T.push_back(c.T);
        out.strike.push_back(c.strikes[si]);
        out.bid.push_back(c.bids[idx]);
        out.ask.push_back(c.asks[idx]);
        out.mid.push_back(c.mids[idx]);
        out.side.push_back(side);
      }
    }
  }
  return out;
}

Result<OptionRef> OptionChain::at(OptionId id) const {
  if (cid_uid(id) != uid_) {
    return Err(ErrorCode::NotFound, "OptionChain::at: id belongs to another underlying");
  }
  const auto under = u_.get_underlying(uid_);
  if (!under.has_value()) {
    return Err(ErrorCode::NotFound, "OptionChain::at: underlying not resolvable");
  }
  const Underlying &U = *under.value();
  const ExpiryId exp = cid_expiry(id);
  if (exp >= U.chains.size()) {
    return Err(ErrorCode::NotFound, "OptionChain::at: unknown expiry");
  }
  const Chain &c = U.chains[exp];
  const std::uint16_t si = cid_strike_idx(id);
  if (si >= c.n_strikes()) {
    return Err(ErrorCode::NotFound, "OptionChain::at: unknown strike");
  }
  const Side side = cid_side(id);
  const std::size_t idx = chain_index(si, side);

  OptionRef ref;
  ref.id = id;
  ref.expiry_ns = c.expiry_ns;
  ref.T = c.T;
  ref.strike = c.strikes[si];
  ref.side = side;
  ref.bid = c.bids[idx];
  ref.ask = c.asks[idx];
  ref.mid = c.mids[idx];
  ref.bid_size = c.bid_sizes[idx];
  ref.ask_size = c.ask_sizes[idx];
  return Ok(ref);
}

Status OptionChain::update_quotes(std::span<const OptionId> ids, std::span<const double> bids,
                                  std::span<const double> asks) {
  if (ids.size() != bids.size() || ids.size() != asks.size()) {
    return Err(ErrorCode::InvalidArgument,
               "OptionChain::update_quotes: ids / bids / asks length mismatch");
  }
  const std::size_t n = ids.size();
  const Underlying &under = underlying();
  std::vector<bool> touched_expiry(under.chains.size(), false);
  bool any_valid = false;
  for (const OptionId id : ids) {
    if (cid_uid(id) != uid_) {
      continue;
    }
    const ExpiryId expiry = cid_expiry(id);
    if (expiry >= under.chains.size()) {
      continue;
    }
    const Chain &expiry_chain = under.chains[expiry];
    if (cid_strike_idx(id) >= expiry_chain.n_strikes()) {
      continue;
    }
    touched_expiry[expiry] = true;
    any_valid = true;
  }
  if (any_valid) {
    if (quote_revision_ == std::numeric_limits<std::uint64_t>::max()) {
      return Err(ErrorCode::OutOfRange, "OptionChain::update_quotes: board revision exhausted");
    }
    for (std::size_t expiry = 0u; expiry < touched_expiry.size(); ++expiry) {
      if (touched_expiry[expiry] &&
          expiry_quote_revisions_[expiry] == std::numeric_limits<std::uint64_t>::max()) {
        return Err(ErrorCode::OutOfRange,
                   "OptionChain::update_quotes: expiry revision exhausted");
      }
    }
  }
  std::vector<std::int32_t> sizes(n, 1);
  std::vector<std::int64_t> ts(n, env_.now_ns);
  std::vector<std::uint8_t> flags(n, 0u);
  for (std::size_t i = 0; i < n; ++i) {
    // Stamp the LOCKED / CROSSED diagnostic bits the loader convention uses so a
    // crossed tick is visible downstream (mids are still recomputed either way).
    if (bids[i] > asks[i]) {
      flags[i] = kQFlagLocked | kQFlagCrossed;
    } else if (bids[i] == asks[i]) {
      flags[i] = kQFlagLocked;
    }
  }
  const QuoteBatch batch{
      .contracts = ids,
      .bids = bids,
      .asks = asks,
      .bid_sizes = std::span<const std::int32_t>(sizes),
      .ask_sizes = std::span<const std::int32_t>(sizes),
      .ts_ns = std::span<const std::int64_t>(ts),
      .flags = std::span<const std::uint8_t>(flags),
  };
  if (Status applied = u_.apply_quotes(batch); !applied.has_value()) {
    return applied;
  }
  if (any_valid) {
    ++quote_revision_;
    for (std::size_t expiry = 0u; expiry < touched_expiry.size(); ++expiry) {
      if (touched_expiry[expiry]) {
        ++expiry_quote_revisions_[expiry];
      }
    }
  }
  return Ok();
}

} // namespace atx::vol
