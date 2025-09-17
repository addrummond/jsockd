#!/bin/sh

export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=dangerously_allow_invalid_signatures

./node_modules/.bin/esbuild streamsrender.mjs --bundle --outfile=bundle.mjs --sourcemap --format=esm
(
    set -e
    cd ../jsockd_server
    ./mk.sh Debug
    build_Debug/jsockd -c ../readable-streams-test/bundle.mjs ../readable-streams-test/bundle.qjsbc
    JSOCKD=build_Debug/jsockd ../scripts/eval.sh 'x => x.cmd()' -m ../readable-streams-test/bundle.qjsbc -sm ../readable-streams-test/bundle.mjs.map
    #build_Debug/jsockd -m ../readable-streams-test/bundle.qjsbc -s /tmp/sock
)
