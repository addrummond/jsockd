#!/bin/sh
# gcc-to-cl: mimic gcc, call MSVC cl with translated flags.
# Usage:
#   gcc-to-cl.sh [gcc-like options] files...
# Notes:
# - Requires cl.exe and link.exe (MSVC) in PATH.
# - Strips unknown gcc flags safely.
# - Handles -c compilation and linking for exe or dll (-shared).
# - Translates -I, -D, -U, -O*, -o, -L/-l, -shared.
# - Ignores: -fPIC, -Wall/-Wextra/-Werror, -std=*, -pedantic, -pipe, -M*, -fno-*, -fvisibility=*, etc.

set -eu

# Detect cl
if ! command -v cl >/dev/null 2>&1; then
  echo "error: cl.exe not found in PATH (run from MSVC Developer Prompt or set up env)" >&2
  exit 127
fi

# State
mode_compile=0      # -c
mode_shared=0       # -shared -> produce DLL
outfile=""          # -o <file>
cflags=""           # accumulated cl compile flags
ldflags=""          # accumulated link.exe flags (libraries, dirs)
incflags=""         # /I...
defflags=""         # /D...
undefs=""           # /U...
objs=""             # object files if -c
srcs=""             # source files (.c .cpp .cc)
libs=""             # libs for link.exe
libdirs=""          # /LIBPATH:...
other_cl=""         # passthrough to cl (rare)
other_link=""       # extra link flags

# Helper: append with space
append() {
  eval "$1=\"\${$1}\${$1:+ }$2\""
}

# Map common gcc optimization flags
map_opt() {
  case "$1" in
    -O0) append cflags /Od ;;
    -O1) append cflags /O1 ;;
    -O2|-Os) append cflags /O2 ;;
    -O3) append cflags /O2 ;; # cl has /O2, not O3
    *) ;;
  esac
}

# Parse args
i=1
while [ "$#" -gt 0 ]; do
  arg="$1"; shift

  case "$arg" in
    -c) mode_compile=1 ;;
    -shared) mode_shared=1 ;;
    -o)
      [ "$#" -gt 0 ] || { echo "error: -o requires an argument" >&2; exit 2; }
      outfile="$1"; shift
      ;;
    -U*)
      val="${arg#-U}"
      if [ -z "$val" ] && [ "$#" -gt 0 ]; then val="$1"; shift; fi
      [ -n "$val" ] && append undefs "/U $val"
      ;;
    -L*)
      val="${arg#-L}"
      if [ -z "$val" ] && [ "$#" -gt 0 ]; then val="$1"; shift; fi
      [ -n "$val" ] && append libdirs "/LIBPATH:$val"
      ;;
    -l*)
      val="${arg#-l}"
      if [ -z "$val" ] && [ "$#" -gt 0 ]; then val="$1"; shift; fi
      # Turn -lfoo into foo.lib unless it already ends with .lib
      case "$val" in
        *.lib) append libs "$val" ;;
        *) append libs "$val.lib" ;;
      esac
      ;;
    -O*)
      map_opt "$arg"
      ;;
    # Language and warnings (strip or map minimally)
    -Wall|-Wextra|-Werror|-Wno-*)
      : # ignore gcc warning flags
      ;;
    -std=*)
      # cl has /std:c11,/std:c17 only for C, but support varies; ignore to be safe
      # You can map -std=c17 -> /std:c17 if you know toolset supports it
      case "$arg" in
        -std=c11) append cflags /std:c11 ;;
        -std=c17) append cflags /std:c17 ;;
        *) : ;;
      esac
      ;;
    -fPIC|-fpic|-fno-plt|-fno-exceptions|-fno-rtti|-fvisibility=*|-fno-*)
      : # ignore unsupported gcc flags safely
      ;;
    -M|-MM|-MD|-MMD|-MP|-MF|-MT|-MQ)
      # dependency gen: ignore silently
      # if flag takes value, consume next
      case "$arg" in
        -MF|-MT|-MQ) [ "$#" -gt 0 ] && shift ;;
      esac
      ;;
    -static)
      # cl/link does static linking differently; ignore or add /MT if you specifically want static CRT
      append cflags /MT
      ;;
    -static-libgcc|-static-libstdc++)
      : ;;
    -g)
      # ignore
      ;;
    -Wl,*)
      # linker flags: parse minimal /SUBSYSTEM or /NODEFAULTLIB if needed
      lflags="${arg#-Wl,}"
      # split by comma
      IFS_SAVE="$IFS"; IFS=,
      for lf in $lflags; do
        case "$lf" in
          -subsystem=*)
            subsys="${lf#-subsystem=}"
            # map console/gui
            case "$subsys" in
              console) append other_link /SUBSYSTEM:CONSOLE ;;
              windows) append other_link /SUBSYSTEM:WINDOWS ;;
              *) append other_link "/SUBSYSTEM:$subsys" ;;
            esac
            ;;
          -nodefaultlib=*)
            nlib="${lf#-nodefaultlib=}"
            append other_link "/NODEFAULTLIB:$nlib"
            ;;
          -nodefaultlib)
            append other_link /NODEFAULTLIB
            ;;
          *) : ;;
        esac
      done
      IFS="$IFS_SAVE"
      ;;
    -m64|-m32)
      : # toolset selection handled outside; ignore
      ;;
    -*)
      # Unknown flag: best-effort pass-through to cl for compile-only flags
      # You may want to log these for visibility
      # echo "note: passing unknown flag to cl: $arg" >&2
      append other_cl "$arg"
      ;;
    *)
      # source or object
      case "$arg" in
        *.c|*.cc|*.cpp|*.cxx|*.C) append srcs "$arg" ;;
        *.o|*.obj) append objs "$arg" ;;
        *.lib) append libs "$arg" ;;
        *) append srcs "$arg" ;;
      esac
      ;;
  esac
done

# If no sources, nothing to do
if [ -z "$srcs" ] && [ -z "$objs" ]; then
  echo "error: no input files" >&2
  exit 2
fi

# Default to C17 if available; comment out if your toolset lacks /std:c17
# append cflags /std:c17

# Handle -c (compile only)
if [ "$mode_compile" -eq 1 ]; then
  # /Fo for object output; if multiple sources and single -o given, cl writes multiple .obj ignoring /Fo file form
  fo=""
  if [ -n "$outfile" ]; then
    fo="/Fo$outfile"
  fi
  set -x
  exec cmd "/c cl /nologo -c $cflags $incflags $defflags $undefs $other_cl $fo $srcs"
fi

# Linking path: produce EXE or DLL
# cl can both compile and link; for better control we can still use cl and pass link flags.
# Decide output:
fe=""
ldkind=""
if [ "$mode_shared" -eq 1 ]; then
  # DLL: /LD and /Fe for DLL name (cl accepts .dll target name)
  append cflags /LD
  if [ -n "$outfile" ]; then
    fe="/Fe$outfile"
  fi
  ldkind="dll"
else
  # EXE
  if [ -n "$outfile" ]; then
    fe="/Fe$outfile"
  fi
  ldkind="exe"
fi

# If user passed .o files, cl treats them as .obj; ensure extensions
objs_arg=""
if [ -n "$objs" ]; then
  objs_arg="$objs"
fi

# Final link flags (via /link separator)
linksep="/link"
linkargs="$other_link"
[ -n "$libdirs" ] && linkargs="$libdirs $linkargs"
[ -n "$libs" ] && linkargs="$linkargs $libs"

# Execute cl for compile+link
set -x
exec cmd "/c cl $cflags $incflags $defflags $undefs $other_cl $fe $srcs $objs_arg $linksep $CL_LDFLAGS $linkargs"
