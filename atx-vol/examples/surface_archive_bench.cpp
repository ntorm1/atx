// surface_archive_bench.cpp — deserialization throughput for the ATXVSA2 priced
// curve-surface archive.
//
// The archive is a HOT-PATH artifact: a quoting desk memory-maps a multi-symbol
// archive and, per incoming request, resolves ONE symbol to a fully-usable priced
// surface. This bench measures that path end to end:
//
//   * open()               — one-shot framing + header/metadata CRC validation.
//   * reconstruct_symbol() — the hot single-surface path: a hash probe + one record
//                     parse, then direct curve construction into an OWNED
//                     PricedSurface. Reported as ns/surface and surfaces/sec, plus
//                     the effective MB/s (record bytes / time).
//   * reconstruct_all()    — rebuild every surface (bulk warm-load).
//
// Owned reconstruct, not the zero-copy `map_*` views: this bench measures the cost
// of materializing a priced surface, which is what it measured on v1.
//
// Data-free: it fabricates a realistic multi-symbol book (parsimonious eSSVI
// single-name surfaces + dense 40-node convex index surfaces) so it always runs.
// Accuracy is proven elsewhere (the SPY OPRA round-trip test); this is purely a
// throughput proof.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "atx/vol/priced_surface.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

using namespace atx::vol;

namespace {

double now_ns() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::nano>(clock::now().time_since_epoch()).count();
}

// A parsimonious eSSVI surface (single-name shape): `n` ascending-T slices.
PricedSurface make_essvi_surface(std::uint32_t uid, int n) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.12 * static_cast<double>(i);
    const double F = 100.0;
    EssviParams e{};
    e.theta = 0.04 + 0.006 * static_cast<double>(i);
    e.phi = 1.4 - 0.04 * static_cast<double>(i);
    e.rho = -0.35 + 0.015 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = F;
    cs.push(std::make_unique<EssviCurve>(e, /*df=*/1.0));
    ctx.push_back(SliceContext{T, F, 0.0, 0.02, 200, 5});
  }
  PricingContext pc;
  pc.S = 100.0;
  pc.r = 0.043;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = AlOpts{};
  pc.uid = uid;
  return PricedSurface::create(std::move(cs), std::move(ctx), pc).value();
}

// A dense convex index surface (SPY shape): `n` slices, each `nodes` convex nodes.
PricedSurface make_convex_surface(std::uint32_t uid, int n, int nodes) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.12 * static_cast<double>(i);
    const double F = 100.0;
    const double df = 1.0;
    ConvexSliceFit fit;
    fit.T = T;
    fit.F = F;
    fit.df = df;
    fit.rmse_price = 0.3;
    fit.n_obs = static_cast<std::size_t>(nodes);
    fit.n_active = 4;
    fit.u.resize(static_cast<std::size_t>(nodes));
    fit.C.resize(static_cast<std::size_t>(nodes));
    // A convex, decreasing synthetic call-price curve (size-realistic; pricing
    // validity is irrelevant to deserialization timing).
    for (int j = 0; j < nodes; ++j) {
      const double K = F * (0.6 + 0.8 * static_cast<double>(j) / static_cast<double>(nodes - 1));
      fit.u[static_cast<std::size_t>(j)] = K;
      const double intrinsic = df * (F > K ? (F - K) : 0.0);
      fit.C[static_cast<std::size_t>(j)] = intrinsic + 2.0;
    }
    cs.push(std::make_unique<ConvexDenseCurve>(std::move(fit)));
    ctx.push_back(SliceContext{T, F, 0.0, 0.02, static_cast<std::size_t>(nodes), 3});
  }
  PricingContext pc;
  pc.S = 100.0;
  pc.r = 0.043;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = AlOpts{};
  pc.uid = uid;
  return PricedSurface::create(std::move(cs), std::move(ctx), pc).value();
}

}  // namespace

