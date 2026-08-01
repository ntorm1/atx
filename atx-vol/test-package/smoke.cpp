// Out-of-tree consumer smoke test for the INSTALLED atx-vol package (plan 5.1).
//
// This translation unit is compiled against an install prefix, never against
// the source tree. It therefore proves four separate things that an in-tree
// test cannot:
//
//   1. the Tier-A one-include public API (`atx/vol/vol.hpp`) is installed and
//      self-contained -- every header it reaches, including the atx-core
//      headers it depends on, made it into the prefix;
//   2. `Result<T>` / `Status` are USABLE out of tree: both the value and the
//      error arm are exercised below, so the vendored tl::expected the aliases
//      resolve to has to be reachable from the installed include tree with NO
//      FetchContent anywhere in this project;
//   3. `atx::vol` links -- the exported target carries whatever link
//      dependencies the static/shared library actually needs;
//   4. the library is deterministic: the driver script runs this binary twice
//      and byte-compares stdout.
//
// Output is fixed-precision on purpose (%.12f): a run-to-run diff has to be a
// real numeric change, not a formatting artefact.

#include <cstdio>
#include <string>

#include "atx/vol/vol.hpp"

namespace {

using atx::vol::Result;
using atx::vol::Side;
using atx::vol::Status;

int failures = 0;

void check(bool ok, const char *what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    ++failures;
  }
}

} // namespace

int main() {
  // -- European primitive: a pure, deterministic kernel ----------------------
  const double F = 100.0;
  const double K = 105.0;
  const double T = 0.5;
  const double sigma = 0.25;
  const double df = 0.98;

  const double call = atx::vol::black76_price(F, K, T, sigma, df, Side::Call);
  std::printf("black76_call    %.12f\n", call);

  // -- Result<T> value arm: round-trip the price back through the inverter ---
  const Result<double> iv = atx::vol::implied_vol(call, F, K, T, df, Side::Call);
  check(iv.has_value(), "implied_vol round-trip returned an error");
  if (iv.has_value()) {
    std::printf("implied_vol     %.12f\n", *iv);
  }

  // -- Result<T> error arm: T <= 0 is documented InvalidArgument. This is what
  //    actually forces tl::expected's error storage, atx::vol::Error and
  //    ErrorCode to be compilable AND linkable out of tree; a header-only
  //    happy-path smoke would never touch the error arm at all.
  const Result<double> bad = atx::vol::implied_vol(call, F, K, -1.0, df, Side::Call);
  check(!bad.has_value(), "implied_vol accepted a non-positive maturity");
  if (!bad.has_value()) {
    std::printf("iv_error        %s\n", bad.error().to_string().c_str());
  }

  // -- American pricer: the library's headline route, through Result<T> ------
  const Result<double> put =
      atx::vol::american_price(100.0, 105.0, T, sigma, 0.04, 0.01, Side::Put);
  check(put.has_value(), "american_price returned an error");
  if (put.has_value()) {
    std::printf("american_put    %.12f\n", *put);
  }

  // -- Status (Result<void>) is nameable and constructible out of tree -------
  const Status ok{};
  check(ok.has_value(), "a default-constructed Status is not a success");

  // The version string proves a COMPILED symbol (not just a header) is linked.
  const std::string version{atx::vol::version()};
  std::printf("atx_vol_version %s\n", version.c_str());

  if (failures != 0) {
    std::printf("SMOKE FAILED (%d)\n", failures);
    return 1;
  }
  std::printf("SMOKE OK\n");
  return 0;
}
