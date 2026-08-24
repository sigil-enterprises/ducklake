#!/usr/bin/env python3
"""
A minimal crypta that speaks CryptaWireManifest@v2 over a Unix socket.

PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.

WHAT THIS IS NOT: it is not crypta, and it performs no cryptography. There is no
KEK, no HSM, no AEAD - the "wrapped" blob is a reversible encoding of the DEK
alongside the identity it was issued for. Nothing here says anything about
whether crypta's cryptography is sound; `scripts/mvp_crypta_proof.sh` is what
runs against the real service.

WHAT IT IS FOR: the one property the SQL fixture group needs and cannot get any
other way - a socket that is REACHABLE, so ATTACH's self-test passes and the
provider is actually installed on the catalog. Everything downstream of that
(the wrapped blob in ducklake_data_file, the refusal of a plaintext key row, the
refusal of a blob bound to a different file) then becomes reachable from a
.test file.

It does enforce the binding, because a fake that handed back a DEK for any
identity would make the downgrade and substitution tests pass vacuously.

The blob layout mirrors the real one where it matters: it starts with the four
bytes "DLK1", which base64-encode to "RExL" - the magic `CryptaClient::
LooksWrapped` keys on. A blob that did not start that way would be treated as a
plaintext key and the fixture would be testing nothing.

  fake_crypta.py <socket-path>
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
WIRE_SCHEMA = "CryptaWireManifest@v2"


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
    Compact separators are NOT cosmetic. `DuckLakeCryptaProvider::SelfTest`
    looks for the literal substring `"ok":true`, so a health response
    pretty-printed as `"ok": true` is rejected as not-ok. The real crypta
    emits compact JSON; this has to as well or the fixture never attaches.
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
    # The whole point: all four components of the identity must match. A blob
    # pasted onto another file's row, or read under a different lake id, fails
    # here exactly as it fails against the real service.
    if payload.get("identity") != identity:
        return None
    return payload.get("dek")


#: Every operation is appended here, one CSV line per request, when
#: DUCKLAKE_FAKE_CRYPTA_OPLOG is set. This is what lets a .test file assert that
#: something was NOT asked of the key service - "the plaintext row was refused
#: BEFORE the socket" is a claim about a call that did not happen, and no
#: assertion on an error message can carry it.
OPLOG = os.environ.get("DUCKLAKE_FAKE_CRYPTA_OPLOG")
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
        return dumps({"schema": WIRE_SCHEMA, "status": "ok", "ok": True, "root": "fake-crypta-for-sql-fixtures"})

    items = request.get("items", [])
    # Every reply item ECHOES the identity it answers for, exactly as the real
    # service does - crypta's reply item is a `WrappedKeyEntry` / `PlainKey`, an
    # identity beside the value (src/server.rs). Until #31 this fake emitted the
    # bare value, and that made it useless as a fixture for the client's
    # reply-side binding: a guard that checks an echoed identity, driven by a
    # service that never echoes one, passes without ever running.
    #
    # `dumps` here uses sort_keys=True, so the echo comes back with its members
    # in a DIFFERENT order than the client wrote them. That is deliberate and it
    # is load-bearing: it is what requires the client to compare the four VALUES
    # rather than the bytes it sent, and a byte comparison would refuse this
    # fixture on every single ATTACH.
    if operation == "wrap_batch":
        wrapped = [
            {"identity": item["identity"], "wrapped": wrap(item["identity"], item["dek"])} for item in items
        ]
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
            except Exception as exception:  # a malformed request is the caller's problem, not a crash
                response = error("fake crypta failed: %s" % exception)
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
    # The runner waits for this line before starting the suite, so an ATTACH
    # never races the bind.
    sys.stdout.write("fake crypta listening on %s\n" % path)
    sys.stdout.flush()

    def shutdown(_signal_number, _frame):
        try:
            listener.close()
        finally:
            os.unlink(path) if os.path.exists(path) else None
            os._exit(0)

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    while True:
        try:
            connection, _ = listener.accept()
        except OSError:
            return
        threading.Thread(target=serve, args=(connection,), daemon=True).start()


if __name__ == "__main__":
    main()
