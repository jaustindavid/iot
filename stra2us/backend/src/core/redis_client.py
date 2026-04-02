import redis.asyncio as redis
import os

# Connect to Redis
def get_redis_client():
    redis_url = os.getenv("REDIS_URL", "redis://localhost:6379/0")
    return redis.from_url(redis_url, decode_responses=False)
