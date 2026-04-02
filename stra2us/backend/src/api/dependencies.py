from fastapi import Request, HTTPException, Security, Depends
from fastapi.security import APIKeyHeader
from core.redis_client import get_redis_client
from core.security import verify_signature, verify_timestamp
import json

client_id_header = APIKeyHeader(name="X-Client-ID")
timestamp_header = APIKeyHeader(name="X-Timestamp")
signature_header = APIKeyHeader(name="X-Signature")

async def verify_device_request(
    request: Request,
    client_id: str = Depends(client_id_header),
    timestamp_str: str = Depends(timestamp_header),
    signature: str = Depends(signature_header)
):
    try:
        timestamp = int(timestamp_str)
    except ValueError:
        raise HTTPException(status_code=400, detail="Invalid X-Timestamp format")

    if not verify_timestamp(timestamp):
        raise HTTPException(status_code=401, detail="Request expired or replay detected")

    redis = get_redis_client()
    
    # Fetch client secret
    secret_hex_bytes = await redis.get(f"client:{client_id}:secret")
    if not secret_hex_bytes:
        raise HTTPException(status_code=401, detail="Invalid Client ID")
    secret_hex = secret_hex_bytes.decode('utf-8')
        
    # Read the raw body
    body = await request.body()
    uri = str(request.url.path)

    if not verify_signature(secret_hex, uri, body, timestamp, signature):
        raise HTTPException(status_code=401, detail="Invalid Signature")

    # Fetch ACLs
    acl_json = await redis.get(f"client:{client_id}:acl")
    acl = json.loads(acl_json) if acl_json else {"read_write": "*"}

    return {
        "client_id": client_id,
        "acl": acl
    }

async def check_acl(client_context: dict, requested_resource: str, mode: str = "read_write"):
    # Simple ACL check: if mode is "read_only" and trying to write, reject.
    # Pattern matching can be added if ACLs get complex.
    acl = client_context["acl"]
    if acl.get("read_write") == "*":
        return True
    
    # Future: parse read_write or read_only fields
    if "read_only" in acl and mode == "write":
        raise HTTPException(status_code=403, detail="Forbidden: Write access denied")
        
    return True
