#!/bin/sh

#
# This test repeatedly sends the server different commands so that its query
# cache fills up. Some old queries will be evicted from the cache. This
# should exercise all of the dynamic allocation code paths in the server.
#

N_ITERATIONS=100

rm -f /tmp/jsockd_test_sock1 /tmp/jsockd_test_sock2

cd jsockd_server
./mk.sh Debug

rm -f valgrind_private_signing_key*
./build_Debug/jsockd -k valgrind_private_signing_key
export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=$(cat valgrind_private_signing_key.pubkey)

# Compile the example module to QuickJS bytecode.
./build_Debug/jsockd -c tests/valgrind_server/bundle.mjs /tmp/bundle.qjsb -k valgrind_private_signing_key.privkey

DASH_B_ARG=""
if [ ! -z "$JSOCKD_JS_SERVER_SOCKET_SEP_CHAR_HEX" ]; then
    DASH_B_ARG="-b $JSOCKD_JS_SERVER_SOCKET_SEP_CHAR_HEX"
fi

# Start the server with Valgrind
valgrind --error-exitcode=1 --leak-check=full --track-origins=yes -- ./build_Debug/jsockd $DASH_B_ARG -i 500000 -m /tmp/bundle.qjsb -s /tmp/jsockd_test_sock1 /tmp/jsockd_test_sock2 -sm tests/valgrind_server/bundle.mjs.map &
server_pid=$!

# Start the client
(
    i=0
    while ( ! [ -e /tmp/jsockd_test_sock1 ] || ! [ -e /tmp/jsockd_test_sock2 ] ) && [ $i -lt 15 ]; do
      echo "Waiting for server to start"
      sleep 1
      i=$(($i + 1))
    done
    sleep 1

    test_input_file="/tmp/jsockd_js_server_valgrind_test_input${JSOCKD_JS_SERVER_SOCKET_SEP_CHAR_HEX}"

    if [ -z "$JSOCKD_JS_SERVER_SOCKET_SEP_CHAR_HEX" ]; then
      ./tests/valgrind_server/gen_test_cases.sh $N_ITERATIONS > $test_input_file
      echo "?quit" >> $test_input_file
    else
      ./tests/valgrind_server/gen_test_cases.sh $N_ITERATIONS | awk 1 ORS=$(printf "\x${JSOCKD_JS_SERVER_SOCKET_SEP_CHAR_HEX}") > $test_input_file
      printf "?quit\x$$JSOCKD_JS_SERVER_SOCKET_SEP_CHAR_HEX}" >> $test_input_file
    fi

    # Wait for a second so that the QuickJS env for /tmp/jsockd_test_sock2
    # shuts down, to check that the env is cleaned up correctly.
    ( printf "1\n() => 99\n99\n" ; sleep 1 ) | nc -w 5 -U /tmp/jsockd_test_sock2

    nc -w 5 -U /tmp/jsockd_test_sock1 < $test_input_file
) 2>&1 &
client_pid=$!

echo "Waiting for client..."
wait $client_pid
echo "Client terminated"

echo "Waiting for server..."
wait $server_pid
valgrind_exit_code=$?
echo "Server terminated"

echo "Valgrind exit code: $valgrind_exit_code"

if [ $valgrind_exit_code -ne 0 ]; then
    echo
    echo "********************"
    echo "Valgrind detected errors."
    echo "********************"
    exit 1
fi
