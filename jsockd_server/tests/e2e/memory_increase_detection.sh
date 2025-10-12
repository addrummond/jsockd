#!/bin/sh

set -e

export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=dangerously_allow_invalid_signatures

cd jsockd_server

./mk.sh Debug

cat <<END >/tmp/jsockd_memory_increase_test_example_module.mjs
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
build_Debug/jsockd -c /tmp/jsockd_memory_increase_test_example_module.mjs /tmp/jsockd_memory_increase_test_example_module.qjsb

# Generate input for a series of commands/param pairs like
#   (m, p) => { (global_var=(globalThis.global_var ?? { })); globalVar[p] = { }; return "foo"; }
#   $n
# which should leak memory.
awk 'BEGIN { for (i = 0; i < 2000; i++) { print "cmd"i"\n(m, p) => { (globalThis.globalVar=(globalThis.global_var ?? { })); globalThis.globalVar[p] = { \"foo\": p }; return \"foo\"; }\n"i } }' > /tmp/jsockd_memory_increase_test_input

./mk.sh Debug

rm -f /tmp/jsockd_memory_increase_test_sock
./build_Debug/jsockd -m /tmp/jsockd_memory_increase_test_example_module.qjsb -s /tmp/jsockd_memory_increase_test_sock > /tmp/jsockd_memory_increase_test_output 2>&1 &
server_pid=$!

i=0
while ! [ -e /tmp/jsockd_memory_increase_test_sock ] && [ $i -lt 15 ]; do
  echo "Waiting for server to start"
  sleep 1
  i=$(($i + 1))
done
sleep 1

echo "Sending output to server..."
# Cute little Perl one-liner to insert a short pause between each input line.
# We do this so that we don't send the input so quickly that the server doesn't
# have time to do the expected number of thread state resets.
perl -e "use Time::HiRes qw(usleep); while (<>) { print; usleep(2500); }; sleep(1); kill \"TERM\", $server_pid;" < /tmp/jsockd_memory_increase_test_input | ( nc -U /tmp/jsockd_memory_increase_test_sock >/dev/null || true )

echo "Waiting for server to exit..."
wait $server_pid
server_exit_code=$?
echo "Server exited with code $server_exit_code"

echo "Server has exited, checking exit code..."
if [ $server_exit_code -ne 0 ]; then
    echo "Server exited with an error code, which is unexpected."
    exit 1
fi

n_state_resets=$(grep "Memory usage has increased" /tmp/jsockd_memory_increase_test_output | wc -l | sed -e 's/^[[:space:]]*//')
# Rough sanity check
echo "Number of state resets due to memory increase: $n_state_resets"
if [ $n_state_resets -lt 2 ] || [ $n_state_resets -gt 20 ]; then
    echo "Expected approximately 5 state resets due to memory increases, but found $n_state_resets."
    echo "Server output:"
    cat /tmp/jsockd_memory_increase_test_output
    exit 1
fi
