# Stra2us - IoT Telemetry Service

Stra2us is a high-performance, stateless IoT messaging and configuration management relay system designed implicitly for resource-constrained devices (like the ESP32 and Particle Photon 2). 

It features an asynchronous Python backend layered on top of Redis, and ships alongside a zero-malloc C++ client SDK.

## Design Architecture

- **Stateless Backend Mechanism:** The backend HTTP application stores absolutely zero persistent state in process memory. It relies purely on Redis. This allows it to scale horizontally indefinitely out-of-the-box.
- **HMAC-SHA256 Signatures:** IoT Devices transmit data safely over unencrypted streams or without heavy Transport Layer Security overhead by cryptographically hashing their device secrets against the Unix timestamp. The server rigorously restricts replay attacks tighter than a 5-minute drift window.
- **Broadcast Streams:** Message queues actively operate natively via Redis Streams (`XADD`/`XREAD`). A stream acts as a destructive-free pipeline. Devices fetch messages concurrently at distinct rates unbothered by others, polling on uniquely defined server-bound cursors.
- **Micro-serialization:** Communication payload is deeply packed using `MessagePack`. This cuts the byte envelope overhead exponentially vs JSON, saving crucial IoT bandwidth and minimizing TCP boundaries.

## Technical Stack
- **Backend:** `Python 3.9+`, `FastAPI`, `Uvicorn`, `Redis Lists/Streams`.
- **Client (Device):** Built natively with C++ (`Arduino.h`).
- **Dashboard:** Zero-build vanilla HTML / JS querying dashboard internals safely wrapped via custom `.htpasswd` encryption middleware.

## Installation 

### 1. Requirements
Ensure your host machine explicitly has installed:
- Python 3.9 or higher
- Redis Server (running on localhost:6379 natively)

### 2. Backend Bootup
You can start the server locally with extreme ease.
```bash
cd backend
./start.sh
```
*Note: This automatically initializes a virtual environment, installs dependencies, and boots `uvicorn` on port 8000.*

### 3. Dashboard Admin User
To view the web-monitoring dashboard at `http://localhost:8000/admin`, you must initialize an administrative login. 
```bash
cd backend
source venv/bin/activate
python create_admin.py super_username super_password
```
*This command seamlessly generates a secure HMAC encrypted `admin.htpasswd` file, mapping to securely signed session cookies upon frontend login.*

## Using the CLI Toolkit
You can test the queue manually without utilizing C++ hardware by invoking the included helper client utility script!

First, login to the visual admin panel and create an IoT device to generate its Client-ID and Secret values.

Then, execute arbitrary commands cleanly:

**Publish to Queue (with 3600 second optional TTL):**
```bash
python test_client.py --client-id xxx --secret xxx publish sensor_data "{\"pulse\": true}" --ttl 3600
```

**Follow a Broadcast Queue:**
```bash
# Safely loops and drains queue streams using rapid HTTP 204 short-circuit bounds
python test_client.py --client-id xxx --secret xxx follow sensor_data --delay 1.0
```

**Set and Read KV Storage Nodes:**
```bash
python test_client.py --client-id xxx --secret xxx set device-10 "{\"throttle\": 500}"
python test_client.py --client-id xxx --secret xxx get device-10
```
