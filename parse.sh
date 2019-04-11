#!/bin/bash

exe="$1"

shift 1

for f in $@; do
	#echo "$f"
	#echo "cat $f | xargs addr2line -ips -e $exe  > $f-symb"
	cat $f | xargs addr2line -ps -e $exe  > $f-symb

done
