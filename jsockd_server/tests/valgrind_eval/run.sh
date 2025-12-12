#!/bin/sh

set -e

cd jsockd_server
./mk.sh Debug

rm -f valgrind_private_signing_key*
./build_Debug/jsockd -k valgrind_private_signing_key
export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=$(cat valgrind_private_signing_key.pubkey)

# Compile the example module to QuickJS bytecode.
./build_Debug/jsockd -c tests/valgrind_server/bundle.mjs /tmp/bundle.qjsb -k valgrind_private_signing_key.privkey

# Run some eval commands, which shouldn't leak memory.

JSOCKD="valgrind --error-exitcode=1 --leak-check=full --track-origins=yes -- ./build_Debug/jsockd"

#$JSOCKD -e '(() => { return { foo: "bar" }; })()'
#$JSOCKD -m /tmp/bundle.qjsb -e '(() => { return { foo: "bar" }; })()'
#$JSOCKD -m /tmp/bundle.qjsb -e 'M.getAValue()';
#$JSOCKD -m /tmp/bundle.qjsb -e '(() => { return { foo: "bar" }; })()' -sm tests/valgrind_server/bundle.mjs.map
#$JSOCKD -m /tmp/bundle.qjsb -e 'M.getAValue()' -sm tests/valgrind_server/bundle.mjs.map

#echo '(() => { return { foo: "bar" }; })()' > /tmp/valgrind_eval_test_input ; $JSOCKD -e - < /tmp/valgrind_eval_test_input
#echo '(() => { return { foo: "bar" }; })()' > /tmp/valgrind_eval_test_input ; $JSOCKD -m /tmp/bundle.qjsb -e - < /tmp/valgrind_eval_test_input
#echo 'M.getAValue()' > /tmp/valgrind_eval_test_input ; $JSOCKD -m /tmp/bundle.qjsb -e - < /tmp/valgrind_eval_test_input
echo '(() => { return { foo: "bar" }; })()' > /tmp/valgrind_eval_test_input ;  $JSOCKD -m /tmp/bundle.qjsb -e - -sm tests/valgrind_server/bundle.mjs.map < /tmp/valgrind_eval_test_input
#echo 'M.getAValue()' > /tmp/valgrind_eval_test_input ; $JSOCKD -m /tmp/bundle.qjsb -e - -sm tests/valgrind_server/bundle.mjs.map < /tmp/valgrind_eval_test_input
