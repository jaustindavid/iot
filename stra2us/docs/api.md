# Stra2Us — API Reference

All device endpoints require HMAC-SHA256 request signing. Admin endpoints require HTTP Basic Auth (or a valid session cookie issued after login).

---

## Authentication

### Device Requests (HMAC)

Every device request must include the following headers:

| Header | Value |
|---|---|
| `X-Client-ID` | The registered client ID |
| `X-Timestamp` | Current Unix epoch time (seconds) |
| `X-Signature` | HMAC-SHA256 hex digest (see below) |
| `Content-Type` | `application/x-msgpack` or `text/plain` |

**Signature calculation:**

```
HMAC-SHA256(secret_bytes, URI + body_bytes + timestamp_string)
```

- `URI` is the path including query string (e.g. `/q/sensor?ttl=3600`)
- Requests with no body use an empty byte string for the body component
- Timestamp must be within ±300 seconds of the server clock

### Admin Requests

Admin endpoints (`/api/admin/*` and `/admin/*`) require HTTP Basic Auth on the first request. The server issues a session cookie (`admin_session`) that is accepted on subsequent requests.

---

## Unauthenticated Endpoints

### `GET /health`

Liveness check. No authentication required. Safe to call before SNTP sync.

**Response `200 OK`:**
```json
{"status": "ok"}
```

---

## Device Endpoints

### `POST /q/{topic}` — Publish to Queue

Publish a message to a named topic queue.

**Query Parameters:**

| Parameter | Type | Default | Description |
|---|---|---|---|
| `ttl` | int | 3600 | Message TTL in seconds (max 604800 = 7 days) |

**Content Types:**

- `application/x-msgpack` — Body must be a valid MessagePack-encoded value. Stored as-is.
- `text/plain` — Body is a raw UTF-8 string. Server wraps it in MessagePack before storage. Subscribers receive a properly-wrapped msgpack string.

The server stores the authenticated `X-Client-ID` alongside every message in the stream, enabling attribution metadata in consumer responses (see below).

**Response `200 OK`:**
```json
{"status": "ok"}
```

**Error Responses:**
- `400` — Invalid MessagePack payload or non-UTF-8 text body
- `401` — Missing or invalid HMAC signature / timestamp out of window
- `403` — Client ACL does not permit writes to this topic

---

### `GET /q/{topic}` — Consume from Queue

Read the next available message for this client. Each client maintains its own independent read cursor, so multiple clients can consume from the same topic independently.

**Query Parameters:**

| Parameter | Type | Default | Description |
|---|---|---|---|
| `envelope` | bool | `false` | When `true`, wraps the payload in a metadata envelope |

**Response `200 OK` (default — raw payload):**  
Body is raw MessagePack bytes (`Content-Type: application/x-msgpack`).

**Response `200 OK` (`?envelope=true`):**  
Body is a MessagePack-encoded dict:
```json
{
  "data": "heartbeat",
  "client_id": "bb32",
  "received_at": 1712412399
}
```
- `data` — The payload exactly as published (string, map, or other msgpack value).
- `client_id` — The authenticated publisher's `X-Client-ID`; cannot be forged by the client.
- `received_at` — Unix seconds derived from the Redis Stream entry ID — authoritative server-side receive time, independent of any client clock.

> **Backward compatibility:** The default response format (raw payload bytes) is unchanged. Existing consumers are not affected. Opt in per-request with `?envelope=true`.

**Response `204 No Content`:**  
Queue is empty or all available messages have been consumed.

**Error Responses:**
- `401` — Authentication failure
- `403` — Client ACL does not permit reads from this topic

---

### `POST /kv/{key}` — Write KV Value

Write a persistent key-value entry.

**Content Types:** Same as `/q/{topic}` — accepts `application/x-msgpack` or `text/plain`.

**Response `200 OK`:**
```json
{"status": "ok"}
```

---

### `GET /kv/{key}` — Read KV Value

