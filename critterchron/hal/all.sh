#!/bin/sh

for DEVICE in $(ls devices|grep -v device|xargs -n 1 -I filename basename filename .h)
do
  echo ">> make DEVICE=$DEVICE swarm flash"
  make DEVICE=$DEVICE swarm flash
  sleep 10
done
