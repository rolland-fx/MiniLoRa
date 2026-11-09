"""Insights tests: delivery ratio (f_cnt gaps), battery projection,
missed-cycle detection — the pure math, no DB needed."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from minilora_server.insights import (CRITICAL_MV, DEGRADED_MV,
                                      battery_projection, delivery_ratio,
                                      missed_cycles)

DAY = 86400


# ---- delivery ratio ----

def test_delivery_perfect_link():
    r = delivery_ratio([10, 11, 12, 13, 14])
    assert r["sent"] == 5 and r["lost"] == 0
    assert r["ratio"] == 1.0


def test_delivery_with_gaps():
    # 10, 11, 15, 18 received: 12-14 and 16-17 never arrived
    r = delivery_ratio([10, 11, 15, 18])
    assert r["sent"] == 4 and r["lost"] == 5
    assert r["expected"] == 9
    assert r["ratio"] == round(4 / 9, 4)


def test_delivery_duplicate_and_reset():
    # duplicate (retransmit) adds nothing; a reset starts a fresh span
    r = delivery_ratio([10, 10, 11, 3, 4])
    assert r["lost"] == 0
    assert r["resets"] == 1
    assert r["ratio"] == 1.0


def test_delivery_insufficient_data():
    assert delivery_ratio([])["ratio"] is None
    assert delivery_ratio([7])["ratio"] is None


# ---- battery projection ----

def test_battery_flags_only_without_enough_points():
    rows = [(1000, 3400), (2000, 3390), (3000, 3380)]
    r = battery_projection(rows)
    assert r["bat_mv_now"] == 3380
    assert r["slope_mv_day"] is None
    assert r["degraded_now"] is True and r["critical_now"] is False


def test_battery_projection_draining():
    now = 1_800_000_000
    # 40 mV/day drain: 4000 mV now-ish, linear
    rows = [(now - 4 * DAY, 4160), (now - 3 * DAY, 4120),
            (now - 2 * DAY, 4080), (now - DAY, 4040), (now, 4000)]
    r = battery_projection(rows)
    assert r["slope_mv_day"] == -40.0
    assert r["days_to_cutoff"] == round((3300 - 4000) / -40.0, 1)
    assert r["days_to_cutoff"] == 17.5
    assert r["warn_soon"] is False


def test_battery_charging_has_no_projection():
    now = 1_800_000_000
    rows = [(now - 4 * DAY, 3300), (now - 3 * DAY, 3400),
            (now - 2 * DAY, 3500), (now - DAY, 3600), (now, 3700)]
    r = battery_projection(rows)
    assert r["days_to_cutoff"] is None
    assert r["critical_now"] is False


def test_battery_critical_now():
    rows = [(1, CRITICAL_MV - 10)] * 5
    r = battery_projection(rows)
    assert r["critical_now"] is True and r["degraded_now"] is False


# ---- missed cycles ----

def test_no_missed_cycles_on_cadence():
    # 30 min sleep + 10 s awake, frames arriving on cadence
    rows = [(1000 + i * 1810, 30, 10) for i in range(5)]
    r = missed_cycles(rows, now_s=rows[-1][0] + 1810)
    assert r["missed"] == 0 and r["missed_since_last"] == 0
    assert r["stale"] is False


def test_missed_cycle_counted():
    # one frame every ~1810 s, then a 3x gap (2 cycles never arrived)
    rows = [(1000, 30, 10), (2810, 30, 10), (8240, 30, 10)]
    r = missed_cycles(rows, now_s=rows[-1][0])
    assert r["missed"] == 2


def test_open_gap_and_stale():
    rows = [(1000, 30, 10)]  # expected interval 1810 s
    r = missed_cycles(rows, now_s=1000 + 3 * 1810)
    assert r["missed_since_last"] == 2
    assert r["stale"] is False
    r2 = missed_cycles(rows, now_s=1000 + DAY + 3600)
    assert r2["stale"] is True


def test_small_gaps_never_count():
    # history-drip style bursts: sub-cadence gaps are noise, not losses
    rows = [(1000, 30, 10), (1100, 30, 10), (1200, 30, 10)]
    r = missed_cycles(rows, now_s=1200)
    assert r["missed"] == 0
