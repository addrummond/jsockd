#!/bin/sh

if [ -z "$CLANG_FORMAT_COMMAND" ] && [ ! -f ./node_modules/.bin/clang-format ]; then
  echo "Please run 'npm install' first to install clang-format."
  exit 1
fi

CLANG_FORMAT_COMMAND=${CLANG_FORMAT_COMMAND:-./node_modules/.bin/clang-format -i}

# The '.*/' pattern matches both './' and './/' and therefore works with both
# BSD and GNU find.
find ./ \( -name '*.c' -or -name '*.h' \) -not -path '.*/tests/unit/lib/*' -not -path '.*/src/lib/*' -not -path '.*/build_*' -exec $CLANG_FORMAT_COMMAND {} \;
