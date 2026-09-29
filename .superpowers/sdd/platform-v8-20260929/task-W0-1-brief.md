# Brief: task W0-1

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task W0-1: one source of truth for the research window; seal at 2024-01-01

**Files:**
- Create: `atx-impl/strategies/research_window.json`
- Create: `atx-engine/include/atx/engine/data/research_window.hpp`
- Create: `atx-engine/tools/research_window.py`
- Create: `atx-engine/tools/test_research_window.py`
- Create: `atx-engine/tests/data_research_window_test.cpp`
- Modify (constants only, each replaced by a read of the new source):
  `atx-engine/src/data/strategy_data.cpp:21,101,130`; `atx-impl/src/strategy_risk_verb.cpp:41,859-864,918,1005`;
  `atx-impl/src/strategy_live.hpp:44-49`, `strategy_live.cpp:343-347`; `atx-impl/tools/fit_composition_weights.py:225,233,409-418`;
  `atx-impl/tools/alpha_report_card.py` (seal docstring and check); `atx-engine/tools/prepare_recent_research.py:70,239,918`;
  `atx-engine/tools/prepare_research_fields.py:660,2560`; `atx-engine/tools/prepare_identity_bridge.py:585-594`;
  `atx-engine/tools/research_fields_sec.py:76`; `atx-engine/tools/research_fields_holdings.py` (seal constant);
  `atx-engine/tools/build_fundamental_events.py` (seal constant).
- Not modified: `atx-impl/src/stage_equity_*.cpp` and `trial_ledger.hpp:124` (old pipeline, not on the research path).

**Interfaces:**
- Produces `research_window.json`:

```json
{
  "schema": "atx.research-window/v2",
  "train_begin": "2020-01-01",
  "train_end_exclusive": "2024-01-01",
  "seal_begin": "2024-01-01",
  "hidden": {
    "read_twice_at_book_level": ["2024-01-01", "2025-01-01"],
    "never_read": ["2025-01-01", null]
  },
  "supersedes": {"schema": "research-seal-v1", "train_end_exclusive": "2023-01-01", "seal_begin": "2025-01-01"},
  "owner_ruling": {"date": "2026-09-29", "text": "expand TRAIN to include 2023; keep 2024+ hidden and out of sample"}
}
```

- Produces `research_window.hpp`:

```cpp
#pragma once
#include <cstdint>
#include <string_view>

namespace atx::engine::data {
// Generated values; the JSON file is the source and a test pins the two together.
inline constexpr std::int64_t kTrainBeginNs = 1'577'836'800'000'000'000LL;        // 2020-01-01T00:00Z
inline constexpr std::int64_t kTrainEndExclusiveNs = 1'704'067'200'000'000'000LL; // 2024-01-01T00:00Z
inline constexpr std::int64_t kSealBeginNs = 1'704'067'200'000'000'000LL;         // 2024-01-01T00:00Z
inline constexpr std::string_view kResearchWindowId = "research-window-v2";
[[nodiscard]] constexpr bool is_sealed(std::int64_t session_ns) noexcept { return session_ns >= kSealBeginNs; }
}  // namespace atx::engine::data
```

- Produces `research_window.py`: `load() -> dict`, `TRAIN_BEGIN_NS`, `TRAIN_END_NS`, `SEAL_NS`, `SEAL_DATE`, `is_sealed(ns: int) -> bool`.

- [ ] **Step 1: write the failing tests.** Python: `test_json_and_module_agree`, `test_seal_refuses_2024` (a role manifest whose
  last session is 2024-01-02 makes `prepare_research_fields`, `fit_composition_weights` and `alpha_report_card` raise
  `ValueError` naming the seal), `test_2023_session_is_train`. gtest: `ResearchWindow.HeaderMatchesJson` (parse the JSON at
  test time, compare three integers), `ResearchWindow.RefusesSealedSession` (`read_strategy_role` on a synthetic role ending
  2024-01-02 returns `InvalidArgument`), `ResearchWindow.Accepts2023`.
- [ ] **Step 2: run them and see them fail** (`pytest atx-engine/tools/test_research_window.py -q`; root builds
  `atx-engine-data-tests` and runs `--gtest_filter=ResearchWindow.*`).
- [ ] **Step 3: add the three new files; replace each listed constant by the shared value.** Error strings name
  `research-window-v2` and the date 2024-01-01.
- [ ] **Step 4: grep gate.** `grep -rn "2023-01-01\|2025-01-01\|1_672_531_200\|1'735'689'600" atx-engine/tools atx-engine/src/data
  atx-impl/src/strategy_*.cpp atx-impl/src/strategy_*.hpp atx-impl/tools` returns only comments that cite history and the
  `supersedes` block.
- [ ] **Step 5: run all Python tests and the C++ target tests; commit** `feat(protocol): research-window-v2, TRAIN 2020-2023, seal 2024-01-01`.

**Acceptance (root):** all tests green; the grep gate is empty; on the existing 3-year role every tool still runs (the role
ends before the new TRAIN end) and the v7.1 cycle outputs are byte-identical except manifest strings that name the window id.

