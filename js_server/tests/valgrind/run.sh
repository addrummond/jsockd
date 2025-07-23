#!/bin/sh
set -e

#
# This test repeatedly sends the server different commands so that its query
# cache fills up. Some old queries will be evicted from the cache. This
# should exercise all of the dynamic allocation code paths in the server.
#

N_ITERATIONS=100

rm -f /tmp/jsockd_test_sock
rm -f /tmp/jsockd_test_valgrind_exit_code

openssl genpkey -algorithm ed25519 -out private_signing_key.pem
openssl pkey -inform pem -pubout -outform der -in private_signing_key.pem | tail -c 32 | xxd -p | tr -d '\n' > public_signing_key
export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=$(cat public_signing_key)

# Compile the example module to QuickJS bytecode.
./tools-bin/compile_es6_module example_module.mjs /tmp/example_module.qjsb private_signing_key.pem

cd js_server

# Start the server with Valgrind
./mk.sh Debug
(
    valgrind --error-exitcode=1 --leak-check=full --track-origins=yes -- \
        ./build_Debug/js_server /tmp/example_module.qjsb /tmp/jsockd_test_sock
    echo $? > /tmp/jsockd_test_valgrind_exit_code
) &
server_pid=$!

# Start the client
(
    i=0
    while ! [ -e /tmp/jsockd_test_sock ] && [ $i -lt 15 ]; do
      echo "Waiting for server to start"
      sleep 1
      i=$(($i + 1))
    done
    sleep 1

    if [ -z "$JSOCKD_JS_SERVER_SOCKET_SEP_CHAR_HEX" ]; then
      ./tests/valgrind/gen_test_cases.sh $N_ITERATIONS > /tmp/jsockd_js_server_valgrind_test_input
      echo "?quit" >> /tmp/jsockd_js_server_valgrind_test_input
    else
      ./tests/valgrind/gen_test_cases.sh $N_ITERATIONS | awk 1 ORS=$(printf "\x${JSOCKD_JS_SERVER_SOCKET_SEP_CHAR_HEX}") > /tmp/jsockd_js_server_valgrind_test_input
      printf "?quit\x$$JSOCKD_JS_SERVER_SOCKET_SEP_CHAR_HEX}" >> /tmp/jsockd_js_server_valgrind_test_input
    fi

    nc -w 5 -U /tmp/jsockd_test_sock < /tmp/jsockd_js_server_valgrind_test_input
) 2>&1 &
client_pid=$!

wait $client_pid

i=0
while [ ! -f /tmp/jsockd_test_valgrind_exit_code ] && [ $i -lt 60 ]; do
  echo "Waiting for server to exit"
  sleep 1
  i=$(($i + 1))
done

if ! [ -f /tmp/jsockd_test_valgrind_exit_code ]; then
    echo "Server did not exit gracefully, killing it"
    kill $server_pid || true
    exit 1
else
    valgrind_exit_code=$(cat /tmp/jsockd_test_valgrind_exit_code)
    echo "Valgrind exit code: $valgrind_exit_code"

    if [ $valgrind_exit_code -ne 0 ]; then
        echo
        echo "********************"
        echo "Valgrind detected errors."
        echo "********************"
    fi

    exit $valgrind_exit_code
fi
