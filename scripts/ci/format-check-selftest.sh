#!/usr/bin/env bash
# Positive control for `make format-check`: a check that only ever reports
# "clean" is indistinguishable from one that silently stopped checking
# anything, so this proves the gate actually fires on a bad file before its
# clean runs are trusted.
set -euo pipefail

PROJ_DIR="$1"
cd "$PROJ_DIR"

TMP_FILE="src/format_check_selftest_fixture.cpp"
cleanup() { rm -f "$TMP_FILE"; }
trap cleanup EXIT

cat > "$TMP_FILE" <<'EOF'
#include <cstdio>
int   BadlyFormatted (  int x,int y ) {
        if(x>y){
    return x;
        }
  return y;
}
EOF

set +e
make format-check >/tmp/format-check-selftest-bad.log 2>&1
BAD_STATUS=$?
set -e

if [ "$BAD_STATUS" -eq 0 ]; then
  echo "FAIL: format-check passed on a deliberately malformed file - the gate is not firing." >&2
  cat /tmp/format-check-selftest-bad.log >&2
  exit 1
fi
echo "OK: format-check correctly failed on the malformed fixture (exit $BAD_STATUS)."

# Overwrite with already-clean content rather than running the tree-wide
# fixer, so this control cannot reformat unrelated files as a side effect.
cat > "$TMP_FILE" <<'EOF'
#include <cstdio>
int BadlyFormatted(int x, int y) {
	if (x > y) {
		return x;
	}
	return y;
}
EOF

set +e
make format-check >/tmp/format-check-selftest-good.log 2>&1
GOOD_STATUS=$?
set -e

if [ "$GOOD_STATUS" -ne 0 ]; then
  echo "FAIL: format-check still failed after auto-fixing the fixture." >&2
  cat /tmp/format-check-selftest-good.log >&2
  exit 1
fi
echo "OK: format-check passes once the fixture is fixed - red then green confirmed."
