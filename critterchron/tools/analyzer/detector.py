"""Detection logic — threshold rules + fleet-MAD outliers.

Inputs: a stream of `(device, ts, metrics)` records, where `metrics`
is a flat numeric dict from `parser.numeric_metrics`. Output: alert
dicts with the shape documented in FAILURE_TRIAGE.md §3.

Detection classes implemented:

  1. ThresholdRule — static `metric op value` checks. Cheap, blunt,
     useful for absolute floors/ceilings (heap_free < 5000).
  2. FleetMadDetector — robust outlier per metric across the current
     fleet snapshot. Median + MAD; flag any device whose metric is
     >K MADs from the fleet median. Needs `min_devices` samples in
     the current window before firing.

Per-device temporal outliers (FAILURE_TRIAGE.md §3 detection class 3)
are deferred until we know which metrics are worth tracking
temporally — see the doc.
"""

from __future__ import annotations

import statistics
import time
from dataclasses import dataclass, field
from typing import Iterable


@dataclass
class ThresholdRule:
    metric: str
    op: str          # one of: "<", "<=", ">", ">=", "==", "!="
    value: float
    name: str = ""   # human-readable rule name; defaults to f"{metric}_{op}_{value}"
    auto_dump: bool = False

    def __post_init__(self):
        if self.op not in ("<", "<=", ">", ">=", "==", "!="):
            raise ValueError(f"ThresholdRule: bad op {self.op!r}")
        if not self.name:
            self.name = f"threshold:{self.metric}{self.op}{self.value}"

    def check(self, device: str, ts: int, metrics: dict) -> dict | None:
        if self.metric not in metrics:
            return None
        v = metrics[self.metric]
        if not _cmp(v, self.op, self.value):
            return None
        return {
            "device": device,
            "ts": ts,
            "metric": self.metric,
            "rule": self.name,
            "value": v,
            "detail": f"{self.metric}={v} (rule {self.op} {self.value})",
            "auto_dump": self.auto_dump,
        }


def _cmp(v, op, t) -> bool:
    if op == "<":  return v <  t
    if op == "<=": return v <= t
    if op == ">":  return v >  t
    if op == ">=": return v >= t
    if op == "==": return v == t
    if op == "!=": return v != t
    raise ValueError(op)


@dataclass
class FleetMadDetector:
    """Flag a device whose `metric` is >K MADs from the fleet median.

    Holds a small per-device cache of the most recent reading. On each
    new sample, recompute the fleet median + MAD across the current
    snapshot (one reading per device) and check the just-arrived
    sample against it.

    `min_devices` gates firing — until that many distinct devices
    have reported, the fleet stats are too thin to trust.

    `freshness_seconds` ages out stale device readings so a long-dead
    device doesn't anchor the fleet median forever.
    """
    metric: str
    mad_multiplier: float = 4.0
    min_devices: int = 3
    freshness_seconds: int = 300
    auto_dump: bool = False
    name: str = ""

    # Internal: device → (ts, value)
    _last: dict = field(default_factory=dict, repr=False)

    def __post_init__(self):
        if not self.name:
            self.name = f"fleet_mad:{self.metric}"

    def update(self, device: str, ts: int, metrics: dict) -> dict | None:
        if self.metric not in metrics:
            return None
        v = metrics[self.metric]
        self._last[device] = (ts, v)

        # Drop stale entries before computing fleet stats.
        cutoff = ts - self.freshness_seconds
        fresh = {d: (t, val) for d, (t, val) in self._last.items() if t >= cutoff}
        if len(fresh) < self.min_devices:
            return None

        values = [val for _, val in fresh.values()]
        median = statistics.median(values)
        mad = statistics.median([abs(x - median) for x in values])
        # Degenerate fleet (everyone identical): no outlier signal.
        if mad == 0:
            return None
        deviation = abs(v - median) / mad
        if deviation < self.mad_multiplier:
            return None
        return {
            "device": device,
            "ts": ts,
            "metric": self.metric,
            "rule": self.name,
            "value": v,
            "fleet_median": median,
            "fleet_mad": mad,
            "deviation_mads": round(deviation, 2),
            "fleet_size": len(fresh),
            "detail": (f"{self.metric}={v} vs fleet median={median} "
                       f"mad={mad} ({deviation:.1f} MADs)"),
            "auto_dump": self.auto_dump,
        }


