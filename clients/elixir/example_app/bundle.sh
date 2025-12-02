#!/bin/sh

set -e

(cd ../../../jsockd_server/ && ./mk.sh Debug )

mix deps.get
mix deps.compile

../../../jsockd_server/build_Debug/jsockd -c example_module.mjs priv/example_module.qjsbc
