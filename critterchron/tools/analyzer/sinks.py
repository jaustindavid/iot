"""Alert sinks — where alerts get sent.

Two sinks today:
  log     — emit a structured line via the stdlib logger.
  webhook — POST the alert as JSON to a URL (Slack/Discord-compatible).

Sinks are constructed from the YAML config and called in order. A
sink failure is logged but doesn't stop the others — alerting is
best-effort, not the source of truth (sqlite is).
"""

from __future__ import annotations

import json
import logging
from dataclasses import dataclass

import requests


log = logging.getLogger("analyzer.sinks")


class Sink:
    def emit(self, alert: dict) -> None:
        raise NotImplementedError


@dataclass
class LogSink(Sink):
    level: str = "WARNING"

    def emit(self, alert: dict) -> None:
        lvl = getattr(logging, self.level.upper(), logging.WARNING)
        msg = (f"ALERT device={alert['device']} rule={alert['rule']} "
               f"detail={alert.get('detail', '')}")
        log.log(lvl, msg)


@dataclass
class WebhookSink(Sink):
    url: str
    timeout: float = 5.0
    # Slack-incoming-webhook compatibility: wrap in {"text": "..."}.
    # Set False to send the raw alert dict (Discord, custom endpoints).
    slack_text: bool = True

    def emit(self, alert: dict) -> None:
        if self.slack_text:
            text = (f":rotating_light: *{alert['rule']}* on `{alert['device']}`\n"
                    f"{alert.get('detail', '')}")
            body = {"text": text}
        else:
            body = alert
        try:
            r = requests.post(self.url, json=body, timeout=self.timeout)
            if r.status_code >= 400:
                log.error("webhook %s -> %d: %s", self.url, r.status_code,
                          r.text[:200])
        except requests.RequestException as e:
            log.error("webhook %s failed: %s", self.url, e)


def build_sinks(specs: list) -> list[Sink]:
    """Build sinks from a list of `{type: ..., ...}` config dicts."""
    out: list[Sink] = []
    for spec in specs or []:
        kind = spec.get("type")
        if kind == "log":
            out.append(LogSink(level=spec.get("level", "WARNING")))
        elif kind == "webhook":
            url = spec.get("url")
            if not url:
                raise ValueError("webhook sink missing `url`")
            out.append(WebhookSink(
                url=url,
                timeout=float(spec.get("timeout", 5.0)),
                slack_text=bool(spec.get("slack_text", True)),
            ))
        else:
            raise ValueError(f"unknown sink type {kind!r}")
    return out
