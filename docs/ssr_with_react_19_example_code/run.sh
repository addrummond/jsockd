#!/bin/sh

which jsockd >/dev/null 2>&1
if [ $? -ne 0 ]; then
    echo "jsockd not found in PATH. Please install jsockd and ensure it is in your PATH."
    exit 1
fi

set -e

go mod tidy
rm -f mykey.privkey mykey.pubkey
jsockd -k mykey
npm i
npx esbuild ./server.jsx --bundle --outfile=server-bundle.mjs --sourcemap --format=esm
npx esbuild ./client.jsx --bundle --outfile=client-bundle.mjs --sourcemap --format=esm
jsockd -k mykey.privkey -c server-bundle.mjs server-bundle.quickjs_bytecode

export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=$(cat mykey.pubkey)

go run main.go
