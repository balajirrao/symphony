#!/bin/bash

len=`expr $(echo $1 | wc -c) - 1`

echo $len:$1 | nc -U $2 | cut -d ':' -f 2;