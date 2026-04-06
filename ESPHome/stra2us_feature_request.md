# Stra2us — Feature Requests from ESPHome Integration

**Filed by:** ESPHome / Coati Clock team  
**Date:** 2026-04-06  
**Contact:** austin (jaustindavid)  

These requests arose during integration of the `IoTClient` C++ library into an
ESPHome-based ESP32-C3 firmware (`bb32.yaml`). They are **not blocking** — the
current implementation works correctly with the server as-is — but they would
meaningfully improve the experience for embedded clients going forward.

---

## FR-1 — Plain-text / raw-string publish endpoint

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

## FR-2 — HTTP status code exposed through IoTClient response

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

## FR-3 — Server health / ping endpoint (no auth required)

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

## FR-4 — IoTClient: Add `publishQueue` overload for raw strings

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

*These requests are low-priority and carry no urgency — the current server API
is fully functional for the Coati Clock use case.  File as backlog items at
your discretion.*