int main() {
  constexpr int kSurfaces = 512;
  constexpr int kIters = 200;  // repetitions for the timed loops

  // Build a mixed multi-symbol book: half parsimonious eSSVI, half dense convex.
  std::vector<PricedSurface> surfaces;
  std::vector<std::string> names;
  surfaces.reserve(kSurfaces);
  names.reserve(kSurfaces);
  for (int i = 0; i < kSurfaces; ++i) {
    char buf[16] = {};
    std::snprintf(buf, sizeof buf, "SYM%05d", i);
    names.emplace_back(buf);
    if ((i & 1) == 0) {
      surfaces.push_back(make_essvi_surface(static_cast<std::uint32_t>(i + 1), 6));
    } else {
      surfaces.push_back(make_convex_surface(static_cast<std::uint32_t>(i + 1), 6, 40));
    }
  }

  std::vector<SurfaceArchiveItem> items;
  items.reserve(kSurfaces);
  for (int i = 0; i < kSurfaces; ++i) {
    items.push_back(SurfaceArchiveItem{names[static_cast<std::size_t>(i)],
                                       &surfaces[static_cast<std::size_t>(i)]});
  }

  auto built = write_surface_archive_v2(items);
  if (!built) {
    std::printf("write failed: %s\n", built.error().to_string().c_str());
    return 1;
  }
  const std::size_t archive_bytes = built->size();

  // Time open() (framing + header/metadata CRC).
  const double t_open0 = now_ns();
  auto opened = SurfaceArchiveV2::open(std::move(*built));
  const double t_open1 = now_ns();
  if (!opened) {
    std::printf("open failed: %s\n", opened.error().to_string().c_str());
    return 1;
  }
  const SurfaceArchiveV2 archive = std::move(*opened);

  std::printf("== ATXVSA2 archive ==\n");
  std::printf("surfaces           : %d  (eSSVI + dense-convex mix)\n", kSurfaces);
  std::printf("archive size       : %.2f MB\n", static_cast<double>(archive_bytes) / (1024.0 * 1024.0));
  std::printf("open()             : %.1f us\n", (t_open1 - t_open0) / 1000.0);

  // Bench 1: single-symbol random access (the hot path).
  std::uint64_t sink = 0;
  const double t1a = now_ns();
  for (int it = 0; it < kIters; ++it) {
    for (int i = 0; i < kSurfaces; ++i) {
      // Deterministic pseudo-random probe order (coprime stride).
      const int idx = static_cast<int>((static_cast<std::uint64_t>(i) * 2654435761ull +
                                        static_cast<std::uint64_t>(it) * 40503ull) %
                                       kSurfaces);
      auto ps = archive.reconstruct_symbol(names[static_cast<std::size_t>(idx)]);
      if (ps) {
        sink += ps->n_slices();
      }
    }
  }
  const double t1b = now_ns();
  const double n_map = static_cast<double>(kIters) * kSurfaces;
  const double ns_per = (t1b - t1a) / n_map;
  std::printf("\nreconstruct_symbol (random single-surface access):\n");
  std::printf("  ns / surface     : %.1f\n", ns_per);
  std::printf("  surfaces / sec   : %.2f M\n", 1000.0 / ns_per);

  // Bench 2: reconstruct_all (bulk warm-load).
  const double t2a = now_ns();
  std::uint64_t sink2 = 0;
  for (int it = 0; it < kIters; ++it) {
    auto all = archive.reconstruct_all();
    if (all) {
      sink2 += all->size();
    }
  }
  const double t2b = now_ns();
  const double ns_per_all = (t2b - t2a) / (static_cast<double>(kIters) * kSurfaces);
  // Honest rate: bytes actually parsed/reconstructed (sum of record sizes), not the
  // file size (the inter-record alignment gaps and the page-padded tail are never read).
  std::size_t reconstructed_bytes = 0;
  for (const ArchiveV2DirEntry& de : archive.directory()) {
    reconstructed_bytes += static_cast<std::size_t>(de.surface_size);
  }
  const double recon_mb = static_cast<double>(reconstructed_bytes) / (1024.0 * 1024.0);
  const double mbps = recon_mb / ((t2b - t2a) / 1e9 / static_cast<double>(kIters));
  std::printf("\nreconstruct_all (rebuild every surface):\n");
  std::printf("  ns / surface     : %.1f\n", ns_per_all);
  std::printf("  record bytes/pass: %.2f MB (vs %.2f MB file)\n", recon_mb,
              static_cast<double>(archive_bytes) / (1024.0 * 1024.0));
  std::printf("  effective rate   : %.0f MB/s\n", mbps);

  std::printf("\n[sink %llu %llu]\n", static_cast<unsigned long long>(sink),
              static_cast<unsigned long long>(sink2));
  return 0;
}
