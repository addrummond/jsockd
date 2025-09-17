#!/bin/sh

# A wrapper around jsockd that makes it easy to execute a single command and
# view the output.
#
# Usage:
#     JSOCKD=/path/to/jsockd eval.sh <command> [-m <module>] [-sm <source_map>]

JSOCKD=${JSOCKD:-jsockd}

uid=$(xxd -l16 -ps /dev/urandom)
socket="/tmp/${uid}.jsockd_sock"

command="$1"
shift

rm -f /tmp/${uid}.jsockd_sock_ready
printf "0" > /tmp/${uid}.jsockd_status
printf "" > /tmp/${uid}.jsockd_status_emsg

(
    $JSOCKD -b 1e -s $socket $@ 2>&1 | (
        IFS=
        while read -r line; do
            case $line in
                "READY 1")
                    touch /tmp/${uid}.jsockd_sock_ready
                    ;;
                *)
                    if printf "%s" "$line" | grep -Eq '^[*.] jsockd [^ ]+ \[ERROR\]'; then
                        echo "1" > /tmp/${uid}.jsockd_status
                        printf "$line" > /tmp/${uid}.jsockd_status_emsg
                        break
                    else
                        printf "%s" $line
                    fi
                    ;;
            esac
        done
    )
) >/dev/null 2>&1 &
jsockd_pid=$!

sleep 0.001 2>/dev/null
sleep_frac_exit_code=$?

i=0
while ! [ -e /tmp/${uid}.jsockd_sock_ready ]; do
   if [ $(cat /tmp/${uid}.jsockd_status) -ne 0 ]; then
       echo "jsockd failed to start up:"
       cat /tmp/${uid}.jsockd_status_emsg
       rm -f /tmp/${uid}.jsockd_status /tmp/${uid}.jsockd_status_emsg
       exit 1
   fi

   if [ $sleep_frac_exit_code -ne 0 ]; then
       sleep 1
   else
       sleep 0.01
   fi
   if { [ $sleep_frac_exit_code -eq 0 ] && [ $i -gt 500 ]; } || { [ $sleep_frac_exit_code -ne 0 ] && [ $i -gt 5 ]; }; then
       echo "Timed out waiting for jsockd to be ready"
       exit 1
   fi
   i=$(($i + 1))
done

# Some versions of awk are funny about setting RS to the null byte, so use
# the ASCII record sep char instead.
output=$(printf "$uid\x1e%s\x1e0\x1e?quit\x00" "$command" | nc -U $socket)

case $output in
    "$uid exception "*)
        output=$(printf %s "$output" | dd bs=1 skip=43 2>/dev/null)

        jq --version > /dev/null
        if [ $? -ne 0 ]; then
            echo "jq is not installed; cannot pretty-print JSON error"
            echo
            printf "%s" "$output"
            exit 1
        fi

        echo "Pretty-printed error from jsockd:"
        printf "%s" "$output" | jq -r .pretty
        printf "\nRaw JSON error: %s\n" "$output"

        ;;
    *)
        printf "%s\n" "$output" | dd bs=1 skip=33 2>/dev/null
        ;;
esac

rm -f /tmp/${uid}.jsockd_sock_ready /tmp/${uid}.jsockd_status /tmp/${uid}.jsockd_status_emsg

wait $jsockd_pid
