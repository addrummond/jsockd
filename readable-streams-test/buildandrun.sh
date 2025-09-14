#!/bin/sh

export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=dangerously_allow_invalid_signatures

./node_modules/.bin/esbuild streamsrender.mjs --bundle --outfile=bundle.mjs --sourcemap --format=esm
(
    set -e
    cd ../jsockd_server
    ./mk.sh Debug run -c ../readable-streams-test/bundle.mjs ../readable-streams-test/bundle.qjsbc
    ../jsockd_server/build_Debug/jsockd -m ../readable-streams-test/bundle.qjsbc -s /tmp/sock
)
