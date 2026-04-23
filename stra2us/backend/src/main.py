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
        if cookie:
            cookie_user = verify_session_token(cookie)
            if cookie_user:
                request.state.admin_user = cookie_user
                return await call_next(request)

        # Check Basic Auth
        auth_header = request.headers.get("Authorization")
        if auth_header and auth_header.startswith("Basic "):
            try:
                encoded_creds = auth_header.split(" ")[1]
                decoded_creds = base64.b64decode(encoded_creds).decode("utf-8")
                username, password = decoded_creds.split(":", 1)

                if verify_password(username, password):
                    # Valid — hand the username to downstream deps for ACL checks.
                    request.state.admin_user = username
                    response = await call_next(request)
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
        # Log device data APIs and firmware downloads
        if path.startswith("/q/") or path.startswith("/kv/") or path.startswith("/firmware/"):
            if path.startswith("/firmware/"):
                client_id = request.client.host if request.client else "unknown"
                log_status = (
                    "Hit (200)"          if status == 200 else
                    "Not Modified (304)" if status == 304 else
                    f"Miss ({status})"
                )
            else:
                client_id = request.headers.get("X-Client-ID", "unknown")
                log_status = f"Success ({status})" if 200 <= status < 300 else f"Error ({status})"

            log_entry = {
                "timestamp": int(time.time()),
                "client_id": client_id,
                "action":    f"{request.method} {path}",
                "status":    log_status,
            }

            redis = get_redis_client()
            await redis.xadd("system:activity_log", {
                "timestamp": str(log_entry["timestamp"]),
                "client_id": log_entry["client_id"],
                "action":    log_entry["action"],
                "status":    log_entry["status"],
            }, maxlen=150000, approximate=True)
            # Trim entries older than 24 hours
            min_id = str((int(time.time()) - 86400) * 1000)
            await redis.xtrim("system:activity_log", minid=min_id)

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

# Mount Static UI
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
app.mount("/admin", StaticFiles(directory=os.path.join(BASE_DIR, "static"), html=True), name="static")

# Mount Firmware OTA directory. Default /firmware matches the Docker
# volume mount in docker-compose.yml; override with STRA2US_FIRMWARE_DIR
# for bare local dev or non-container deployments.
FIRMWARE_DIR = os.environ.get("STRA2US_FIRMWARE_DIR", "/firmware")
os.makedirs(FIRMWARE_DIR, exist_ok=True)
app.mount("/firmware", StaticFiles(directory=FIRMWARE_DIR), name="firmware")

@app.get("/health")
def health_check():
    return {"status": "ok"}

@app.get("/")
def read_root():
    return {"status": "IoT Telemetry Service is running. Access /admin for Management UI."}
