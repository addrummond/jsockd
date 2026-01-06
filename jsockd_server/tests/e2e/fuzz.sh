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

n_lines=1000

if ! [ -z "$1" ]; then
    # Use the data supplied
    echo "Using supplied data in $1"
    base64 -d < "$1" > /tmp/jsockd_fuzz_test_random_data
else
    # Generate random test data
    echo "Generating random test data..."
    awk 'BEGIN{srand(); for (nl = 0; nl < ARGV[1]; nl++) { n_bytes = int(rand()*100); for (i = 0; i < n_bytes; i++) { printf "%02x", int(rand()*255) } } }' "$n_lines" |
    xxd -r -p > /tmp/jsockd_fuzz_test_random_data
    printf "\n?reset\nx\nx => x\n\"foo\"\n?quit\n" >> /tmp/jsockd_fuzz_test_random_data
fi

export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=dangerously_allow_invalid_signatures

cd jsockd_server
./mk.sh Debug

cat <<END >/tmp/jsockd_fuzz_example_module.mjs
export const getAValue = () => ({
  foo: "bar",
});
export const myIdentityFunction = (x) => x;
export const throwError = () => {
  "a line";
  throw new Error("foo!");
};
END

# Compile the example module to QuickJS bytecode.
./build_Debug/jsockd -c /tmp/jsockd_fuzz_example_module.mjs /tmp/example_module.qjsb

./build_Debug/jsockd -m /tmp/example_module.qjsb -s /tmp/jsockd_fuzz_test_sock > /tmp/jsockd_fuzz_test_server_output 2>&1 &
server_pid=$!

sleep 1

# Start the client
(
    i=0
    while ! [ -e /tmp/jsockd_fuzz_test_sock ] && [ $i -lt 15 ]; do
      echo "Waiting for server to start"
      sleep 1
      i=$(($i + 1))
    done
    sleep 1
    echo "Server started, sending random data and commands..."
    nc -w 5 -U /tmp/jsockd_fuzz_test_sock < /tmp/jsockd_fuzz_test_random_data >/tmp/jsockd_fuzz_test_output
    echo "Random data and commands sent."
) 2>&1 &
client_pid=$!

wait $client_pid
wait $server_pid
server_exit_code=$?

echo "The last 3 lines of the server output:"
last_three_lines=$(tail -n 3 /tmp/jsockd_fuzz_test_output)
echo "$last_three_lines"
expected_last_three_lines=$(printf "reset\nx ok {}\nquit\n")
if [ "$last_three_lines" != "$expected_last_three_lines" ]; then
    echo "Unexpected last three lines of server output"
    exit 1
fi

if [ $server_exit_code -ne 0 ]; then
    echo "Server exited with code $server_exit_code, which is unexpected."
    echo "The random data that triggered this (base64):"
    echo "___START___"
    base64 /tmp/jsockd_fuzz_test_random_data
    echo "___END___"
    echo "The server output:"
    cat /tmp/jsockd_fuzz_test_server_output
fi
exit $server_exit_code
