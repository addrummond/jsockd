#!/bin/sh

# A wrapper around jsockd that makes it easy to execute a single command and
# view the output.
#
# Usage:
#     JSOCKD=/path/to/jsockd eval.sh <command> [-m <module>] [-sm <source_map>]

JSOCKD=${JSOCKD:-jsockd}

uid=$(xxd -l16 -ps /dev/urandom)
socket="/tmp/${uuid}.jsockd_sock"

command="$1"
shift

( $JSOCKD -b 00 -s $socket $@ 2>&1 >/dev/null ) &
jsockd_pid=$!

printf "$uid\x00%s\x000\x00?quit\x00" "$command" | nc -w 5 -U $socket | sed "/^${uid}/!d" | sed -e "s/^${uid} //"

wait $jsockd_pid
