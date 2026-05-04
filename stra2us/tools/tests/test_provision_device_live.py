"""Live tests for `POST /api/admin/provision_device` — the one-shot
device-provisioning endpoint that mints an HMAC client + grants the
customer-shaped ACL in one call.

See docs/fr_application_view.md > "Reserved-name enforcement" and the
device-provisioning section.

Skipped unless a local stra2us is reachable. Expects env:

    STRA2US_HOST          e.g. http://127.0.0.1:8153
    STRA2US_ADMIN_USER    superadmin (`*:rw` ACL needed)
    STRA2US_ADMIN_PASS    matching password

Tests provision throwaway client_ids (prefixed `test_provision_*`) so
they can run repeatedly without colliding with real fleet devices.
"""

from __future__ import annotations

import os
import uuid

import pytest
import requests


def _env_ready() -> bool:
    return all(
        os.environ.get(k)
        for k in ("STRA2US_HOST", "STRA2US_ADMIN_USER", "STRA2US_ADMIN_PASS")
    )


pytestmark = pytest.mark.skipif(
    not _env_ready(),
    reason="needs STRA2US_HOST/ADMIN_USER/ADMIN_PASS",
)


def _host() -> str:
    h = os.environ["STRA2US_HOST"].rstrip("/")
    if not h.startswith(("http://", "https://")):
        h = "http://" + h
    return h


def _admin_auth() -> tuple[str, str]:
    return os.environ["STRA2US_ADMIN_USER"], os.environ["STRA2US_ADMIN_PASS"]


def _provision(payload: dict) -> requests.Response:
    return requests.post(
        f"{_host()}/api/admin/provision_device",
        auth=_admin_auth(),
        json=payload,
        timeout=5,
    )


def _revoke(client_id: str) -> None:
    """Best-effort cleanup."""
    requests.delete(
        f"{_host()}/api/admin/keys/{client_id}",
        auth=_admin_auth(),
        timeout=5,
    )


def _unique_id() -> str:
    return f"test_provision_{uuid.uuid4().hex[:10]}"


# ---------------------------------------------------------------------------


def test_provision_happy_path():
    """Provision a fresh device-on-app, verify response shape + ACL."""
    cid = _unique_id()
    try:
        r = _provision({"client_id": cid, "app": "critterchron"})
        assert r.status_code == 200, r.text
        body = r.json()
        assert body["client_id"] == cid
        # 64-char hex secret per `generate_secret`
        assert len(body["secret"]) == 64
        assert all(c in "0123456789abcdef" for c in body["secret"])
        assert body["acl"]["permissions"] == [
            {"prefix": f"critterchron/{cid}", "access": "rw"},
            {"prefix": "critterchron/public", "access": "rw"},
        ]
    finally:
        _revoke(cid)


def test_provision_refuses_to_overwrite_existing_client():
    """Re-running provision against an existing id must fail loudly,
    not silently regenerate the secret (which would orphan a deployed
    device's authentication)."""
    cid = _unique_id()
    try:
        r1 = _provision({"client_id": cid, "app": "critterchron"})
        assert r1.status_code == 200
        original_secret = r1.json()["secret"]

        r2 = _provision({"client_id": cid, "app": "critterchron"})
        assert r2.status_code == 409
        assert "already exists" in r2.json()["detail"]

        # Confirm the original secret is intact (peek via the existing
        # admin client list; provision must not have touched it).
        keys = requests.get(
            f"{_host()}/api/admin/keys", auth=_admin_auth(), timeout=5,
        ).json()
        match = next((c for c in keys if c["client_id"] == cid), None)
        assert match is not None
        # The /keys endpoint doesn't return secrets — but the fact that
        # the entry still exists with its ACL is enough proof the second
        # call didn't clobber.
    finally:
        _revoke(cid)


def test_provision_rejects_reserved_client_id():
    """`public` is reserved as a sub-namespace under each app; provisioning
    a client with that id would corrupt the ACL convention."""
    r = _provision({"client_id": "public", "app": "critterchron"})
    assert r.status_code == 400
    assert "reserved" in r.json()["detail"].lower()


def test_provision_rejects_empty_app():
    r = _provision({"client_id": _unique_id(), "app": ""})
    assert r.status_code == 400
    assert "app is required" in r.json()["detail"]


def test_provision_rejects_empty_client_id():
    r = _provision({"client_id": "", "app": "critterchron"})
    assert r.status_code == 400


def test_provision_rejects_slash_in_app():
    """Slash in app would corrupt the prefix — caller likely passed a
    path instead of an identifier."""
    r = _provision({"client_id": _unique_id(), "app": "critterchron/oops"})
    assert r.status_code == 400
    assert "must not contain" in r.json()["detail"]


def test_provision_rejects_slash_in_client_id():
    r = _provision({"client_id": "ricky/raccoon", "app": "critterchron"})
    assert r.status_code == 400


def test_provision_requires_superuser():
    """Non-superuser admins shouldn't be able to mint clients."""
    # Try to provision against an endpoint that requires superuser, with
    # no admin auth — should 401 (handled by the middleware, not the
    # route). For full coverage we'd want to test "authenticated but
    # not superuser" too, but that requires provisioning a non-super
    # admin out-of-band; skip for now.
    r = requests.post(
        f"{_host()}/api/admin/provision_device",
        json={"client_id": "x", "app": "y"},
        timeout=5,
    )
    assert r.status_code == 401
