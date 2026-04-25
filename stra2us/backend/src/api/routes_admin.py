from fastapi import APIRouter, HTTPException, Query, Request, Depends
from fastapi.responses import JSONResponse
from pydantic import BaseModel
from typing import List, Optional
import json
import msgpack
import time
from core.redis_client import get_redis_client
from core.security import generate_secret
from api.dependencies import (
    ADMIN_ACL_KEY_FMT,
    get_admin_context,
    require_admin_kv,
    require_admin_queue,
    require_admin_superuser,
    check_acl,
)
from core.admin_auth import HTPASSWD_FILE
import os

router = APIRouter()

class ClientCreate(BaseModel):
    client_id: str

class KVPayload(BaseModel):
    value: str

class AclPermission(BaseModel):
    prefix: str
    access: str  # "r" or "rw"

class AclUpdate(BaseModel):
    permissions: List[AclPermission]

class ClientBackupEntry(BaseModel):
    client_id: str
    secret: str
    acl: dict

class BackupPayload(BaseModel):
    exported_at: int
    clients: List[ClientBackupEntry]

@router.get("/keys")
async def list_keys(_: dict = Depends(require_admin_superuser)):
    redis = get_redis_client()
    keys = await redis.keys("client:*:secret")
    result = []
    for k in keys:
        if isinstance(k, bytes):
            k = k.decode('utf-8')
        client_id = k.split(":")[1]
        acl_json = await redis.get(f"client:{client_id}:acl")
        acl = json.loads(acl_json) if acl_json else {}
        result.append({
            "client_id": client_id,
            "acl": acl
        })
    return result

@router.post("/keys")
async def create_client(client: ClientCreate, _: dict = Depends(require_admin_superuser)):
    redis = get_redis_client()
    secret = generate_secret()
    await redis.set(f"client:{client.client_id}:secret", secret)
    # New clients start with no permissions (deny-all); edit ACL separately.
    acl = {"permissions": []}
    await redis.set(f"client:{client.client_id}:acl", json.dumps(acl))
    return {
        "client_id": client.client_id,
        "secret": secret,
        "acl": acl
    }

@router.put("/keys/{client_id}/acl")
async def update_acl(client_id: str, acl_update: AclUpdate, _: dict = Depends(require_admin_superuser)):
    redis = get_redis_client()
    existing = await redis.get(f"client:{client_id}:secret")
    if not existing:
        raise HTTPException(status_code=404, detail="Client not found")
    acl = {"permissions": [p.dict() for p in acl_update.permissions]}
    await redis.set(f"client:{client_id}:acl", json.dumps(acl))
    return {"status": "ok", "client_id": client_id, "acl": acl}

@router.delete("/keys/{client_id}")
async def revoke_client(client_id: str, _: dict = Depends(require_admin_superuser)):
    redis = get_redis_client()
    await redis.delete(f"client:{client_id}:secret")
    await redis.delete(f"client:{client_id}:acl")
    return {"status": "ok"}

@router.get("/whoami")
async def whoami(admin_ctx: dict = Depends(get_admin_context)):
    """Return the logged-in admin's identity + ACL so the UI can hide
    features the caller can't use (e.g. Key Management for a scoped admin).
    Not a security boundary — the routes themselves still enforce."""
    acl = admin_ctx["acl"]
    is_superuser = any(
        p.get("prefix") == "*" and p.get("access") == "rw"
        for p in acl.get("permissions", [])
    )
    return {
        "username": admin_ctx["client_id"],
        "acl": acl,
        "is_superuser": is_superuser,
    }


# --- Admin users ---
#
# Admin accounts live in htpasswd (auth) + Redis (ACL). The UI can read
# the union and update ACLs; create/delete/password-reset stay CLI-only
# to avoid putting credential management in the browser session.

