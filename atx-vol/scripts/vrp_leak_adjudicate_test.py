"""Smoke tests for vrp_leak_adjudicate.py (presence-based leak adjudication).

Self-contained tmp-dir fixtures: a 180-session synthetic vrp_panel_v1 with
two dense symbols (AAA, BBB) plus the bar-holey attack symbol HHH (present
two sessions of every three -- its label-generation axis is SPARSER than
the pooled axis, the fix-2 review's demonstrated-leak shape), and a matching
surface_presence file. Pins that the shipped emitted-axis semantics
adjudicate clean (exit 0), that the reverted pooled-axis semantics are
DETECTED as leaking on the same corpus (exit 1 with determinate
violations), and that panel/presence inconsistency fails closed (exit 2).

Run: python -m pytest atx-vol/scripts/vrp_leak_adjudicate_test.py -q
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from vrp_leak_adjudicate import build_observations, main
from vrp_panel_qa import COLUMNS

_META = "# schema=vrp_panel_v1\n# horizon_days=21\n"
_HEADER = "\t".join(COLUMNS)
_BASE_TS = 1_600_000_000_000_000_000
_DAY_NS = 86_400_000_000_000
_N_SESSIONS = 180
_TAIL = 21


def _date(d: int) -> str:
    return f"2020-{d:03d}"


def _row(symbol: str, d: int, labeled: bool) -> str:
    rv, label = ("0.25", "0.001875") if labeled else ("nan", "nan")
    fields = [
        symbol, _date(d), str(_BASE_TS + d * _DAY_NS), "100.0", "0.2", "0.206", rv, label,
        "-3", "-3", "-3", "0.2", "0.01", "-0.05", "0.001", "0.02", "0", "0.1",
    ]
    return "\t".join(fields)


def _hhh_session(q: int) -> int:
    return 3 * (q // 2) + (q % 2)  # bars: two sessions of every three


def _panel_lines() -> list[str]:
    lines = []
    for d in range(_N_SESSIONS):
        labeled = d + _TAIL < _N_SESSIONS
        lines.append(_row("AAA", d, labeled))
        lines.append(_row("BBB", d, labeled))
    n_bars = 120  # sessions 0..178 with d % 3 != 2
    for q in range(n_bars):
        lines.append(_row("HHH", _hhh_session(q), q + _TAIL < n_bars))
    return lines


def _presence_lines(drop_hhh_date: str | None = None) -> list[str]:
    hhh_dates = {_date(_hhh_session(q)) for q in range(120)}
    lines = []
    for d in range(_N_SESSIONS):
        date = _date(d)
        lines.append(f"KEY {date}")
        lines.append("surface AAA uid=1 slices=10 bytes=100")
        lines.append("surface BBB uid=2 slices=10 bytes=100")
        if date in hhh_dates and date != drop_hhh_date:
            lines.append("surface HHH uid=3 slices=10 bytes=100")
    return lines


class TestAdjudicator(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
        self.panel = self.tmp / "panel.tsv"
        self.panel.write_text(
            _META + _HEADER + "\n" + "\n".join(_panel_lines()) + "\n", encoding="utf-8"
        )
        self.presence = self.tmp / "surface_presence.txt"
        self.presence.write_text("\n".join(_presence_lines()) + "\n", encoding="utf-8")
        self.walk = ["--min-train-sessions", "90", "--test-sessions", "20",
                     "--step-sessions", "20"]

    def _run(self, *extra: str) -> int:
        return main(["--panel", str(self.panel), "--presence", str(self.presence),
                     *self.walk, *extra])

    def test_emitted_axis_adjudicates_clean(self) -> None:
        # The shipped trainer semantics: label_end is the emitted-axis end
        # (an upper bound on the true bar-axis end), span cap 42. Zero rows
        # with true end past their fold's test start.
        self.assertEqual(self._run(), 0)

    def test_pooled_axis_leak_is_detected(self) -> None:
        # The reverted 5c0f9504 semantics on the SAME corpus: pooled t+21
        # understates HHH's bar-holey windows, rows decided within
        # (test_min - 32, test_min - 21) sessions are admitted leaking --
        # the adjudicator must catch them.
        self.assertEqual(self._run("--label-end-axis", "pooled"), 1)

    def test_presence_inconsistency_fails_closed(self) -> None:
        # An emitted HHH row whose date is missing from presence breaks the
        # subset property the structural bound rests on: exit 2, never a
        # silent PASS.
        self.presence.write_text(
            "\n".join(_presence_lines(drop_hhh_date=_date(0))) + "\n", encoding="utf-8"
        )
        self.assertEqual(self._run(), 2)

    def test_nonzero_bad_spot_meta_fails_closed(self) -> None:
        # fix2-review minor (a): the panel builder's bar axis is presence AND
        # finite positive spot (load_vrp_series skips bad-spot sessions), so
        # n_bad_spot > 0 makes presence DENSER than the true bar axis and the
        # presence-derived 21st forward bar comes too early -- an OPTIMISTIC
        # oracle that could miss a real leak. The adjudicator must refuse the
        # corpus (exit 2), never pass it.
        text = self.panel.read_text(encoding="utf-8")
        self.panel.write_text(
            text.replace(_META, _META + "# n_bad_spot=2\n", 1), encoding="utf-8"
        )
        self.assertEqual(self._run(), 2)

    def test_zero_bad_spot_meta_stays_clean(self) -> None:
        # The real-corpus shape (SP100 carries `# n_bad_spot=0`): a zero
        # counter certifies presence == bar axis, adjudication proceeds.
        text = self.panel.read_text(encoding="utf-8")
        self.panel.write_text(
            text.replace(_META, _META + "# n_bad_spot=0\n", 1), encoding="utf-8"
        )
        self.assertEqual(self._run(), 0)

    def test_successor_only_presence_hole_fails_closed(self) -> None:
        # fix2-review minor (b): date 178 is HHH's LAST bar -- a tail row's
        # date, never an admitted decision date, but exactly the successor
        # class that recorded label ends rest on. Dropping it from presence
        # must trip the WIDENED emitted-but-not-present scan (every panel row
        # date >= coverage_start), not silently adjudicate against a thinner
        # bar axis.
        self.presence.write_text(
            "\n".join(_presence_lines(drop_hhh_date=_date(178))) + "\n", encoding="utf-8"
        )
        self.assertEqual(self._run(), 2)

    def test_observation_build_matches_trainer_semantics(self) -> None:
        # Unit pin mirroring the C++ suite: HHH bar q's emitted-axis end is
        # bar q+21 (31/32 pooled sessions out), never pooled t+21.
        from vrp_panel_qa import parse_tsv_file

        _header, rows = parse_tsv_file(self.panel)
        built = build_observations(rows, "emitted", 42)
        hhh = [o for o in built.obs if o.symbol == "HHH"]
        self.assertEqual(len(hhh), 99)
        for o in hhh:
            d = (o.ts - _BASE_TS) // _DAY_NS
            q = 2 * (d // 3) + (d % 3)
            self.assertEqual(o.label_end_date, _date(_hhh_session(q + 21)))
        self.assertEqual(built.n_rejected_span_cap, 0)
        # A 21-session cap rejects every HHH row (spans 31/32 > 21).
        capped = build_observations(rows, "emitted", 21)
        self.assertEqual(capped.n_rejected_span_cap, 99)


if __name__ == "__main__":
    unittest.main()
