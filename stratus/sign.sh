#!/bin/sh
#
# Austin David, austin@austindavid.com
# Sat May  2 11:52:57 EDT 2020
#
# signs a text file for use with Stratus
# Usage: $0 <filename>
#
# <filename> must include ^secret key: <gibberish>\n
#
# this will be added (prepended) to the content for MD5 hashing.
# A new file will be created ($1.signed), then uploaded thusly:
# scp $file.signed $(cat dest.ssh)/$file

FILENAME=$1
if [ -z "$FILENAME" -o ! -f "$FILENAME" ]; then
  echo "Usage: $0 <filename>"
  exit 1
fi

SECRET=$(grep "^secret key" $FILENAME|sed -e 's/^secret key: //')
if [ -z "$SECRET" ]; then
  echo "ERROR: $FILENAME must contain a line like:"
  echo "secret key: <asdasdwq093>"
  exit 2
fi

SIGNED_FILE=$FILENAME.signed
egrep -v "^MD5|^secret key:" $FILENAME > $SIGNED_FILE

MD5SUM=$((echo $SECRET; cat $SIGNED_FILE) | md5 | cut -f 1 -d " ")
echo "MD5SUM: $MD5SUM"

# echo "MD5: $MD5SUM" >> $SIGNED_FILE
echo "signature: $MD5SUM" >> $SIGNED_FILE

if [ ! -f dest.ssh ]; then
  echo "ERROR: dest.ssh must exist"
  exit 3
fi

scp $SIGNED_FILE $(cat dest.ssh)/$(basename $FILENAME)

shift
if [ ! -z "$1" ]; then
  exec $0 $*
fi
