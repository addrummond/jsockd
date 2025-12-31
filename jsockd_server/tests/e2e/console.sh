#!/bin/sh

set -e

cd jsockd_server
./mk.sh Debug
./mk.sh Release

CMD1='console.log("MESSAGE1", "MESSAGE2")'
RES1="<console.log>: MESSAGE1 MESSAGE2"

CMD2='console.warn("MESSAGE1", "MESSAGE2")'
RES2="<console.warn>: MESSAGE1 MESSAGE2"

CMD3='console.info("MESSAGE1", "MESSAGE2")'
RES3="<console.info>: MESSAGE1 MESSAGE2"

CMD4='console.error("MESSAGE1", "MESSAGE2")'
RES4="<console.error>: MESSAGE1 MESSAGE2"

CMD5='console.debug("MESSAGE1", "MESSAGE2")'
RES5="<console.debug>: MESSAGE1 MESSAGE2"

CMD6='console.trace("MESSAGE1", "MESSAGE2")'
RES6="<console.trace>: MESSAGE1 MESSAGE2"

CMD7='console.log("MESSAGE1", {foo: "bar"})'
RES7='<console.log>: MESSAGE1 { foo: "bar" }'

i=1
while [ ! -z "$(eval echo\ \$CMD$i)" ]; do
    CMD="$(eval echo \$CMD$i)"
    RES="$(eval echo \$RES$i)"
    printf "Running command '%s' with debug build..." "$CMD"
    OUTPUT="$(./build_Debug/jsockd -e "$CMD" 2>&1 | head -n 1)"
    if [ "$OUTPUT" != "$RES" ]; then
         printf "bad\n"
         echo "Bad output for command '$CMD':"
         echo "Expected: '$RES'"
         echo "Got:      '$OUTPUT'"
    else
        printf "ok\n"
    fi
    printf "Running command '%s' with release build..." "$CMD"
    OUTPUT="$(./build_Release/jsockd -e "$CMD" 2>&1 | head -n 1)"
    if [ "$OUTPUT" != "$RES" ]; then
         printf "bad\n"
         echo "Bad output for command '$CMD':"
         echo "Expected: '$RES'"
         echo "Got:      '$OUTPUT'"
    else
        printf "ok\n"
    fi
    i=$(($i + 1))
done
