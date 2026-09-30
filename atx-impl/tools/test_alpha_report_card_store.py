"""v8 C-1: the report card's per-candidate invariant block is stored under the fitter's store root and reused; only the
correlation block is recomputed. Byte identity against the pre-store card and between hit and recompute (synthetic).

Run: python -m pytest -q -p no:cacheprovider atx-impl/tools/test_alpha_report_card_store.py
"""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import alpha_report_card as arc  # noqa: E402
import fit_composition_weights as fcw  # noqa: E402
import test_alpha_report_card as base  # noqa: E402

PRE_STORE_CARD_BLOB = "75f6e1593463f1466f6c92db1999651f0dbb2229"  # alpha_report_card.py at ef11f462 (v8 base)
IDS = ["a", "b", "c", "d", "e"]


def signals_for(panel, ids=IDS, seed=31):
    rng = np.random.default_rng(seed)
    shape = panel["close"].shape
    return {i: rng.standard_normal(shape) + (0.5 * k) * np.arange(shape[1])[None, :] / shape[1]
            for k, i in enumerate(ids)}


def world(root: Path, panel, signals, admitted, **kw) -> base.CardWorld:
    return base.CardWorld(root, panel, signals, admitted=admitted, ref_horizons=(21,), **kw)


def outputs(out: Path) -> dict:
    return {p.name: p.read_bytes() for p in out.iterdir()}


def counts(err: str) -> str:
    return [x for x in err.splitlines() if x.startswith("card: computed ")][-1]


