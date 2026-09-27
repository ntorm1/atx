# Independent exposure/residual and filing-clock runtime audit

2026-09-26. **Approved as bounded runtime evidence**: reports `52f5cbf180835e94c61fdd9594b4a19640a69ce7` and `63e172942cbe21b9ad342745a834bd254e5f4902`. Read-only audit of pool2 artifacts at observed root `0bfac41e2e07f56327254d116fe07fbf352da293`; no configure, build, test rerun or payload access. The companion `exposure-filing-runtime-audit.json` records each expected/actual hash, exact cases, receipts, source differences and action counts.

All 33 non-executable bindings across the two packets match their committed receipts, including both qualification indexes, XMLs, test/build logs and receipts. Exact source/target/filter/exit/time/binary-hash receipt objects match. Every C++ XML testcase is `run/completed`, without failure, error or skip; case-name sets exactly match the published lists. The Python logs contain all 19 named successful cases and matching `Ran`/`OK` summaries; PowerShell's wrapping of native stderr is formatting, not a failing unittest result.

| Packet | Independently verified result | Native seconds |
|---|---:|---:|
| ExposurePanel | 6/6 | 0.010 |
| FactoryResidualObjective | 6/6 | 5.746 |
| D3 decoder + affected identity/fundamental consumers | 25/25 | 0.004 |
| New acceptance-clock exporter | 10/10 | 0.194 |
| Existing exporter | 9/9 | 0.002 |

The I2/A4 initial build failure is preserved and correctly disclosed: exit1 after13.3670732s, missing closing brace in the digest helper. `f4c6ff38` adds exactly that brace. The corrected source `4816bb85` also adds the source-review document, without another production change. Resume passed in21.8520023s; logs show five compiler attempts across four distinct new CPPs and three links, with no PCH/dependency actions. The unchanged repeat has zero actions, Ninja's no-work message and an empty cache summary. Configuration21.252283s and repeat3.750396s are attributed separately; this is no cold-build speedup claim.

D3 compiled/tested source is `08240b13`; the new Python cohort ran at `f0ffc66d`, whose only difference to that source is a review document. Its build passed first attempt in35.1366575s: three CPPs, two links, and no PCH/dependency actions. The log reports17.7s configuration plus0.9s generation, already included in the build wall time. No initial failure is hidden in this packet.

Executable attribution is intentionally historical where a later build replaced the file:

- I2 data hash `b9829f248a45264c93d758b78f341da38b6683bbfa4defa2b93dcfe93b04dea9` is preserved by its original receipt; the file now matches D3's `daa97b4362f1287f460fa0d925e52f5c6b098df17d61b985d17c227da3bb6314`.
- A4 residual-kernel hash `d38430293fe2a62e13d77535d65e9201ad318d692add775dff5305f15f394e3a` is historical; current engine-IC hash is `18d06ad8317afddae241a696f35aacc6e19940b2f958437e0e3a7ea77ad34de4` after the later consumer relink. This audit does not attribute the earlier six executions to the current binary.
- D3's data executable exactly matched its original bound hash at this audit. A later provider build can make that hash historical too without changing this observation.

Index hashes independently match `ef523c3cc985a7731d993de905b69a80773cbcd73dcbdcea2889448756dc5f93` (I2/A4) and `5297d42ef4cae72142b340a15a83a20cd56d8f1ecd6d1ad85de2bd457c22fe71` (D3). These bounded cohorts qualify their declared source slices. They do not establish source-vendor authenticity, real PIT coverage, the I2 persisted stage, class/split-qualified cap, full original A4/D3/I2 gates, screen recall or tradeable alpha. Counts must not be added to older overlapping qualification totals.
