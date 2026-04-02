import sys
import os
sys.path.insert(0, os.path.dirname(__file__))

from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles
from fastapi.middleware.cors import CORSMiddleware
from api.routes_device import router as device_router
from api.routes_admin import router as admin_router

app = FastAPI(title="IoT Telemetry Service")

import time
import msgpack
import base64
from fastapi import Request, Response
from core.redis_client import get_redis_client
from core.admin_auth import verify_password, generate_session_token, verify_session_token

@app.middleware("http")
async def admin_auth_middleware(request: Request, call_next):
    path = request.url.path
    if path.startswith("/admin") or path.startswith("/api/admin"):
        # Check cookie first
        cookie = request.cookies.get("admin_session")
        if cookie and verify_session_token(cookie):
            return await call_next(request)
            
        # Check Basic Auth
        auth_header = request.headers.get("Authorization")
        if auth_header and auth_header.startswith("Basic "):
            try:
                encoded_creds = auth_header.split(" ")[1]
                decoded_creds = base64.b64decode(encoded_creds).decode("utf-8")
                username, password = decoded_creds.split(":", 1)
                
                if verify_password(username, password):
                    # Valid! Proceed with request
                    response = await call_next(request)
                    # Issue session cookie
                    token = generate_session_token(username)
                    response.set_cookie(key="admin_session", value=token, httponly=True)
                    return response
            except Exception:
                pass # Fall through to 401
                
        # Not authenticated
        return Response(status_code=401, headers={"WWW-Authenticate": 'Basic realm="Admin Area"'})
        
    return await call_next(request)

@app.middleware("http")
async def activity_log_middleware(request: Request, call_next):
    try:
        response = await call_next(request)
        status = response.status_code
    except Exception as e:
        status = 500
        raise e
    finally:
        path = request.url.path
        # Only log device data APIs, not admin dashboard or static files
        if path.startswith("/q/") or path.startswith("/kv/"):
            client_id = request.headers.get("X-Client-ID", "unknown")
            method = request.method
            action = f"{method} {path}"
            
            log_status = f"Success ({status})" if 200 <= status < 300 else f"Error ({status})"
            
            log_entry = {
                "timestamp": int(time.time()),
                "client_id": client_id,
                "action": action,
                "status": log_status
            }
            
            redis = get_redis_client()
            await redis.lpush("system:activity_log", msgpack.packb(log_entry))
            await redis.ltrim("system:activity_log", 0, 999)
            
    return response

# Allow CORS for development convenience
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Admin API
app.include_router(admin_router, prefix="/api/admin", tags=["admin"])

# Device API
app.include_router(device_router, tags=["device"])

# Mount Static UI (Protected via external .htaccess in production as specified by user)
app.mount("/admin", StaticFiles(directory="src/static", html=True), name="static")

@app.get("/")
def read_root():
    return {"status": "IoT Telemetry Service is running. Access /admin for Management UI."}
