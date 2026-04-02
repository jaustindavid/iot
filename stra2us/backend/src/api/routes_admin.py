from fastapi import APIRouter, HTTPException
from pydantic import BaseModel
from typing import List, Optional
import json
import msgpack
from core.redis_client import get_redis_client
from core.security import generate_secret

router = APIRouter()

class ClientCreate(BaseModel):
    client_id: str
    acl_read_write: str = "*"

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
