#!/bin/sh
set -e

#
# This test sends the server a bunch of random junk followed by '?reset'
# and then a valid command. The invalid data should not cause the server
# to quit or to crash.
#

rm -f /tmp/jsockd_fuzz_test_sock
rm -f /tmp/jsockd_fuzz_test_exit_code
rm -f /tmp/jsockd_fuzz_test_random_data

n_lines=10000

# Generate random test data
awk 'BEGIN{srand(); for (nl = 0; nl < ARGV[1]; nl++) { n_bytes = int(rand()*100); for (i = 0; i < n_bytes; i++) { printf "%02x", int(rand()*255) } } }' $n_lines |
xxd -r -p > /tmp/jsockd_fuzz_test_random_data
printf "\n?reset\nx\nx => x\n\"foo\"\n?quit\n" >> /tmp/jsockd_fuzz_test_random_data

openssl genpkey -algorithm ed25519 -out private_signing_key.pem
openssl pkey -inform pem -pubout -outform der -in private_signing_key.pem | tail -c 32 | xxd -p | tr -d '\n' > public_signing_key
export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=$(cat public_signing_key)

# Compile the example module to QuickJS bytecode.
./tools-bin/compile_es6_module private_signing_key.pem example_module.mjs /tmp/example_module.qjsb

cd js_server

./mk.sh Debug
(
    ./build_Debug/js_server /tmp/example_module.qjsb /tmp/jsockd_test_sock
    echo $? > /tmp/jsockd_fuzz_test_exit_code
) >/dev/null 2>&1 &
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
    echo "Server started, sending random data and commands..."
    nc -w 5 -U /tmp/jsockd_test_sock < /tmp/jsockd_fuzz_test_random_data >/tmp/jsockd_fuzz_test_output
    echo "Random data and commands sent."
) 2>&1 &
client_pid=$!

wait $client_pid

i=0
while [ ! -f /tmp/jsockd_fuzz_test_exit_code ] && [ $i -lt 60 ]; do
  echo "Waiting for server to exit"
  sleep 1
  i=$(($i + 1))
done

if ! [ -f /tmp/jsockd_fuzz_test_exit_code ]; then
    echo "Server did not exit gracefully, killing it"
    kill $server_pid || true
    exit 1
else
    echo "The last 3 lines of the server output:"
    last_three_lines=$(tail -n 3 /tmp/jsockd_fuzz_test_output)
    echo "$last_three_lines"
    expected_last_three_lines=$(printf "reset\nx {}\nquit\n")
    if [ "$last_three_lines" != "$expected_last_three_lines" ]; then
        echo "Unexpected last three lines of server output"
        exit 1
    fi

    exit_code=$(cat /tmp/jsockd_fuzz_test_exit_code)
    if [ $exit_code -ne 0 ]; then
        echo "Server exited with code $exit_code, which is unexpected."
        echo "The random data that triggered this (base64):"
        echo "___START___"
        base64 /tmp/jsockd_fuzz_test_random_data
        echo "___END___"
    fi
    exit $exit_code
fi
