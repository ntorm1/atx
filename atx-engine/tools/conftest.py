"""pytest harness for atx-engine/tools (platform v8 W0-1).

The synthetic fixtures of the field and role builders (test_prepare_research_fields*.py, test_research_fields_*.py,
test_linked_operating_*.py, test_build_fundamental_events.py, ...) are dated through 2024 and were written under the
superseded window research-seal-v1 (seal 2025-01-01). This file is imported before any test module, so binding that
window here keeps their values and refusals exactly as written; the tools copy the seal at import.

The repository window (research-window-v2, seal 2024-01-01) is what every tool binds in production. It is pinned by
test_research_window.py, which checks it, and the tools' refusals under it, in fresh interpreters that never load
this file. atx-impl/tools loads its own instance of research_window.py (engine_tools.py), so a mixed run
``pytest atx-engine/tools atx-impl/tools`` keeps the repository window there.

P9 ruling A1-C: the bind stays through wave 1. Without it 115 tests of 22 legacy-dated modules fail (their fixtures
run Oct-Dec 2024 sessions, 2024 13F quarters, ...; ``pytest --noconftest``), which wave-2 lane T2 re-dates before it
deletes this bind (G-P9).
Lane A1's seal behaviour is pinned under the repository window in fresh interpreters meanwhile
(test_prepare_research_fields_seal.py, test_field_registry.py).
"""
import research_window

research_window.bind(research_window.superseded())
