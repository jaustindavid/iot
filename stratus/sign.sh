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
# A new file will be created ($1.signed), then uploaded.  You
# probably need to edit the last couple lines.


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

MD5SUM=$((echo $SECRET; cat $SIGNED_FILE) | md5)
echo "MD5SUM: $MD5SUM"

echo "MD5: $MD5SUM" >> $SIGNED_FILE

scp $SIGNED_FILE www:www/df/$(basename $FILENAME)
aws s3 cp $SIGNED_FILE s3://stratus-iot/$(basename $FILENAME) --grants read=uri=http://acs.amazonaws.com/groups/global/AllUsers
