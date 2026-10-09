# Vendored: Peter Jäckel — "Let's Be Rational" (LBR)

Bench-only vendored copy of Peter Jäckel's *Let's Be Rational* implied-volatility
inverter, used by `bench/iv_shootout_bench.cpp` (`BM_IvShootout_Jaeckel`) as the
**same-host head-to-head reference** for atx-vol's scalar IV inversion. LBR is the
standing SOTA for European IV inversion (~180 ns/op to full machine precision,
P. Jäckel 2013, <http://www.jaeckel.org/LetsBeRational.pdf>).

This exists so the sprint can replace *"we cite Jäckel's published 180 ns"* with a
number measured on **this** machine, next to atx-vol, against the same oracle.

## License — verified permissive, redistribution permitted

See `LICENSE` (verbatim). Jäckel's grant is:

> Copyright © 2013-2014 Peter Jäckel. Permission to use, copy, modify, and
> distribute this software is freely granted, provided that this notice is
> preserved.

That is an MIT-class permissive grant; redistribution is permitted as long as the
notice is preserved. Every vendored source file keeps its original header
unmodified, so the condition is met. `erf_cody.cpp` is W. J. Cody's public-domain
netlib SPECFUN erf/erfc/erfcx (Math. Comp. 1969) f2c-translated within the LBR
distribution — see `LICENSE` for its provenance. No file was edited; all
build-time adaptation (warnings-off, `NOMINMAX`) lives in `CMakeLists.txt`, not in
the sources, precisely to keep provenance and the license notice byte-clean.

## Provenance

- **Upstream:** `https://github.com/vollib/lets_be_rational` (`src/`), the widely
  used verbatim mirror of `www.jaeckel.org/LetsBeRational.7z`.
- **Upstream commit:** `fed5ddf391301ff133d770348cdbe7cc5fc7b054` (2015-04-03).
- **Retrieved:** 2026-07-17 (WS-0 / M2, infra-measure worktree).
- **Files (byte-for-byte, unmodified):**
  `LetsBeRational.cpp`, `normaldistribution.{h,cpp}`, `rationalcubic.{h,cpp}`,
  `erf_cody.cpp`, `importexport.h`.
- **Deliberately NOT vendored:** the SWIG `.i`, Python bindings, Excel `.xll`
  glue, `setup.py`, and `version.h` — none are needed to call the pure-C++
  inversion entry point.

## Public entry point used

```cpp
extern "C" double implied_volatility_from_a_transformed_rational_guess(
    double price, double F, double K, double T, double q); // q=+1 call, -1 put
```

`price` is the **undiscounted** (forward) option value. atx-vol's grid stores the
discounted premium, so the bench passes `price/df`. Jäckel returns σ directly.

## Build

`CMakeLists.txt` compiles these into a small static library `lbr_lets_be_rational`
with warnings suppressed (`-w` / `/w`; upstream itself is built `-w -fpermissive`)
and **without** `atx_warnings`/`-Werror`, `NOMINMAX` defined so the vestigial
`<windows.h>` include cannot leak `min`/`max` macros. Only
`bench/iv_shootout_bench.cpp` links it; it never enters the shipped `atx::vol`
library.
