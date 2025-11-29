#!/bin/sh

set -e

n_iterations=$1

# A series of normal commands that succeed and yield a result.
awk 'BEGIN { for (i = 0; i < ARGV[1]; i++) { print i"\n(m) => { "i"; return m.getAValue().foo; }\n\"dummy_input1\"" } }' $n_iterations

# # A use of TextEncoder and TextDecoder
# awk 'BEGIN { for (i = 0; i < ARGV[1]; i++) { print i"\n_ => new TextDecoder().decode(new TextEncoder().encode(\"foo bar amp\"))\n\"dummy_input2\"" } }' $n_iterations

# # A series of normal commands that are aborted with `?reset` before input is sent.
# awk 'BEGIN { for (i = 0; i < ARGV[1]; i++) { print i"\n(m) => { "i"; return m.getAValue().foo; }\n?reset" } }' $n_iterations

# # A series of commands that fail with a syntax error.
# awk 'BEGIN { for (i = 0; i < ARGV[1]; i++) { print i"\n(m) => { "i"=99; }\n\"dummy_input2\"" } }' $n_iterations

# # A series of commands that fail with a runtime error.
# awk 'BEGIN { for (i = 0; i < ARGV[1]; i++) { print i"\n(m) => { "i"; ({}).foo.bar; }\n\"dummy_input3\"" } }' $n_iterations

# # A command that does a bunch of allocation and then times out.
# printf "uniqueid\n() => { let a = []; for (let i = 0; i < 10; ++i) { a.push({}); } ; for (;;) ; }\n\"dummy_input4\"\n"

# # A command where the command id is truncated
# printf "?truncated\n(m) => 1\n\"dummy_input5\"\n"

# # A command where the query is truncated
# printf "a_unique_id\n?truncated\n\"(m) => 1\n\"dummy_input6\"\n"

# # A command where the param is truncated
# printf "a_unique_id2\n(m) => 1\n?truncated\n"

# # A command where field is truncated
# printf "?truncated\n?truncated\n?truncated\n"

# # An invalid input sequence that triggered a memory leak bug once.
# cat <<EOF
# x
# x => { return [1,2,3]; };
# "input"x
# x => { return [1,2,3]; };
# "input"y
# EOF

# # Force thread state reset a couple of times to check for memory leaks on that code path
# printf "?tsreset\nunique\nx => 'foo'\n99\n?tsreset\nunique\nx => 'foo'\n99\nunique\nx => 'foo'\n99\n"
