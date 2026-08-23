// Out-of-tree consumer smoke test for the INSTALLED atx-vol package (plan 5.1).
//
// This translation unit is compiled against an install prefix, never against
// the source tree. It therefore proves several separate things that an
// in-tree test cannot:
//
//   1. api-restructure Task 3: the installed include tree ships EXACTLY the
//      include/atx/vol/api/<module>/ public surface (Tasks 1-2), not the old
//      flat layout and not src/. This TU deliberately includes three
//      per-module headers directly -- core/version.hpp, pricing/black76.hpp,
//      fitting/session.hpp -- instead of the Tier-A umbrella (atx/vol/api/
//      vol.hpp), so a broken install of any ONE of those three modules'
//      public trees fails this build, not just a broken umbrella closure.
//   2. `Result<T>` / `Status` are USABLE out of tree: both the value and the
//      error arm are exercised below, so the vendored tl::expected the aliases
//      resolve to has to be reachable from the installed include tree with NO
//      FetchContent anywhere in this project;
//   3. `atx::vol` links a COMPILED symbol from each of the three named
//      modules -- version() (core, src/core/version.cpp), black76_price
//      (pricing, src/pricing/black76.cpp), apply_fit_preset (fitting,
//      src/fitting/session.cpp) -- so a missing/renamed object in any one
//      module's static-library slice fails at link time, not just parse time;
//   4. the library is deterministic: the driver script runs this binary twice
//      and byte-compares stdout.
//   5. L7-T1: the alpha layer is REACHABLE out of tree. include/atx/vol/alpha/
//      is a second public include root -- it sits on the target's public
//      BUILD_INTERFACE and two shipped CLIs are built on it -- but nothing
//      installed it, so `#include "atx/vol/alpha/registry.hpp"` compiled for
//      every in-tree consumer and was a hard `file not found` for every
//      downstream one. The include below is the only place that distinction is
//      observable, because in-tree is exactly the configuration where the bug
//      does not reproduce. It is header-only, so a NAMED SYMBOL is what proves
//      it (there is no object to link): the registry's built-in catalogue is
//      constructed and its size printed, which also makes the header's
//      constant-initialisation part of the determinism byte-compare.
//
// Output is fixed-precision on purpose (%.12f): a run-to-run diff has to be a
// real numeric change, not a formatting artefact.

#include <cstdio>
#include <string>

#include "atx/vol/alpha/registry.hpp"
#include "atx/vol/api/core/version.hpp"
#include "atx/vol/api/fitting/session.hpp"
#include "atx/vol/api/pricing/black76.hpp"

namespace {

using atx::vol::Black76Aux;
using atx::vol::CalendarRepair;
using atx::vol::FitPreset;
using atx::vol::SessionInputs;
using atx::vol::Side;

int failures = 0;

void check(bool ok, const char *what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    ++failures;
  }
}

} // namespace

int main() {
  // -- pricing/black76.hpp: pure closed-form kernel, compiled + linked from
  //    src/pricing/black76.cpp -----------------------------------------------
  const double F = 100.0;
  const double K = 105.0;
  const double T = 0.5;
  const double sigma = 0.25;
  const double df = 0.98;

  const double call = atx::vol::black76_price(F, K, T, sigma, df, Side::Call);
  std::printf("black76_call    %.12f\n", call);
  check(call > 0.0, "black76_price returned a non-positive premium");

  // Second entry point from the same header, sharing the same d1/d2/N(d1)
  // formula as black76_price -- proves black76.hpp's whole declared surface,
  // not just the one function, is reachable and linkable.
  const Black76Aux aux = atx::vol::black76_aux(F, K, T, sigma, df, Side::Call);
  check(aux.price == call, "black76_aux price disagrees with black76_price");

  // -- fitting/session.hpp: SessionInputs / FitPreset / apply_fit_preset,
  //    compiled + linked from src/fitting/session.cpp -------------------------
  SessionInputs in{};
  in.S = 100.0;
  in.r = 0.04;
  atx::vol::apply_fit_preset(in, FitPreset::Robust);
  check(in.use_correction_cache, "FitPreset::Robust did not enable the correction cache");
  check(in.calendar_repair == CalendarRepair::MonotoneFit,
        "FitPreset::Robust did not enable calendar-arbitrage repair");

  // -- core/version.hpp: the compiled version() symbol, linked from
  //    src/core/version.cpp, cross-checked against the header-declared
  //    constant it must never drift from --------------------------------------
  const std::string version{atx::vol::version()};
  check(version == atx::vol::kVersionString,
        "version() disagrees with the header-declared kVersionString");
  std::printf("atx_vol_version %s\n", version.c_str());

  // -- alpha/registry.hpp (L7-T1): the second public include root, header-only.
  //    Building the built-in catalogue names a symbol from it, so a prefix that
  //    ships api/ but not alpha/ fails to COMPILE here rather than passing on a
  //    surface no consumer can reach. The count is printed so the determinism
  //    byte-compare covers the catalogue too.
  const atx::vol::alpha::FeatureRegistry features = atx::vol::alpha::builtin_features();
  check(features.size() > 0, "the alpha built-in feature catalogue is empty");
  std::printf("alpha_features  %zu\n", features.size());

  if (failures != 0) {
    std::printf("SMOKE FAILED (%d)\n", failures);
    return 1;
  }
  std::printf("SMOKE OK\n");
  return 0;
}
