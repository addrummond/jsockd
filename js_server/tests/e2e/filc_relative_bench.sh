#!/bin/sh

set -e

cd js_server

./mk.sh Release
TOOLCHAIN_FILE=TC-fil-c.cmake ./mk.sh Release

# build_Debug_TC-fil-c.cmake
JS_SERVER=build_Release/js_server
FILC_JS_SERVER=build_Release_TC-fil-c.cmake/js_server

npm install

./node_modules/.bin/esbuild --target=es2018 --format=esm --bundle tests/e2e/filc_relative_bench/bench.jsx --inject:./tests/e2e/filc_relative_bench/shims.mjs --outfile=tests/e2e/filc_relative_bench/bundle.js

openssl genpkey -algorithm ed25519 -out filc_bench_private_signing_key.pem
openssl pkey -inform pem -pubout -outform der -in filc_bench_private_signing_key.pem | tail -c 32 | xxd -p | tr -d '\n' > filc_bench_public_signing_key

export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=$(cat filc_bench_public_signing_key)

../tools-bin/compile_es6_module tests/e2e/filc_relative_bench/bundle.js tests/e2e/filc_relative_bench/bundle.qjsbc filc_bench_private_signing_key.pem

# m => m.renderToString(m.createElement(m.AccordionDemo))
