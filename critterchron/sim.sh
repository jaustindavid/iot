#!/bin/sh

name=$1
shift
if [ -f "agents/$name.crit" ]
then
  echo ">> python main.py agents/$name.crit $*"
  python main.py "agents/$name.crit" "$@"
else
  echo "agents/$name.crit doesn't exist :("
fi
