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

while ! [ -e $socket ]; do
    sleep 0.01
done

# Some versions of awk are funny about setting RS to the null byte, so use
# the ASCII record sep char instead.
output=$(printf "$uid\x1e%s\x1e0\x1e?quit\x00" "$command" | nc -w 5 -U $socket | awk "BEGIN { RS=\"\\x1e\"; FS=\"\" } /^${uid}/ { print \$0 }" )

case $output in
    "$uid exception "*)
        output=$(echo "$output" | dd bs=1 skip=43 2>/dev/null)

        echo "Error from jsockd:"

        jq --version > /dev/null
        if [ $? -ne 0 ]; then
            echo "jq is not installed; cannot pretty-print JSON error"
            echo
            echo "$output"
            exit 1
        fi

        echo "$output" #| jq -r .raw
        ;;
    *)
        echo "$output"
        ;;
esac

wait $jsockd_pid
