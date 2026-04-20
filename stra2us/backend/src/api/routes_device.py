from fastapi import APIRouter, Depends, HTTPException, Request, Response
from core.redis_client import get_redis_client
from core.security import sign_payload
import msgpack
from .dependencies import verify_device_request, check_acl
import time

router = APIRouter()

MSGPACK_MT = "application/x-msgpack"

def signed_response(context: dict, request: Request, body: bytes,
                    status_code: int = 200,
                    media_type: str = MSGPACK_MT) -> Response:
    """Wrap `body` in a Response and attach X-Response-{Timestamp,Signature}.

    Signature layout matches the request direction: HMAC-SHA256 over
    `URI + body + timestamp`, keyed by the requesting client's shared secret.
    A client that already holds its own secret can verify without any new
    key material. Empty-body responses (e.g. 204) still get signed over the
    URI + empty-bytes + timestamp so the caller can trust the status line.
    """
    ts = int(time.time())
    uri = str(request.url.path)
    sig = sign_payload(context["secret_hex"], uri, body, ts)
    headers = {
        "X-Response-Timestamp": str(ts),
        "X-Response-Signature": sig,
    }
    return Response(content=body, status_code=status_code,
                    media_type=media_type, headers=headers)

def signed_msgpack(context: dict, request: Request, obj,
                   status_code: int = 200) -> Response:
    return signed_response(context, request, msgpack.packb(obj),
                           status_code=status_code)

@router.post("/q/{topic}")
async def publish_message(
    topic: str,
    request: Request,
    ttl: int = 3600,
    context: dict = Depends(verify_device_request)
):
    if ttl > 604800:
        ttl = 604800

    await check_acl(context, f"q/{topic}", mode="write")
    content_type = request.headers.get("content-type", "")
    body = await request.body()
    
    if "text/plain" in content_type:
        try:
            # Wrap raw string in msgpack
            body = msgpack.packb(body.decode("utf-8"))
        except Exception:
            raise HTTPException(status_code=400, detail="Invalid UTF-8 payload")
    else:
        try:
            # Validate existing msgpack
            if len(body) > 0:
                msgpack.unpackb(body)
        except Exception:
            raise HTTPException(status_code=400, detail="Invalid MessagePack payload")

    redis = get_redis_client()
    exp_time = int(time.time()) + ttl
    publisher_id = context["client_id"]

    # Store payload, expiry, and the authenticated publisher identity
    await redis.xadd(f"q:{topic}", {
        "payload": body,
        "exp": str(exp_time),
        "client_id": publisher_id,
    })
    # Global TTL to prevent abandoned topic memory leaks
    await redis.expire(f"q:{topic}", 604800)

    return signed_msgpack(context, request, {"status": "ok"})

@router.get("/q/{topic}")
async def consume_message(
    topic: str,
    request: Request,
    envelope: bool = False,
    context: dict = Depends(verify_device_request)
):
    """Consume the next message from a topic queue.

    When ?envelope=true, the response is a msgpack-packed dict:
      {"data": <decoded payload>, "client_id": "<publisher>", "received_at": <unix seconds>}
    When omitted or false, the raw msgpack payload bytes are returned (legacy behaviour).
    """
    await check_acl(context, f"q/{topic}", mode="read")
    redis = get_redis_client()
    consumer_id = context["client_id"]
    cursor_key = f"cursor:{consumer_id}:q:{topic}"

    last_id = await redis.get(cursor_key)
    if last_id is None:
        last_id = "0-0"
    elif isinstance(last_id, bytes):
        last_id = last_id.decode('utf-8')

    current_time = int(time.time())

    while True:
        messages = await redis.xread({f"q:{topic}": last_id}, count=50)

        if not messages:
            return signed_response(context, request, b"", status_code=204)

        stream_name, records = messages[0]

        for msg_id, fields in records:
            last_id = msg_id.decode() if isinstance(msg_id, bytes) else msg_id

            exp = int(fields[b"exp"])
            if current_time <= exp:
                await redis.set(cursor_key, last_id)

                raw_payload = fields[b"payload"]

                if not envelope:
                    return signed_response(context, request, raw_payload)

                # --- Envelope mode ---
                # Decode the stored payload so it becomes the `data` field value
                try:
                    decoded_data = msgpack.unpackb(raw_payload, raw=False)
                except Exception:
                    decoded_data = raw_payload  # pass raw bytes through if unparseable

                # received_at: Redis Stream IDs are "{unix_ms}-{seq}" — authoritative server time
                ms_str = last_id.split("-")[0]
                received_at = int(ms_str) // 1000

                publisher_id = fields.get(b"client_id", b"unknown")
                if isinstance(publisher_id, bytes):
                    publisher_id = publisher_id.decode("utf-8")

                wrapped = msgpack.packb({
                    "data": decoded_data,
                    "client_id": publisher_id,
                    "received_at": received_at,
                }, use_bin_type=True)
                return signed_response(context, request, wrapped)

        # advance cursor and keep polling if all current batch were expired
        await redis.set(cursor_key, last_id)

@router.post("/kv/{key:path}")
async def write_kv(
    key: str,
    request: Request,
    context: dict = Depends(verify_device_request)
):
    await check_acl(context, f"kv/{key}", mode="write")
    content_type = request.headers.get("content-type", "")
    body = await request.body()
    
    if "text/plain" in content_type:
        try:
            body = msgpack.packb(body.decode("utf-8"))
        except Exception:
            raise HTTPException(status_code=400, detail="Invalid UTF-8 payload")
    else:
        try:
            if len(body) > 0:
                msgpack.unpackb(body)
        except Exception:
            raise HTTPException(status_code=400, detail="Invalid MessagePack payload")

    redis = get_redis_client()
    await redis.set(f"kv:{key}", body)

    return signed_msgpack(context, request, {"status": "ok"})

@router.get("/kv/{key:path}")
async def read_kv(
    key: str,
    request: Request,
    context: dict = Depends(verify_device_request)
):
    await check_acl(context, f"kv/{key}", mode="read")
    redis = get_redis_client()
    val = await redis.get(f"kv:{key}")

    if val is None:
        return signed_msgpack(context, request, {"status": "not_found"})

    return signed_response(context, request, val)
