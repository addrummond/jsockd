#!/bin/bash
# ^ specifying bash here for 'time' builtin

# If debugging this locally, first run
#     ./build_quickjs.sh native linux_x86_64_filc
# (on Linux/x86_64)

#
# This script benchmarks the following:
#
# * The performance of the Fil-C Linux/x86_64 build relative to the normal
#   Linux/x86_64 build when rendering some React components.
# * The performance of Node vs. JSockD for rendering some React components.

set -e

N_ITERATIONS=100
N_VS_NODE_ITERATIONS=1000
BUILD="${BUILD:-Release}"

cd js_server

./mk.sh $BUILD
TOOLCHAIN_FILE=TC-fil-c.cmake ./mk.sh $BUILD

JS_SERVER=build_$BUILD/js_server
FILC_JS_SERVER=build_${BUILD}_TC-fil-c.cmake/js_server

npm install

./node_modules/.bin/esbuild --target=es2018 --format=esm --bundle tests/e2e/relative_bench/bench.jsx --outfile=tests/e2e/relative_bench/bundle.js

openssl genpkey -algorithm ed25519 -out filc_bench_private_signing_key.pem
openssl pkey -inform pem -pubout -outform der -in filc_bench_private_signing_key.pem | tail -c 32 | xxd -p | tr -d '\n' > filc_bench_public_signing_key

export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=$(cat filc_bench_public_signing_key)

../tools-bin/compile_es6_module tests/e2e/relative_bench/bundle.js tests/e2e/relative_bench/bundle.qjsbc filc_bench_private_signing_key.pem

#
# Build benchmark command input
#
rm -f /tmp/jsockd_filc_relative_bench_sock_command_input
i=0
new_command_count=0
while [ $i -lt $N_ITERATIONS ]; do
    cat <<EOF >>/tmp/jsockd_filc_relative_bench_sock_command_input
command_id
m => [$new_command_count, m.renderToString(m.createElement(m.AccordionDemo))][1]
"foo"
EOF
    # Change the command every 30 iterations so that we bench the bytecode compiler too.
    i=$(($i + 1))
    if [ $((i % 30)) -eq 0 ]; then
        new_command_count=$(($new_command_count + 1))
    fi
done
echo ?quit >> /tmp/jsockd_filc_relative_bench_sock_command_input

#
# Benchark the server (regular x86_64 build)
#
rm -f /tmp/jsockd_filc_relative_bench_regular_server_exit_code
(
    ./build_$BUILD/js_server -m tests/e2e/relative_bench/bundle.qjsbc -s /tmp/jsockd_filc_relative_bench_sock ;
    echo $? > /tmp/jsockd_filc_relative_bench_regular_server_exit_code
) &
regular_server_pid=$!
i=0
while ! [ -e /tmp/jsockd_filc_relative_bench_sock ] && [ $i -lt 15 ]; do
  echo "Waiting for regular x86_64 server to start"
  sleep 1
  i=$(($i + 1))
done
sleep 1
echo "Regular x86_64 server started..."

# Send a bunch of React SSR rendering commands to the regular x86_64 server
time ( nc -U /tmp/jsockd_filc_relative_bench_sock < /tmp/jsockd_filc_relative_bench_sock_command_input > /tmp/jsockd_filc_relative_bench_sock_command_output_regular )

wait $regular_server_pid

n_good_responses=$( fgrep AccordionRoot /tmp/jsockd_filc_relative_bench_sock_command_output_regular | wc -l )
if ! [ "$n_good_responses" -eq $N_ITERATIONS ]; then
    echo "Output sanity check failed for regular x86_64 server, got $n_good_responses responses, expected $N_ITERATIONS"
    exit 1
fi
if ! [ 0 -eq $(cat /tmp/jsockd_filc_relative_bench_regular_server_exit_code) ]; then
    echo "Regular x86_64 server did not exit cleanly"
    exit 1
fi

