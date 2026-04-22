#!/bin/sh

set -x

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
