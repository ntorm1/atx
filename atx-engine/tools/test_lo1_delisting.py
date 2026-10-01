"""linked-operating-v1 with --delisting / --delisting-returns (platform v8 F-0): with the options off the lo1 role is
byte identical to the base commit's (manifest included), and with them on the marks and the applied returns are
linked-operating-v3's (same stage, same rule, same patched payloads), on the synthetic role of
test_linked_operating_v2 / test_linked_operating_v3. Ruling E-39 (lane DLRET): the --delisting-returns lo1 role is a
Ruling E-25 label role of the plain lo1 decision role (test_lo1_label_role_pair_for_the_e25_check)."""
import contextlib
import datetime as dt
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

import numpy as np

import prepare_recent_research as tool
import prepare_research_fields as prf
import test_linked_operating_v2 as v2t
import test_linked_operating_v3 as v3t
import test_prepare_recent_research as base

LO1 = "linked-operating-v1"
PATCHED = ("close.f64", "raw_close.f64", "volume.f64", "present.u8")
# lo1 (flag off) of LinkedOperatingV3's fixture, computed with prepare_recent_research.py at the F-0 base commit
# c560b8c8 (before linked-operating-v1 accepted --delisting): the SHA-256 of the manifest's raw bytes with the
# temporary directory, the fixture's input pins and both code identities replaced by tokens (identity_tokens), and
# the SHA-256 of every payload file.
LO1_GOLDEN_MANIFEST = "f94a13a06d91f1ae8e674a7a497139e00333809a614c670ea833ac7d839a26de"
LO1_GOLDEN_PAYLOADS = {
    "close.f64": "50bc6a139fbb6fe3cfb5fcaa368d3a794db056891f9def414c895c2dd2324c12",
    "ids.u64": "ea0d7246ca8346f6137619c0aaf7410ae4ffe69d1f0f702d6760e661de6df857",
    "member.u8": "037080a8f113c905a87ab119500507ccb3b1e64f2d4e0b253d1d8ee1c0492fe4",
    "present.u8": "89c6ee57952df32bdae847ef2a5be2f5472f793cd7c798f10547c2b7360110c8",
    "raw_close.f64": "910bca4d73d122bbd6707aa11aaf8c3d080d0ab7bf2e527095b86e535cbe2eb9",
    "sessions.i64": "b8543b9b98157c364b338e741f883ababa50ed7c4e6dfea9e96f910731afe7c2",
    "volume.f64": "4dec1a885916a4c8f9a2c61d7d32d4fb3bf8db13980d3b7251eb40318a331240"}
# The lo1 role pair of test_lo1_label_role_pair_for_the_e25_check as the C++ manifest rule reads it
# (atx-impl/tests/strategy_target_replay_test.cpp, NavLabelRoleLo1.*): <role>/manifest.json, tokenised_manifest's text.
# After a change of the tool's manifest, rewrite it with ATX_WRITE_LO1_LABEL_ROLE_PAIR=1 and re-run those gtests.
LABEL_PAIR_FIXTURE = Path(__file__).resolve().parents[2] / "atx-impl" / "tests" / "fixtures" / "lo1_label_role_pair"
LABEL_PAIR_WRITE = "ATX_WRITE_LO1_LABEL_ROLE_PAIR"


def tokenised_manifest(case, out) -> str:
    """Role ``out``'s raw manifest text with environment tokens. Only the temporary directory, the fixture's input pins
    (the delisting stage's included) and the executed code's identities are tokenised; key order, formatting and every
    other byte are the tool's."""
    root, v2 = case.base, case.v2
    text = (root / out / "manifest.json").read_bytes().decode("utf-8")
    pins = {v2.role_sha: "role-sha", v2.bridge_sha: "bridge-sha", v2.sic_sha: "sic-sha",
            base.sha((root / "bridge/links.parquet").read_bytes()): "bridge-file-sha",
            base.sha((root / "events/sic_events.parquet").read_bytes()): "sic-file-sha",
            base.sha((root / "source.parquet").read_bytes()): "source-sha",
            base.sha((root / "cache/manifest.json").read_bytes()): "projection-sha",
            v2.del_sha: "delisting-sha",
            base.sha((root / "delisting/events.parquet").read_bytes()): "delisting-file-sha"}
    for name, module in (("prepare_recent_research", tool), ("prepare_research_fields", prf)):
        for key, value in prf.code_identity(Path(module.__file__).resolve()).items():
            pins[value] = f"{name}.{key}"
    for value, token in pins.items():
        text = text.replace(f'"{value}"', f'"<{token}>"')
    for r in sorted({str(root), str(root.resolve())}, key=len, reverse=True):
        text = text.replace(json.dumps(r)[1:-1], "<BASE>")
    return text


