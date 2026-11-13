#!/bin/sh
# Print the flags `lake build` needs on Linux:
#   lake build -Kcflags="$(scripts/linux-flags.sh cflags)" -Klinklibs="$(scripts/linux-flags.sh libs)"
# Libraries are named by absolute path: Lean's lld searches no distro directories, and
# adding -L/usr/lib64 would pair Lean's libc startup files with the distro's libc.
set -eu
case "${1:-}" in
  cflags) pkg-config --cflags gio-2.0 ;;
  libs)
    for l in gio-2.0 gobject-2.0 glib-2.0; do
      p=$(cc -print-file-name=lib$l.so)
      case "$p" in /*) printf '%s ' "$p" ;; *) echo "lib$l.so not found: install libglib2.0-dev (Debian) or glib2-devel (Fedora)" >&2; exit 1 ;; esac
    done
    echo ;;
  *) echo "usage: $0 cflags|libs" >&2; exit 2 ;;
esac
