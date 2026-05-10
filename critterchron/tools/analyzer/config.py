"""YAML config loader for the analyzer.

The schema is documented by `analyzer.example.yaml`; this file does
shallow validation (catch typos and missing required fields) and
returns a dict the runner can act on directly.
"""

from __future__ import annotations

from pathlib import Path

import yaml

from .detector import Detector, FleetMadDetector, ThresholdRule
from .sinks import build_sinks


class ConfigError(RuntimeError):
    pass


def load(path: str | Path) -> dict:
    p = Path(path)
    if not p.is_file():
        raise ConfigError(f"config not found: {p}")
    with p.open("r") as fh:
        doc = yaml.safe_load(fh) or {}
    if not isinstance(doc, dict):
        raise ConfigError(f"{p}: top-level YAML must be a mapping")

    queues = doc.get("queues") or []
    if not isinstance(queues, list) or not queues:
        raise ConfigError(f"{p}: `queues` must be a non-empty list")
    for q in queues:
        if not isinstance(q, dict) or "topic" not in q:
            raise ConfigError(f"{p}: each `queues` entry needs a `topic`")

    detector = Detector(
        threshold_rules=[_build_threshold(r) for r in doc.get("threshold_rules") or []],
        fleet_rules=[_build_fleet(r) for r in doc.get("fleet_rules") or []],
    )

    return {
        "queues": queues,
        "poll_interval_seconds": float(doc.get("poll_interval_seconds", 5.0)),
        "storage_path": doc.get("storage_path", "analyzer.sqlite"),
        "metrics_retention_seconds": int(doc.get("metrics_retention_seconds", 24 * 3600)),
        # How long to suppress repeated emit of the same (device, rule) alert
        # to sinks. Defaults to 60s — long enough to dedup the "rule fires
        # every heartbeat" pattern, short enough that genuinely persistent
        # issues still surface periodically. sqlite alert log is unaffected;
        # every alert is still persisted for forensics.
        "alert_dedup_seconds": int(doc.get("alert_dedup_seconds", 60)),
        "detector": detector,
        "sinks": build_sinks(doc.get("sinks") or [{"type": "log"}]),
        "auto_dump": doc.get("auto_dump") or {},  # {enabled, cooldown_seconds}
    }


def _build_threshold(spec: dict) -> ThresholdRule:
    for k in ("metric", "op", "value"):
        if k not in spec:
            raise ConfigError(f"threshold rule missing `{k}`: {spec}")
    return ThresholdRule(
        metric=spec["metric"],
        op=spec["op"],
        value=float(spec["value"]),
        name=spec.get("name", ""),
        auto_dump=bool(spec.get("auto_dump", False)),
    )


def _build_fleet(spec: dict) -> FleetMadDetector:
    if "metric" not in spec:
        raise ConfigError(f"fleet rule missing `metric`: {spec}")
    return FleetMadDetector(
        metric=spec["metric"],
        mad_multiplier=float(spec.get("mad_multiplier", 4.0)),
        min_devices=int(spec.get("min_devices", 3)),
        freshness_seconds=int(spec.get("freshness_seconds", 300)),
        auto_dump=bool(spec.get("auto_dump", False)),
        name=spec.get("name", ""),
    )
