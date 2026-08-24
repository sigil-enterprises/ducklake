#!/usr/bin/env bash
# Makes a `clang-format` binary matching CI's `pip install clang_format==11.0.1`
# available at TOOLS_BIN/clang-format.
#
# CI (ubuntu-22.04, x86_64) gets clang-format 11.0.1 from PyPI's manylinux
# x86_64 wheel. That same PyPI release ALSO ships a macOS
# `macosx_10_9_universal2` wheel that covers arm64 (Apple Silicon) fine - so on
# macOS, `pip install clang_format==11.0.1` just works and is tried first here,
# because it is bit-for-bit the same clang-format CI enforces against (no
# version-skew false failures).
#
# The gap is Linux arm64 (this repo's own arm64 devcontainer, see
# .devcontainer/Dockerfile's ARCHITECTURE NOTE): PyPI's clang_format==11.0.1
# publishes manylinux wheels only for i686/x86_64, none for aarch64/arm64, so
# the pip install fails there. For that case only, fall back to a system
# clang-format 11.x (Homebrew on macOS, apt/llvm.org on Linux). That binary is
# NOT guaranteed to byte-match CI's 11.0.1 - patch-level clang-format releases
# occasionally differ on line-wrapping edge cases - but it is the closest
# available substitute, and it is only reached when the exact match cannot
# install.
set -euo pipefail

TOOLS_BIN="$1"
VENV_PYTHON="$2"
mkdir -p "$TOOLS_BIN"

VENV_BIN="$(dirname "$VENV_PYTHON")"

# 1. Exact match: the same PyPI release CI uses.
if "$VENV_PYTHON" -m pip install -q 'clang-format==11.0.1' 2>/tmp/setup-clang-format-pip.log; then
  if [ -x "$VENV_BIN/clang-format" ]; then
    ln -sf "$VENV_BIN/clang-format" "$TOOLS_BIN/clang-format"
    exit 0
  fi
fi
echo "pip install clang_format==11.0.1 unavailable here (expected on Linux arm64 - no manylinux aarch64 wheel is published); falling back to a system clang-format 11.x." >&2
cat /tmp/setup-clang-format-pip.log >&2 2>/dev/null || true

# 2. Fallback: a system-provided clang-format 11.x (version may not exactly
# match CI's 11.0.1 patch release - see header comment).
if command -v clang-format >/dev/null 2>&1 && clang-format --version | grep -q '11\.'; then
  ln -sf "$(command -v clang-format)" "$TOOLS_BIN/clang-format"
  exit 0
fi

for candidate in \
  clang-format-11 \
  /opt/homebrew/opt/clang-format@11/bin/clang-format-11 \
  /usr/local/opt/clang-format@11/bin/clang-format-11 \
  /opt/homebrew/Cellar/clang-format@11/*/bin/clang-format-11 \
  /usr/local/Cellar/clang-format@11/*/bin/clang-format-11 \
  /usr/lib/llvm-11/bin/clang-format \
  /usr/bin/clang-format-11
do
  for hit in $candidate; do
    if [ -x "$hit" ]; then
      ln -sf "$hit" "$TOOLS_BIN/clang-format"
      exit 0
    fi
  done
done

echo "No clang-format 11.x found. Install one with:" >&2
echo "    macOS:  brew install clang-format@11" >&2
echo "    Debian/Ubuntu: see https://apt.llvm.org for a clang-format-11 package" >&2
exit 1
