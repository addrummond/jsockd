#!/bin/sh

set -e

npm i

export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=dangerously_allow_invalid_signatures

./node_modules/.bin/esbuild m.mjs --bundle --outfile=bundle.mjs --sourcemap --format=esm

echo "TODO need to replace jsockd_compile_es6_module with jsockd -c"
exit 1
../../../../tools-bin/jsockd_compile_es6_module bundle.mjs bundle.qjsbc

( cd ../../../ && ./mk.sh Debug run -s /tmp/sock -m tests/e2e/sourcemaps/bundle.qjsbc -sm tests/e2e/sourcemaps/bundle.mjs.map )
