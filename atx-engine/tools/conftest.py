"""pytest harness for atx-engine/tools (platform v8 W0-1; P9 lane T2).

The suite runs under the repository window (research-window-v2, seal 2024-01-01), the window every tool binds in
production; nothing here binds a window globally. P9 lane T2 (ruling A1-C) re-dated the synthetic fixtures that were
written under the superseded window research-seal-v1 (seal 2025-01-01) so every in-window date is before 2024-01-01;
rows that test the seal stay on or after it.

Ruling T2-GOLD: test_linked_operating_v2.py, test_linked_operating_v3.py and test_lo1_delisting.py keep expected
hashes of role-builder bytes produced under research-seal-v1 (the seal text is inside the pinned manifests), and an
expected hash is never edited (rule 4). Those three modules request ``superseded_research_window`` below, module by
module; every other module, and every test after them, sees the repository window. Ruling T2-GOLD-4 applies the same
window to one test of test_prepare_research_fields_sic.py through t2_gold.superseded_window directly.
"""
import pytest

import t2_gold


@pytest.fixture(scope="module")
def superseded_research_window():
    """Ruling T2-GOLD: research-seal-v1 for one module (t2_gold.superseded_window)."""
    with t2_gold.superseded_window():
        yield
