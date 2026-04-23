#!/bin/sh

set -x

arg=$1
shift

if [ "$arg" = "all" ]
then
  echo "publishing all ..."
  for CRIT in agents/*
  do
    python tools/publish_ir.py $CRIT $@
  done
elif [ -f agents/$arg.crit ]
then
  echo ">> python tools/publish_ir.py agents/$arg.crit $@"
  python tools/publish_ir.py agents/$arg.crit $@
else
  echo "agents/$arg.crit does not exist; nothing to do"
fi
