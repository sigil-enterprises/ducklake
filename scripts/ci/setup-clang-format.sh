#!/usr/bin/env bash
# Makes a `clang-format` v11 binary available at TOOLS_BIN/clang-format.
#
# Locates a real clang-format11 install rather than requiring `pip install
# clang_format==11.0.1`, which has no arm64 wheel (no CPython arm64 manylinux
# build was ever published for that package) and so cannot install on Apple
# Silicon. CI (ubuntu-22.04, x86_64) keeps using the pip package unchanged;
# this script is the arm64/macOS path.
set -euo pipefail

TOOLS_BIN="$1"
mkdir -p "$TOOLS_BIN"

if command -v clang-format >/dev/null 2>&1 && clang-format --version | grep -q '11\.'; then
  ln -sf "$(command -v clang-format)" "$TOOLS_BIN/clang-format"
  exit 0
fi

for candidate in \
  clang-format-11 \
  /opt/homebrew/opt/clang-format@11/bin/clang-format-11 \
  /usr/local/opt/clang-format@11/bin/clang-format-11 \
  /opt/homebrew/Cellar/clang-format@11/*/bin/clang-format-11 \
  /usr/local/Cellar/clang-format@11/*/bin/clang-format-11
do
  for hit in $candidate; do
    if [ -x "$hit" ]; then
      ln -sf "$hit" "$TOOLS_BIN/clang-format"
      exit 0
    fi
  done
done

echo "No clang-format 11.x found. On macOS/arm64, install it with:" >&2
echo "    brew install clang-format@11" >&2
echo "(pip's clang_format==11.0.1 has no arm64 wheel, so it cannot be used here.)" >&2
exit 1
