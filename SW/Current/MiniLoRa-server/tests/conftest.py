"""Shared test setup: point the server's Database at a throwaway file
BEFORE any test module imports minilora_server.main (the Database opens
its path in a module-level statement). Without this, whatever module
imports first decides the DB — test_config_backup/test_settings don't set
MINILORA_DB and would run against the LIVE data/minilora.sqlite3, dedup
against real device rows and pollute them with test frames.

This conftest pins the whole suite to data/test_minilora.sqlite3 (deleted
on each run); test_p2p.py's own delete still runs for its stricter
fresh-slate needs. The file is a test artifact, never the live DB."""

import os
from pathlib import Path

_db = str(Path(__file__).resolve().parent.parent / "data" /
          "test_minilora.sqlite3")
os.environ["MINILORA_DB"] = _db
if os.path.exists(_db):
    os.remove(_db)