def _read_htpasswd_users() -> List[str]:
    if not os.path.exists(HTPASSWD_FILE):
        return []
    users = []
    with open(HTPASSWD_FILE, "r") as f:
        for line in f:
            line = line.strip()
            if not line or ":" not in line:
                continue
            users.append(line.split(":", 1)[0])
    return users


@router.get("/admin_users")
async def list_admin_users(_: dict = Depends(require_admin_superuser)):
    """Return every admin account known to htpasswd, with its ACL record
    if present. Users without a Redis ACL row are surfaced with an empty
    permissions list so the UI can flag them ("needs provisioning")."""
    redis = get_redis_client()
    users = _read_htpasswd_users()
    out = []
    for user in users:
        raw = await redis.get(ADMIN_ACL_KEY_FMT.format(user=user))
        if raw:
            try:
                acl = json.loads(raw)
                provisioned = True
            except ValueError:
                acl = {"permissions": []}
                provisioned = True  # row exists but is corrupt — treat as provisioned+broken
        else:
            acl = {"permissions": []}
            provisioned = False
        out.append({"username": user, "acl": acl, "provisioned": provisioned})
    return out


@router.put("/admin_users/{username}/acl")
async def update_admin_user_acl(username: str, acl_update: AclUpdate, _: dict = Depends(require_admin_superuser)):
    """Create or replace the Redis ACL row for an admin user. 404 if the
    username isn't in htpasswd — UI shouldn't be able to grant permissions
    to a non-existent account."""
    if username not in _read_htpasswd_users():
        raise HTTPException(status_code=404, detail="Admin user not found in htpasswd")
    redis = get_redis_client()
    acl = {"permissions": [p.dict() for p in acl_update.permissions]}
    await redis.set(ADMIN_ACL_KEY_FMT.format(user=username), json.dumps(acl))
    return {"status": "ok", "username": username, "acl": acl}


@router.get("/stats")
async def get_stats(admin_ctx: dict = Depends(get_admin_context)):
    redis = get_redis_client()
    q_keys = await redis.keys("q:*")
    kv_keys = await redis.keys("kv:*")

    queues = []
    for qk in q_keys:
        if isinstance(qk, bytes): qk = qk.decode('utf-8')
        topic = qk.split(":", 1)[1]
        try:
            await check_acl(admin_ctx, f"q/{topic}", mode="read")
        except HTTPException:
            continue
        count = await redis.xlen(qk)
        queues.append({"topic": topic, "count": count})

    kvs = []
    for kvk in kv_keys:
        if isinstance(kvk, bytes): kvk = kvk.decode('utf-8')
        name = kvk.split(":", 1)[1]
        try:
            await check_acl(admin_ctx, f"kv/{name}", mode="read")
        except HTTPException:
            continue
        kvs.append({"key": name})

    return {
        "queues": queues,
        "kvs": kvs
    }

@router.get("/peek/q/{topic}")
async def peek_queue(topic: str, _: dict = Depends(require_admin_queue("read"))):
    redis = get_redis_client()
    # Peek at oldest message using xrange
    messages = await redis.xrange(f"q:{topic}", min="-", max="+", count=1)
    if not messages:
        return {"status": "empty", "message": None}
        
    try:
        msg_id, fields = messages[0]
        payload = fields[b"payload"]
        decoded = msgpack.unpackb(payload)
        return {"status": "ok", "message": decoded, "hex": payload.hex()}
    except Exception:
        return {"status": "ok", "message": "unparseable_msgpack", "hex": payload.hex()}

