#!/bin/sh

set -e

export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=dangerously_allow_invalid_signatures

# Compile the example module to QuickJS bytecode.
./tools-bin/compile_es6_module example_module.mjs /tmp/jsockd_memory_increase_test_example_module.qjsb

cd js_server

# Generate input for a series of commands/param pairs like
#   (m, p) => { (global_var=(globalThis.global_var ?? { })); globalVar[p] = { }; return "foo"; }
#   $n
# which should leak memory.
awk 'BEGIN { for (i = 0; i < 2000; i++) { print "cmd"i"\n(m, p) => { (globalThis.globalVar=(globalThis.global_var ?? { })); globalThis.globalVar[p] = { \"foo\": p }; return \"foo\"; }\n"i } }' > /tmp/jsockd_memory_increase_test_input
echo "?quit" >> /tmp/jsockd_memory_increase_test_input

./mk.sh Debug

rm -f /tmp/jsockd_memory_increase_test_sock
rm -f /tmp/jsockd_memory_increase_test_server_exit_code
# Start the server
(
    ./build_Debug/js_server -m /tmp/jsockd_memory_increase_test_example_module.qjsb -s /tmp/jsockd_memory_increase_test_sock > /tmp/jsockd_memory_increase_test_output 2>&1
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

echo "Sending output to server..."
cat /tmp/jsockd_memory_increase_test_input | ( nc -U /tmp/jsockd_memory_increase_test_sock >/dev/null || true )

i=0
while ! [ -f /tmp/jsockd_memory_increase_test_server_exit_code ] && [ $i -lt 15 ]; do
  echo "Waiting for server to exit"
  sleep 1
  i=$(($i + 1))
done

if ! [ -f /tmp/jsockd_memory_increase_test_server_exit_code ]; then
    echo "Server failed to exit"
    kill -9 $server_pid
    exit 1
fi

echo "Server has exited, checking exit code..."
if [ $(cat /tmp/jsockd_memory_increase_test_server_exit_code) -ne 0 ]; then
    echo "Server exited with an error code, which is unexpected."
    exit 1
fi

n_state_resets=$(grep "Memory usage has increased" /tmp/jsockd_memory_increase_test_output | wc -l | sed -e 's/^[[:space:]]*//')
# Rough sanity check
echo "Number of state resets due to memory increase: $n_state_resets"
if [ $n_state_resets -lt 2 ] || [ $n_state_resets -gt 20 ]; then
    echo "Expected approximately 5 state resets due to memory increases, but found $n_state_resets."
    echo "Server output:"
    cat tmp/jsockd_memory_increase_test_output
    exit 1
fi