#
# Benchark the server (Fil-C x86_64 build)
#
rm -f /tmp/jsockd_filc_relative_bench_filc_server_exit_code
(
    ./build_${BUILD}_TC-fil-c.cmake/js_server -m tests/e2e/relative_bench/bundle.qjsbc -s /tmp/jsockd_filc_relative_bench_sock_filc ;
    echo $? > /tmp/jsockd_filc_relative_bench_filc_server_exit_code
) &
filc_server_pid=$!
i=0
while ! [ -e /tmp/jsockd_filc_relative_bench_sock_filc ] && [ $i -lt 15 ]; do
  echo "Waiting for Fil-C x86_64 server to start"
  sleep 1
  i=$(($i + 1))
done
sleep 1
echo "Fil-C x86_64 server started..."

# Send a bunch of React SSR rendering commands to the Fil-C x86_64 server
time ( nc -U /tmp/jsockd_filc_relative_bench_sock_filc < /tmp/jsockd_filc_relative_bench_sock_command_input > /tmp/jsockd_filc_relative_bench_sock_command_output_filc )

wait $filc_server_pid

n_good_responses=$( fgrep AccordionRoot /tmp/jsockd_filc_relative_bench_sock_command_output_filc | wc -l )
if ! [ "$n_good_responses" -eq $N_ITERATIONS ]; then
    echo "Output sanity check failed for Fil-C x86_64 server, got $n_good_responses responses, expected $N_ITERATIONS"
    exit 1
fi
if ! [ 0 -eq $(cat /tmp/jsockd_filc_relative_bench_filc_server_exit_code) ]; then
    echo "Fil-C x86_64 server did not exit cleanly"
    exit 1
fi

#
# See how long NodeJS takes to render the components
#

# Time 1000 component renders using node after warmup
node -e 'const m = await import("./tests/e2e/relative_bench/bundle.js"); for (let i = 0; i < 10; ++i) { m.renderToString(m.createElement(m.AccordionDemo)); } /* <-- allow warm up before timing */ console.time("render"); for (let i = 0; i < 1000; ++i) { JSON.stringify(m.renderToString(m.createElement(m.AccordionDemo))) } console.timeEnd("render")'

# Start the server
./build_$BUILD/js_server -m tests/e2e/relative_bench/bundle.qjsbc -s /tmp/jsockd_filc_relative_bench_sock &
regular_server_pid=$!
i=0
while ! [ -e /tmp/jsockd_filc_relative_bench_sock ] && [ $i -lt 15 ]; do
  echo "Waiting for regular x86_64 server to start for bench vs. NodeJS"
  sleep 1
  i=$(($i + 1))
done
sleep 1
echo "Regular x86_64 server started for NodeJS bench..."

rm -f /tmp/jsockd_relative_bench_vs_node_command_input
i=0
while [ $i -lt $N_VS_NODE_ITERATIONS ]; do
   printf "x\nm => m.renderToString(m.createElement(m.AccordionDemo))\n{}\n?exectime\n" >> /tmp/jsockd_relative_bench_vs_node_command_input
   i=$(($i + 1))
done
echo "?quit" >> /tmp/jsockd_relative_bench_vs_node_command_input

total_nodejs_ns=$( ( ( node -e "const m = await import('./tests/e2e/relative_bench/bundle.js'); for (let i = 0; i < 10; ++i) { console.log('OUT', m.renderToString(m.createElement(m.AccordionDemo))); } /* <-- allow warm up before timing */ console.time('render'); for (let i = 0; i < Number(process.argv[1]); ++i) { m.renderToString(m.createElement(m.AccordionDemo, JSON.parse('{}'))) } console.timeEnd('render')" $N_VS_NODE_ITERATIONS | grep '^render' | awk '{print $2}' | sed -e s/ms$// | tr -d '\n' ) && echo ' * 1000000' ) | bc )
total_jsockd_ns=$(nc -U /tmp/jsockd_filc_relative_bench_sock < /tmp/jsockd_relative_bench_vs_node_command_input | awk '/^[0-9]/ { n+=$1 } END { print n }')

echo "Time to render React component $N_VS_NODE_ITERATIONS times":
echo "  NodeJS: $total_nodejs_ns ns"
echo "  JSockD: $total_jsockd_ns ns"
