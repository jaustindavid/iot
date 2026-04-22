#!/bin/sh

if [ -f hal/devices/$1.h ]
then
  echo "python3 tools/set_ir_pointer.py $1 $2"
  python3 tools/set_ir_pointer.py $1 $2 --server $STRA2US_HOST --client-id $STRA2US_CLIENT --secret $STRA2US_KEY
else
  echo "oop -- $1 not a target?  hal/devices/$1.h doesn't exist"
fi
