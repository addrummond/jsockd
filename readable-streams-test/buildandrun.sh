#!/bin/sh

export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=dangerously_allow_invalid_signatures

./node_modules/.bin/esbuild streamsrender.mjs --bundle --outfile=bundle.mjs --sourcemap --format=esm
(
    set -e
    cd ../jsockd_server
    ./mk.sh Debug
    JSOCKD=build_Debug/jsockd ../scripts/eval.sh 'x => x.cmd()' -m ../readable-streams-test/bundle.qjsbc
)
