# Brief: task E-4

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task E-4: report seal check (needs OD-5)

**Files:** `atx-impl/tools/mega_report/data.py:23,49`, `atx-impl/tools/test_mega_report_seal.py`

```python
import re

_HEX = re.compile(r'^(?:fp_|ic1_)?[0-9a-f]{16,64}$')
_NAMED = re.compile(r'(validation|holdout|(?<![A-Za-z])VAL(?![A-Za-z]))')
_YEAR = re.compile(r'(?<!\d)(20\d\d)')

def path_is_sealed(rel_path: str, first_sealed_year: int = 2024) -> bool:
    """True when a path names hidden data. Hash-named parts are ignored; date-shaped parts are not."""
    if _NAMED.search(rel_path):
        return True
    for part in re.split(r'[\\/._-]', rel_path):
        if not part or _HEX.match(part):
            continue
        if any(int(y) >= first_sealed_year for y in _YEAR.findall(part)):
            return True
    return False
```

- [ ] **Step 1:** test `test_seal_regex_hex_vs_date`: `fp_2e2025f0aa11bb22/droe.f64` is read; `nav-2023-2024/x.csv`,
  `x_20250131.csv`, `nav-2026/y.csv`, `VAL/z.csv` are refused; `train-2020-2023-lo1/daily.csv` is read. Paths are taken
  relative to the research output root, so a run-date-stamped sprint folder never enters the check; the test
  `test_seal_paths_are_relative_to_out_root` pins that.
- [ ] **Step 2:** implement; `first_sealed_year` comes from `research_window.py`. **Step 3:** root re-renders the v7 pitch:
  0 unavailable blocks.

