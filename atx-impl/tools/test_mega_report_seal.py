"""mega_report seal check (platform v8 E-4, OD-5): date-shaped path parts are refused, hash-named parts are not, and
paths are taken relative to the research output root. Ruling E-11: tracked documents under .superpowers/sdd/ and
docs/plans/ are exempt from the year rule by root class, never from the named pattern; a memmapped cache payload
passes the same check. Synthetic files only.

Run: python -m pytest atx-impl/tools/test_mega_report_seal.py -q
"""
from pathlib import Path
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
from engine_tools import research_window as rw  # noqa: E402
from mega_report import data as D  # noqa: E402
from mega_report import pitch as P  # noqa: E402
from test_mega_report_sig_corr import ROLE_SHA, World, fake_ids  # noqa: E402

READ = ("fp_2e2025f0aa11bb22/droe.f64", "train-2020-2023-lo1/daily.csv", "ic1_" + "2024" * 4 + "/x.json",
        "build-equity/mega-candidate-cache-v71/3e79978a858cbf6b723ff7a896d56814f7505dde11b805c30c5b909783ebb809/a.json")
REFUSED = ("nav-2023-2024/x.csv", "x_20250131.csv", "nav-2026/y.csv", "VAL/z.csv",
           "build-equity/recent-fast-validation-2023-2024-v1/manifest.json", "holdout/a.csv")
# E-4 concern 6: the three v7 pitch trial-accounting quote sources (run-date-stamped documents)
V7_QUOTES = (".superpowers/sdd/mega-alpha-20260926/progress.md", ".superpowers/sdd/platform-20260928/progress.md",
             "docs/plans/2026-09-28-mega-alpha-scorecard-v6.md")
DOC_REFUSED = ("docs/plans/x-validation-notes.md", "build-equity/nav-2024/x.csv", ".superpowers/sdd/s/holdout/a.md",
               "docs/plans/../build-equity/nav-2024/x.csv", "build-equity/docs/plans/nav-2025/x.csv")


class SealRegex(unittest.TestCase):
    def test_first_sealed_year_is_the_research_windows(self):
        self.assertEqual(D.FIRST_SEALED_YEAR, rw.FIRST_SEALED_YEAR)
        self.assertEqual(D.FIRST_SEALED_YEAR, int(rw.SEAL_DATE[:4]))

    def test_seal_regex_hex_vs_date(self):
        for path in READ:
            self.assertFalse(D.path_is_sealed(path), path)
        for path in REFUSED:
            self.assertTrue(D.path_is_sealed(path), path)
        # the year threshold is the window's: one year earlier seals the 2023 part too
        self.assertTrue(D.path_is_sealed("train-2020-2023-lo1/daily.csv", D.FIRST_SEALED_YEAR - 1))
        self.assertFalse(D.path_is_sealed("nav-2023/x.csv"))
        self.assertTrue(D.path_is_sealed(f"nav-{D.FIRST_SEALED_YEAR}/x.csv"))
        self.assertTrue(D.path_is_sealed("a\\nav-2026\\y.csv"))   # Windows separators split too


