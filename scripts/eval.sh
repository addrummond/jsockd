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

( $JSOCKD -b 1e -s $socket $@ 2>&1 >/dev/null ) &
jsockd_pid=$!

i=0
while ! [ -e $socket ]; do
   i=$(($i + 1))
   if [ $i -gt 10000000 ]; then
       echo "Timed out waiting for jsockd to create socket $socket"
       exit 1
   fi
done

# Some versions of awk are funny about setting RS to the null byte, so use
# the ASCII record sep char instead.
output=$(printf "$uid\x1e%s\x1e0\x1e?quit\x00" "$command" | nc -U $socket  )

case $output in
    "$uid exception "*)
        output=$(printf %s "$output" | dd bs=1 skip=43 2>/dev/null)

        echo "Pretty-printed error from jsockd:"
        jq --version > /dev/null
        if [ $? -ne 0 ]; then
            echo "jq is not installed; cannot pretty-print JSON error"
            echo
            printf "%s" "$output"
            exit 1
        fi

        printf "%s" "$output" | jq -r .pretty
        printf "\nRaw JSON error: %s\n" "$output"

        ;;
    *)
        printf "%s" "$output"
        ;;
esac

wait $jsockd_pid
