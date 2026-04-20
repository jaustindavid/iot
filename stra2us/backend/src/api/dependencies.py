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
        "secret_hex": secret_hex,
        "acl": acl
    }

async def check_acl(client_context: dict, requested_resource: str, mode: str = "read"):
    acl = client_context["acl"]

    # New ACL schema: {"permissions": [{"prefix": "...", "access": "r|rw"}, ...]}
    # Strip the resource-type segment (q/ or kv/) — permissions are namespace-only.
    resource_path = requested_resource
    for type_prefix in ("q/", "kv/"):
        if resource_path.startswith(type_prefix):
            resource_path = resource_path[len(type_prefix):]
            break

    for perm in acl.get("permissions", []):
        prefix = perm.get("prefix", "")
        access = perm.get("access", "r")
        # Match wildcard, exact, or prefix + "/" hierarchy
        if prefix == "*" or resource_path == prefix or resource_path.startswith(prefix + "/"):
            if mode == "write" and access != "rw":
                raise HTTPException(status_code=403, detail="Forbidden: Write access denied")
            return True

    raise HTTPException(status_code=403, detail=f"Forbidden: No permission for '{requested_resource}'")
