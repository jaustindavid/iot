# Stra2us — Feature Requests from ESPHome Integration

**Filed by:** ESPHome / Coati Clock team  
**Date:** 2026-04-06  
**Contact:** austin (jaustindavid)  

These requests arose during integration of the `IoTClient` C++ library into an
ESPHome-based ESP32-C3 firmware (`bb32.yaml`).

**All FRs have been implemented as of 2026-04-06.**

---

## FR-1 — Plain-text / raw-string publish endpoint ✅ Implemented

### Background

The current `POST /q/{topic}` endpoint requires the request body to be a valid
MessagePack-encoded value.  On the server side, `routes_device.py` validates
this with `msgpack.unpackb(body)` before streaming to Redis.

For simple heartbeat or status publishes (e.g. `"heartbeat"`, `"ok"`,
`"door_open"`), the embedded client must first serialize the string through
`cmp_write_str()` before sending.  This works, but it adds ~5 bytes of msgpack
framing overhead and requires the `cmp` library to be compiled into every
firmware image — even projects that never need rich structured data.

### Request

Add an alternative content-type path that accepts a raw UTF-8 string body and
wraps it in msgpack internally before storing in Redis:

```
Content-Type: text/plain
POST /q/{topic}
Body: heartbeat
```

The server could detect `Content-Type: text/plain` and call
`msgpack.packb(body.decode('utf-8'))` itself before the `xadd`.  Subscribers
using the existing msgpack consumer path would receive a properly-wrapped
msgpack string — no subscriber changes needed.

### Why it matters for embedded clients

- Eliminates the `cmp` dependency for publish-only use cases
- Reduces flash footprint by ~8 KB (cmp.c is large)
- Simplifies the lambda in `bb32.yaml` — the payload becomes a literal string
  with no serialization code at all

### Suggested implementation surface

`backend/src/api/routes_device.py` — `publish_message()`:

```python
content_type = request.headers.get("content-type", "")
if "text/plain" in content_type:
    body = msgpack.packb((await request.body()).decode("utf-8"))
else:
    body = await request.body()
    if len(body) > 0:
        msgpack.unpackb(body)  # validate
```

---

## FR-2 — HTTP status code exposed through IoTClient response ✅ Implemented

### Background

`IoTClient::sendSignedRequest()` currently returns a plain `bool` — `true` on
successful TCP exchange, `false` on connection failure.  It does not surface the
HTTP status code to the caller.

This means:
- A `401 Unauthorized` (bad credentials / expired timestamp) looks identical to
  a `200 OK` from the caller's perspective.
- A `204 No Content` (empty queue) cannot be distinguished from a successful
  read, except by checking whether `outLen == 0` — which is ambiguous if a
  valid zero-length body is ever returned.

### Request

Extend the return type of `sendSignedRequest()` (and the public API methods) to
return the HTTP status code (or -1 on connection failure):

```cpp
// Proposed new signature:
int publishQueue(const char* topic, const uint8_t* payload, size_t payloadLen);
// Returns: HTTP status code (200 = ok, 401 = auth fail, etc.), or -1 on TCP error.
```

This is a **breaking API change**.  Callers that check `if (result)` would need
to change to `if (result == 200)`.  Suggest bumping `library.properties` to
`version=2.0.0` if adopted.

### Why it matters for embedded clients

- Allows ESPHome firmware to distinguish auth failures (bad secret after
  re-provisioning) from network failures and log them differently
- Allows `consumeQueue` to cleanly report "empty" vs "error" without the
  `outLen == 0` heuristic

---

## FR-3 — Server health / ping endpoint (no auth required) ✅ Implemented

### Background

The embedded client has no way to verify connectivity to the Stra2us server
without performing a signed publish or consume — which consumes a Redis stream
entry.

### Request

Add a lightweight, unauthenticated health endpoint:

```
GET /health
Response: 200 OK, body: {"status": "ok"}
```

This would allow firmware to check server reachability at boot (before SNTP
sync, while the HMAC timestamp is still invalid) without incurring any
side-effects on the message store.

---

## FR-4 — IoTClient: Add `publishQueue` overload for raw strings ✅ Implemented

