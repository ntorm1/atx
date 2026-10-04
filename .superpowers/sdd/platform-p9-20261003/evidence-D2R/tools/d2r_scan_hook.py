"""D2R scan hook (scratch, lane D2R): records every argv that reaches the fitter's argparse parser.

Active only when D2R_SCAN_LOG is set. Records, never changes behaviour: the original parse_known_args runs unchanged.
The fitter's parser is recognised by its options (--composition and --runner-summary-sha256)."""
import argparse
import json
import os
import sys
import traceback

_LOG = os.environ.get("D2R_SCAN_LOG")
if _LOG:
    _orig = argparse.ArgumentParser.parse_known_args

    def _d2r_parse_known_args(self, args=None, namespace=None):
        try:
            opts = getattr(self, "_option_string_actions", {})
            if "--composition" in opts and "--runner-summary-sha256" in opts:
                argv = list(sys.argv[1:] if args is None else args)
                longs = sorted(o for o in opts if o.startswith("--"))
                tokens = []
                for tok in argv:
                    if not isinstance(tok, str) or not tok.startswith("--") or tok == "--":
                        continue
                    head = tok.split("=", 1)[0]
                    if head in opts:
                        continue
                    tokens.append({"token": tok, "matches": [o for o in longs if o.startswith(head)]})
                stack = [f"{f.filename}:{f.lineno}:{f.name}" for f in traceback.extract_stack()[:-1][-8:]]
                rec = {"pid": os.getpid(), "test": os.environ.get("PYTEST_CURRENT_TEST"), "argv": [str(a) for a in argv],
                       "nonexact": tokens, "allow_abbrev": self.allow_abbrev, "stack": stack}
                with open(_LOG, "a", encoding="utf-8") as fh:
                    fh.write(json.dumps(rec) + "\n")
        except Exception as exc:  # never let the recorder change the run
            try:
                with open(_LOG, "a", encoding="utf-8") as fh:
                    fh.write(json.dumps({"hook_error": repr(exc)}) + "\n")
            except Exception:
                pass
        return _orig(self, args, namespace)

    argparse.ArgumentParser.parse_known_args = _d2r_parse_known_args