class SealRegistry(unittest.TestCase):
    def test_seal_paths_are_relative_to_out_root(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "platform-v8-20260929" / "research-out"   # a run-date-stamped sprint folder
            files = {rel: rel.encode() for rel in READ + REFUSED}
            for rel, data in files.items():
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_bytes(data)
            self.assertTrue(D.path_is_sealed(root.as_posix()))   # the absolute path would be refused
            reg = D.Registry(root)
            for rel in READ:
                self.assertEqual(reg.read_bytes(rel), files[rel], rel)
                self.assertEqual(reg.files[rel]["status"], "read")
            for rel in REFUSED:
                self.assertIsNone(reg.read_bytes(rel), rel)
                self.assertEqual(reg.files[rel]["status"], "refused (sealed)")
                self.assertNotIn("sha256", reg.files[rel])   # never opened, never hashed
            self.assertEqual(reg.read_bytes(root / READ[0]), files[READ[0]])   # absolute under the root: relative key


class DocumentRoots(unittest.TestCase):
    """Ruling E-11: document sources are exempt from the year rule by root class; the named pattern still applies."""

    def test_v7_quote_sources_are_read_again(self):
        for path in V7_QUOTES:
            self.assertTrue(D.is_document_path(path), path)
            self.assertFalse(D.path_is_sealed(path), path)
            self.assertFalse(D.path_is_sealed(path.replace("/", "\\")), path)   # Windows separators
        for path in DOC_REFUSED:
            self.assertTrue(D.path_is_sealed(path), path)
        self.assertFalse(D.is_document_path("docs/plans/../build-equity/nav-2024/x.csv"))
        self.assertFalse(D.is_document_path("build-equity/docs/plans/nav-2025/x.csv"))   # a root prefix, not a part

    def test_quote_sources_through_the_registry(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for rel in V7_QUOTES + ("docs/plans/x-validation-notes.md", "build-equity/nav-2024/x.csv"):
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text("head\nTRAIN construction cells 37\nnext\n", encoding="utf-8")
            reg = D.Registry(root)
            for rel in V7_QUOTES:
                q = D.quote_source(reg, rel, "construction cells", 2)
                self.assertEqual(q, {"path": rel, "line": 2, "text": "TRAIN construction cells 37\nnext"}, rel)
                self.assertEqual(reg.files[rel]["status"], "read")
            self.assertIsNone(D.quote_source(reg, "docs/plans/x-validation-notes.md", "construction"))
            self.assertIsNone(reg.read_bytes("build-equity/nav-2024/x.csv"))
            for rel in ("docs/plans/x-validation-notes.md", "build-equity/nav-2024/x.csv"):
                self.assertEqual(reg.files[rel], {"status": "refused (sealed)"}, rel)

    def test_sealed_records_without_opening(self):
        with tempfile.TemporaryDirectory() as tmp:
            reg = D.Registry(Path(tmp))
            self.assertTrue(reg.sealed("cache/nav-2025/a.f64"))      # need not exist: nothing is opened or stat'ed
            self.assertEqual(reg.files["cache/nav-2025/a.f64"], {"status": "refused (sealed)"})
            self.assertFalse(reg.sealed("cache/nav-2023/a.f64"))
            self.assertNotIn("cache/nav-2023/a.f64", reg.files)


@unittest.mock.patch.object(P, "_ids", fake_ids)
class MemmapPayloadSeal(unittest.TestCase):
    """E-4 open risk closed: a memmapped cache payload is seal-checked before it is stat'ed or mapped."""

    def sealed_world(self, tmp: str, entries: bool) -> World:
        w = World(Path(tmp) / "w", "v2", entries=entries)
        d = w.cache_root / ROLE_SHA
        stem = f"a.{w.dsl['a'][:16]}"
        (d / f"{stem}.f64").rename(d / "a_2025.f64")            # a date-shaped payload name, sidecar unchanged
        side = d / f"{stem}.json"
        side.write_text(side.read_text().replace(f'"{stem}.f64"', '"a_2025.f64"'))
        summ = w.root / "u" / "summary.json"
        summ.write_text(summ.read_text().replace(f"{stem}.f64", "a_2025.f64"))
        return w

    def check(self, entries: bool):
        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as tmp:
            w = self.sealed_world(tmp, entries)
            ctx = w.ctx()
            res = P.analysis(ctx, "sig_corr")
            self.assertIn("refused (sealed)", res.get("_error", ""))
            key = f"cache/{ROLE_SHA}/a_2025.f64"
            self.assertEqual(ctx.reg.files[key], {"status": "refused (sealed)"})
            self.assertFalse(any(v.get("status", "").startswith("memmap") and "a_2025" in k
                                 for k, v in ctx.reg.files.items()))

    def test_scanned_payload_is_seal_checked(self):
        self.check(entries=False)

    def test_summary_entry_payload_is_seal_checked(self):
        self.check(entries=True)

    def test_unsealed_payloads_are_still_mapped(self):
        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as tmp:
            w = World(Path(tmp) / "w", "v2")
            ctx = w.ctx()
            self.assertNotIn("_error", P.analysis(ctx, "sig_corr"))
            self.assertEqual(sum(1 for v in ctx.reg.files.values() if v.get("status", "").startswith("memmap")), 3)


if __name__ == "__main__":
    unittest.main()
