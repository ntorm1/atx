#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "pricing/american_discrete_div_reference.hpp"

namespace atx::vol::test::ddiv_matrix {

// Canonical inputs for the original D0/D1 50-case matrix. Event arithmetic is
// deliberately unchanged: alignment perturbations and iterative quarterly sums
// must reproduce the original binary64 schedule, not a rounded date surrogate.

struct InputCase {
  std::string id;
  std::string group;
  double S = 100.0;
  double K = 100.0;
  double T = 1.0;
  double sigma = 0.25;
  double r = 0.04;
  double q = 0.0;
  Side side = Side::Put;
  ddiv_ref::CashProcess process = ddiv_ref::CashProcess::SpotLiquidator;
  std::vector<CashDividend> divs;
};

[[nodiscard]] inline InputCase make(std::string id, std::string group, double S, double K, double T,
                        double sigma, double r, Side side, std::vector<CashDividend> divs,
                        ddiv_ref::CashProcess process = ddiv_ref::CashProcess::SpotLiquidator) {
  InputCase c;
  c.id = std::move(id);
  c.group = std::move(group);
  c.S = S;
  c.K = K;
  c.T = T;
  c.sigma = sigma;
  c.r = r;
  c.side = side;
  c.process = process;
  c.divs = std::move(divs);
  return c;
}

[[nodiscard]] inline std::vector<CashDividend> quarterly(double first, double amount, double T) {
  std::vector<CashDividend> out;
  for (double t = first; t <= T + 1e-12; t += 0.25) {
    out.push_back({t, amount});
  }
  return out;
}

// The matrix of the lane brief, grouped A-I (doc section 3 explains each group).
[[nodiscard]] inline std::vector<InputCase> matrix() {
  std::vector<InputCase> m;
  constexpr double kSig = 0.25;
  constexpr double kR = 0.04;
  // A: ex-date alignment perturbations. T 0.5, one 2.00 at 0.25 = step 150.5 of
  // the 301 lattice (round-half-even -> 150); +-1 step; +-0.01 step around the
  // rounding boundary.
  {
    const double dt = 0.5 / 301.0;
    const std::array<std::pair<const char *, double>, 5> taus{
        {{"-1step", 0.25 - dt}, {"base", 0.25}, {"+1step", 0.25 + dt},
         {"150.49", 150.49 * dt}, {"150.51", 150.51 * dt}}};
    int i = 1;
    for (const auto &[tag, tau] : taus) {
      m.push_back(make("A" + std::to_string(i) + "p", std::string("A align ") + tag, 100, 100,
                       0.5, kSig, kR, Side::Put, {{tau, 2.0}}));
      m.push_back(make("A" + std::to_string(i) + "c", std::string("A align ") + tag, 100, 95,
                       0.5, kSig, kR, Side::Call, {{tau, 2.0}}));
      ++i;
    }
  }
  // B: dividend on the first step, on the last step before expiry, on expiry.
  {
    const double dt = 1.0 / 301.0;
    const std::array<std::pair<const char *, double>, 3> taus{
        {{"first step", 0.7 * dt}, {"last step", 1.0 - 0.4 * dt}, {"on expiry", 1.0}}};
    int i = 1;
    for (const auto &[tag, tau] : taus) {
      m.push_back(make("B" + std::to_string(i) + "p", std::string("B ") + tag, 100, 100, 1.0,
                       kSig, kR, Side::Put, {{tau, 3.0}}));
      m.push_back(make("B" + std::to_string(i) + "c", std::string("B ") + tag, 100, 95, 1.0,
                       kSig, kR, Side::Call, {{tau, 3.0}}));
      ++i;
    }
  }
  // C: multiple dividends, quarterly 1.00 from 0.10: 2, 4, 8 events.
  for (const double T : {0.5, 1.0, 2.0}) {
    const auto divs = quarterly(0.1, 1.0, T);
    const std::string n = std::to_string(divs.size());
    m.push_back(make("C" + n + "p", "C " + n + " events", 100, 100, T, kSig, kR, Side::Put, divs));
    m.push_back(make("C" + n + "c", "C " + n + " events", 100, 95, T, kSig, kR, Side::Call, divs));
  }
  // D: LEAPS. T 3 with 12 quarterly 1.00; an SPY-scale long chain (the L3f worst
  // contract's shape: S 772.98, T 2.182, sigma 0.35, nine quarterly 1.8311).
  {
    const auto divs = quarterly(0.1, 1.0, 3.0);
    m.push_back(make("D12p", "D LEAPS 3y", 100, 100, 3.0, kSig, kR, Side::Put, divs));
    m.push_back(make("D12c", "D LEAPS 3y", 100, 95, 3.0, kSig, kR, Side::Call, divs));
    const auto spy = quarterly(0.15, 1.8311, 2.182);
    m.push_back(make("DSPYp", "D LEAPS SPY", 772.98, 700, 2.182, 0.35, 0.047, Side::Put, spy));
    m.push_back(make("DSPYc", "D LEAPS SPY", 772.98, 858, 2.182, 0.35, 0.047, Side::Call, spy));
  }
  // E: large dividend / spot: 1 %, 5 %, 20 % at mid-life; and the review-m1
  // contract (dev200: S 6.375, K 5, cash 4.10 = 64 % of spot, 5.9 days out).
  for (const double D : {1.0, 5.0, 20.0}) {
    const std::string tag = std::to_string(static_cast<int>(D));
    m.push_back(make("E" + tag + "p", "E D/S " + tag + "%", 100, 100, 1.0, kSig, kR, Side::Put,
                     {{0.5, D}}));
    m.push_back(make("E" + tag + "c", "E D/S " + tag + "%", 100, 95, 1.0, kSig, kR, Side::Call,
                     {{0.5, D}}));
  }
  {
    const std::vector<CashDividend> m1{{0.016272795852273703, 4.0999999046325684}};
    m.push_back(make("E64p", "E D/S 64% (review m1)", 6.375, 5.0, 0.34715297818183899, 0.2,
                     0.046780478209257126, Side::Put, m1));
    m.push_back(make("E64c", "E D/S 64% (review m1)", 6.375, 5.0, 0.34715297818183899, 0.2,
                     0.046780478209257126, Side::Call, m1));
  }
  // F: exercise immediately before the jump (deep in the money).
  m.push_back(make("F1c", "F ITM pre-jump", 100, 70, 0.5, kSig, kR, Side::Call, {{0.45, 5.0}}));
  m.push_back(make("F1p", "F ITM pre-jump", 100, 130, 0.5, kSig, kR, Side::Put, {{0.45, 5.0}}));
  m.push_back(make("F2c", "F ITM pre-jump", 100, 80, 0.25, kSig, kR, Side::Call, {{0.24, 4.0}}));
  m.push_back(make("F2p", "F ITM pre-jump", 100, 125, 0.25, kSig, kR, Side::Put, {{0.24, 4.0}}));
  // G: r = 0.
  m.push_back(make("G1p", "G r=0", 100, 100, 1.0, kSig, 0.0, Side::Put, {{0.5, 3.0}}));
  m.push_back(make("G1c", "G r=0", 100, 95, 1.0, kSig, 0.0, Side::Call, {{0.5, 3.0}}));
  m.push_back(make("G4p", "G r=0", 100, 100, 1.0, kSig, 0.0, Side::Put, quarterly(0.1, 1.0, 1.0)));
  m.push_back(make("G4c", "G r=0", 100, 95, 1.0, kSig, 0.0, Side::Call, quarterly(0.1, 1.0, 1.0)));
  // H: the escrowed convention, against the Escrowed* lattices.
  m.push_back(make("H1p", "H escrowed", 100, 100, 1.0, kSig, kR, Side::Put, {{0.5, 3.0}},
                   ddiv_ref::CashProcess::Escrowed));
  m.push_back(make("H1c", "H escrowed", 100, 95, 1.0, kSig, kR, Side::Call, {{0.5, 3.0}},
                   ddiv_ref::CashProcess::Escrowed));
  m.push_back(make("H4p", "H escrowed", 100, 100, 1.0, kSig, kR, Side::Put,
                   quarterly(0.1, 1.0, 1.0), ddiv_ref::CashProcess::Escrowed));
  m.push_back(make("H4c", "H escrowed", 100, 95, 1.0, kSig, kR, Side::Call,
                   quarterly(0.1, 1.0, 1.0), ddiv_ref::CashProcess::Escrowed));
  // I: cash that can exceed spot: liquidator (the kernel's) vs survivor.
  for (const ddiv_ref::CashProcess process : {ddiv_ref::CashProcess::SpotLiquidator,
                                             ddiv_ref::CashProcess::SpotSurvivor}) {
    const char *tag = (process == ddiv_ref::CashProcess::SpotLiquidator) ? "L" : "S";
    m.push_back(make(std::string("I") + tag + "p",
                     std::string("I cash>spot ") + atx::vol::ddiv_ref::to_string(process), 10, 8,
                     0.5, 0.8, 0.03, Side::Put, {{0.25, 6.0}}, process));
    m.push_back(make(std::string("I") + tag + "c",
                     std::string("I cash>spot ") + atx::vol::ddiv_ref::to_string(process), 10, 8,
                     0.5, 0.8, 0.03, Side::Call, {{0.25, 6.0}}, process));
  }
  return m;
}

// Stable FNV-1a fingerprint, with integers/double bits encoded least-significant
// byte first, length-prefixed IDs and schedules, then ordered model fields.
// This is an accidental-drift check, not a cryptographic evidence hash.
template <class Cases>
[[nodiscard]] std::uint64_t input_fingerprint(const Cases &cases) {
  std::uint64_t hash = 14695981039346656037ULL;
  const auto byte = [&hash](std::uint8_t value) {
    hash = (hash ^ value) * 1099511628211ULL;
  };
  const auto word = [&byte](std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      byte(static_cast<std::uint8_t>(value & 255U));
      value >>= 8;
    }
  };
  word(cases.size());
  for (const auto &c : cases) {
    word(c.id.size());
    for (const char ch : c.id) {
      byte(static_cast<std::uint8_t>(ch));
    }
    for (const double value : {c.S, c.K, c.T, c.sigma, c.r, c.q}) {
      word(std::bit_cast<std::uint64_t>(value));
    }
    word(static_cast<std::uint64_t>(c.side));
    word(static_cast<std::uint64_t>(c.process));
    word(c.divs.size());
    for (const auto &div : c.divs) {
      word(std::bit_cast<std::uint64_t>(div.tau));
      word(std::bit_cast<std::uint64_t>(div.amount));
    }
  }
  return hash;
}

} // namespace atx::vol::test::ddiv_matrix