@dataclass
class StalenessDetector:
    """Flag a device whose `metric` hasn't changed across N consecutive
    samples while `up` is advancing.

    Catches the "frozen engine, live heartbeat" class — the failure mode
    where the telemetry thread keeps publishing but the physics tick is
    wedged, so engine-derived metrics (`seeks_fail`, `phys_*`, etc.) go
    bit-for-bit identical from one heartbeat to the next. Threshold
    rules and fleet-MAD outliers both miss this — they look at *values*,
    not at whether values are *changing*. See FAILURE_TRIAGE.md §3
    "where the gap is" + the debug_frozen_engine_live_heartbeat memory
    note.

    The `up` advance check is the load-bearing distinguisher. A truly
    offline device just stops sending heartbeats; the analyzer never
    sees its samples and we don't false-positive. A frozen-engine
    device keeps publishing, with `up=` and `wall=` ticking forward
    while engine metrics flatline. So: only fire when `up` is advancing
    *and* the watched metric is stuck.

    Choose metrics that are naturally jittery on a healthy device.
    `phys_max_us`, `interp_max_us`, `seeks_fail` are good candidates.
    `agents` is borderline (steady-state for many scripts). `heap_free`
    can be too noisy/quiet depending on workload.
    """
    metric: str
    min_unchanged_samples: int = 5
    require_up_advancing: bool = True
    name: str = ""
    auto_dump: bool = False

    # Per-device state: device → (last_value, unchanged_count, last_up)
    _state: dict = field(default_factory=dict, repr=False)

    def __post_init__(self):
        if not self.name:
            self.name = f"staleness:{self.metric}"

    def update(self, device: str, ts: int, metrics: dict) -> dict | None:
        if self.metric not in metrics:
            return None
        v = metrics[self.metric]
        up_now = metrics.get("up")
        prev = self._state.get(device)

        if prev is None:
            # First observation for this device. Stash baseline; can't
            # decide staleness yet.
            self._state[device] = (v, 1, up_now)
            return None

        prev_v, prev_count, prev_up = prev

        if v != prev_v:
            # Value changed → reset the streak.
            self._state[device] = (v, 1, up_now)
            return None

        # Value unchanged → extend the streak.
        new_count = prev_count + 1
        self._state[device] = (v, new_count, up_now)

        if new_count < self.min_unchanged_samples:
            return None

        if self.require_up_advancing:
            # Distinguish frozen-engine (up advancing, metric stuck)
            # from offline/clock-stalled (up not advancing). If we
            # can't tell from this sample's `up` field, stay silent.
            if up_now is None or prev_up is None:
                return None
            if up_now <= prev_up:
                return None

        return {
            "device": device,
            "ts": ts,
            "metric": self.metric,
            "rule": self.name,
            "value": v,
            "unchanged_samples": new_count,
            "up_now": up_now,
            "up_prev": prev_up,
            "detail": (f"{self.metric}={v} unchanged across {new_count} samples "
                       f"while up advanced ({prev_up}→{up_now})"),
            "auto_dump": self.auto_dump,
        }


@dataclass
class Detector:
    """Composite — runs all configured rules per sample."""
    threshold_rules: list = field(default_factory=list)
    fleet_rules: list = field(default_factory=list)
    staleness_rules: list = field(default_factory=list)

    def evaluate(self, device: str, ts: int, metrics: dict) -> list[dict]:
        out = []
        for r in self.threshold_rules:
            a = r.check(device, ts, metrics)
            if a is not None:
                out.append(a)
        for r in self.fleet_rules:
            a = r.update(device, ts, metrics)
            if a is not None:
                out.append(a)
        for r in self.staleness_rules:
            a = r.update(device, ts, metrics)
            if a is not None:
                out.append(a)
        return out
