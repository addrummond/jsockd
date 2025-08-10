#!/bin/bash
# ^ specifying bash for this one as we need the 'time' keyword

set -e

N_ITERATIONS=10000

# TODO: complete
exit 0

cd js_server

./mk.sh Release
TOOLCHAIN_FILE=TC-fil-c.cmake ./mk.sh Release

# build_Debug_TC-fil-c.cmake
JS_SERVER=build_Release/js_server
FILC_JS_SERVER=build_Release_TC-fil-c.cmake/js_server

npm install

./node_modules/.bin/esbuild --target=es2018 --format=esm --bundle tests/e2e/filc_relative_bench/bench.jsx --outfile=tests/e2e/filc_relative_bench/bundle.js

openssl genpkey -algorithm ed25519 -out filc_bench_private_signing_key.pem
openssl pkey -inform pem -pubout -outform der -in filc_bench_private_signing_key.pem | tail -c 32 | xxd -p | tr -d '\n' > filc_bench_public_signing_key

export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=$(cat filc_bench_public_signing_key)

../tools-bin/compile_es6_module tests/e2e/filc_relative_bench/bundle.js tests/e2e/filc_relative_bench/bundle.qjsbc filc_bench_private_signing_key.pem

# Start the server (regular x86_64 build)
(
    ./build_Release/js_server -m tests/e2e/filc_relative_bench/bundle.qjsbc -s /tmp/jsockd_filc_relative_bench_sock ;
    echo $? > /tmp/jsockd_filc_relative_bench_regular_server_exit_code
) &
regular_server_pid=$!
i=0
while ! [ -e /tmp/jsockd_filc_relative_bench_sock] && [ $i -lt 15 ]; do
  echo "Waiting for regular x86_64 server to start"
  sleep 1
  i=$(($i + 1))
done
sleep 1

time (
    i=0
    while [ $i -lt $N_ITERATIONS ]; do
    echo "Iteration $i"
    # Run the client against the regular x86_64 server
    nc -U /tmp/jsockd_filc_relative_bench_sock <<EOF
    command_id
    m => m.renderToString(m.createElement(m.AccordionDemo))
    0
    EOF
    i=$(($i + 1))
    done
)

echo ?quit > nc -U /tmp/jsockd_filc_relative_bench_sock

# m => m.renderToString(m.createElement(m.AccordionDemo))