class CardStore(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        cls.panel = base.make_panel(dates=240, names=330, score_begin=100, seed=12)
        cls.signals = signals_for(cls.panel)
        cls.full = world(cls.root / "full", cls.panel, cls.signals, ["a", "c", "e"])
        code, err = cls.full.run(cls.root / "ref")
        assert code == 0, err
        cls.ref = outputs(cls.root / "ref")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_card_invariant_block_reused(self):
        work = self.root / "fit-work"
        # a smaller library first (v7.0-like): its three candidates are stored
        part = world(self.root / "part", self.panel, {i: self.signals[i] for i in IDS[:3]}, ["a", "c"])
        self.assertEqual(part.train_sha, self.full.train_sha)
        code, err = part.run(self.root / "part-out", ["--work-dir", str(work)])
        self.assertEqual((code, counts(err)), (0, "card: computed 3, reused 0"), err)
        # the full library (v7.1-like): two new invariant blocks, byte-identical cards
        code, err = self.full.run(self.root / "full-out", ["--work-dir", str(work)])
        self.assertEqual((code, counts(err)), (0, "card: computed 2, reused 3"), err)
        self.assertEqual(outputs(self.root / "full-out"), self.ref)
        code, err = self.full.run(self.root / "full-again", ["--work-dir", str(work), "--workers", "1"])
        self.assertEqual((code, counts(err)), (0, "card: computed 0, reused 5"), err)
        self.assertEqual(outputs(self.root / "full-again"), self.ref)
        self.assertEqual(sorted(p.name for p in work.iterdir()), [f"{self.full.train_sha[:16]}-{fcw.window_id()}"])
        self.assertEqual(len(list((work / f"{self.full.train_sha[:16]}-{fcw.window_id()}" / "card").iterdir())), 5)
        # another admitted set changes only the correlation block: stored blocks serve it, bytes equal a fresh run
        original = self.full.admission.read_bytes()
        adm = json.loads(original)
        adm["admitted"] = ["b", "d"]
        for row in adm["candidates"]:
            row["status"] = "admitted" if row["id"] in adm["admitted"] else "reject_redundant"
        self.full.admission.write_bytes(json.dumps(adm, indent=2).encode())
        try:
            code, err = self.full.run(self.root / "readmitted", ["--work-dir", str(work)])
            self.assertEqual((code, counts(err)), (0, "card: computed 0, reused 5"), err)
            code, _ = self.full.run(self.root / "readmitted-fresh")
            self.assertEqual(code, 0)
        finally:
            self.full.admission.write_bytes(original)
        self.assertEqual(outputs(self.root / "readmitted"), outputs(self.root / "readmitted-fresh"))
        new, old = base.load(self.root / "readmitted", "a"), json.loads(self.ref["card-a.json"])
        self.assertNotEqual(new["correlation"], old["correlation"])
        for key in ("decay", "ic_by_size_tercile", "ic_by_ff12", "worldquant", "turnover", "coverage", "runner_check"):
            self.assertEqual(new[key], old[key], key)

    def test_a_tampered_or_foreign_block_is_a_miss(self):
        work = self.root / "tamper-work"
        self.assertEqual(self.full.run(self.root / "t1", ["--work-dir", str(work)])[0], 0)
        cards = sorted((work / f"{self.full.train_sha[:16]}-{fcw.window_id()}" / "card").iterdir())
        j = json.loads(cards[0].read_bytes())
        j["body"]["pnl"][3] = 1.0
        cards[0].write_bytes(json.dumps(j).encode())
        code, err = self.full.run(self.root / "t2", ["--work-dir", str(work)])
        self.assertEqual((code, counts(err)), (0, "card: computed 1, reused 4"), err)
        self.assertEqual(outputs(self.root / "t2"), self.ref)

    def test_cards_byte_identical_to_the_pre_store_card(self):
        try:
            old = subprocess.run(["git", "cat-file", "-p", PRE_STORE_CARD_BLOB], capture_output=True, check=True,
                                 cwd=Path(__file__).resolve().parent).stdout
        except (OSError, subprocess.CalledProcessError):
            self.skipTest("git history with the pre-store card blob is unavailable")
        (self.root / "old").mkdir(exist_ok=True)
        path = self.root / "old" / "alpha_report_card_pre_store.py"
        path.write_bytes(old)
        spec = importlib.util.spec_from_file_location("alpha_report_card_pre_store", path)
        pre = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(pre)
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(pre.main(self.full.argv(self.root / "pre")), 0)
        self.assertEqual(outputs(self.root / "pre"), self.ref)


class CoverageFlags(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        panel = base.make_panel(dates=240, names=330, score_begin=100, start="2020-09-01", seed=14)
        years = arc.years_of(panel["sessions"])
        rng = np.random.default_rng(3)
        full = rng.standard_normal(panel["close"].shape)
        event = rng.standard_normal(panel["close"].shape)
        event[years == 2020] = np.nan                      # an event field empty for a whole year
        sparse = rng.standard_normal(panel["close"].shape)
        sparse[np.ix_(years == 2021, np.arange(0, 330))] = np.nan
        sparse[np.ix_(years == 2021, np.arange(0, 80))] = rng.standard_normal((int((years == 2021).sum()), 80))
        cls.world = world(cls.root / "w", panel, {"full": full, "event": event, "sparse": sparse},
                          ["full", "event", "sparse"])
        cls.code, cls.err = cls.world.run(cls.root / "flags", ["--coverage-flags"])
        cls.plain_code, _ = cls.world.run(cls.root / "plain")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_card_low_coverage_reported(self):
        self.assertEqual((self.code, self.plain_code), (0, 0), self.err)
        event, sparse, full = (base.load(self.root / "flags", i) for i in ("event", "sparse", "full"))
        self.assertEqual(event["coverage_flag"]["empty_years"], ["2020"])
        self.assertTrue(event["coverage_flag"]["low"])
        self.assertEqual(sparse["coverage_flag"]["low_years"], ["2021"])
        self.assertEqual(sparse["coverage_flag"]["empty_years"], [])
        self.assertLess(sparse["coverage"]["2021"], 0.8)
        self.assertEqual(full["coverage_flag"], {**full["coverage_flag"], "low": False, "low_years": [],
                                                 "empty_years": []})
        # flagged candidates are scored and ranked, never dropped
        index = json.loads((self.root / "flags" / "index.json").read_bytes())
        rows = {r["id"]: r for r in index["candidates"]}
        self.assertEqual(sorted(rows), ["event", "full", "sparse"])
        self.assertEqual({i: rows[i]["coverage_low"] for i in rows}, {"event": True, "full": False, "sparse": True})
        self.assertIsNotNone(rows["sparse"]["rank"])
        # the runner's 80% rule removes the thin year from the IC; the flag says why it is missing
        self.assertEqual(sparse["ic_by_year"]["21"].get("2021", {"n": 0})["n"], 0)

    def test_without_the_switch_nothing_changes(self):
        plain = outputs(self.root / "plain")
        self.assertNotIn(b"coverage_flag", b"".join(plain.values()))
        self.assertNotIn(b"coverage_low", plain["index.json"])
        flagged = outputs(self.root / "flags")
        for cid in ("event", "sparse", "full"):
            card = json.loads(flagged[f"card-{cid}.json"])
            card.pop("coverage_flag")
            self.assertEqual(arc.canonical(card), plain[f"card-{cid}.json"])


if __name__ == "__main__":
    unittest.main()
