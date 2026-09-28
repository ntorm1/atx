"""Synthetic checks for mega_report.pitch an_sig_corr on v1 and v2 candidate caches (no real data).

Run: python -m pytest atx-impl/tools/test_mega_report_sig_corr.py -q
"""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
import unittest.mock

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mega_report import analysis as A  # noqa: E402
from mega_report import data as D  # noqa: E402
from mega_report import pitch as P  # noqa: E402

ROLE_SHA, FIELDS_SHA, LIB_SHA = "3e" * 32, "1d" * 32, "11" * 32
IDS, FIELD_IDS, ADMITTED = ["a", "b", "c"], {"c"}, ["a", "c"]
ND, NI = 7, 60
FIELDS = {"sv_ratio126": "34" * 32, "book_to_price": "56" * 32}
V1, V2 = "atx.dsl-candidate-signal/v1", "atx.dsl-candidate-signal/v2"


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def fk16(fields: dict) -> str:
    return sha("".join(f"field={k}:{fields[k]}\n" for k in sorted(fields)).encode())[:16]


class World:
    """Report root: role/ (manifest + member.u8), cache/ (the runner's --candidate-cache DIR) and u/summary.json.

    ``layout`` v1: cache/[<identity>/]<role>/<id>.{f64,json}, field ids under cache/<fields sha>/; no summary entries.
    ``layout`` v2: cache/[<identity>/]<role>/[fp_<fk16>/]<id>.<dsl16>.{f64,json}; ``entries`` lists them in the
    summary with runner-style relative, backslash-separated paths.
    """

    def __init__(self, root: Path, layout: str, identity: str = "", entries: bool = True, seed: int = 3):
        self.root, self.layout = root, layout
        rng = np.random.default_rng(seed)
        base = rng.normal(size=(ND, NI))
        self.signals = {"a": base + 0.3 * rng.normal(size=(ND, NI)), "b": rng.normal(size=(ND, NI)),
                        "c": -base + 0.5 * rng.normal(size=(ND, NI))}
        self.signals["b"][:, :5] = np.nan
        (root / "role").mkdir(parents=True)
        (root / "role" / "manifest.json").write_text(json.dumps(
            {"dates": ND, "instruments": NI, "score_begin": 1, "score_end": ND}))
        self.member = np.ones((ND, NI), dtype=np.uint8)
        self.member[:, -3:] = 0
        (root / "role" / "member.u8").write_bytes(self.member.tobytes())
        self.cache_root = root / "cache" / identity if identity else root / "cache"
        self.dsl = {i: sha(f"rank(close) * {k}".encode()) for k, i in enumerate(IDS)}
        rows = [self.write_entry(i, self.dsl[i], LIB_SHA) for i in IDS]
        cache = {"directory": f"cache\\{ROLE_SHA}", "hits": 0}
        if layout == "v2" and entries:
            cache["entries"] = rows
        (root / "u").mkdir()
        (root / "u" / "summary.json").write_text(json.dumps(
            {"status": "complete", "roles": [{"role": "train", "manifest_sha256": ROLE_SHA, "candidate_cache": cache}]}))

    def entry_dir(self, cid: str, fields: dict) -> Path:
        d = self.cache_root / ROLE_SHA
        if self.layout == "v1":
            return self.cache_root / FIELDS_SHA if cid in FIELD_IDS else d
        return d / f"fp_{fk16(fields)}" if cid in FIELD_IDS else d

    def write_entry(self, cid: str, dsl: str, library: str, signal=None, fields=FIELDS) -> dict:
        d = self.entry_dir(cid, fields)
        d.mkdir(parents=True, exist_ok=True)
        stem = cid if self.layout == "v1" else f"{cid}.{dsl[:16]}"
        data = np.ascontiguousarray((self.signals[cid] if signal is None else signal).astype("<f8")).tobytes()
        (d / f"{stem}.f64").write_bytes(data)
        meta = {"schema": V1 if self.layout == "v1" else V2, "candidate_id": cid, "dsl_sha256": dsl,
                "library_sha256": library, "role_manifest_sha256": ROLE_SHA, "role": "train", "dates": ND,
                "instruments": NI, "bytes": len(data), "payload": f"{stem}.f64", "payload_sha256": sha(data)}
        if self.layout == "v2":
            meta["field_payload_sha256"] = fields if cid in FIELD_IDS else {}
        (d / f"{stem}.json").write_text(json.dumps(meta, indent=2))
        rel = d.relative_to(self.root).as_posix().replace("/", "\\")
        return {"id": cid, "layout": self.layout, "sidecar": f"{rel}\\{stem}.json", "payload": f"{rel}\\{stem}.f64",
                "payload_sha256": sha(data), "field_payload_sha256": meta.get("field_payload_sha256", {})}

    def ctx(self, lib_sha=LIB_SHA, **analysis):
        cfg = {"analysis": {"u_pass": "u", "role": "role", "candidate_cache": "cache", **analysis}}
        metrics = {"sum.source_bindings.library_sha256": lib_sha, "sum.role_sha256": ROLE_SHA}
        return SimpleNamespace(cfg=cfg, reg=D.Registry(self.root), final=object(), analyses={},
                               metric=lambda cell, path: metrics.get(path))

    def reference(self) -> np.ndarray:
        rows = list(range(1, ND))
        return A.signal_corr(lambda k, d: self.signals[IDS[k]][d], IDS, rows, lambda d: self.member[d] == 1)["r"]


