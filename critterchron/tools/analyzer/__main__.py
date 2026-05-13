"""Analyzer runner — `python3 -m tools.analyzer --config <yaml>`.

Long-running process. Polls each configured queue, parses heartbeats,
feeds the detector, persists metrics + alerts, dispatches alerts to
sinks, and (optionally) writes `dump_now: 1` to KV to trigger §1's
on-device snapshot path.

Stra2us auth: the analyzer is a stra2us client like any other. Reads
credentials from env (STRA2US_HOST / STRA2US_CLIENT_ID /
STRA2US_SECRET_HEX) or CLI flags. Its client_id needs read ACL on
every queue topic in the config plus write ACL on `<app>/<device>/dump_now`
KV keys if `auto_dump.enabled` is true.

Cursors are server-side per client_id, so the analyzer reads its own
private progression through each stream — running multiple analyzers
(e.g. one per environment) on different client_ids is safe.

Note: a fresh client_id starts at the oldest retained message in the
stream, so first-run drains historical telemetry before catching up
to live. Subsequent runs resume from the last-acked message.
"""

from __future__ import annotations

import argparse
import logging
import sys
import time
from pathlib import Path

# Make the repo root importable when run as `python3 -m tools.analyzer`.
_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parent.parent
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

from tools.s2s_extras import client_from_env, S2sClient, Stra2usError  # noqa: E402

from . import config as analyzer_config  # noqa: E402
from .parser import numeric_metrics, parse_heartbeat  # noqa: E402
from .storage import Storage  # noqa: E402


log = logging.getLogger("analyzer")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="analyzer",
                                 description="critterchron failure-triage analyzer")
    ap.add_argument("--config", required=True, help="path to analyzer YAML")
    ap.add_argument("--server", default=None,
                    help="Stra2us host (overrides $STRA2US_HOST)")
    ap.add_argument("--client-id", default=None,
                    help="analyzer client_id (overrides $STRA2US_CLIENT_ID)")
    ap.add_argument("--secret", default=None,
                    help="analyzer secret hex (overrides $STRA2US_SECRET_HEX)")
    ap.add_argument("--once", action="store_true",
                    help="drain the queues once and exit; useful for tests")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(name)s %(levelname)s %(message)s")
    # urllib3 + requests log every HTTP request at DEBUG; under -v the
    # heartbeep poll loop drowns out everything else. Clamp them to
    # INFO so analyzer's own DEBUG output stays readable.
    logging.getLogger("urllib3").setLevel(logging.INFO)
    logging.getLogger("requests").setLevel(logging.INFO)

    try:
        cfg = analyzer_config.load(args.config)
    except analyzer_config.ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return 2

    try:
        client = client_from_env(args.server, args.client_id, args.secret)
    except Stra2usError as e:
        print(f"client error: {e}", file=sys.stderr)
        return 2

    storage = Storage(cfg["storage_path"])
    log.info("analyzer up: %d queues, %d threshold rules, %d fleet rules",
             len(cfg["queues"]),
             len(cfg["detector"].threshold_rules),
             len(cfg["detector"].fleet_rules))

    # Process-local cooldown trackers. Both reset on restart, which is fine
    # — restart is an operator action and we'd rather over-emit once than
    # silently swallow a real failure across analyzer process boundaries.
    #
    # `_alert_cooldown` gates SINK emission (log/webhook) per (device, rule)
    # to prevent the "rule fires every heartbeat" log spam. sqlite alert
    # log is unaffected — every alert is still persisted.
    #
    # `_dump_cooldown` gates auto-dump KV writes; longer interval since a
    # snapshot publish is heavier than a log line.
    cfg["_alert_cooldown"] = {}
    cfg["_dump_cooldown"] = {}

    last_prune = 0
    try:
        while True:
            for q in cfg["queues"]:
                _drain_queue(client, q, cfg, storage)

            now = int(time.time())
            if now - last_prune > 600:  # every 10 min
                deleted = storage.prune_metrics(cfg["metrics_retention_seconds"], now)
                if deleted:
                    log.info("pruned %d old metric rows", deleted)
                last_prune = now

            if args.once:
                return 0
            time.sleep(cfg["poll_interval_seconds"])
    except KeyboardInterrupt:
        log.info("stopped")
        return 0
    finally:
        storage.close()


