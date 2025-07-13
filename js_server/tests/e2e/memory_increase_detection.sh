#!/bin/sh

set -e

export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=dangerously_allow_invalid_signatures

# Compile the example module to QuickJS bytecode.
./tools-bin/compile_es6_module example_module.mjs /tmp/jsockd_memory_increase_test_example_module.qjsb

cd js_server

# Generate input for a series of commands like
#   m => { global_var_<counter> = { } }
# which should leak memory.
awk 'BEGIN { for (i = 0; i < 2000; i++) { print i"\nm => { global_var_"i"={x"i": "i"}; return \"foo\"; }\n\"foo\"" } }' > /tmp/jsockd_memory_increase_test_input
echo "?quit" >> /tmp/jsockd_memory_increase_test_input

./mk.sh Debug

# Start the server
(
    ./build_Debug/js_server /tmp/jsockd_memory_increase_test_example_module.qjsb /tmp/jsockd_memory_increase_test_sock > /tmp/jsockd_memory_increase_test_output 2>&1
    echo $? > /tmp/jsockd_memory_increase_test_server_exit_code
) &
server_pid=$!

i=0
while ! [ -e /tmp/jsockd_memory_increase_test_sock ] && [ $i -lt 15 ]; do
  echo "Waiting for server to start"
  sleep 1
  i=$(($i + 1))
done
sleep 1

cat /tmp/jsockd_memory_increase_test_input | nc -w 5 -U /tmp/jsockd_memory_increase_test_sock > /dev/null

wait $server_pid
echo "Server has exited, checking exit code..."
if [ $(cat /tmp/jsockd_memory_increase_test_server_exit_code) -ne 0 ]; then
    echo "Server exited with an error code, which is unexpected."
    exit 1
fi

n_state_resets=$(grep "Memory usage has increased" /tmp/jsockd_memory_increase_test_output | wc -l | sed -e 's/^[[:space:]]*//')
# Rough sanity check
if [ $n_state_resets -lt 2 ] || [ $n_state_resets -gt 20 ]; then
    echo "Expected approximately 5 state resets due to memory increases, but found $n_state_resets."
    exit 1
fi