def fake_ids(ctx):
    return IDS, {i: f"t_{i}" for i in IDS}, {}, ADMITTED


@unittest.mock.patch.object(P, "_ids", fake_ids)
class SigCorrCacheLayouts(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def world(self, name, layout, **kw):
        return World(self.root / name, layout, **kw)

    def sig_corr(self, world, **kw):
        return P.analysis(world.ctx(**kw), "sig_corr")

    def assert_matches(self, res, world, source):
        self.assertNotIn("_error", res)
        self.assertEqual(res["source"], source)
        np.testing.assert_array_equal(res["r"], world.reference())
        self.assertEqual(res["ids"], IDS)
        self.assertTrue(np.isfinite(res["r"]).all())

    def test_v1_cache_is_scanned_including_the_fields_directory(self):
        w = self.world("v1", "v1")
        self.assert_matches(self.sig_corr(w), w, "cache scan")

    def test_v1_entry_of_another_library_is_not_used(self):
        w = self.world("v1_lib", "v1")
        res = self.sig_corr(w, lib_sha="22" * 32)
        self.assertIn("cache payloads missing", res["_error"])

    def test_v2_summary_entries_name_the_exact_payloads(self):
        w = self.world("v2", "v2")
        # a stale entry of another DSL for "a" beside it, recorded by this library: the summary disambiguates
        w.write_entry("a", "99" * 32, LIB_SHA, signal=np.zeros((ND, NI)))
        ctx = w.ctx()
        res = P.analysis(ctx, "sig_corr")
        self.assert_matches(res, w, "summary entries")
        listed = [k for k, v in ctx.reg.files.items() if v.get("status", "").startswith("memmap")]
        self.assertEqual(len(listed), 3)
        self.assertTrue(any(f"fp_{fk16(FIELDS)}/c.{w.dsl['c'][:16]}.f64" in k for k in listed), listed)

    def test_v2_summary_and_v1_summary_give_the_same_matrix(self):
        v1, v2 = self.world("same_v1", "v1"), self.world("same_v2", "v2")
        np.testing.assert_array_equal(self.sig_corr(v1)["r"], self.sig_corr(v2)["r"])

    def test_v2_cache_scanned_when_the_summary_has_no_entries(self):
        w = self.world("v2_scan", "v2", identity="dslvm1_clang18.1_fma", entries=False)
        # an entry reused from an older library (content key) is taken when it is the only one
        w.write_entry("b", w.dsl["b"], "22" * 32)
        self.assert_matches(self.sig_corr(w), w, "cache scan")
        # this library's entry wins over another library's entry of another DSL
        w.write_entry("a", "99" * 32, "22" * 32, signal=np.zeros((ND, NI)))
        self.assert_matches(self.sig_corr(w), w, "cache scan")

    def test_ambiguous_scan_is_reported_not_guessed(self):
        w = self.world("ambiguous", "v2", entries=False)
        w.write_entry("c", w.dsl["c"], LIB_SHA, fields={"sv_ratio126": "78" * 32})  # a second field payload version
        res = self.sig_corr(w)
        self.assertIn("cache payloads missing for ['c']", res["_error"])
        self.assertIn("ambiguous: ['c']", res["_error"])

    def test_summary_entry_gone_falls_back_to_the_scan(self):
        w = self.world("gone", "v2")
        doc = json.loads((w.root / "u" / "summary.json").read_text())
        doc["roles"][0]["candidate_cache"]["entries"][0]["payload_sha256"] = "0" * 64  # names no sidecar
        (w.root / "u" / "summary.json").write_text(json.dumps(doc))
        self.assert_matches(self.sig_corr(w), w, "summary entries + cache scan")

    def test_never_raises(self):
        w = self.world("broken", "v2", entries=False)
        (w.root / "u" / "summary.json").write_text("{not json")
        (w.cache_root / ROLE_SHA / "garbage.json").write_text("[1, 2")
        (w.cache_root / ROLE_SHA / "list.json").write_text("[1, 2]")
        self.assert_matches(self.sig_corr(w), w, "cache scan")
        for name, cfg in (("no_cache", {"candidate_cache": "absent"}), ("no_u", {"u_pass": "absent"})):
            res = self.sig_corr(self.world(name, "v2", entries=False), **cfg)
            if name == "no_cache":
                self.assertIn("cache payloads missing", res["_error"])
            else:
                self.assertNotIn("_error", res)
        w = self.world("short", "v2")
        path = w.cache_root / ROLE_SHA / f"a.{w.dsl['a'][:16]}.f64"
        path.write_bytes(path.read_bytes()[:-8])
        self.assertIn("payload geometry mismatch for a", self.sig_corr(w)["_error"])


if __name__ == "__main__":
    unittest.main()