def identity_tokens(case, out):
    """(SHA-256 of ``tokenised_manifest(case, out)``, {payload: SHA-256})."""
    payloads = {p.name: base.sha(p.read_bytes())
                for p in sorted((case.base / out).iterdir()) if p.name != "manifest.json"}
    return hashlib.sha256(tokenised_manifest(case, out).encode("utf-8")).hexdigest(), payloads


def strip_membership(block):
    """The delisting block without its per-role membership read-outs (kept_member_at_last_session, members cleared)."""
    doc = json.loads(json.dumps(block))
    for e in doc["events"]:
        e.pop("kept_member_at_last_session")
    for c in doc["by_cause"].values():
        c.pop("kept_member_at_last_session")
    if "applied" in doc:
        doc["applied"].pop("members_cleared_on_termination_session")
    return doc


class Lo1Delisting(unittest.TestCase):
    def setUp(self):
        self.v3 = v3t.LinkedOperatingV3("test_cli")
        self.v3.setUp()
        self.v2, self.base, self.days = self.v3.v2, self.v3.base, self.v3.days

    def tearDown(self):
        self.v3.tearDown()

    def lo1(self, out, **kw):
        return self.v2.restrict(out, universe=LO1, **kw)

    def lo1_delisting(self, out, **kw):
        return self.lo1(out, delisting=self.base / "delisting", delisting_sha256=self.v2.del_sha, **kw)

    def lo3(self, out, **kw):
        return self.v3.restrict(out, **kw)

    def member(self, out):
        return self.v3.member(out)

    def payload(self, out, name):
        return (self.base / out / name).read_bytes()

    def test_lo1_delisting_off_is_byte_identical(self):
        m = self.lo1("lo1")
        self.assertEqual(identity_tokens(self, "lo1"), (LO1_GOLDEN_MANIFEST, LO1_GOLDEN_PAYLOADS))
        # the tokenised code block is exactly the executed code's identity, and nothing of the options appears
        self.assertEqual(m["universe"]["inputs"]["code"],
                         {"prepare_recent_research": prf.code_identity(Path(tool.__file__).resolve()),
                          "prepare_research_fields": prf.code_identity(Path(prf.__file__).resolve())})
        self.assertNotIn("delisting", m["universe"])
        self.assertNotIn("delisting", m["universe"]["inputs"])
        self.assertEqual(json.loads(self.payload("lo1", "manifest.json")), m)
        # the pre-existing v1 golden (LinkedOperatingUniverse's fixture, W5b base commit) still holds
        case = base.LinkedOperatingUniverse("test_restricts_members_point_in_time_and_copies_payloads")
        case.setUp()
        try:
            self.assertEqual(v2t.normalized_v1(case, case.restrict()), (v2t.V1_GOLDEN_MANIFEST, v2t.V1_GOLDEN_MEMBER))
        finally:
            case.tearDown()

    def test_lo1_delisting_returns_match_lo3_semantics(self):
        plain = self.lo1("lo1")
        marked = self.lo1_delisting("lo1m")
        ret = self.lo1_delisting("lo1r", delisting_returns=True)
        lo3p = self.lo3("lo3p")
        lo3r = self.lo3("lo3r", delisting_returns=True)
        # 1. marking alone adds the two delisting blocks and changes no payload byte and no other manifest value
        for name in sorted(plain["files"]) + ["member.u8"]:
            self.assertEqual(self.payload("lo1m", name), self.payload("lo1", name), name)
        doc = json.loads(json.dumps(marked))
        self.assertFalse(doc["universe"].pop("delisting")["returns_applied"])
        doc["universe"]["inputs"].pop("delisting")
        self.assertEqual(doc, plain)
        # 2. the same stage and the same rules: the block is lo3's but for the per-role membership read-outs
        for mine, theirs in ((marked, lo3p), (ret, lo3r)):
            self.assertEqual(mine["universe"]["inputs"]["delisting"], theirs["universe"]["inputs"]["delisting"])
            self.assertEqual(strip_membership(mine["universe"]["delisting"]),
                             strip_membership(theirs["universe"]["delisting"]))
        d = ret["universe"]["delisting"]
        self.assertEqual((d["rule"], d["return_rule"], d["stage_rule"], d["returns_applied"]),
                         (tool.DELISTING_MARK_RULE, tool.DELISTING_RETURN_RULE, tool.DELISTING_STAGE_RULE, True))
        self.assertEqual(ret["universe"]["inputs"]["delisting"]["manifest_sha256"], self.v2.del_sha)
        # 3. the same patched payloads: the return rule reads the base payloads and the stage, not the membership
        for name in PATCHED:
            self.assertEqual(self.payload("lo1r", name), self.payload("lo3r", name), name)
        for name in ("sessions.i64", "ids.u64"):
            self.assertEqual(self.payload("lo1r", name), self.payload("lo1", name), name)
        role = self.v2.arrays("role")
        col = {int(x): k for k, x in enumerate(role["ids"])}
        r = self.v2.arrays("lo1r")
        c3, t = col[3], v2t.STOP + 1
        self.assertAlmostEqual(r["close.f64"][t, c3] / r["close.f64"][t - 1, c3] - 1, -0.55, places=12)
        self.assertEqual(d["applied"]["terminations"], 1)
        self.assertNotEqual(self.payload("lo1r", "close.f64"), self.payload("lo1", "close.f64"))
        # 4. the membership read-outs are each role's own: kept at L from its plain role, members cleared on T only
        days = [int(x) for x in np.fromfile(self.base / "role" / "sessions.i64", dtype="<i8")]
        for out_plain, out_ret, m in (("lo1", "lo1r", ret), ("lo3p", "lo3r", lo3r)):
            before, after = self.member(out_plain), self.member(out_ret)
            cleared = []
            for e in m["universe"]["delisting"]["events"]:
                j = col[e["security_id"]]
                t_last = [k for k, s in enumerate(self.days) if s == e["last_session"]]
                self.assertEqual(e["kept_member_at_last_session"], bool(t_last and before[t_last[0], j]), out_ret)
                if e["returns_applied"]:
                    tt = self.days.index(e["termination_session"])
                    self.assertEqual(after[tt, j], 0)
                    if before[tt, j]:
                        cleared.append([tt, j])
            self.assertEqual(np.argwhere(before != after).tolist(), cleared, out_ret)
            self.assertEqual(m["universe"]["delisting"]["applied"]["members_cleared_on_termination_session"],
                             len(cleared))
            self.assertEqual(m["universe"]["kept_member_counts"], after.astype(np.int64).sum(axis=1).tolist())
            self.assertEqual(m["score_member_counts"], m["universe"]["kept_member_counts"][m["score_begin"]:])
        self.assertEqual(len(days), self.v2.nd)
        # 5. the lo1 restriction itself is untouched: same rule, reasons and limits as the flag-off role
        for key in ("id", "rule", "limits", "class_status_required", "dropped_by_reason", "base_member_cells"):
            self.assertEqual(ret["universe"][key], plain["universe"][key], key)

    def test_lo1_delisting_cli_and_refusals(self):
        with self.assertRaisesRegex(ValueError, "go together"):
            self.lo1("x1", delisting=self.base / "delisting")
        with self.assertRaisesRegex(ValueError, "--delisting-returns"):
            self.lo1("x2", delisting_returns=True)
        with self.assertRaisesRegex(ValueError, "delisting manifest SHA-256"):
            self.lo1("x3", delisting=self.base / "delisting", delisting_sha256="0" * 64)
        common = ["role", "--base-role", str(self.base / "role"), "--base-role-sha256", self.v2.role_sha,
                  "--identity-bridge", str(self.base / "bridge"), "--identity-bridge-sha256", self.v2.bridge_sha,
                  "--sic-events", str(self.base / "events"), "--sic-events-sha256", self.v2.sic_sha,
                  "--universe", LO1, "--out", str(self.base / "cli")]
        stage = ["--delisting", str(self.base / "delisting"), "--delisting-sha256", self.v2.del_sha]
        for argv in (common + ["--delisting", str(self.base / "delisting")],
                     common + ["--delisting-returns", str(self.base / "delisting")],
                     common + stage + ["--delisting-returns", str(self.base / "events")]):
            with patch.object(sys, "argv", ["prepare_recent_research.py", *argv]), \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                tool.main()
        for out in ("x1", "x2", "x3", "cli"):
            self.assertFalse((self.base / out).exists(), out)
        argv = common + stage + ["--delisting-returns", str(self.base / "delisting" / "events.parquet")]
        with patch.object(sys, "argv", ["prepare_recent_research.py", *argv]):
            tool.main()
        self.lo1_delisting("api", delisting_returns=True)
        for name in PATCHED + ("member.u8",):
            self.assertEqual(self.payload("cli", name), self.payload("api", name), name)
        m = json.loads(self.payload("cli", "manifest.json"))
        self.assertEqual((m["universe"]["id"], m["universe"]["delisting"]["returns_applied"]), (LO1, True))

    def test_lo1_label_role_pair_for_the_e25_check(self):
        """Ruling E-39: the lo1 role built with --delisting --delisting-returns is the Ruling E-25 label role of the
        plain lo1 decision role (B0a's role: no delisting option).
        Declarations: returns_applied true (review B-3 reads it on --role only), the stage manifest and its
        events.parquet pinned, the cleared members counted.
        Payloads, the rule nav --label-role applies when it loads them (strategy_target_replay.cpp load_label_role,
        check_declared_clearing): sessions and ids shared; every decision-present cell present at the same close and
        raw close, bit for bit; presence added on the applied termination sessions only; member & present & close > 0
        equal; member.u8 different on exactly the declared cells, each a lagged member the decision role has absent and
        the label role prices and clears.
        Manifests: both equal LABEL_PAIR_FIXTURE after tokenised_manifest, the bytes the C++ manifest rule admits
        (NavLabelRoleLo1.*)."""
        decision = self.lo1("decision")
        label = self.lo1_delisting("label", delisting_returns=True)
        u, d = label["universe"], label["universe"]["delisting"]
        self.assertNotIn("delisting", decision["universe"])
        self.assertEqual((u["id"], d["returns_applied"]), (LO1, True))
        self.assertEqual(u["inputs"]["delisting"]["manifest_sha256"], self.v2.del_sha)
        self.assertEqual([s["sha256"] for s in u["inputs"]["delisting"]["sources"]],
                         [self.v2.del_sha, base.sha((self.base / "delisting/events.parquet").read_bytes())])
        cleared = d["applied"]["members_cleared_on_termination_session"]
        self.assertGreater(cleared, 0)   # the pair exercises the declared clearing
        a, b = self.v2.arrays("decision"), self.v2.arrays("label")
        for name in ("sessions.i64", "ids.u64"):
            self.assertEqual(self.payload("label", name), self.payload("decision", name), name)
        on = a["present.u8"] == 1
        self.assertTrue((b["present.u8"][on] == 1).all())
        for name in ("close.f64", "raw_close.f64"):
            self.assertTrue(np.array_equal(a[name][on].view("<u8"), b[name][on].view("<u8")), name)
        col = {int(x): k for k, x in enumerate(a["ids"])}
        terminations = sorted([self.days.index(e["termination_session"]), col[e["security_id"]]]
                              for e in d["events"] if e["returns_applied"])
        self.assertEqual(np.argwhere((b["present.u8"] == 1) & ~on).tolist(), terminations)
        self.assertEqual(len(terminations), d["applied"]["terminations"])
        with np.errstate(invalid="ignore"):
            live = [(x["member.u8"] == 1) & (x["present.u8"] == 1) & (x["close.f64"] > 0) for x in (a, b)]
        self.assertTrue(np.array_equal(live[0], live[1]))
        differ = np.argwhere(a["member.u8"] != b["member.u8"])
        self.assertEqual(len(differ), cleared)
        for t, j in differ:
            self.assertEqual((a["member.u8"][t, j], a["present.u8"][t, j], b["member.u8"][t, j], b["present.u8"][t, j]),
                             (1, 0, 0, 1), (t, j))
        for role in ("decision", "label"):
            text, path = tokenised_manifest(self, role), LABEL_PAIR_FIXTURE / role / "manifest.json"
            if os.environ.get(LABEL_PAIR_WRITE) == "1":
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(text.encode("utf-8"))
            self.assertEqual(path.read_bytes().replace(b"\r\n", b"\n").decode("utf-8"), text, role)


if __name__ == "__main__":
    unittest.main()
