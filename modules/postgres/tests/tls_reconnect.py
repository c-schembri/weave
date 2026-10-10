"""Independent GSS-decline peer: a refused fresh direct-TLS socket cannot trigger host failover."""

import argparse
import json
import socket
import struct
import subprocess
import threading
import time


def listener():
    result = socket.socket()
    result.bind(("127.0.0.1", 0))
    result.listen(64)
    result.settimeout(0.1)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    args = parser.parse_args()
    audit = []
    errors = []
    lock = threading.Lock()
    stopped = threading.Event()
    sockets = []
    workers = []
    process = None
    started = time.monotonic()

    def refuse(sock):
        event = {"port": sock.getsockname()[1]}
        try:
            with sock:
                while not stopped.is_set():
                    try:
                        connection, _ = sock.accept()
                        break
                    except socket.timeout:
                        continue
                else:
                    return
                event["accepted_seconds"] = time.monotonic() - started
                # The peer is still awaiting N, so it cannot race the listener close.
                sock.close()
                with connection:
                    connection.settimeout(5)
                    request = bytearray()
                    while len(request) < 8:
                        block = connection.recv(8 - len(request))
                        if not block:
                            raise RuntimeError("Early EOF before GSS request")
                        request.extend(block)
                    event["request"] = request.hex()
                    if request != struct.pack("!II", 8, 80877104):
                        raise RuntimeError("Not a GSSENCRequest")
                    connection.sendall(b"N")
                    event["after_decline"] = connection.recv(65536).hex()
                    if event["after_decline"]:
                        raise RuntimeError("Direct TLS reused the old socket")
        except Exception as error:
            with lock:
                errors.append(str(error))
        finally:
            with lock:
                audit.append(event)

    secondary_count = 0

    def secondary(sock):
        nonlocal secondary_count
        sock.settimeout(0.1)
        while not stopped.is_set():
            try:
                connection, _ = sock.accept()
            except socket.timeout:
                continue
            with connection:
                secondary_count += 1

    try:
        process = subprocess.Popen([args.executable], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        header = process.stdout.readline().strip()
        if not header.startswith("sessions "):
            raise RuntimeError("Probe did not publish its session count: " + header)
        count = int(header.split()[1])
        assert count in {4, 20, 36}
        second = listener()
        sockets.append(second)
        inputs = [str(second.getsockname()[1])]
        worker = threading.Thread(target=secondary, args=(second,))
        workers.append(worker)
        worker.start()
        for _ in range(count):
            first = listener()
            sockets.append(first)
            inputs.append(str(first.getsockname()[1]))
            worker = threading.Thread(target=refuse, args=(first,))
            workers.append(worker)
            worker.start()
        stdout, stderr = process.communicate("\n".join(inputs) + "\n", timeout=90)
    finally:
        if process is not None:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)
        stopped.set()
        for worker in workers:
            worker.join(timeout=12)
        for sock in sockets:
            sock.close()
    report = dict(returncode=process.returncode, sessions=count, audited=len(audit), secondary=secondary_count,
                  audit=audit, errors=errors, stdout=stdout, stderr=stderr,
                  live_threads=sum(worker.is_alive() for worker in workers))
    print(json.dumps(report, indent=2), flush=True)
    assert process.returncode == 0 and not stderr and not errors
    assert stdout.startswith("PASS refusal: ")
    assert len(audit) == count and secondary_count == 0 and not report["live_threads"]
    assert len({event["port"] for event in audit}) == count
    assert all(event["request"] == struct.pack("!II", 8, 80877104).hex()
               and not event["after_decline"] for event in audit)


if __name__ == "__main__":
    main()
