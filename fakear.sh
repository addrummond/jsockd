#!/bin/sh
# ar-to-lib: mimic GNU ar, call MSVC lib with translated flags.
# Usage examples:
#   ar-to-lib.sh rcs libfoo.a a.o b.o
#   ar-to-lib.sh t libfoo.a
#   ar-to-lib.sh d libfoo.a a.o
#   ar-to-lib.sh x libfoo.a a.o
#
# Notes:
# - Requires lib.exe in PATH (MSVC Developer Prompt or environment set up).
# - Keeps filenames as-is; does not translate extensions.
# - Supports operations: r/rc/rcs (create/update), t (list), d (delete), x (extract).
# - Ignores: -v, -s (indexing is implicit with lib).
# - Does not implement advanced GNU ar flags beyond the basics.

set -eu

if ! command -v lib >/dev/null 2>&1; then
  echo "error: lib.exe not found in PATH (run from MSVC Developer Prompt or set up env)" >&2
  exit 127
fi

op=""            # operation: r/rc/rcs/t/d/x
arflags=""       # raw flag token (for diagnostics)
archive=""       # .a/.lib
members=""       # space-separated members (.o/.obj)
verbose=0

# Helper: append to var with space
append() {
  eval "$1=\"\${$1}\${$1:+ }$2\""
}

# Filename translation removed: use names as provided

# Parse arguments in GNU ar style
# ar FLAGS ARCHIVE [MEMBERS...]
if [ "$#" -lt 2 ]; then
  echo "usage: ar-to-lib.sh <flags> <archive> [members...]" >&2
  exit 2
fi

arflags="$1"; shift
archive="$1"; shift

# Decode flags token (e.g., rcs, rc, r, t, d, x; optional v, s)
case "$arflags" in
  *v*) verbose=1 ;;
esac
case "$arflags" in
  *t*) op="t" ;;
  *d*) op="d" ;;
  *x*) op="x" ;;
  *r*) op="r" ;;  # r/rc/rcs treated the same for MSVC lib
  *)
    echo "error: unsupported ar flags: $arflags" >&2
    exit 2
    ;;
esac

# Collect remaining members (if any)
while [ "$#" -gt 0 ]; do
  append members "$1"
  shift
done

# Map archive and members (no translation)
libfile="$archive"

# Verbose helper
log() { [ "$verbose" -eq 1 ] && echo "$*" >&2 || : ; }

case "$op" in
  r)
    # Create/update: lib /OUT:libfile members...
    if [ -z "$members" ]; then
      echo "error: r/rc/rcs requires members" >&2
      exit 2
    fi
    # Use members as-is
    log "lib /OUT:$libfile $members"
    exec lib /NOLOGO /OUT:"$libfile" $members
    ;;

  t)
    # List archive contents: lib /LIST libfile
    log "lib /LIST $libfile"
    exec lib /NOLOGO /LIST "$libfile"
    ;;

  d)
    # Delete members: lib /REMOVE:member ... /OUT:libfile libfile
    if [ -z "$members" ]; then
      echo "error: d requires members" >&2
      exit 2
    fi
    # Build REMOVE args (no translation)
    remargs=""
    for m in $members; do
      append remargs "/REMOVE:$m"
    done
    # MSVC lib rewrites the archive; provide /OUT explicitly
    log "lib $remargs /OUT:$libfile $libfile"
    exec lib /NOLOGO $remargs /OUT:"$libfile" "$libfile"
    ;;

  x)
    # Extract members: lib /EXTRACT:member ... libfile
    if [ -z "$members" ]; then
      echo "error: x requires members" >&2
      exit 2
    fi
    exargs=""
    for m in $members; do
      append exargs "/EXTRACT:$m"
    done
    log "lib $exargs $libfile"
    exec lib /NOLOGO $exargs "$libfile"
    ;;

  *)
    echo "error: unexpected op: $op" >&2
    exit 2
    ;;
esac
