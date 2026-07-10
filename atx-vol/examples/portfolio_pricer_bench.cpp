// portfolio_pricer_bench.cpp — throughput of the PricedSurface-native portfolio
// pricer + Taylor PnL-explain.
//
// The pricer is a HOT-PATH artifact: a book of positions over many underlyings is
// repriced against a moving set of surfaces (or a base/shifted pair for PnL).
// This bench measures:
//
//   * the raw kernel floor  — a single-thread loop of PricedSurface::greeks over
//     the unique contracts (the SOTA cold Andersen-Lake Greeks solve);
//   * price()               — the same work with dedup + scatter + aggregation,
//     across {1,2,4,8,hw} threads (overhead headroom vs the floor, thread scaling,
//     and thread-count determinism of the portfolio total);
//   * pnl_explain()         — base Greeks + target reprice + Taylor decomposition.
//
// Data-free: it fabricates a mixed book (dense-convex index surfaces + eSSVI
// single-name surfaces) so it always runs. Accuracy is proven in the tests.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "atx/vol/black76.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

using namespace atx::vol;

namespace {

double now_ns() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::nano>(clock::now().time_since_epoch()).count();
}

PricingContext pc_of(std::uint32_t uid) {
  PricingContext pc;
  pc.S = 100.0;
  pc.r = 0.043;
  pc.now_ts_ns = 1700000000000000000LL;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  return pc;
}

PricedSurface make_essvi(std::uint32_t uid, int n) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.12 * static_cast<double>(i);
    EssviParams e{};
    e.theta = 0.04 + 0.006 * static_cast<double>(i);
    e.phi = 1.4 - 0.04 * static_cast<double>(i);
    e.rho = -0.35 + 0.015 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = 100.0;
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-0.043 * T)));
    ctx.push_back(SliceContext{T, 100.0, 0.0, 0.02, 200, 5});
  }
  return PricedSurface::create(std::move(cs), std::move(ctx), pc_of(uid)).value();
}

PricedSurface make_convex(std::uint32_t uid, int n, int nodes) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.12 * static_cast<double>(i);
    const double df = std::exp(-0.043 * T);
    const double sigma = 0.18 + 0.01 * static_cast<double>(i);
    ConvexSliceFit fit;
    fit.T = T;
    fit.F = 100.0;
    fit.df = df;
    fit.rmse_price = 0.3;
    fit.n_obs = static_cast<std::size_t>(nodes);
    fit.n_active = 4;
    fit.u.resize(static_cast<std::size_t>(nodes));
    fit.C.resize(static_cast<std::size_t>(nodes));
    for (int j = 0; j < nodes; ++j) {
      const double K =
          100.0 * (0.7 + 0.6 * static_cast<double>(j) / static_cast<double>(nodes - 1));
      fit.u[static_cast<std::size_t>(j)] = K;
      fit.C[static_cast<std::size_t>(j)] = black76_price(100.0, K, T, sigma, df, Side::Call);
    }
    cs.push(std::make_unique<ConvexDenseCurve>(std::move(fit)));
    ctx.push_back(SliceContext{T, 100.0, 0.0, 0.02, static_cast<std::size_t>(nodes), 3});
  }
  return PricedSurface::create(std::move(cs), std::move(ctx), pc_of(uid)).value();
}

} // namespace

