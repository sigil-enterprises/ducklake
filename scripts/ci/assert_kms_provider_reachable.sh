#!/usr/bin/env bash
#
# PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
#
# Assert AT RUNTIME that this build carries a concrete KMS envelope provider.
#
# WHY NOT A SYMBOL GREP. A pattern naming a symbol with internal linkage can
# never match, so a check built on one reports a confident zero on a healthy
# build and a dead one alike - the defect commit 7f910fe went red for. This asks
# the built binary instead, and reads which of two refusals it gives.
#
# THE TWO ARMS, and why they are a control rather than a formality:
#   provider ABSENT  -> ATTACH refuses with "has no KMS encryption provider",
#                       thrown BEFORE the factory is called.
#   provider PRESENT -> the factory is called, the provider tries the socket,
#                       and refuses with "cannot reach the key service".
# The socket path handed in below does not exist, so a provider-carrying build
# can ONLY produce the second. The first is exactly what a provider-less build
# produces, which is what makes a pass here mean something.
#
# REFUSAL vs CRASH: a refusal exits non-zero WITH an `::error::` annotation. A
# non-zero exit without one is a crash.

set -uo pipefail

DUCKDB="${1:?usage: assert_kms_provider_reachable.sh <duckdb-binary>}"

if [ ! -x "${DUCKDB}" ]; then
  echo "::error::no duckdb binary at ${DUCKDB}"
  exit 1
fi

WORK="$(mktemp -d /tmp/kmsreach.XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT

DEAD_SOCKET="${WORK}/definitely-not-a-socket"

output="$("${DUCKDB}" -c "ATTACH 'ducklake:${WORK}/probe.db' AS probe (DATA_PATH '${WORK}/files', ENCRYPTED, ENCRYPTION_SOCKET '${DEAD_SOCKET}', ENCRYPTION_LAKE_ID 'linkage-probe')" 2>&1)"
status=$?

echo "--- attach output ---"
echo "${output}"
echo "--- end ---"

if [ "${status}" -eq 0 ]; then
  echo "::error::an ATTACH pointing ENCRYPTION_SOCKET at ${DEAD_SOCKET}, which does not exist, SUCCEEDED. Either the envelope options were ignored or the provider never tried to reach anything."
  exit 1
fi

if grep -qF "has no KMS encryption provider" <<< "${output}"; then
  echo "::error::this build has NO concrete KMS encryption provider - the factory is unregistered, so every enveloped ATTACH is refused and no envelope fixture can run. Overlay a provider into src/crypta-provider before building."
  exit 1
fi

if ! grep -qF "cannot reach the key service" <<< "${output}"; then
  echo "::error::the ATTACH was refused, but by neither of the two refusals this control can read. The control cannot tell a provider-carrying build from a provider-less one, so it proves nothing. Refusal was: ${output}"
  exit 1
fi

echo "this build carries a KMS provider: the factory ran and the provider, not the catalog, refused the dead socket"
