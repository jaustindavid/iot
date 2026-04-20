"""Minimal Stra2us HTTP client for dev tooling.

Mirrors the device-side signing protocol in
hal/particle/src/Stra2usClient.cpp:
  - Request sig:  HMAC_SHA256(secret_bytes, uri + body + ts_str)
  - Headers:      X-Client-ID, X-Timestamp, X-Signature
  - Response sig: HMAC_SHA256 over uri + response_body + resp_ts,
                  in X-Response-Signature / X-Response-Timestamp

Values in /kv are msgpack-encoded. Passing a str here sends a msgpack
string; passing bytes sends msgpack bin. GET returns the decoded value.

Not async, not streaming, not retrying — the upload tool runs once per
invocation and failures should surface loudly.
"""

from __future__ import annotations
import hashlib  # noqa: F401  — handy for callers computing sha256
import hmac
import os
import time
from dataclasses import dataclass
from urllib.parse import quote

import msgpack
import requests


CLOCK_DRIFT_SECONDS = 300  # must match server + device policy


class Stra2usError(RuntimeError):
    pass


@dataclass
class S2sClient:
    base_url: str                 # e.g. "http://stra2us.austindavid.com:8153"
    client_id: str
    secret_hex: str               # 64-char hex → 32-byte secret
    timeout: float = 10.0
    verify_response: bool = True

    # ----- signing helpers -----

    def _secret_bytes(self) -> bytes:
        return bytes.fromhex(self.secret_hex)

    def _sign(self, uri: str, body: bytes, ts: int) -> str:
        payload = uri.encode("utf-8") + body + str(ts).encode("utf-8")
        return hmac.new(self._secret_bytes(), payload, hashlib.sha256).hexdigest()

    def _verify_resp(self, uri: str, body: bytes, headers) -> None:
        if not self.verify_response:
            return
        ts  = headers.get("X-Response-Timestamp")
        sig = headers.get("X-Response-Signature")
        if not ts or not sig:
            raise Stra2usError("Response missing signing headers")
        now = int(time.time())
        if abs(now - int(ts)) > CLOCK_DRIFT_SECONDS:
            raise Stra2usError(f"Response timestamp drift too large ({now - int(ts)}s)")
        payload = uri.encode("utf-8") + body + ts.encode("utf-8")
        expected = hmac.new(self._secret_bytes(), payload, hashlib.sha256).hexdigest()
        if not hmac.compare_digest(expected, sig):
            raise Stra2usError("Response signature mismatch")

    # ----- request core -----

    def _request(self, method: str, uri: str, body: bytes,
                 content_type: str | None) -> requests.Response:
        ts  = int(time.time())
        sig = self._sign(uri, body, ts)
        headers = {
            "X-Client-ID": self.client_id,
            "X-Timestamp": str(ts),
            "X-Signature": sig,
        }
        if content_type:
            headers["Content-Type"] = content_type
        r = requests.request(method, self.base_url + uri, data=body,
                             headers=headers, timeout=self.timeout)
        if 200 <= r.status_code < 300:
            self._verify_resp(uri, r.content, r.headers)
        return r

    # ----- KV API -----

    @staticmethod
    def _kv_uri(key: str) -> str:
        # Per-segment URL-encoding so slashes in the key stay as path
        # separators but unusual chars (shouldn't appear, but defensive)
        # get escaped.
        return "/kv/" + "/".join(quote(p, safe="") for p in key.split("/"))

    def put(self, key: str, value) -> requests.Response:
        """Write /kv/<key>. Server uses POST for writes (not PUT); name kept
        as `put` for caller ergonomics. `value` is msgpack-encoded first."""
        body = msgpack.packb(value, use_bin_type=True)
        r = self._request("POST", self._kv_uri(key), body, "application/x-msgpack")
        if not (200 <= r.status_code < 300):
            raise Stra2usError(f"POST {key} → {r.status_code}: {r.text[:200]}")
        return r

    def get(self, key: str):
        """GET /kv/<key>. Returns the msgpack-decoded value, or None on 404."""
        r = self._request("GET", self._kv_uri(key), b"", None)
        if r.status_code == 404:
            return None
        if not (200 <= r.status_code < 300):
            raise Stra2usError(f"GET {key} → {r.status_code}: {r.text[:200]}")
        return msgpack.unpackb(r.content, raw=False)


# ----- credential lookup -----

def client_from_env(base_url: str | None = None,
                    client_id: str | None = None,
                    secret_hex: str | None = None) -> S2sClient:
    """Build an S2sClient, falling back to env vars.

    Required env vars (if not passed explicitly):
      STRA2US_HOST         e.g. "stra2us.austindavid.com:8153"
                           (override with --server http://host:port)
      STRA2US_CLIENT_ID    publisher client id
      STRA2US_SECRET_HEX   64-char hex

    `base_url` may be a bare "host:port" (→ http://) or a full URL.
    """
    url = base_url or os.environ.get("STRA2US_HOST", "")
    if not url:
        raise Stra2usError("No server URL (pass --server or set STRA2US_HOST)")
    if not url.startswith(("http://", "https://")):
        url = "http://" + url
    url = url.rstrip("/")

    cid = client_id or os.environ.get("STRA2US_CLIENT_ID")
    sec = secret_hex or os.environ.get("STRA2US_SECRET_HEX")
    if not cid or not sec:
        raise Stra2usError("Missing client_id / secret_hex (pass flags or set env)")

    return S2sClient(base_url=url, client_id=cid, secret_hex=sec)
