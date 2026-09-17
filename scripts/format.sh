#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if ! command -v clang-format >/dev/null 2>&1; then
  echo "clang-format not found" >&2
  exit 1
fi

find "$ROOT_DIR/include" "$ROOT_DIR/src" "$ROOT_DIR/tests" "$ROOT_DIR/examples" \
  -type f \
  \( -name "*.h" -o -name "*.hpp" -o -name "*.c" -o -name "*.cc" -o -name "*.cpp" -o -name "*.cxx" \) \
  -exec clang-format -i {} +
