from fastapi import APIRouter, Depends, HTTPException, Request, Response
from core.redis_client import get_redis_client
import msgpack
from .dependencies import verify_device_request, check_acl
import time

router = APIRouter()

class MsgPackResponse(Response):
    media_type = "application/x-msgpack"

    def render(self, content: any) -> bytes:
        return msgpack.packb(content)

@router.post("/q/{topic}", response_class=MsgPackResponse)
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
    
    await redis.xadd(f"q:{topic}", {"payload": body, "exp": str(exp_time)})
    # Global TTL to prevent abandoned topic memory leaks
    await redis.expire(f"q:{topic}", 604800)
    
    return {"status": "ok"}

@router.get("/q/{topic}", response_class=MsgPackResponse)
async def consume_message(
    topic: str,
    context: dict = Depends(verify_device_request)
):
    await check_acl(context, f"q/{topic}", mode="read")
    redis = get_redis_client()
    client_id = context["client_id"]
    cursor_key = f"cursor:{client_id}:q:{topic}"
    
    last_id = await redis.get(cursor_key)
    if last_id is None:
        last_id = "0-0"
    elif isinstance(last_id, bytes):
        last_id = last_id.decode('utf-8')
        
    current_time = int(time.time())
    
    while True:
        messages = await redis.xread({f"q:{topic}": last_id}, count=50)
        
        if not messages:
            return Response(status_code=204)
            
        stream_name, records = messages[0]
        
        for msg_id, fields in records:
            last_id = msg_id.decode() if isinstance(msg_id, bytes) else msg_id
            
            exp = int(fields[b"exp"])
            if current_time <= exp:
                await redis.set(cursor_key, last_id)
                return Response(content=fields[b"payload"], media_type="application/x-msgpack")
                
        # advance cursor and keep polling if all current batch were expired
        await redis.set(cursor_key, last_id)

@router.post("/kv/{key}", response_class=MsgPackResponse)
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

    return {"status": "ok"}

@router.get("/kv/{key}", response_class=MsgPackResponse)
async def read_kv(
    key: str,
    context: dict = Depends(verify_device_request)
):
    await check_acl(context, f"kv/{key}", mode="read")
    redis = get_redis_client()
    val = await redis.get(f"kv:{key}")

    if not val:
        return {"status": "not_found"}
        
    return Response(content=val, media_type="application/x-msgpack")
