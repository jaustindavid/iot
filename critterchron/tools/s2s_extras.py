"""Critterchron extensions on top of `stra2us_cli.Stra2usClient`.

Upstream `stra2us_cli` covers KV `get` / `put` / `delete`. Critterchron
needs two more things that aren't part of that core surface:

  * **queue consume** — for the analyzer, `snapshot_dump`, and any
    other tool that tails a queue topic (FAILURE_TRIAGE.md §3).
  * **`?ttl=N` on KV writes** — for the `trace_on` safety-net pattern
    (FAILURE_TRIAGE.md §2): set a dangerous knob, let it auto-expire
    server-side if the operator walks away.

Replaces the historical `tools/s2s_client.py`, which carried ~95% of
upstream's HMAC + msgpack + response-sig machinery as a local fork.
The fork drifted: when upstream picked up `bleach` as a transitive
dep for markdown sanitization, the bare-stra2us CLI broke until the
critterchron venv was re-`pip install -e`-d. Inheriting from upstream
means future dep adds + protocol changes ride through automatically.

`client_from_env` keeps the same `(server, client_id, secret_hex)`
positional signature the old local version exposed, so callers don't
need keyword conversion. Internally delegates to upstream's
`resolve_credentials` which adds `~/.stra2us` profile support on top
of bare env vars.
"""

from __future__ import annotations

import time
from urllib.parse import quote

import msgpack
import requests

from stra2us_cli import Stra2usError                  # re-export for callers
from stra2us_cli.client import Stra2usClient as _BaseClient
from stra2us_cli.config import resolve as _resolve_credentials


class S2sClient(_BaseClient):
    """Stra2us client with queue + TTL extensions for critterchron tooling.

    Inherits the full upstream API (`get`, `delete`, signing, response
    verify) unchanged. Overrides `_request` to support an optional
    query string (not part of the HMAC payload — server contract uses
    bare URI for signing). Adds `consume(topic, envelope=True)` and
    extends `put(...)` with an optional `ttl=` kwarg.
    """

    # --- transport: overridden to allow a query string ---------------

    def _request(self, method, uri, body, content_type,
                 extra_headers=None, query_string=None):
        """Like upstream `_request` but appends `?query_string` to the
        request URL. The query is intentionally NOT part of the HMAC
        signing payload — that's the server's contract (it signs over
        `request.url.path` only). Used by `consume` (`?envelope=true`)
        and `put` (`?ttl=N`)."""
        ts = int(time.time())
        sig = self._sign(uri, body, ts)
        headers = {
            "X-Client-ID": self.client_id,
            "X-Timestamp": str(ts),
            "X-Signature": sig,
        }
        if content_type:
            headers["Content-Type"] = content_type
        if extra_headers:
            headers.update(extra_headers)
        url = self.base_url + uri
        if query_string:
            url = url + "?" + query_string
        try:
            r = requests.request(
                method, url, data=body,
                headers=headers, timeout=self.timeout,
            )
        except requests.RequestException as e:
            raise Stra2usError(f"{method} {uri}: {e}") from e
        if 200 <= r.status_code < 300:
            self._verify_resp(uri, r.content, r.headers)
        return r

    # --- KV with TTL --------------------------------------------------

    def put(self, key, value, encrypted=False, ttl=None):
        """POST /kv/<key> with optional server-side TTL.

        Same shape as upstream `put` with an added `ttl` kwarg in
        seconds. Server caps TTL at 1 week (604800). Used by
        `trace_on` so a forgotten trace_mode auto-expires instead of
        running indefinitely."""
        body = msgpack.packb(value, use_bin_type=True)
        extra = {"X-Encrypted": "1"} if encrypted else None
        query = f"ttl={int(ttl)}" if ttl is not None else None
        r = self._request(
            "POST", self._kv_uri(key), body, "application/x-msgpack",
            extra_headers=extra, query_string=query,
        )
        if not (200 <= r.status_code < 300):
            raise Stra2usError(
                f"POST {key} → {r.status_code}: {r.text[:200]}"
            )
        return r

    # --- queue consume ------------------------------------------------

    @staticmethod
    def _q_uri(topic):
        return "/q/" + "/".join(quote(p, safe="") for p in topic.split("/"))

    def consume(self, topic, envelope=True):
        """GET /q/<topic>. Returns the next message or None when the
        queue is empty (HTTP 204).

        With `envelope=True` (default), returns a dict
        ``{"data": <decoded>, "client_id": <publisher>,
        "received_at": <unix>}``. With `envelope=False`, returns the
        raw decoded payload.

        Cursors are tracked server-side per `client_id`, so a
        dedicated analyzer / snapshot-tail client reads its own
        progression through the stream without competing with other
        consumers."""
        query = "envelope=true" if envelope else None
        r = self._request(
            "GET", self._q_uri(topic), b"", None, query_string=query,
        )
        if r.status_code == 204:
            return None
        if not (200 <= r.status_code < 300):
            raise Stra2usError(
                f"GET {topic} → {r.status_code}: {r.text[:200]}"
            )
        return msgpack.unpackb(r.content, raw=False)


def client_from_env(server=None, client_id=None, secret_hex=None,
                    profile=None):
    """Build an `S2sClient` using the same flag → env → profile lookup
    upstream's CLI uses. Mirrors `stra2us_cli.client_from_env` but
    returns the critterchron-extended client and accepts the first
    three arguments positionally (matches the historical local
    `client_from_env` signature so callers don't need to change)."""
    creds = _resolve_credentials(
        server=server,
        client_id=client_id,
        secret_hex=secret_hex,
        profile=profile,
    )
    return S2sClient(
        base_url=creds.base_url,
        client_id=creds.client_id,
        secret_hex=creds.secret_hex,
    )
