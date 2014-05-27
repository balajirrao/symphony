#!/bin/bash

len=`expr $(echo $1 | wc -c) - 1`

echo $len:$1 | nc $2 $3 $4 | cut -d ':' -f 2;