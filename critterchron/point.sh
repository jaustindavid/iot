#!/bin/sh

if [ -f hal/devices/$1.h ]
then
  echo "python3 tools/set_ir_pointer.py $1 $2"
  python3 tools/set_ir_pointer.py $1 $2
else
  echo "oop -- $1 not a target?  hal/devices/$1.h doesn't exist"
fi
