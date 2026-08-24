#!/usr/bin/env python3
"""
A minimal KMS that speaks CryptaWireManifest@v3 over a Unix socket.

PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.

WHY THIS EXISTS BESIDE test/sql/crypta/fake_crypta.py, RATHER THAN REPLACING IT
-------------------------------------------------------------------------------
`test/sql/crypta/fake_crypta.py` speaks `CryptaWireManifest@v2`. The client in
the bench overlay (`src/crypta-provider/crypta_client.cpp:27`) speaks
`CryptaWireManifest@v3`, and refuses anything else with
`crypta refused the request: unsupported schema`. Every .test file beside that
fake also ATTACHes with `CRYPTA_SOCKET` / `CRYPTA_LAKE_ID`, options the catalog
no longer accepts - they are `ENCRYPTION_SOCKET` / `ENCRYPTION_LAKE_ID` now.
So the only fake-KMS fixture in the tree cannot serve the current client, and
the fixtures around it cannot even ATTACH. That is measured, not assumed - see
the ledger on the PR. The stale fake is left EXACTLY as it is because it is
evidence for issue #52; this file is the one that actually talks to the client.

WHAT THIS IS NOT: it is not a KMS. It performs no cryptography. The "wrapped"
blob is a reversible encoding of the DEK alongside the identity it was issued
for. Nothing here says anything about whether the real service's cryptography
is sound.

WHAT IT IS FOR: the single property the envelope end-to-end fixture needs and
cannot get any other way - a REACHABLE socket, so ATTACH's self-test passes and
the provider is installed on the catalog. Everything downstream of that (a
WRAPPED blob in ducklake_data_file, an unwrap on read) then becomes assertable
from a .test file.

It enforces the identity binding, because a fake that handed back a DEK for any
identity would make a substitution test pass vacuously.

The blob starts with the four bytes "DLK1", which base64-encode to "RExL" - the
magic `DuckLakeEncryptionProvider::LooksWrapped` keys on. A blob that did not
start that way would be read back as a plaintext key and the fixture would be
testing nothing.

  fake_kms.py <socket-path>
"""

import base64
import json
import os
import signal
import socket
import struct
import sys
import threading

BLOB_HEADER = b"DLK1"
WIRE_SCHEMA = "CryptaWireManifest@v3"


def read_exact(connection, count):
    buffer = b""
    while len(buffer) < count:
        chunk = connection.recv(count - len(buffer))
        if not chunk:
            return None
        buffer += chunk
    return buffer


def read_frame(connection):
    header = read_exact(connection, 4)
    if header is None:
        return None
    (length,) = struct.unpack(">I", header)
    return read_exact(connection, length)


def write_frame(connection, body):
    payload = body.encode("utf-8")
    connection.sendall(struct.pack(">I", len(payload)) + payload)


def dumps(payload):
    """
    Compact separators are NOT cosmetic. The provider's SelfTest looks for the
    literal substring `"ok":true`, so a health response pretty-printed as
    `"ok": true` is rejected as not-ok, and the lake never attaches.
    """
    return json.dumps(payload, separators=(",", ":"), sort_keys=True)


def error(message):
    return dumps({"schema": WIRE_SCHEMA, "status": "error", "message": message})


def wrap(identity, dek_base64):
    payload = dumps({"identity": identity, "dek": dek_base64}).encode("utf-8")
    return base64.b64encode(BLOB_HEADER + payload).decode("ascii")


def unwrap(identity, wrapped_base64):
    """
    Return the DEK, or None when the blob is not bound to this identity.
    """
    try:
        raw = base64.b64decode(wrapped_base64, validate=True)
    except Exception:
        return None
    if not raw.startswith(BLOB_HEADER):
        return None
    try:
        payload = json.loads(raw[len(BLOB_HEADER):].decode("utf-8"))
    except Exception:
        return None
    if payload.get("identity") != identity:
        return None
    return payload.get("dek")


#: Every operation is appended here, one CSV line per request, when
#: DUCKLAKE_FAKE_KMS_OPLOG is set. This is what lets a fixture assert that
#: something WAS or WAS NOT asked of the key service. "The DEK was wrapped" is a
#: claim about a call, and no assertion on a catalog column alone carries it -
#: a column can hold a wrapped-looking blob for reasons other than a wrap.
OPLOG = os.environ.get("DUCKLAKE_FAKE_KMS_OPLOG")
OPLOG_LOCK = threading.Lock()


def record_operation(operation, item_count):
    if not OPLOG:
        return
    with OPLOG_LOCK:
        with open(OPLOG, "a") as handle:
            handle.write("%s,%d\n" % (operation, item_count))
            handle.flush()


def handle(body):
    request = json.loads(body.decode("utf-8"))
    if request.get("schema") != WIRE_SCHEMA:
        return error("unsupported schema")
    operation = request.get("op")
    record_operation(operation, len(request.get("items", [])))

    if operation == "health":
        return dumps({"schema": WIRE_SCHEMA, "status": "ok", "ok": True, "root": "fake-kms-for-sql-fixtures"})

    items = request.get("items", [])
    # Every reply item ECHOES the identity it answers for, exactly as the real
    # service does. `dumps` uses sort_keys=True, so the echo comes back with its
    # members in a DIFFERENT order than the client wrote them - deliberate, and
    # load-bearing: it is what requires the client to compare the four VALUES
    # rather than the bytes it sent.
    if operation == "wrap_batch":
        wrapped = [{"identity": item["identity"], "wrapped": wrap(item["identity"], item["dek"])} for item in items]
        return dumps({"schema": WIRE_SCHEMA, "status": "ok", "items": wrapped})

    if operation == "unwrap_batch":
        results = []
        for item in items:
            dek = unwrap(item["identity"], item["wrapped"])
            if dek is None:
                return error("unwrap failed: not valid for this KEK and file identity")
            results.append({"identity": item["identity"], "dek": dek})
        return dumps({"schema": WIRE_SCHEMA, "status": "ok", "items": results})

    return error("unsupported op: %s" % operation)


def serve(connection):
    try:
        while True:
            body = read_frame(connection)
            if body is None:
                return
            try:
                response = handle(body)
            except Exception as exception:
                response = error("fake kms failed: %s" % exception)
            write_frame(connection, response)
    finally:
        connection.close()


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    if os.path.exists(path):
        os.unlink(path)

    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(64)
    sys.stdout.write("fake kms listening on %s\n" % path)
    sys.stdout.flush()

    def shutdown(_signal_number, _frame):
        try:
            listener.close()
        finally:
            os._exit(0)

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    while True:
        connection, _ = listener.accept()
        threading.Thread(target=serve, args=(connection,), daemon=True).start()


if __name__ == "__main__":
    main()
