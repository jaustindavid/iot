#!/bin/sh

set -x

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

arg=$1
shift

if [ "$arg" = "all" ]
then
  echo "publishing all ..."
  for CRIT in agents/*
  do
    python tools/publish_ir.py $CRIT --server $STRA2US_HOST --client-id $STRA2US_CLIENT --secret $STRA2US_KEY $@
  done
elif [ -f agents/$arg.crit ]
then
  echo ">> python tools/publish_ir.py agents/$arg.crit $@"
  python tools/publish_ir.py agents/$arg.crit $@ --server $STRA2US_HOST --client-id $STRA2US_CLIENT --secret $STRA2US_KEY
else
  echo "agents/$arg.crit does not exist; nothing to do"
fi
