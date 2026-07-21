#!/usr/bin/env bash
set -euo pipefail

LLVM_BIN=""
for p in /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin; do
  if [ -x "$p/clang" ]; then LLVM_BIN="$p"; break; fi
done
if [ -z "$LLVM_BIN" ]; then
  echo "Homebrew LLVM not found. Install with: brew install llvm" >&2
  exit 1
fi

export PATH="$LLVM_BIN:$PATH"

make "$@"

XBE="bin/default.xbe"
if [ -f "$XBE" ]; then
  XEMU="/Applications/xemu.app/Contents/MacOS/xemu"
  if [ -x "$XEMU" ]; then
    echo "Launching xemu..."
    "$XEMU" -dvd_path "$XBE" -device lpc47m157 -serial stdio &
  else
    echo "xemu not found at $XEMU" >&2
  fi
fi
