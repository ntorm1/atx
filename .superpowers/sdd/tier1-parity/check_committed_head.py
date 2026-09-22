"""Controller check: import the committed package before focused pytest gates."""

import sys

import atx_db  # noqa: F401
import pytest


if __name__ == "__main__":
    raise SystemExit(pytest.main(sys.argv[1:]))