### Background

This request depends on **FR-1**. Currently, `IoTClient::publishQueue` only accepts a `uint8_t*` buffer, which usually contains MessagePack data. Even with the server-side support for `text/plain` requested in FR-1, the client still requires the caller to manage a serialization buffer.

### Request

Add a new public method (overload) to `IoTClient` that accepts a `const char*` message directly. This method should internally set the HTTP `Content-Type` header to `text/plain`.

```cpp
// For sending a simple string as the raw request body
int publishQueue(const char* topic, const char* message);
```

To support this, the internal `sendSignedRequest` method likely needs a new parameter to specify the `Content-Type`, defaulting to msgpack for existing callers.

### Why it matters for embedded clients

- **Code Simplification:** In ESPHome, this allows telemetry to be sent with a single line of code in a YAML lambda, rather than a 10-line block of buffer and serialization setup.
- **Zero Malloc:** The client can point directly at string literals in the firmware's flash memory, maintaining the library's high-performance, low-memory design.

---

## FR-5 — Subscriber Metadata: `client_id` and `received_at` in Consumer Responses ✅ Implemented

**Date:** 2026-04-06

### Background

Every publish request is already authenticated via HMAC-SHA256 with a known `X-Client-ID`. The server therefore knows exactly who sent every message and when it was received. However, this attribution is currently discarded at the subscriber boundary — consumers receive only the raw payload (e.g. `"heartbeat"`), with no indication of who sent it or when.

This became apparent when observing the `coaticlock` queue from multiple clients simultaneously: a `"heartbeat"` message is indistinguishable from any other client's heartbeat, and there is no way to determine message age without the subscriber tracking external state.

### Design Decision

**Client payloads remain unchanged.** Clients post whatever they want — a plain string, a msgpack blob, a JSON object. The server does not inspect or modify the payload.

**The server wraps consumer responses** with an envelope containing attribution metadata derived from the authenticated request:

```json
{
  "data": "heartbeat",
  "client_id": "bb32",
  "received_at": 1712412399
}
```

- `data` — exactly what the client posted (string, decoded from msgpack if applicable)
- `client_id` — taken from the authenticated `X-Client-ID` header at publish time; the client cannot forge or omit this
- `received_at` — Unix timestamp recorded by the server at the moment the message was stored; authoritative and independent of any client-supplied timestamp

### Why server-side attribution is preferable to client self-identification

- **Already authenticated:** The server knows the client ID with certainty. Discarding it forces subscribers to trust client-supplied payload content instead, which is weaker.
- **Timestamps are only meaningful from the server:** An embedded device's `X-Timestamp` header is used for HMAC replay prevention, not message ordering. Only the server's receive time answers "how old is this message?".
- **Enforced consistency:** All messages get metadata regardless of client implementation. A new client that posts a bare `"heartbeat"` string gets attributed automatically, with no convention to follow.
- **Separation of concerns:** Payload is the client's domain. Envelope metadata is the server's domain. Clients should not be required to embed infrastructure-level fields inside their application payloads.

### Suggested implementation surface

`backend/src/api/routes_device.py` — `consume_message()`:

The response body changes from returning the raw msgpack-decoded payload to returning a structured envelope. The Redis Stream entry ID already encodes millisecond-precision receive time (first 13 digits are Unix ms); `client_id` should be stored alongside the payload at publish time (e.g. as a second field in the stream entry).

### Backward compatibility

This is a **breaking change** for existing consumers that expect a bare payload. Suggest a versioned endpoint (`/v2/q/{topic}`) or a request header opt-in (`Accept: application/vnd.stra2us.envelope+json`) to allow gradual migration.

---

| FR | Description | Status |
|---|---|---|
| FR-1 | Plain-text publish endpoint (`text/plain`) | ✅ Implemented |
| FR-2 | HTTP status code returned by IoTClient | ✅ Implemented |
| FR-3 | Unauthenticated `/health` ping endpoint | ✅ Implemented |
| FR-4 | `publishQueue(topic, const char*)` overload | ✅ Implemented |
| FR-5 | Subscriber envelope with `client_id` + `received_at` | ✅ Implemented |
