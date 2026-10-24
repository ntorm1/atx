// Included only from the original reference TU with the research option enabled.
// This keeps its exact model/grid/jump helpers local and avoids a copied model.
namespace research {
namespace {

struct BatchLane {
  Model model;
  LogGrid grid;
  Operator op;
  std::vector<int> steps;
  PdeResult result;
  bool prepared = false;
  bool fresh = false;
  std::array<std::uint64_t, 3> diagnostic_key{};
  std::size_t diagnostic_max_run = 0;
  bool diagnostic_have_key = false;
};

struct Group {
  std::array<std::size_t, 4> index{};
  std::size_t count = 0;
};

struct GroupState {
  ThomasPack pack;
  std::vector<Quad> value, level;
  std::vector<double> jump_values, jump_scratch;
  std::array<BatchLane *, 4> lane{}; // borrows stable prepared storage
  std::array<PdeDiagnostics *, 4> diagnostic{};
  std::array<Result<PdeResult> *, 4> output{};
  BatchDiagnostics *work = nullptr;
  Sweep sweep = nullptr;
  std::size_t count = 0;
  std::uint8_t live = 0;
};

constexpr std::uint8_t lane_bit(std::size_t lane) noexcept {
  return static_cast<std::uint8_t>(1u << lane);
}

Status prepare_lane(BatchLane &lane, const Problem &p, const PdeGrid &grid) {
  if (Status st = validate(p); !st) return st;
  if (Status st = validate_pde_grid(grid); !st) return st;
  lane.model = make_model(p);
  const auto log_grid = make_log_grid(lane.model, p, grid);
  if (!log_grid) return Err(log_grid.error());
  lane.grid = *log_grid;
  lane.op = make_operator(p.sigma, p.r, p.q, lane.grid.h);
  if (!std::isfinite(lane.op.a) || !std::isfinite(lane.op.b) ||
      !std::isfinite(lane.op.c) || lane.op.a < 0.0 || lane.op.c < 0.0) {
    return Err(ErrorCode::OutOfRange,
        "discrete-dividend reference PDE: centered stencil is nonfinite or nonmonotone");
  }
  lane.steps = segment_steps(lane.model.events, p.T, grid.time_base, grid.time_level);
  lane.prepared = true;
  return Ok();
}

bool same_calendar(const BatchLane &a, const BatchLane &b) noexcept {
  if (a.grid.n != b.grid.n || a.model.american != b.model.american ||
      std::bit_cast<std::uint64_t>(a.model.T) != std::bit_cast<std::uint64_t>(b.model.T) ||
      a.steps != b.steps || a.model.events.interior.size() != b.model.events.interior.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.model.events.interior.size(); ++i) {
    if (std::bit_cast<std::uint64_t>(a.model.events.interior[i].t) !=
        std::bit_cast<std::uint64_t>(b.model.events.interior[i].t)) return false;
  }
  return true;
}

std::vector<Group> group_lanes(const std::vector<BatchLane> &lanes) {
  std::vector<Group> groups;
  for (std::size_t i = 0; i < lanes.size(); ++i) {
    if (!lanes[i].prepared) continue;
    auto match = groups.end();
    for (auto it = groups.begin(); it != groups.end(); ++it) {
      if (it->count < 4 && same_calendar(lanes[it->index[0]], lanes[i])) {
        match = it;
        break;
      }
    }
    if (match == groups.end()) groups.push_back({{i, 0, 0, 0}, 1});
    else match->index[match->count++] = i;
  }
  return groups;
}

void initialize_group(GroupState &s, const Group &group, std::vector<BatchLane> &lanes,
                      std::vector<Result<PdeResult>> &output) {
  s.count = group.count;
  s.live = static_cast<std::uint8_t>((1u << group.count) - 1u);
  const auto nodes = static_cast<std::size_t>(lanes[group.index[0]].grid.n) + 1u;
  s.value.resize(nodes); s.level.resize(nodes);
  s.pack.rhs.resize(nodes); s.pack.obstacle.resize(nodes); s.pack.active.resize(nodes);
  s.pack.cp.resize(nodes); s.pack.dp.resize(nodes); s.pack.work.resize(nodes);
  s.jump_values.resize(nodes); s.jump_scratch.resize(nodes);
  for (std::size_t l = 0; l < group.count; ++l) {
    auto &lane = lanes[group.index[l]];
    s.lane[l] = &lane;
    s.output[l] = &output[group.index[l]];
    if (s.work != nullptr) {
      s.diagnostic[l] = &s.work->lanes[group.index[l]];
      s.diagnostic[l]->continuation_run_lengths.resize(nodes, 0);
    }
    const auto kinks = log_kinks_of(lane.model);
    for (std::size_t i = 0; i < nodes; ++i) {
      s.level[i][l] = std::exp(lane.grid.x_min + lane.grid.h * static_cast<double>(i));
      s.value[i][l] = cell_average(lane.model, kinks,
          lane.grid.x_min + lane.grid.h * static_cast<double>(i), lane.grid.h);
    }
  }
}

void refuse_lane(GroupState &s, std::size_t l, atx::core::Error error) {
  *s.output[l] = Err(std::move(error));
  s.live &= static_cast<std::uint8_t>(~lane_bit(l));
}

void record_sweep(GroupState &s, std::size_t l) {
  auto *diagnostic = s.diagnostic[l];
  if (diagnostic == nullptr) return;
  auto &d = *diagnostic;
  auto &lane = *s.lane[l];
  const auto &row = s.pack.rows[l];
  ++d.thomas_sweeps;
  auto &step = d.coefficient_steps.back();
  ++step.thomas_sweeps;
  d.forward_rows += static_cast<std::uint64_t>(lane.grid.n);
  const std::array key{std::bit_cast<std::uint64_t>(row.lo),
      std::bit_cast<std::uint64_t>(row.di), std::bit_cast<std::uint64_t>(row.up)};
  if (!lane.diagnostic_have_key || key != lane.diagnostic_key) {
    lane.diagnostic_key = key;
    lane.diagnostic_have_key = true;
    lane.diagnostic_max_run = 0;
  }
  std::size_t run = 0;
  for (std::size_t i = 1; i < s.value.size(); ++i) {
    const bool identity = i + 1 == s.value.size() || s.pack.active[i][l] != 0;
    if (!identity) {
      ++d.continuation_rows; ++step.continuation_rows; ++run;
      continue;
    }
    ++d.identity_rows;
    if (run != 0) {
      ++d.continuation_runs; ++d.continuation_run_lengths[run];
      step.max_run_length = std::max(step.max_run_length, static_cast<std::uint64_t>(run));
      if (run > lane.diagnostic_max_run) {
        d.exact_key_coefficient_rows += run - lane.diagnostic_max_run;
        lane.diagnostic_max_run = run;
      }
      run = 0;
    }
  }
}

void run_sweep(GroupState &s, std::uint8_t mask) {
  mask &= s.live;
  if (mask == 0) return;
  if (s.work != nullptr) {
    const auto active = static_cast<std::uint64_t>(std::popcount(mask));
    ++s.work->vector_sweeps;
    s.work->active_lane_sweeps += active;
    s.work->tail_slots += 4u - s.count;
    s.work->retired_slots += s.count - active;
    for (std::size_t l = 0; l < s.count; ++l) {
      if ((mask & lane_bit(l)) != 0) record_sweep(s, l);
    }
    for (std::size_t i = 1; i + 1 < s.value.size(); ++i) {
      unsigned on = 0;
      for (std::size_t l = 0; l < s.count; ++l) {
        if ((mask & lane_bit(l)) != 0 && s.pack.active[i][l] != 0) ++on;
      }
      if (on != 0 && on != active) ++s.work->mask_disagreement_rows;
    }
  }
  const auto status = s.sweep(s.pack, mask);
  for (std::size_t l = 0; l < s.count; ++l) {
    if ((mask & lane_bit(l)) != 0 && status[l] != LaneStatus::Success) {
      refuse_lane(s, l, atx::core::Error(ErrorCode::Internal,
          "cash PDE batch research: Thomas callback refused lane"));
    }
  }
}

void commit_step(GroupState &s, std::size_t l) {
  // A converged lane swaps alone. Other lanes keep their current iterate while
  // Howard continues; a vector-wide swap would overwrite retired solutions.
  for (std::size_t i = 0; i < s.value.size(); ++i) {
    std::swap(s.value[i][l], s.pack.work[i][l]);
  }
}

struct PolicyCheck {
  double worst = 0.;
  int worst_row = 0;
  int flips = 0;
};

PolicyCheck choose_policy(GroupState &s, std::size_t l) {
  const auto &r = s.pack.rows[l];
  PolicyCheck check;
  for (std::size_t i = 1; i + 1 < s.value.size(); ++i) {
    const double w = s.pack.work[i][l];
    const double pde_res = r.lo * s.pack.work[i - 1][l] + r.di * w +
        r.up * s.pack.work[i + 1][l] - s.pack.rhs[i][l];
    const double obs_res = w - s.pack.obstacle[i][l];
    const bool on_obstacle = s.pack.active[i][l] != 0;
    const bool want = on_obstacle ? !(pde_res < obs_res) : (obs_res < pde_res);
    if (want != on_obstacle) {
      s.pack.active[i][l] = want ? 1u : 0u;
      ++check.flips;
    }
    const double row_scale = std::abs(r.lo * s.pack.work[i - 1][l]) + std::abs(r.di * w) +
        std::abs(r.up * s.pack.work[i + 1][l]) + std::abs(s.pack.rhs[i][l]) +
        std::abs(s.pack.obstacle[i][l]) + 1.0;
    const double violation = on_obstacle ? -pde_res : -obs_res;
    if (violation / row_scale > check.worst) {
      check.worst = violation / row_scale;
      check.worst_row = static_cast<int>(i);
    }
  }
  return check;
}

void howard(GroupState &s, double t_new) {
  std::uint8_t pending = s.live;
  std::array<PolicyCheck, 4> last{};
  for (int iter = 1; iter <= kMaxPolicyIterations && pending != 0; ++iter) {
    run_sweep(s, pending);
    pending &= s.live;
    for (std::size_t l = 0; l < s.count; ++l) {
      if ((pending & lane_bit(l)) == 0) continue;
      last[l] = choose_policy(s, l);
      const auto &check = last[l];
      const bool solves_lcp = check.worst <= kLcpRoundingUlps * kEps;
      const bool rounding_tie = iter >= kLcpTieIteration && check.worst <= kLcpTieUlps * kEps;
      if (check.flips != 0 && !solves_lcp && !rounding_tie) continue;
      auto &result = s.lane[l]->result;
      if (iter > result.max_policy_iterations) {
        result.max_policy_iterations = iter;
        result.max_policy_time = t_new;
      }
      if (check.flips != 0 && !solves_lcp) ++result.lcp_rounding_ties;
      result.max_lcp_violation = std::max(result.max_lcp_violation, check.worst);
      commit_step(s, l);
      pending &= static_cast<std::uint8_t>(~lane_bit(l));
    }
  }
  for (std::size_t l = 0; l < s.count; ++l) {
    if ((pending & lane_bit(l)) == 0) continue;
    std::array<char, 256> why{};
    const auto &check = last[l];
    std::snprintf(why.data(), why.size(),
        "discrete-dividend reference PDE: policy iteration did not settle within its "
        "cap (t = %.9g; last iteration: %d rows switched, worst relative LCP violation "
        "%.3e at node %d of %d, state %.6g)", t_new, check.flips, check.worst,
        check.worst_row, s.lane[l]->grid.n,
        s.level[static_cast<std::size_t>(check.worst_row)][l]);
    refuse_lane(s, l, atx::core::Error(ErrorCode::Internal, std::string(why.data())));
  }
}

void theta_batch(GroupState &s, double dt, double theta, int seg, double t_new) {
  const auto n = s.value.size() - 1u;
  const double ex = (1.0 - theta) * dt;
  std::uint8_t seeds = 0;
  for (std::size_t l = 0; l < s.count; ++l) {
    if ((s.live & lane_bit(l)) == 0) continue;
    auto &lane = *s.lane[l];
    const auto &m = lane.model;
    for (std::size_t i = 1; i < n; ++i) {
      s.pack.rhs[i][l] = s.value[i][l] + ex * (lane.op.a * s.value[i - 1][l] +
          lane.op.b * s.value[i][l] + lane.op.c * s.value[i + 1][l]);
    }
    const Rows rows{-theta * dt * lane.op.a, 1.0 - theta * dt * lane.op.b,
        -theta * dt * lane.op.c, m.far_low(seg, t_new, s.level[0][l]),
        m.far_high(seg, t_new, s.level[n][l])};
    if (!std::isfinite(rows.lo) || !std::isfinite(rows.di) || !std::isfinite(rows.up) ||
        rows.lo > 0.0 || rows.up > 0.0 ||
        !(rows.di > std::abs(rows.lo) + std::abs(rows.up))) {
      refuse_lane(s, l, atx::core::Error(ErrorCode::OutOfRange,
          "discrete-dividend reference PDE: implicit rows outside supported M-matrix domain"));
      continue;
    }
    s.pack.rows[l] = rows;
    if (s.diagnostic[l] != nullptr) {
      s.diagnostic[l]->coefficient_steps.push_back(
          {{std::bit_cast<std::uint64_t>(rows.lo), std::bit_cast<std::uint64_t>(rows.di),
            std::bit_cast<std::uint64_t>(rows.up)}, 0, 0, 0});
    }
    ++lane.result.time_steps;
    if (!m.american) continue;
    const double shift = m.exercise_shift(seg, t_new);
    for (std::size_t i = 0; i <= n; ++i) {
      s.pack.obstacle[i][l] = payoff(m.side, s.level[i][l] + shift, m.K);
      s.pack.active[i][l] = 0;
    }
    if (lane.fresh) {
      seeds |= lane_bit(l);
      if (s.diagnostic[l] != nullptr) ++s.diagnostic[l]->fresh_seed_sweeps;
    }
  }
  if (!s.lane[0]->model.american) {
    run_sweep(s, s.live);
    for (std::size_t l = 0; l < s.count; ++l) {
      if ((s.live & lane_bit(l)) != 0) commit_step(s, l);
    }
    return;
  }
  run_sweep(s, seeds);
  for (std::size_t l = 0; l < s.count; ++l) {
    if ((s.live & lane_bit(l)) == 0) continue;
    const auto &guess = s.lane[l]->fresh ? s.pack.work : s.value;
    for (std::size_t i = 0; i <= n; ++i) {
      s.pack.active[i][l] =
          (s.pack.obstacle[i][l] > 0.0 && guess[i][l] <= s.pack.obstacle[i][l]) ? 1u : 0u;
    }
    s.lane[l]->fresh = false;
  }
  howard(s, t_new);
}

void run_group(GroupState &s, const PdeGrid &grid) {
  const auto &anchor = *s.lane[0]; // immutable calendar survives a lane refusal
  const int segments = static_cast<int>(anchor.steps.size());
  for (int seg = segments - 1; seg >= 0 && s.live != 0; --seg) {
    const auto j = static_cast<std::size_t>(seg);
    const double t_start = seg == 0 ? 0.0 : anchor.model.events.interior[j - 1u].t;
    const double t_end = seg + 1 == segments ? anchor.model.T : anchor.model.events.interior[j].t;
    double t_old = t_end;
    for (std::size_t l = 0; l < s.count; ++l) s.lane[l]->fresh = true;
    for (int k = 0; k < anchor.steps[j] && s.live != 0; ++k) {
      const double t_new = segment_time(t_start, t_end, anchor.steps[j], k + 1,
          grid.time_grading);
      const double dt = t_old - t_new;
      if (k < grid.rannacher_steps) {
        const double t_mid = t_old - 0.5 * dt;
        theta_batch(s, 0.5 * dt, 1.0, seg, t_mid);
        if (s.live != 0) theta_batch(s, t_mid - t_new, 1.0, seg, t_new);
      } else {
        theta_batch(s, dt, 0.5, seg, t_new);
      }
      t_old = t_new;
    }
    if (seg == 0) continue;
    for (std::size_t l = 0; l < s.count; ++l) {
      if ((s.live & lane_bit(l)) == 0) continue;
      const auto &lane = *s.lane[l];
      for (std::size_t i = 0; i < s.value.size(); ++i) s.jump_values[i] = s.value[i][l];
      jump_at(lane.model, seg, t_start, s.jump_values, s.jump_scratch,
          lane.grid.x_min, lane.grid.h);
      for (std::size_t i = 0; i < s.value.size(); ++i) s.value[i][l] = s.jump_values[i];
    }
  }
  for (std::size_t l = 0; l < s.count; ++l) {
    if ((s.live & lane_bit(l)) == 0) continue;
    const auto &lane = *s.lane[l];
    const auto i0 = static_cast<std::size_t>(lane.grid.i0);
    const double h = lane.grid.h;
    const double vx = (s.value[i0 + 1u][l] - s.value[i0 - 1u][l]) / (2.0 * h);
    const double vxx = (s.value[i0 + 1u][l] - 2.0 * s.value[i0][l] +
        s.value[i0 - 1u][l]) / (h * h);
    auto out = lane.result;
    out.price = s.value[i0][l];
    out.delta = vx / lane.model.state0;
    out.gamma = (vxx - vx) / (lane.model.state0 * lane.model.state0);
    out.space_intervals = lane.grid.n;
    *s.output[l] = Ok(out);
  }
}

} // namespace

std::vector<Result<PdeResult>> pde_price_batch(std::span<const Problem> problems,
    const PdeGrid &grid, Sweep sweep, BatchDiagnostics *diagnostics) {
  if (diagnostics != nullptr) {
    *diagnostics = {};
    diagnostics->lanes.resize(problems.size());
  }
  std::vector<Result<PdeResult>> output(problems.size(),
      Err(ErrorCode::Internal, "cash PDE batch research: incomplete lane"));
  if (sweep == nullptr) {
    for (auto &out : output) {
      out = Err(ErrorCode::InvalidArgument, "cash PDE batch research: null Thomas callback");
    }
    return output;
  }
  std::vector<BatchLane> lanes(problems.size());
  for (std::size_t i = 0; i < problems.size(); ++i) {
    if (Status st = prepare_lane(lanes[i], problems[i], grid); !st) output[i] = Err(st.error());
  }
  const auto groups = group_lanes(lanes);
  if (diagnostics != nullptr) diagnostics->bundles = groups.size();
  for (const auto &group : groups) {
    GroupState state;
    state.work = diagnostics;
    state.sweep = sweep;
    initialize_group(state, group, lanes, output);
    run_group(state, grid);
  }
  return output;
}

} // namespace research
