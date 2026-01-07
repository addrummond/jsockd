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
N_VS_NODE_ITERATIONS=100
BUILD="${BUILD:-Release}"

cd jsockd_server

./mk.sh "$BUILD"
TOOLCHAIN_FILE=TC-fil-c.cmake ./mk.sh "$BUILD"

JS_SERVER=build_$BUILD/jsockd
FILC_JS_SERVER=build_${BUILD}_TC-fil-c.cmake/jsockd

npm ci

./node_modules/.bin/esbuild --target=es2018 --format=esm --bundle tests/e2e/relative_bench/bench.jsx --outfile=tests/e2e/relative_bench/bundle.js

rm -f filc_bench_private_signing_key*
"build_$BUILD/jsockd" -k filc_bench_private_signing_key

export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=$(cat filc_bench_private_signing_key.pubkey)

"build_$BUILD/jsockd" -c tests/e2e/relative_bench/bundle.js tests/e2e/relative_bench/bundle.qjsbc -k filc_bench_private_signing_key.privkey

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

# Time n component renders using node after warmup
total_nodejs_ns=$( node -e "const m = await import('./tests/e2e/relative_bench/bundle.js'); for (let i = 0; i < 10; ++i) { console.log('OUT', m.renderToString(m.createElement(m.AccordionDemo))); } /* <-- allow warm up before timing */ console.time('render'); for (let i = 0; i < Number(process.argv[1]); ++i) { m.renderToString(m.createElement(m.AccordionDemo, JSON.parse('{}'))) } console.timeEnd('render')" $N_VS_NODE_ITERATIONS | grep '^render' | awk '{print $2}' | perl -e '$line = <>; $_ =~ s/ms$//; print int(($line * 1000000) + 0.5)' )

rm -f /tmp/jsockd_relative_bench_vs_node_command_input
i=0
while [ $i -lt $N_VS_NODE_ITERATIONS ]; do
   printf "x\nm => m.renderToString(m.createElement(m.AccordionDemo))\n{}\n?exectime\n" >> /tmp/jsockd_relative_bench_vs_node_command_input
   i=$(($i + 1))
done
echo "?quit" >> /tmp/jsockd_relative_bench_vs_node_command_input

# Start the server (regular Linux/x86_64)
"./build_$BUILD/jsockd" -m tests/e2e/relative_bench/bundle.qjsbc -s /tmp/jsockd_filc_relative_bench_sock &
i=0
while ! [ -e /tmp/jsockd_filc_relative_bench_sock ] && [ $i -lt 15 ]; do
  echo "Waiting for regular x86_64 server to start for bench vs. NodeJS"
  sleep 1
  i=$(($i + 1))
done
sleep 1
echo "Regular x86_64 server started for NodeJS bench..."

# Time n component renders using JSockD Linux/x86_64.
total_jsockd_ns=$(nc -U /tmp/jsockd_filc_relative_bench_sock < /tmp/jsockd_relative_bench_vs_node_command_input | awk '/^[0-9]/ { n+=$1 } END { print n }')

# Start the server (Fil-C Linux/x86_64)
rm -f /tmp/jsockd_filc_relative_bench_sock
"./build_${BUILD}_TC-fil-c.cmake/jsockd" -t 2000000 -m tests/e2e/relative_bench/bundle.qjsbc -s /tmp/jsockd_filc_relative_bench_sock &
i=0
while ! [ -e /tmp/jsockd_filc_relative_bench_sock ] && [ $i -lt 15 ]; do
  echo "Waiting for Fil-C x86_64 server to start for bench vs. NodeJS"
  sleep 1
  i=$(($i + 1))
done
sleep 1
echo "Fil-C x86_64 server started for NodeJS bench..."

# Time n component renders using JSockD Fil-C Linux/x86_64.
total_jsockd_filc_ns=$(nc -U /tmp/jsockd_filc_relative_bench_sock < /tmp/jsockd_relative_bench_vs_node_command_input | awk '/^[0-9]/ { n+=$1 } END { print n }')

echo "Time to render React component $N_VS_NODE_ITERATIONS times":
echo "  NodeJS:       $total_nodejs_ns ns"
echo "  JSockD:       $total_jsockd_ns ns"
echo "  JSockD Fil-C: $total_jsockd_filc_ns ns"
