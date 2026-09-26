# D0 remaining scoped include checks — prepared, not run

Source inspected through `520bc0ccceb34387fe493f0c4add73937b168f54`; subsequent `7be279288e1f47e43c68594626e061b46fe9cb9c` changes only the streaming corpus fixture. This preparation used existing compile commands, native `-t commands`/`-t deps`, source reads, and wrapper `-DryRun`; no configure, compiler, test or CMake edit occurred.

The pool-4 `equity-hygiene` cache remains Debug, PCH OFF, with `C:/atx-wt/pool-4/deps/equity-hygiene`. Every candidate production object below has exactly **one** native compiler action and no PCH, worker, library or test dependency. Receipt: `build-equity-hygiene/w0-vwap-remaining-hygiene-plan.json`.

| Header or changed definition | Smallest useful production consumer | Evidence/next action |
|---|---|---|
| `data/real_panel.hpp`, `data/history_panel.hpp`, `data/adapt_panel.hpp`, `alpha/vwap_rule.hpp` | `atx-engine/src/data/real_panel.cpp` | Existing PCH-off PASS reusable: source and all 34 recorded tracked first-party dependencies are unchanged since compile source `95828c6e`. Receipt `w0-vwap-real-panel-reuse.json`; do not recompile solely for later fixture/docs commits. |
| `data/context.hpp`, new policy state and move forwarding | `atx-engine/src/data/context.cpp` | **Minimal extra standalone check: one action.** Ordinary engine production uses PCH. |
| `alpha/augment.hpp`, `alpha/datafields.hpp`, `config.hpp`, local `MineArgs` | `atx-impl/src/stage_equity_mine.cpp` | Ordinary `equity-dev` impl-core command already has no PCH. Reuse root's exact-source successful owning build once it completes; no duplicate large-TU compile needed. |
| `config.hpp` and CLI parser implementation | `atx-impl/src/config.cpp` | Ordinary impl-core command has no PCH; reuse current root build when green. |
| `stage_discover_detail.hpp`, capacity configuration, resume fingerprint | `stage_discover.cpp`, `store_progress_sink.cpp` | Both ordinary impl-core commands have no PCH; reuse root current build when green. |
| `alpha/wq101_battery.hpp` | WQ101 benchmark TU | Benchmark fixtures have only explicit legacy-policy pins. Benchmark targets are OFF in this hygiene cache; no hygiene coverage claimed and no configure expansion proposed. Owning alpha oracle covers input bits/runtime, not standalone header hygiene. |
| `atx-impl/tests/alpha101_support.hpp` | Test-only helper | Runtime fixture qualification; excluded from production hygiene scope. |

From `C:/atx-wt/pool-4`, after the parent grants a compiler slot, the minimal remaining command is:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/atx-build.ps1 -Preset equity-hygiene -Jobs 1 check atx-engine/src/data/context.cpp
```

If root cannot supply current successful no-PCH impl-core build evidence, the fallback checks below each resolve to one action. Issue separately, never as an uncapped combined native invocation:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/atx-build.ps1 -Preset equity-hygiene -Jobs 1 check atx-impl/src/stage_equity_mine.cpp
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/atx-build.ps1 -Preset equity-hygiene -Jobs 1 check atx-impl/src/config.cpp
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/atx-build.ps1 -Preset equity-hygiene -Jobs 1 check atx-impl/src/stage_discover.cpp
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/atx-build.ps1 -Preset equity-hygiene -Jobs 1 check atx-impl/src/store_progress_sink.cpp
```

Optional direct adapter/history/stage-panel body checks also have one action apiece (`atx-engine/src/data/adapt_panel.cpp`, `atx-engine/src/data/history_panel.cpp`, `atx-impl/src/stage_panel.cpp`), but add no distinct changed-header coverage beyond the consumers above. They are not required merely because the files changed. Successful scoped checks establish production include evidence, not independent self-containment of every header or full-tree hygiene.

The bounded follow-up fixture scan found no further missing raw declaration in owning data/book/impl default augmentation paths: direct adapter fixtures already declare their constructed Raw basis; history fixtures carry raw_close; impl shared synthetic helpers and both mining generators now carry raw_close. DataContext fixtures use unchanged empty-window lowering except the new explicit Raw/V1 and intentional Unknown-rejection case. The delayed-dataset test intentionally errors before augmentation. Optional externally supplied panel fixtures keep strict basis validation; no data was opened. The separately committed streaming corpus fix copies its generated close column, including holes, without changing RNG or universe values.

## Executed context check and integrated-header boundary

The parent later granted the one-object slot after its four-target build completed. At pool-4 source `e00ef0a9c6002edbeef49259e63645d4808ddc93`, the exact `context.cpp` command above compiled one C++ object with PCH OFF, `-j 1` and the unchanged isolated hygiene cache. Native output includes the normal glob check and one compile action, with no diagnostic, link, worker or test. An immediate exact wrapper repeat reported `ninja: no work to do.` and returned authoritative **exit 0**. No configuration, group/layout or PCH setting changed.

The first monitored launch completed in 13.675 seconds, but PowerShell's `Start-Process` object returned a null `ExitCode` property after exit; the receipt preserves that limitation rather than substituting zero. Successful object publication, VALID Ninja dependencies and the direct no-op repeat establish the completed check. The first run launched with zero compiler/Ninja processes, 2.651 GiB available physical memory and 4.827 GiB commit headroom. Sampled minima were 1.672/3.802 GiB respectively; sampled compiler working-set maximum was 0.949 GiB (not a guaranteed instantaneous peak). After completion the readings were 2.760/4.788 GiB. The slot was released promptly before the parent's runtime gates.

Receipts under `build-equity-hygiene/`: `w0-vwap-context-hygiene-closure.log`, `w0-vwap-context-hygiene.log`, `w0-vwap-context-hygiene.stderr.log` (empty), `w0-vwap-context-hygiene-repeat.log`, `w0-vwap-context-hygiene-receipt.json`, `w0-vwap-context-hygiene-deps.log` and `w0-vwap-context-hygiene-parity.json`.

**Exact-source boundary:** Ninja records 347 dependencies. Comparing the TU plus its tracked first-party dependencies gives 84 files against root candidate `fcbcc9d1a351ccd339687389b118aca65e2812b5`: 83 match, including every D0-owned context/adapter/rule file. The transitive `alpha/vm.hpp` differs (root blob `03500e726cdf1ac33954466baaab19c65f35b84d`, pool-4 blob `2cef18f6bc7b1e8d1195e55fe2e3bdb7499f9ed7`). This check therefore proves the scoped pool-4 source, **not** full integrated-header parity. Root was notified; no unowned VM change or extra compilation was performed. If the integrated gate requires this exact transitive revision, import the reviewed VM change and repeat this one-object check in a separately coordinated slot. The earlier `real_panel.cpp` reuse proof and root's ordinary no-PCH impl-core evidence remain separately attributed.