@router.get("/kv_scan")
async def scan_kv(
    prefix: str = Query(..., min_length=1),
    limit: int = 500,
    admin_ctx: dict = Depends(get_admin_context),
):
    """List KV keys matching a literal prefix, filtered to those the
    logged-in admin can read. Intended for UI discovery (e.g.
    `prefix=_catalog/` to list published catalogs). Returns the raw key
    names with their stored byte size; callers fetch values via /peek/kv/*.
    """
    redis = get_redis_client()
    # redis keys are stored under the `kv:` namespace; match that.
    pattern = f"kv:{prefix}*"
    raw_keys = await redis.keys(pattern)

    # Filter by the caller's ACL — check_acl raises on deny, so catch it
    # per-key rather than letting a single unreadable entry fail the scan.
    items = []
    for k in raw_keys:
        if isinstance(k, bytes):
            k = k.decode("utf-8")
        name = k.split(":", 1)[1]
        try:
            await check_acl(admin_ctx, f"kv/{name}", mode="read")
        except HTTPException:
            continue
        size = await redis.strlen(k)
        items.append({"key": name, "bytes": size})
        if len(items) >= limit:
            break
    items.sort(key=lambda it: it["key"])
    # `truncated` now means "the caller's visible result set was capped",
    # not the raw redis KEYS output — UI already treats it as a hint.
    return {"prefix": prefix, "count": len(items), "truncated": len(items) >= limit, "items": items}


@router.get("/peek/kv/{key:path}")
async def peek_kv(key: str, _: dict = Depends(require_admin_kv("read"))):
    redis = get_redis_client()
    msg = await redis.get(f"kv:{key}")
    if not msg:
        return {"status": "empty", "message": None}

    try:
        decoded = msgpack.unpackb(msg)
        return {"status": "ok", "message": decoded, "hex": msg.hex()}
    except Exception:
        return {"status": "ok", "message": "unparseable_msgpack", "hex": msg.hex()}

@router.post("/kv/{key:path}")
async def set_kv(key: str, payload: KVPayload, _: dict = Depends(require_admin_kv("write"))):
    redis = get_redis_client()
    try:
        data = json.loads(payload.value)
    except ValueError:
        data = payload.value
    packed = msgpack.packb(data)
    await redis.set(f"kv:{key}", packed)
    return {"status": "ok"}

@router.delete("/kv/{key:path}")
async def delete_kv(key: str, _: dict = Depends(require_admin_kv("write"))):
    redis = get_redis_client()
    await redis.delete(f"kv:{key}")
    return {"status": "ok"}

@router.delete("/q/{topic}")
async def delete_queue(topic: str, _: dict = Depends(require_admin_queue("write"))):
    redis = get_redis_client()
    await redis.delete(f"q:{topic}")
    return {"status": "ok"}

def _log_resource_from_action(action: str) -> Optional[str]:
    """Parse 'METHOD /q/<topic>' or 'METHOD /kv/<key>' into an ACL
    check target like 'q/<topic>' or 'kv/<key>'. Returns None for
    actions that aren't ACL-scoped (e.g. /firmware/)."""
    try:
        _, path = action.split(" ", 1)
    except ValueError:
        return None
    if path.startswith("/q/"):
        return "q/" + path[len("/q/"):]
    if path.startswith("/kv/"):
        return "kv/" + path[len("/kv/"):]
    return None


@router.get("/logs")
async def get_logs(
    limit: int = 200,
    client_id: Optional[List[str]] = Query(None),
    admin_ctx: dict = Depends(get_admin_context),
):
    redis = get_redis_client()
    # ACL filter is always applied per entry, so always over-fetch to give
    # a scoped admin a full page after denied entries drop out.
    fetch_count = min(limit * 10, 5000)
    records = await redis.xrevrange("system:activity_log", max="+", min="-", count=fetch_count)

    logs = []
    for msg_id, fields in records:
        cid = fields.get(b"client_id", b"unknown")
        if isinstance(cid, bytes):
            cid = cid.decode("utf-8")

        if client_id and cid not in client_id:
            continue

        action = fields.get(b"action", b"")
        status = fields.get(b"status", b"")
        action_str = action.decode("utf-8") if isinstance(action, bytes) else action

        # ACL filter: only show log entries whose target the caller can read.
        # Firmware hits and other non-ACL-scoped actions pass through — they
        # aren't per-app resources.
        resource = _log_resource_from_action(action_str)
        if resource is not None:
            try:
                await check_acl(admin_ctx, resource, mode="read")
            except HTTPException:
                continue

        logs.append({
            "timestamp": int(fields.get(b"timestamp", b"0")),
            "client_id": cid,
            "action":    action_str,
            "status":    status.decode("utf-8") if isinstance(status, bytes) else status,
        })
        if len(logs) >= limit:
            break

    return logs


