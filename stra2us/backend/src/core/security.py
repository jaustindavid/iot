import secrets
import hmac
import hashlib
import time

def generate_secret() -> str:
    """Generate a unique 32-byte shared secret (hex encoded for easy distribution)."""
    return secrets.token_hex(32)

def calculate_signature(secret_hex: str, payload: bytes, timestamp: int) -> str:
    """
    Calculate the HMAC-SHA256 signature for the given payload and timestamp.
    The payload already includes the URI if we pass it concatenated,
    but based on our plan, we'll hash the URI + Body + Timestamp.
    So this function just takes a pre-concatenated bytes buffer.
    """
    secret_bytes = bytes.fromhex(secret_hex)
    return hmac.new(secret_bytes, payload, hashlib.sha256).hexdigest()

def sign_payload(secret_hex: str, uri: str, body: bytes, timestamp: int) -> str:
    """HMAC-SHA256 over URI + body + timestamp. Shared by request-signing
    (client→server) and response-signing (server→client) so both directions
    use byte-identical concatenation order."""
    payload = uri.encode('utf-8') + body + str(timestamp).encode('utf-8')
    return calculate_signature(secret_hex, payload, timestamp)

def verify_signature(secret_hex: str, uri: str, body: bytes, timestamp: int, signature: str) -> bool:
    """
    Verify the signature using a constant-time comparison.
    HMAC over URI + Body + Timestamp.
    """
    expected_mac = sign_payload(secret_hex, uri, body, timestamp)
    return hmac.compare_digest(expected_mac, signature)

def verify_timestamp(timestamp: int, max_drift: int = 300) -> bool:
    """
    Replay mitigation: Ensure timestamp is within the max_drift
    """
    current_time = int(time.time())
    return abs(current_time - timestamp) <= max_drift