int main(int argc, char **argv) {
  constexpr int kUnderlyings = 64;
  constexpr int kSlices = 6;

  // Build one surface per underlying (mixed kinds), and a bumped base/shifted
  // pair for the PnL path.
  std::vector<PricedSurface> surfs;
  std::vector<const PricedSurface *> ptrs;
  surfs.reserve(kUnderlyings);
  for (int u = 1; u <= kUnderlyings; ++u) {
    surfs.push_back((u & 1) ? make_convex(static_cast<std::uint32_t>(u), kSlices, 40)
                            : make_essvi(static_cast<std::uint32_t>(u), kSlices));
  }
  for (const PricedSurface &s : surfs) {
    ptrs.push_back(&s);
  }
  auto surfaces = SurfaceSet::create(ptrs).value();

  // A shifted set (same curves, spot +0.4%, rate +10bp) for PnL-explain.
  std::vector<PricedSurface> shifts;
  std::vector<const PricedSurface *> shift_ptrs;
  shifts.reserve(kUnderlyings);
  for (const PricedSurface &s : surfs) {
    CurveSurface c = s.surface().clone();
    std::vector<SliceContext> ctx(s.context().begin(), s.context().end());
    PricingContext pc = s.pricing();
    pc.S += 0.004 * pc.S;
    pc.r += 0.001;
    shifts.push_back(PricedSurface::create(std::move(c), std::move(ctx), pc).value());
  }
  for (const PricedSurface &s : shifts) {
    shift_ptrs.push_back(&s);
  }
  auto shifted = SurfaceSet::create(shift_ptrs).value();

  // Book: several strikes/expiries per underlying, both sides.
  std::vector<Position> book;
  std::uint64_t id = 0;
  for (int u = 1; u <= kUnderlyings; ++u) {
    for (int i = 0; i < kSlices; ++i) {
      const double T = 0.05 + 0.12 * static_cast<double>(i);
      for (double K : {85.0, 92.0, 98.0, 100.0, 102.0, 108.0, 115.0}) {
        const Side side = (K <= 100.0) ? Side::Put : Side::Call;
        book.push_back({id++, {static_cast<std::uint32_t>(u), K, T, side}, 5.0, 100.0});
      }
    }
  }
  const std::size_t base_positions = book.size();
  const std::size_t target_positions =
      argc > 1 ? static_cast<std::size_t>(std::max(1, std::atoi(argv[1]))) : base_positions;
  if (target_positions > book.size()) {
    const std::vector<Position> seed = book;
    book.reserve(target_positions);
    while (book.size() < target_positions) {
      Position p = seed[book.size() % seed.size()];
      p.id = id++;
      book.push_back(p);
    }
  } else if (target_positions < book.size()) {
    book.resize(target_positions);
  }
  const double build_t0 = now_ns();
  auto pf =
      Portfolio::create(book, PortfolioBuildOptions{.expected_unique_contracts = base_positions})
          .value();
  const double build_ms = (now_ns() - build_t0) / 1.0e6;
  const std::size_t n_pos = pf.n_positions();
  const std::size_t n_ctr = pf.n_contracts();
  const PortfolioPricer pricer(std::move(pf));

  std::printf("== PortfolioPricer bench ==\n");
  std::printf("underlyings        : %d\n", kUnderlyings);
  std::printf("positions          : %zu\n", n_pos);
  std::printf("unique contracts   : %zu\n", n_ctr);
  std::printf("portfolio build    : %.2f ms\n", build_ms);

  // Kernel floor: single-thread loop of the SAME per-contract work price() does
  // (the SOTA Andersen-Lake fair_value solve + the analytic B76 Greeks). This is
  // the raw pricing cost; price() single-thread should sit right on top of it.
  {
    const auto contracts = pricer.portfolio().contracts();
    std::uint64_t sink = 0;
    const double t0 = now_ns();
    for (const OptionContract &c : contracts) {
      const PricedSurface *s = surfaces.find(c.uid);
      auto fv = s->fair_value(c.K, c.T, c.side);
      auto g = s->greeks(c.K, c.T, c.side);
      if (fv && g) {
        sink += (*fv > 0.0 && g->price > 0.0) ? 1u : 0u;
      }
    }
    const double t1 = now_ns();
    const double us = (t1 - t0) / 1000.0;
    std::printf("\nkernel floor (fair_value + Greeks loop, 1 thread):\n");
    std::printf("  %.1f us  |  %.0f contracts/sec  |  %.3f us/contract  [sink %llu]\n", us,
                static_cast<double>(n_ctr) / (us / 1e6), us / static_cast<double>(n_ctr),
                static_cast<unsigned long long>(sink));
  }

  // price() across thread counts + determinism check.
  std::printf("\nprice() (dedup + Greeks fan-out + scatter):\n");
  double ref_total = 0.0;
  for (unsigned nt : {1u, 2u, 4u, 8u, 0u}) {
    const double t0 = now_ns();
    auto fr = pricer.price(surfaces, PriceOptions{nt});
    const double t1 = now_ns();
    const double us = (t1 - t0) / 1000.0;
    const char *label = (nt == 0u) ? "hw" : nullptr;
    char lb[8];
    if (label == nullptr) {
      std::snprintf(lb, sizeof lb, "%u", nt);
      label = lb;
    }
    if (nt == 1u) {
      ref_total = fr->total.pv;
    }
    const bool det = std::fabs(fr->total.pv - ref_total) == 0.0;
    std::printf("  threads=%-2s : %7.1f us | %10.0f contracts/sec | %9.0f positions/sec | "
                "total.pv=%.3f %s\n",
                label, us, static_cast<double>(n_ctr) / (us / 1e6),
                static_cast<double>(n_pos) / (us / 1e6), fr->total.pv,
                det ? "[det ok]" : "[DET MISMATCH]");
  }

  std::printf("\nprice() quote refresh (IV + American mark, no Greeks):\n");
  for (unsigned nt : {1u, 4u, 8u, 0u}) {
    const double t0 = now_ns();
    auto fr = pricer.price(surfaces, PriceOptions{.n_threads = nt, .prices_only = true});
    const double t1 = now_ns();
    const double us = (t1 - t0) / 1000.0;
    char lb[8];
    if (nt == 0u) {
      std::snprintf(lb, sizeof lb, "hw");
    } else {
      std::snprintf(lb, sizeof lb, "%u", nt);
    }
    std::printf("  threads=%-2s : %7.1f us | %10.0f contracts/sec | %9.0f positions/sec | "
                "total.pv=%.3f\n",
                lb, us, static_cast<double>(n_ctr) / (us / 1e6),
                static_cast<double>(n_pos) / (us / 1e6), fr->total.pv);
  }

  // pnl_explain() throughput.
  std::printf("\npnl_explain() (base Greeks + target reprice + Taylor decomp):\n");
  for (unsigned nt : {1u, 4u, 0u}) {
    const double t0 = now_ns();
    auto er = pricer.pnl_explain(surfaces, shifted, PriceOptions{nt});
    const double t1 = now_ns();
    const double us = (t1 - t0) / 1000.0;
    char lb[8];
    if (nt == 0u) {
      std::snprintf(lb, sizeof lb, "hw");
    } else {
      std::snprintf(lb, sizeof lb, "%u", nt);
    }
    std::printf("  threads=%-2s : %7.1f us | %10.0f contracts/sec | total pnl=%.2f\n", lb, us,
                static_cast<double>(n_ctr) / (us / 1e6), er->total.pnl_total);
  }

  // pnl_explain_into() (in-place, warm workspace) + pnl_totals() (no scatter/frame).
  // The in-place win over the returning pnl_explain() is the parallel scatter, the
  // separated fixed-order reduction, and the reused frame/solve scratch (no 141 B/pos
  // frame + no ContractPnl vector allocated per call).
  {
    std::vector<std::uint64_t> id_c(n_pos);
    std::vector<std::uint32_t> uid_c(n_pos);
    std::vector<double> c0(n_pos), c1(n_pos), c2(n_pos), c3(n_pos), c4(n_pos), c5(n_pos), c6(n_pos),
        c7(n_pos), c8(n_pos), c9(n_pos), c10(n_pos), c11(n_pos), c12(n_pos), c13(n_pos), c14(n_pos),
        c15(n_pos);
    std::vector<PriceStatus> stat(n_pos);
    PnlTotals tot{};
    PnlFrameView view{id_c, uid_c, c0,  c1,  c2,  c3,  c4,   c5,   c6, c7,
                      c8,   c9,    c10, c11, c12, c13, c14,  c15,  stat, &tot};
    PortfolioWorkspace ws;
    ws.reserve(n_ctr, n_pos);
    (void)pricer.pnl_explain_into(surfaces, shifted, view, ws); // warm the substrate + scratch

    std::printf("\npnl_explain_into() (in-place, warm ws; parallel scatter + separated reduce):\n");
    std::printf("  dedup ratio        : %.1f positions : 1 unique\n",
                static_cast<double>(n_pos) / static_cast<double>(n_ctr));
    for (unsigned nt : {1u, 4u, 0u}) {
      const double t0 = now_ns();
      (void)pricer.pnl_explain_into(surfaces, shifted, view, ws, PriceOptions{nt});
      const double t1 = now_ns();
      const double us = (t1 - t0) / 1000.0;
      char lb[8];
      if (nt == 0u) {
        std::snprintf(lb, sizeof lb, "hw");
      } else {
        std::snprintf(lb, sizeof lb, "%u", nt);
      }
      std::printf("  threads=%-2s : %7.1f us | %10.0f contracts/sec | %9.0f positions/sec | "
                  "total pnl=%.2f\n",
                  lb, us, static_cast<double>(n_ctr) / (us / 1e6),
                  static_cast<double>(n_pos) / (us / 1e6), tot.pnl_total);
    }

    std::printf("\npnl_totals() (no scatter, no frame):\n");
    for (unsigned nt : {1u, 4u, 0u}) {
      const double t0 = now_ns();
      auto tr = pricer.pnl_totals(surfaces, shifted, ws, PriceOptions{nt});
      const double t1 = now_ns();
      const double us = (t1 - t0) / 1000.0;
      char lb[8];
      if (nt == 0u) {
        std::snprintf(lb, sizeof lb, "hw");
      } else {
        std::snprintf(lb, sizeof lb, "%u", nt);
      }
      std::printf("  threads=%-2s : %7.1f us | %10.0f contracts/sec | %9.0f positions/sec | "
                  "total pnl=%.2f\n",
                  lb, us, static_cast<double>(n_ctr) / (us / 1e6),
                  static_cast<double>(n_pos) / (us / 1e6), tr->pnl_total);
    }
  }
  return 0;
}
