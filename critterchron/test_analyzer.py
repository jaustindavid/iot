#!/usr/bin/env python3
"""Tests for tools/analyzer/. PASS/FAIL idiom, no pytest dependency.

Covers:
  - heartbeat string parsing (real-world examples + edge cases)
  - ThresholdRule firing semantics
  - FleetMadDetector outlier detection + warmup gate
  - Storage round-trips
  - Config loading (success + validation errors)

Network-dependent paths (queue consume, KV writeback) are NOT
exercised here — they're covered by integration smoke against a
live Stra2us instance, not unit tests.

Usage:
    python3 test_analyzer.py
    python3 test_analyzer.py -v
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from tools.analyzer.parser import parse_heartbeat, numeric_metrics
from tools.analyzer.detector import (
    Detector, FleetMadDetector, ThresholdRule,
)
from tools.analyzer.storage import Storage
from tools.analyzer import config as analyzer_config


# ----- harness --------------------------------------------------------

PASS = 0
FAIL = 0
VERBOSE = False


def check(label: str, cond: bool, detail: str = "") -> None:
    global PASS, FAIL
    if cond:
        PASS += 1
        if VERBOSE:
            print(f"  [PASS] {label}")
    else:
        FAIL += 1
        print(f"  [FAIL] {label}: {detail}")


# ----- parser ---------------------------------------------------------

def test_parser_basic():
    line = ("up=12345 wall=1777952751 rssi=-65 mem=85432 rst=0 "
            "fw=v1.2.3 script=swarm@8cd2777d net=hotspot "
            "bri=(0<32<64) phys=(800<1200<3000)us rend=(2000<3500<10000)us "
            "interp=(50<200)us astar=(100<400)us "
            "agents=12 seeks_fail=42")
    p = parse_heartbeat(line)
    check("parser: up", p.get("up") == 12345)
    check("parser: rssi (negative)", p.get("rssi") == -65)
    check("parser: mem", p.get("mem") == 85432)
    check("parser: fw (string)", p.get("fw") == "v1.2.3")
    check("parser: script (name@sha)", p.get("script") == "swarm@8cd2777d")
    check("parser: net", p.get("net") == "hotspot")
    check("parser: bri triple",
          (p.get("bri_min"), p.get("bri"), p.get("bri_max")) == (0, 32, 64))
    check("parser: bri_sched absent", "bri_sched" not in p)
    check("parser: phys triple",
          (p.get("phys_avg_us"), p.get("phys_max_us"), p.get("phys_budget_us"))
          == (800, 1200, 3000))
    check("parser: rend triple",
          (p.get("rend_avg_us"), p.get("rend_max_us"), p.get("rend_budget_us"))
          == (2000, 3500, 10000))
    check("parser: interp pair",
          (p.get("interp_avg_us"), p.get("interp_max_us")) == (50, 200))
    check("parser: astar pair",
          (p.get("astar_avg_us"), p.get("astar_max_us")) == (100, 400))
    check("parser: agents", p.get("agents") == 12)
    check("parser: seeks_fail", p.get("seeks_fail") == 42)


def test_parser_with_sched():
    line = "bri=(0<32<64 sched) seeks_fail=0"
    p = parse_heartbeat(line)
    check("parser: bri_sched=sched",
          p.get("bri_sched") == "sched" and p.get("bri") == 32)


def test_parser_with_sched_err():
    line = "bri=(0<32<64 sched-err) seeks_fail=0"
    p = parse_heartbeat(line)
    check("parser: bri_sched=sched-err", p.get("bri_sched") == "sched-err")


def test_parser_with_light():
    line = "agents=5 light=(100<2000<3500)"
    p = parse_heartbeat(line)
    check("parser: light triple",
          (p.get("light_cal_bright"), p.get("light_raw"), p.get("light_cal_dark"))
          == (100, 2000, 3500))


def test_parser_err_consumes_rest():
    line = "agents=5 seeks_fail=2 err=Net:tcp connect failed after 3 retries"
    p = parse_heartbeat(line)
    check("parser: err_cat", p.get("err_cat") == "Net")
    check("parser: err_msg captures spaces",
          p.get("err_msg") == "tcp connect failed after 3 retries")
    check("parser: pre-err fields preserved",
          p.get("agents") == 5 and p.get("seeks_fail") == 2)


def test_parser_empty():
    check("parser: empty string → empty dict", parse_heartbeat("") == {})
    check("parser: whitespace → empty dict", parse_heartbeat("   ") == {})


def test_numeric_metrics():
    p = parse_heartbeat("agents=5 seeks_fail=2 fw=v1 script=x@deadbeef "
                        "bri=(0<32<64 sched)")
    nm = numeric_metrics(p)
    check("numeric_metrics: includes int fields",
          nm.get("agents") == 5 and nm.get("seeks_fail") == 2)
    check("numeric_metrics: excludes strings",
          "fw" not in nm and "script" not in nm and "bri_sched" not in nm)
    check("numeric_metrics: includes triple-derived ints",
          nm.get("bri_min") == 0 and nm.get("bri_max") == 64)


# ----- detectors ------------------------------------------------------

def test_threshold_rule_fires():
    r = ThresholdRule(metric="mem", op="<", value=5000)
    a = r.check("rachel", 100, {"mem": 4000})
    check("threshold: fires when value < threshold", a is not None)
    if a is not None:
        check("threshold: alert payload shape",
              a["device"] == "rachel" and a["metric"] == "mem"
              and a["value"] == 4000)


def test_threshold_rule_does_not_fire():
    r = ThresholdRule(metric="mem", op="<", value=5000)
    a = r.check("rachel", 100, {"mem": 6000})
    check("threshold: silent when value >= threshold", a is None)


def test_threshold_rule_missing_metric():
    r = ThresholdRule(metric="not_present", op=">", value=0)
    a = r.check("rachel", 100, {"mem": 6000})
    check("threshold: missing metric → no alert", a is None)


def test_fleet_mad_warmup():
    d = FleetMadDetector(metric="seeks_fail", mad_multiplier=4.0,
                         min_devices=3)
    a1 = d.update("a", 100, {"seeks_fail": 0})
    a2 = d.update("b", 100, {"seeks_fail": 0})
    check("fleet_mad: silent before min_devices reached",
          a1 is None and a2 is None)


def test_fleet_mad_outlier():
    d = FleetMadDetector(metric="seeks_fail", mad_multiplier=4.0,
                         min_devices=3, freshness_seconds=1000)
    d.update("a", 100, {"seeks_fail": 1})
    d.update("b", 100, {"seeks_fail": 2})
    d.update("c", 100, {"seeks_fail": 1})
    d.update("d", 100, {"seeks_fail": 2})
    # Now 4 fresh devices; introduce a 5th outlier.
    a = d.update("bad", 101, {"seeks_fail": 50})
    check("fleet_mad: flags large outlier", a is not None)
    if a is not None:
        check("fleet_mad: payload includes deviation_mads",
              "deviation_mads" in a and a["deviation_mads"] > 4.0)


def test_fleet_mad_skips_zero_mad():
    d = FleetMadDetector(metric="seeks_fail", mad_multiplier=4.0,
                         min_devices=3)
    # Three devices all reporting identical values → MAD=0; should NOT
    # treat any small deviation as infinite-sigma noise.
    for dev, val in [("a", 5), ("b", 5), ("c", 5)]:
        d.update(dev, 100, {"seeks_fail": val})
    a = d.update("d", 100, {"seeks_fail": 6})
    check("fleet_mad: zero MAD → silent", a is None)


def test_fleet_mad_freshness():
    d = FleetMadDetector(metric="seeks_fail", mad_multiplier=4.0,
                         min_devices=3, freshness_seconds=10)
    d.update("a", 100, {"seeks_fail": 1})
    d.update("b", 100, {"seeks_fail": 1})
    d.update("c", 100, {"seeks_fail": 1})
    # 30 seconds later, only one device reports — others have aged out.
    a = d.update("a", 130, {"seeks_fail": 99})
    check("fleet_mad: stale fleet doesn't anchor median", a is None)


def test_detector_composite():
    det = Detector(
        threshold_rules=[ThresholdRule(metric="mem", op="<", value=5000)],
        fleet_rules=[FleetMadDetector(metric="seeks_fail", min_devices=3)],
    )
    # Warm fleet detector with non-zero MAD (varied baseline values).
    det.evaluate("a", 100, {"mem": 9000, "seeks_fail": 1})
    det.evaluate("b", 100, {"mem": 9000, "seeks_fail": 2})
    det.evaluate("c", 100, {"mem": 9000, "seeks_fail": 3})
    # Both rules should fire for `bad`
    alerts = det.evaluate("bad", 100, {"mem": 1000, "seeks_fail": 99})
    rules_fired = {a["rule"] for a in alerts}
    check("detector: composite fires both rules",
          any("threshold" in r for r in rules_fired)
          and any("fleet_mad" in r for r in rules_fired),
          detail=f"rules_fired={rules_fired}")


# ----- storage --------------------------------------------------------

def test_storage_metrics_roundtrip():
    with tempfile.TemporaryDirectory() as td:
        s = Storage(Path(td) / "t.sqlite")
        s.record_metrics("rachel", 100, {"mem": 8000, "agents": 5})
        s.record_metrics("rachel", 130, {"mem": 7000, "agents": 5})
        rows = s.recent_metric("rachel", "mem", n=5)
        check("storage: recent_metric returns DESC ts",
              len(rows) == 2 and rows[0][0] == 130 and rows[0][1] == 7000.0)
        s.close()


def test_storage_alerts_roundtrip():
    with tempfile.TemporaryDirectory() as td:
        s = Storage(Path(td) / "t.sqlite")
        s.record_alert({
            "device": "rachel", "ts": 100, "metric": "mem",
            "rule": "heap_low", "value": 4000, "detail": "mem=4000",
        })
        recent = s.recent_alerts(n=5)
        check("storage: recent_alerts returns row",
              len(recent) == 1 and recent[0]["rule"] == "heap_low")
        check("storage: last_alert_ts works",
              s.last_alert_ts("rachel", "heap_low") == 100)
        s.close()


def test_storage_prune():
    with tempfile.TemporaryDirectory() as td:
        s = Storage(Path(td) / "t.sqlite")
        s.record_metrics("a", 100, {"mem": 1})
        s.record_metrics("a", 200, {"mem": 2})
        s.record_metrics("a", 300, {"mem": 3})
        deleted = s.prune_metrics(keep_seconds=100, now=300)
        check("storage: prune deletes pre-cutoff rows", deleted == 1)
        check("storage: post-prune query intact",
              len(s.recent_metric("a", "mem", n=5)) == 2)
        s.close()


# ----- config ---------------------------------------------------------

def test_config_load_minimal():
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "c.yaml"
        p.write_text(
            "queues:\n"
            "  - topic: critterchron/public/heartbeep\n"
            "threshold_rules:\n"
            "  - metric: mem\n"
            "    op: '<'\n"
            "    value: 5000\n"
        )
        cfg = analyzer_config.load(p)
        check("config: loads minimal yaml",
              len(cfg["queues"]) == 1 and len(cfg["detector"].threshold_rules) == 1)
        check("config: default sink installed when none specified",
              len(cfg["sinks"]) == 1)


def test_config_rejects_missing_queues():
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "c.yaml"
        p.write_text("queues: []\n")
        try:
            analyzer_config.load(p)
            check("config: empty queues raises", False, "no exception raised")
        except analyzer_config.ConfigError:
            check("config: empty queues raises", True)


def test_config_rejects_bad_threshold():
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "c.yaml"
        p.write_text(
            "queues:\n  - topic: x\n"
            "threshold_rules:\n  - metric: mem\n    op: '<'\n"  # missing value
        )
        try:
            analyzer_config.load(p)
            check("config: threshold missing value raises", False)
        except analyzer_config.ConfigError:
            check("config: threshold missing value raises", True)


def test_config_example_loads():
    """The shipped analyzer.example.yaml must always parse."""
    p = _HERE / "analyzer.example.yaml"
    if not p.is_file():
        check("config: example yaml present", False, "missing file")
        return
    cfg = analyzer_config.load(p)
    check("config: example yaml loads",
          len(cfg["queues"]) >= 1
          and len(cfg["detector"].threshold_rules) >= 1
          and len(cfg["detector"].fleet_rules) >= 1)


# ----- runner ---------------------------------------------------------

ALL_TESTS = [
    test_parser_basic,
    test_parser_with_sched,
    test_parser_with_sched_err,
    test_parser_with_light,
    test_parser_err_consumes_rest,
    test_parser_empty,
    test_numeric_metrics,
    test_threshold_rule_fires,
    test_threshold_rule_does_not_fire,
    test_threshold_rule_missing_metric,
    test_fleet_mad_warmup,
    test_fleet_mad_outlier,
    test_fleet_mad_skips_zero_mad,
    test_fleet_mad_freshness,
    test_detector_composite,
    test_storage_metrics_roundtrip,
    test_storage_alerts_roundtrip,
    test_storage_prune,
    test_config_load_minimal,
    test_config_rejects_missing_queues,
    test_config_rejects_bad_threshold,
    test_config_example_loads,
]


def main(argv: list[str]) -> int:
    global VERBOSE
    if "-v" in argv or "--verbose" in argv:
        VERBOSE = True
    for t in ALL_TESTS:
        if VERBOSE:
            print(f"--- {t.__name__} ---")
        try:
            t()
        except Exception as e:
            global FAIL
            FAIL += 1
            print(f"  [FAIL] {t.__name__} raised: {e!r}")
    total = PASS + FAIL
    print(f"\n{PASS}/{total} passed" + ("" if FAIL == 0 else f" — {FAIL} failed"))
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