def _drain_queue(client: S2sClient, q: dict, cfg: dict, storage: Storage) -> None:
    """Pull every available message from one queue, parse, detect, dispatch."""
    topic = q["topic"]
    label = q.get("label", topic)
    while True:
        try:
            msg = client.consume(topic, envelope=True)
        except Stra2usError as e:
            log.error("consume %s failed: %s", topic, e)
            return
        if msg is None:
            return  # 204: empty
        _process(msg, label, cfg, storage, client)


def _process(msg: dict, label: str, cfg: dict,
             storage: Storage, client: S2sClient) -> None:
    """Handle one envelope-wrapped message."""
    device = msg.get("client_id", "unknown")
    ts = int(msg.get("received_at", time.time()))
    raw = msg.get("data")

    # Heartbeat payload is a string. Snapshot/trace topics may carry
    # dicts (§1/§2) — we ignore those here; this drain is for metrics.
    if not isinstance(raw, str):
        log.debug("skipping non-string message on %s from %s (%s)",
                  label, device, type(raw).__name__)
        return

    parsed = parse_heartbeat(raw)
    metrics = numeric_metrics(parsed)
    if not metrics:
        return

    storage.record_metrics(device, ts, metrics)

    alerts = cfg["detector"].evaluate(device, ts, metrics)
    dedup_window = cfg["alert_dedup_seconds"]
    alert_cd: dict = cfg["_alert_cooldown"]
    for alert in alerts:
        alert["queue"] = label
        # Always persist — forensic record is the source of truth, and
        # "when did this start firing" needs every sample.
        storage.record_alert(alert)
        # Emit to sinks only if outside the per-(device, rule) dedup
        # window. First fire always emits; subsequent fires within the
        # window are silently swallowed.
        cd_key = (alert["device"], alert["rule"])
        last_emit = alert_cd.get(cd_key)
        if last_emit is None or (alert["ts"] - last_emit) >= dedup_window:
            alert_cd[cd_key] = alert["ts"]
            for sink in cfg["sinks"]:
                try:
                    sink.emit(alert)
                except Exception as e:  # sink failure shouldn't kill the loop
                    log.exception("sink failed: %s", e)
        if alert.get("auto_dump"):
            _maybe_trigger_dump(alert, cfg, client, storage)


def _maybe_trigger_dump(alert: dict, cfg: dict,
                        client: S2sClient, storage: Storage) -> None:
    """Set `<app>/<device>/dump_now=1` if cooldown allows.

    Cooldown is per-(device, rule) and tracked in-process: a persistent
    failure shouldn't thrash dumps every heartbeat. Default 600s.
    """
    auto = cfg.get("auto_dump") or {}
    if not auto.get("enabled"):
        return
    app = auto.get("app")
    if not app:
        log.error("auto_dump.enabled but auto_dump.app missing — refusing to fire")
        return
    cooldown = int(auto.get("cooldown_seconds", 600))
    cooldowns: dict = cfg["_dump_cooldown"]
    key_t = (alert["device"], alert["rule"])
    last = cooldowns.get(key_t)
    if last is not None and (alert["ts"] - last) < cooldown:
        log.debug("dump_now skipped (cooldown) for %s/%s",
                  alert["device"], alert["rule"])
        return
    kv_key = f"{app}/{alert['device']}/dump_now"
    # Generation-number semantics on the device side: any nonzero int
    # different from the device's last-seen value triggers one dump.
    # Use `int(time.time())` so each auto-dump gets a unique token —
    # if the analyzer fires twice in a second for the same device+rule
    # the cooldown above would already short-circuit; otherwise the
    # second-resolution timestamp is a fine generation token.
    gen = int(time.time())
    try:
        client.put(kv_key, gen)
        cooldowns[key_t] = alert["ts"]
        log.warning("dump_now=%d set for %s (rule=%s)",
                    gen, alert["device"], alert["rule"])
    except Stra2usError as e:
        log.error("dump_now write failed for %s: %s", alert["device"], e)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
