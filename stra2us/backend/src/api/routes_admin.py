from fastapi import APIRouter, HTTPException, Query
from fastapi.responses import JSONResponse
from pydantic import BaseModel
from typing import List, Optional
import json
import msgpack
import time
from core.redis_client import get_redis_client
from core.security import generate_secret

router = APIRouter()

class ClientCreate(BaseModel):
    client_id: str
    acl_read_write: str = "*"

class ClientBackupEntry(BaseModel):
    client_id: str
    secret: str
    acl: dict

class BackupPayload(BaseModel):
    exported_at: int
    clients: List[ClientBackupEntry]

@router.get("/keys")
async def list_keys():
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
async def create_client(client: ClientCreate):
    redis = get_redis_client()
    secret = generate_secret()
    # Save secret
    await redis.set(f"client:{client.client_id}:secret", secret)
    # Save ACL
    acl = {"read_write": client.acl_read_write}
    await redis.set(f"client:{client.client_id}:acl", json.dumps(acl))
    
    return {
        "client_id": client.client_id,
        "secret": secret,
        "acl": acl
    }

@router.delete("/keys/{client_id}")
async def revoke_client(client_id: str):
    redis = get_redis_client()
    await redis.delete(f"client:{client_id}:secret")
    await redis.delete(f"client:{client_id}:acl")
    return {"status": "ok"}

@router.get("/stats")
async def get_stats():
    redis = get_redis_client()
    q_keys = await redis.keys("q:*")
    kv_keys = await redis.keys("kv:*")
    
    queues = []
    for qk in q_keys:
        if isinstance(qk, bytes): qk = qk.decode('utf-8')
        count = await redis.xlen(qk)
        queues.append({"topic": qk.split(":", 1)[1], "count": count})
        
    kvs = []
    for kvk in kv_keys:
        if isinstance(kvk, bytes): kvk = kvk.decode('utf-8')
        kvs.append({"key": kvk.split(":", 1)[1]})
        
    return {
        "queues": queues,
        "kvs": kvs
    }

@router.get("/peek/q/{topic}")
async def peek_queue(topic: str):
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

@router.get("/peek/kv/{key}")
async def peek_kv(key: str):
    redis = get_redis_client()
    msg = await redis.get(f"kv:{key}")
    if not msg:
        return {"status": "empty", "message": None}
        
    try:
        decoded = msgpack.unpackb(msg)
        return {"status": "ok", "message": decoded, "hex": msg.hex()}
    except Exception:
        return {"status": "ok", "message": "unparseable_msgpack", "hex": msg.hex()}

@router.delete("/kv/{key}")
async def delete_kv(key: str):
    redis = get_redis_client()
    await redis.delete(f"kv:{key}")
    return {"status": "ok"}
    
@router.delete("/q/{topic}")
async def delete_queue(topic: str):
    redis = get_redis_client()
    await redis.delete(f"q:{topic}")
    return {"status": "ok"}

@router.get("/logs")
async def get_logs(limit: int = 50):
    redis = get_redis_client()
    logs_raw = await redis.lrange("system:activity_log", 0, limit - 1)
    logs = []
    for l in logs_raw:
        try:
            logs.append(msgpack.unpackb(l))
        except Exception:
            pass
    return logs


# --- Backup / Restore ---

@router.get("/keys/backup")
async def backup_keys():
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
async def restore_keys(payload: BackupPayload, force: bool = Query(False)):
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
async def stream_monitor(topic: str, limit: int = 50):
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

        raw_payload = fields.get(b"payload", b"")
        try:
            data = msgpack.unpackb(raw_payload, raw=False)
        except Exception:
            data = raw_payload.hex()

        client_id = fields.get(b"client_id", b"unknown")
        if isinstance(client_id, bytes):
            client_id = client_id.decode("utf-8")

        messages.append({
            "id": msg_id,
            "received_at": received_at,
            "client_id": client_id,
            "data": data,
        })

    return messages
