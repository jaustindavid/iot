import redis.asyncio as redis
import os

# Connect to Redis
def get_redis_client():
    redis_url = os.getenv("REDIS_URL", "redis://localhost:6379/0")
    try:
        client = redis.from_url(redis_url, decode_responses=False)
        return client
    except Exception as e:
        print(f"ERROR: Failed to connect to Redis at {redis_url}")
        print(f"Exception: {e}")
        raise e