# --- Backup / Restore ---

@router.get("/keys/backup")
async def backup_keys(_: dict = Depends(require_admin_superuser)):
    """Export all client IDs, secrets, and ACLs as a JSON blob.
    WARNING: Response contains raw HMAC secrets. Treat like a password vault.
    """
    redis = get_redis_client()
    secret_keys = await redis.keys("client:*:secret")
    clients = []
    for k in secret_keys:
        if isinstance(k, bytes):
            k = k.decode('utf-8')
        client_id = k.split(":")[1]
        secret = await redis.get(f"client:{client_id}:secret")
        acl_json = await redis.get(f"client:{client_id}:acl")
        secret_str = secret.decode('utf-8') if isinstance(secret, bytes) else secret
        acl = json.loads(acl_json) if acl_json else {}
        clients.append({
            "client_id": client_id,
            "secret": secret_str,
            "acl": acl,
        })

    payload = {
        "exported_at": int(time.time()),
        "clients": clients,
    }
    return JSONResponse(content=payload, headers={
        "Content-Disposition": "attachment; filename=stra2us_backup.json"
    })


@router.post("/keys/restore")
async def restore_keys(payload: BackupPayload, force: bool = Query(False), _: dict = Depends(require_admin_superuser)):
    """Restore client credentials from a backup JSON blob.
    By default, skips clients that already exist.
    Pass ?force=true to overwrite existing entries.
    """
    redis = get_redis_client()
    results = {"restored": [], "skipped": [], "overwritten": []}

    for client in payload.clients:
        existing = await redis.get(f"client:{client.client_id}:secret")
        if existing and not force:
            results["skipped"].append(client.client_id)
            continue

        await redis.set(f"client:{client.client_id}:secret", client.secret)
        await redis.set(f"client:{client.client_id}:acl", json.dumps(client.acl))

        if existing:
            results["overwritten"].append(client.client_id)
        else:
            results["restored"].append(client.client_id)

    return results


# --- Topic Stream Monitor ---

@router.get("/stream/q/{topic}")
async def stream_monitor(
    topic: str,
    limit: int = 50,
    client_id: Optional[List[str]] = Query(None),
    _: dict = Depends(require_admin_queue("read")),
):
    """Read-only scan of the last N messages from a topic stream.
    Uses XREVRANGE — does not advance any subscriber cursor.
    """
    redis = get_redis_client()
    records = await redis.xrevrange(f"q:{topic}", max="+", min="-", count=limit)

    messages = []
    now = int(time.time())
    for msg_id, fields in records:
        if isinstance(msg_id, bytes):
            msg_id = msg_id.decode()

        # received_at derived from stream entry ID millisecond prefix (authoritative)
        ms_str = msg_id.split("-")[0]
        received_at = int(ms_str) // 1000

        # Skip expired messages
        exp = int(fields.get(b"exp", b"0"))
        if now > exp:
            continue

        cid = fields.get(b"client_id", b"unknown")
        if isinstance(cid, bytes):
            cid = cid.decode("utf-8")

        if client_id and cid not in client_id:
            continue

        raw_payload = fields.get(b"payload", b"")
        try:
            data = msgpack.unpackb(raw_payload, raw=False)
        except Exception:
            data = raw_payload.hex()

        messages.append({
            "id": msg_id,
            "received_at": received_at,
            "client_id": cid,
            "data": data,
        })

    return messages
