# Optimized IC: completed TRAIN, frozen validation pending

Clean source/configured provenance: `0f618a45b3bd256eddd3a96f64893746063818d9`.
Shared VM workers source `c3942160` -> `c60ee179`, fixtures `dcf67f1c` ->
`d5d38ea4`; scoped compiler change `0f618a45`. Review `28c12720` -> `b2449fcb`.

The focused build passed in **31.631s, Jobs3**. Generated commands changed only
`ic_screen.cpp`, `strategy_ic_runner.cpp` and `strategy_ic_composition.cpp` to
`/O2 /Ob2 /clang:-finline`, with source-local PCH exceptions. Debug CRT, global
PCH, precise FP defaults, ISA and dependency builds stayed unchanged. All
**13 focused native cases passed in 1.618s** (guard 1.844s).

The four-worker real run completed all **48 TRAIN candidates and one combined
evaluation in 131.666s**. It then completed 13 validation candidates and started
a fourteenth before the overall 180s cap stopped it at180.218s. The process
peaked at896,258,048 bytes RSS (855 MiB). The overall summary remains `running`
because termination was external; only its completed TRAIN role is qualified.

For the same first ten completed TRAIN candidates, elapsed time fell from
84.772s to24.090s: an observed **3.52x improvement**. All62,376 comparable daily
IC values were numerically identical. One truncated CSV row from the deliberately
stopped baseline was excluded and explicitly counted; the first audit failed
on that truncated row before the corrected audit completed in1.078s.

TRAIN combined rank IC is0.03499/0.04993/0.05983 at5/21/63 sessions respectively;
overlap-aware standard errors are0.01424/0.02464/0.04887. Signs were fitted on
TRAIN, so these are training diagnostics. They are not portfolio Sharpe or
out-of-sample evidence. Fixed48 weights, all candidates and missing-neutral
semantics remain unchanged.

TRAIN planned target change averages **35.32% per calendar month**, including
initial deployment; maximum month80.08%. Mean planned gross is0.9012, maximum
absolute net0.0645 and mean component contribution coverage0.8980. These expose
remaining turnover/construction work; they are not actual fills or $1bn capacity.

Saved TRAIN orientations:
`build-equity/recent-fast-ic-v2/orientations.json`, SHA256
`5106fdc13fc5347c9c2c670714c134a1978e6b7a0d2d2b5dd79bacb7dfb782d6`.
Next run resumes validation from this exact artifact, with no TRAIN refit.
Parallel date-row IC and its shared-pool caller are the next core optimization.

Exact evidence: `fast-ic-optimized-20260926/index.json`, 31 copied artifacts,
14,396,882 bytes, plus the generated three-command audit. Daily series, partial
validation receipts and both audit attempts are retained. The earlier exact
packet/report remains unchanged. 2025+ is reserved; corporate-action expansion
is deferred. No costed Sharpe >=1 or turnover/capacity goal is claimed achieved.
