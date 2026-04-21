#!/bin/sh

# Load STRA2US_KEY (admin secret) from .env.local — gitignored sidecar so the
# key doesn't live in a committed script. Fail loudly if it's missing or the
# key isn't set.
SCRIPT_DIR=$(dirname "$0")
if [ -f "$SCRIPT_DIR/.env.local" ]; then
  . "$SCRIPT_DIR/.env.local"
else
  echo "!! $SCRIPT_DIR/.env.local not found; create it with STRA2US_KEY=..." >&2
  exit 1
fi
: "${STRA2US_KEY:?set STRA2US_KEY in .env.local}"

STRA2US_HOST=stra2us.austindavid.com:8153
STRA2US_CLIENT=admin

if [ -f hal/devices/$1.h ]
then
  echo "python3 tools/set_ir_pointer.py $1 $2"
  python3 tools/set_ir_pointer.py $1 $2 --server $STRA2US_HOST --client-id $STRA2US_CLIENT --secret $STRA2US_KEY
else
  echo "oop -- $1 not a target?  hal/devices/$1.h doesn't exist"
fi
