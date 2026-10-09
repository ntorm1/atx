# NAV determinism reference bytes — `rel-avx2`

Reference `RunArchive` **section bytes** for the SPY-dispersion NAV determinism
gate on the `rel-avx2` preset (Release + `/arch:AVX2`, i.e. the FMA-contraction
route). They exist so the avx2 gate leg can do a true `cmp` against committed
bytes instead of comparing sha256 digests transcribed by hand through a chain of
gate reports.

## Why this directory exists

Every gate since Sprint 3 carried the same finding forward — S3 F-4 -> S4 F-5 ->
S5 F-4 -> S6-T29 -> closeout-sprint audit item **C4**: the `rel` leg compared
real bytes while the `rel-avx2` leg could only compare *digests*, because no
avx2 reference bytes had ever been recorded in the repository. Six sprints of
transcription is the weak link, not the digests themselves. Committing one
`runarchive dump` puts both legs on identical evidentiary footing, which is what
C4 asked for.

## Provenance

| | |
|---|---|
| Corpus | `C:\atx-data\spy-dispersion\runs\parity-full` — 135 sessions, 7 rolls |
| Preset | `rel-avx2` (`CMAKE_CXX_FLAGS` carries `/arch:AVX2`; inherits `rel`, so LTO is on) |
| Commit | `52d51d9` (branch `feat/vol-v1-release`, v1.0.0 release gate) |
| Producer | `atxvol_spy_dispersion_backtest runarchive dump <run> <section> --tsv` |
| `final_nav` | `123243.11724602008` — the avx2 anchor, unchanged since `1be0668` |

The four corpus pins were verified on-pin before the run that produced these
bytes, and the run reproduced every one of the five section digests recorded by
the Sprint-4 gate. These are therefore not a re-pin: they are the long-standing
avx2 values, written down as bytes for the first time.

## What is here, and what is deliberately not

The **five determinism-stable sections**:

| File | sha256 |
|---|---|
| `trade_schedule.tsv` | `5c82b77e10de1ecedf782559d954336a833e7dd9d3cbf5acc57dcebcf99f0652` |
| `projected_schedule.tsv` | `81475bcc3ceed47697a11ab27dddecafb7a080ae552533f2772d4dfce0de8376` |
| `projected_cold.tsv` | `b5a1bbff272ee30ec516e17d03d0c4168262e5db48236ba309efa3d53b59731b` |
| `mark_divergence.tsv` | `c9a04d1bcf0e3c07138e3ba4752c6c7ca762e68dffc5af9f607000cd2fcd6085` |
| `meta.tsv` | `f1bf11e3bc583e0398810e6e12b199f31b9a5ecdb16e789e6df9f223ad2011ac` |

`mark_divergence` and `meta` are byte-identical to the `rel` route: the first is
empty of divergences on this corpus, the second records the invocation, which
does not depend on the ISA. `trade_schedule`, `projected_schedule` and
`projected_cold` are the genuinely ISA-divergent three.

The `diagnostics` section is **excluded on purpose**. It embeds wall-clock
telemetry, so it can never be byte-stable across runs and no gate compares it.
For the same reason the `run.atxrun` container itself is not reference material —
the gate compares *sections*, never the container.

## Reproducing / comparing

`meta.tsv` is sensitive to the invocation string, so the `--out` argument must be
given in its **relative** form or the `meta` comparison will fail for a reason
that has nothing to do with determinism:

```
<exe> build-schedule         --run <run>
<exe> project-schedule       --run <run>
<exe> run-projected-backtest --run <run> --schedule <run>\projected_schedule.tsv \
                             --execution cold --out projected_cold_backtest.tsv
<exe> runarchive dump <run> <section> --tsv > <section>.tsv
```

with `<exe> = build-rel-avx2/bin/atxvol_spy_dispersion_backtest`. Dump under a
shell that does not rewrite line endings — a PowerShell `>` redirect will
CRLF-normalise the stream and every `cmp` will fail spuriously.

The `rel` counterpart of these bytes lives in the release sprint's workspace
(`baseline/sections-1be0668-rel/`), which is not part of the repository; the
`rel` anchor is `123243.11724603444`.
