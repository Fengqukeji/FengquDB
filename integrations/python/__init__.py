"""Copyable Python SDK entry: from FengquDB import Store, generate."""
import sys
from pathlib import Path

_archive = str(Path(__file__).resolve().parent / "fengqudb.zip")
if _archive not in sys.path:
    sys.path.insert(0, _archive)

from fengqudb import *  # noqa: F401,F403,E402
from fengqudb_models import generate  # noqa: F401,E402
