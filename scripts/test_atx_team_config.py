"""Credential generation preserves identity, expiry and exclusive output ownership."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location("atx_team", Path(__file__).with_name("atx-team.py"))
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class TeamConfigTest(unittest.TestCase):
    def test_policy_contains_only_hashes_and_expiry_is_exact(self):
        policy, private = MODULE.create_documents(
            "Research", ["operator:admin:Operator", "alice:researcher:Alice"], 90, 1000)
        self.assertEqual(len(policy["principals"]), 2)
        for principal, credential in zip(policy["principals"], private["credentials"]):
            self.assertNotIn("token", principal)
            self.assertEqual(principal["expires_at_ms"], 1000 + 90 * 86_400_000)
            self.assertEqual(principal["token_sha256"], hashlib.sha256(
                credential["token"].encode("ascii")).hexdigest())
            self.assertRegex(credential["token"], r"^[A-Za-z0-9_-]{43}$")
        self.assertNotEqual(private["credentials"][0]["token"], private["credentials"][1]["token"])

    def test_reserved_duplicate_missing_admin_and_bad_names_are_rejected(self):
        for members in (["local:admin"], ["alice:admin", "alice:viewer"],
                        ["alice:researcher"], ["alice:admin:"], ["alice:admin:A\nB"],
                        ["alice:owner"], ["alice:admin:   "], ["alice:admin:" + "é" * 65]):
            with self.subTest(members=members), self.assertRaises(ValueError):
                MODULE.create_documents("Research", members, 90, 1000)

    def test_existing_output_is_preserved(self):
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder) / "team"
            documents = MODULE.create_documents("Research", ["alice:admin"], 0, 1000)
            MODULE.write_documents(target, *documents)
            before = (target / "credentials.json").read_bytes()
            self.assertEqual(documents[0]["principals"][0]["expires_at_ms"], 0)
            with self.assertRaises(FileExistsError):
                MODULE.write_documents(target, *documents)
            self.assertEqual((target / "credentials.json").read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
