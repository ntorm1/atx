#include "fitting/term_prior.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::Ok;

[[nodiscard]] bool positive(double value) noexcept { return std::isfinite(value) && value > 0.0; }

[[nodiscard]] bool valid_node(const TermPriorNode &node) noexcept {
  if (!positive(node.variance_years) || !positive(node.atm_vol) || !positive(node.weight)) {
    return false;
  }
  return std::all_of(node.multiples.begin(), node.multiples.end(),
                     [](double value) { return positive(value); });
}

} // namespace

Result<TermPrior> TermPrior::create(std::span<const TermPriorNode> nodes,
                                    const TermPriorConfig &config) {
  if (!positive(config.decay_log_time)) {
    return Err(ErrorCode::InvalidArgument, "term prior: decay must be finite and positive");
  }
  if (nodes.size() > kTermPriorMaxNodes) {
    return Err(ErrorCode::InvalidArgument, "term prior: too many expiry nodes");
  }
  TermPrior out;
  out.config_ = config;
  out.nodes_.assign(nodes.begin(), nodes.end());
  for (const TermPriorNode &node : out.nodes_) {
    if (!valid_node(node)) {
      return Err(ErrorCode::InvalidArgument, "term prior: invalid node");
    }
  }
  std::sort(out.nodes_.begin(), out.nodes_.end(),
            [](const TermPriorNode &a, const TermPriorNode &b) {
              return a.variance_years < b.variance_years;
            });
  for (std::size_t i = 1; i < out.nodes_.size(); ++i) {
    if (!(out.nodes_[i].variance_years > out.nodes_[i - 1].variance_years)) {
      return Err(ErrorCode::InvalidArgument, "term prior: duplicate node time");
    }
  }
  return Ok(std::move(out));
}

std::optional<TermPriorPoint> TermPrior::at(double variance_years,
                                            std::optional<std::uint64_t> exclude) const noexcept {
  if (!positive(variance_years) || nodes_.empty()) {
    return std::nullopt;
  }
  std::array<std::size_t, kTermPriorMaxNodes> keep{};
  std::size_t count = 0;
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    if (!exclude || nodes_[i].id != *exclude) {
      keep[count++] = i;
    }
  }
  if (count == 0) {
    return std::nullopt;
  }
  // Weighted pool-adjacent-violators: the least-squares non-decreasing fit of
  // ATM total variance in time. Each merge removes one block, so the inner loop
  // is bounded by the node count overall.
  std::array<double, kTermPriorMaxNodes> block_theta{};
  std::array<double, kTermPriorMaxNodes> block_weight{};
  std::array<std::size_t, kTermPriorMaxNodes> block_size{};
  std::size_t blocks = 0;
  for (std::size_t j = 0; j < count; ++j) {
    const TermPriorNode &node = nodes_[keep[j]];
    block_theta[blocks] = node.atm_vol * node.atm_vol * node.variance_years;
    block_weight[blocks] = node.weight;
    block_size[blocks] = 1;
    ++blocks;
    while (blocks >= 2 && block_theta[blocks - 2] > block_theta[blocks - 1]) {
      const double weight = block_weight[blocks - 2] + block_weight[blocks - 1];
      block_theta[blocks - 2] = (block_theta[blocks - 2] * block_weight[blocks - 2] +
                                 block_theta[blocks - 1] * block_weight[blocks - 1]) /
                                weight;
      block_weight[blocks - 2] = weight;
      block_size[blocks - 2] += block_size[blocks - 1];
      --blocks;
    }
  }
  std::array<double, kTermPriorMaxNodes> theta{};
  std::size_t position = 0;
  for (std::size_t b = 0; b < blocks; ++b) {
    for (std::size_t c = 0; c < block_size[b]; ++c) {
      theta[position++] = block_theta[b];
    }
  }
  const auto time = [&](std::size_t j) { return nodes_[keep[j]].variance_years; };
  std::size_t upper = 0;
  while (upper < count && time(upper) < variance_years) {
    ++upper;
  }
  TermPriorPoint point;
  point.nodes_used = count;
  double nearest = 0.0;
  if (upper == 0 || upper == count) {
    const std::size_t end = upper == 0 ? 0 : count - 1;
    point.atm_vol = std::sqrt(theta[end] / time(end));
    point.multiples = nodes_[keep[end]].multiples;
    nearest = std::abs(std::log(variance_years / time(end)));
  } else {
    const std::size_t lower = upper - 1;
    const double fraction = (variance_years - time(lower)) / (time(upper) - time(lower));
    const double total = std::lerp(theta[lower], theta[upper], fraction);
    point.atm_vol = std::sqrt(total / variance_years);
    const auto &low = nodes_[keep[lower]].multiples;
    const auto &high = nodes_[keep[upper]].multiples;
    for (std::size_t g = 0; g < kTermPriorGridSize; ++g) {
      point.multiples[g] = std::lerp(low[g], high[g], fraction);
    }
    nearest = std::min(std::abs(std::log(variance_years / time(lower))),
                       std::abs(std::log(variance_years / time(upper))));
  }
  point.confidence = std::exp(-nearest / config_.decay_log_time);
  if (!positive(point.atm_vol) || !positive(point.confidence)) {
    return std::nullopt;
  }
  return point;
}

} // namespace atx::vol