Read a persistent key-value entry.

**Response `200 OK`:**  
Body is raw MessagePack bytes.

**Response `200 OK` (key not found):**
```json
{"status": "not_found"}
```

> Note: Returns `200` with a `not_found` body rather than `404` to simplify embedded client logic.

---

## Admin Endpoints

All admin endpoints require Basic Auth or a valid `admin_session` cookie.

### `GET /api/admin/stats`

Returns counts and listings of all active queues and KV pairs.

---

### `GET /api/admin/keys`

Lists all registered client IDs and their ACLs. Does **not** return secrets.

---

### `POST /api/admin/keys`

Register a new client and generate a secret.

**Request body (JSON):**
```json
{
  "client_id": "ESP32-Weather-01",
  "acl_read_write": "*"
}
```

`acl_read_write`: `"*"` for full access, `"read_only"` for read-only.

**Response:**
```json
{
  "client_id": "ESP32-Weather-01",
  "secret": "aabbcc...(64 hex chars)...",
  "acl": {"read_write": "*"}
}
```

> ⚠️ The secret is only returned once. Store it immediately.

---

### `DELETE /api/admin/keys/{client_id}`

Revoke a client. The device will immediately receive `401` on all future requests.

---

### `GET /api/admin/keys/backup`

Export all client credentials (IDs, secrets, ACLs) as a JSON file.

**Response:** JSON file download (`Content-Disposition: attachment`).

```json
{
  "exported_at": 1712345678,
  "clients": [
    {
      "client_id": "ESP32-Weather-01",
      "secret": "aabbcc...",
      "acl": {"read_write": "*"}
    }
  ]
}
```

> ⚠️ **Security:** This response contains raw HMAC secrets. Treat the file like a password manager export. Never commit to version control.

---

### `POST /api/admin/keys/restore`

Restore credentials from a backup JSON body.

**Query Parameters:**

| Parameter | Default | Description |
|---|---|---|
| `force` | `false` | If `true`, overwrites existing clients. Default skips them. |

**Request body:** A backup JSON blob (same format as the backup response above).

**Response:**
```json
{
  "restored": ["new-device-1"],
  "skipped": ["existing-device"],
  "overwritten": []
}
```

---

### `GET /api/admin/peek/q/{topic}`

Peek at the oldest message in a queue without consuming it.

---

### `GET /api/admin/peek/kv/{key}`

Peek at the value of a KV key, decoded from MessagePack.

---

### `DELETE /api/admin/q/{topic}`

Delete an entire queue and all its messages.

---

### `DELETE /api/admin/kv/{key}`

Delete a KV entry.

---

### `GET /api/admin/logs`

Returns the most recent activity log entries, newest first. Logs are stored in a Redis Stream with dual retention: entries older than 24 hours are trimmed, with a safety cap of 150,000 entries (~11 MB) to bound storage from unusually chatty clients.

**Query Parameters:**

| Parameter | Default | Description |
|---|---|---|
| `limit` | 200 | Max number of log entries to return |
| `client_id` | *(none)* | Filter by one or more client IDs. Repeat the parameter to select multiple clients (e.g. `?client_id=bb32&client_id=coaticlock`). When omitted, all clients are returned. |

**Response `200 OK`:**
```json
[
  {
    "timestamp": 1712345678,
    "client_id": "bb32",
    "action": "POST /q/coaticlock",
    "status": "Success (200)"
  }
]
```

---

## Redis Key Schema

| Pattern | Type | Description |
|---|---|---|
| `client:{id}:secret` | String | Client HMAC secret (hex) |
| `client:{id}:acl` | String (JSON) | Client ACL permissions |
| `q:{topic}` | Stream | Message queue |
| `kv:{key}` | String | Persistent KV value (raw msgpack) |
| `cursor:{client_id}:q:{topic}` | String | Per-client read cursor for a queue |
| `system:activity_log` | Stream | Activity log — 24h retention with 150K entry safety cap |
