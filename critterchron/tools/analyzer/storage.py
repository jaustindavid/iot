"""sqlite-backed storage for the analyzer.

Two tables:
  metrics — per-device per-tick numeric readings, capped to a rolling
            window via timestamp pruning.
  alerts  — every alert emitted, never pruned automatically (let
            operators rotate the file when it gets large).

The schema is wide-and-flat-keyed: `(device, ts, metric)` rather than
one column per metric. Heartbeats keep adding fields; a vertical schema
absorbs that without migrations.

Concurrency: single-writer assumed (one analyzer process). sqlite's
default journaling is fine.
"""

from __future__ import annotations

import json
import sqlite3
import time
from contextlib import contextmanager
from pathlib import Path


_SCHEMA = """
CREATE TABLE IF NOT EXISTS metrics (
    device TEXT NOT NULL,
    ts     INTEGER NOT NULL,
    metric TEXT NOT NULL,
    value  REAL NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_metrics_device_ts ON metrics(device, ts);
CREATE INDEX IF NOT EXISTS idx_metrics_ts ON metrics(ts);

CREATE TABLE IF NOT EXISTS alerts (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    ts        INTEGER NOT NULL,
    device    TEXT NOT NULL,
    metric    TEXT,
    rule      TEXT NOT NULL,
    value     REAL,
    payload   TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_alerts_ts ON alerts(ts);
CREATE INDEX IF NOT EXISTS idx_alerts_device_rule ON alerts(device, rule);
"""


class Storage:
    def __init__(self, path: str | Path):
        self.path = str(path)
        Path(self.path).parent.mkdir(parents=True, exist_ok=True)
        self.conn = sqlite3.connect(self.path, isolation_level=None)
        self.conn.row_factory = sqlite3.Row
        self.conn.executescript(_SCHEMA)

    def close(self) -> None:
        self.conn.close()

    @contextmanager
    def _tx(self):
        self.conn.execute("BEGIN")
        try:
            yield
            self.conn.execute("COMMIT")
        except Exception:
            self.conn.execute("ROLLBACK")
            raise

    # ----- writes -----

    def record_metrics(self, device: str, ts: int, metrics: dict) -> None:
        rows = [(device, ts, k, float(v)) for k, v in metrics.items()]
        if not rows:
            return
        with self._tx():
            self.conn.executemany(
                "INSERT INTO metrics(device, ts, metric, value) "
                "VALUES (?,?,?,?)", rows)

    def record_alert(self, alert: dict) -> int:
        with self._tx():
            cur = self.conn.execute(
                "INSERT INTO alerts(ts, device, metric, rule, value, payload) "
                "VALUES (?,?,?,?,?,?)",
                (alert.get("ts", int(time.time())),
                 alert["device"],
                 alert.get("metric"),
                 alert["rule"],
                 alert.get("value"),
                 json.dumps(alert, default=str)))
            return cur.lastrowid

    def prune_metrics(self, keep_seconds: int, now: int | None = None) -> int:
        """Delete rows older than `now - keep_seconds`. Returns rowcount."""
        cutoff = (now if now is not None else int(time.time())) - keep_seconds
        cur = self.conn.execute("DELETE FROM metrics WHERE ts < ?", (cutoff,))
        return cur.rowcount

    # ----- reads (mostly for tests / ops queries) -----

    def recent_metric(self, device: str, metric: str, n: int = 10) -> list[tuple]:
        cur = self.conn.execute(
            "SELECT ts, value FROM metrics "
            "WHERE device=? AND metric=? ORDER BY ts DESC LIMIT ?",
            (device, metric, n))
        return [(r["ts"], r["value"]) for r in cur.fetchall()]

    def recent_alerts(self, n: int = 20) -> list[dict]:
        cur = self.conn.execute(
            "SELECT ts, device, metric, rule, value, payload FROM alerts "
            "ORDER BY ts DESC LIMIT ?", (n,))
        return [{"ts": r["ts"], "device": r["device"], "metric": r["metric"],
                 "rule": r["rule"], "value": r["value"],
                 "payload": json.loads(r["payload"])} for r in cur.fetchall()]

    def last_alert_ts(self, device: str, rule: str) -> int | None:
        """Used by the runner for `dump_now` rate-limiting."""
        cur = self.conn.execute(
            "SELECT ts FROM alerts WHERE device=? AND rule=? "
            "ORDER BY ts DESC LIMIT 1", (device, rule))
        row = cur.fetchone()
        return row["ts"] if row else None
