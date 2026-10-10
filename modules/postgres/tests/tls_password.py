"""Observe terminal credential failures independently of the client's attempt report."""

import argparse
import json
import socket
import subprocess
import threading


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--runtime", action="store_true")
    args = parser.parse_args()
    listeners = []
    counts = [0, 0]
    data = []
    errors = []
    clients = []
    stopped = threading.Event()
    lock = threading.Lock()

    def consume(client):
        try:
            with client:
                client.settimeout(15)
                received = bytearray()
                while block := client.recv(4096):
                    received.extend(block)
                with lock:
                    data.append(received.hex())
        except OSError as error:
            with lock:
                errors.append(str(error))

    def accept(index):
        listener = listeners[index]
        # Drain completed connections still queued in listen() before counting the evidence.
        while True:
            try:
                client, _ = listener.accept()
            except socket.timeout:
                if stopped.is_set():
                    break
                continue
            with lock:
                counts[index] += 1
                worker = threading.Thread(target=consume, args=(client,))
                clients.append(worker)
            worker.start()

    for _ in range(2):
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(128)
        listener.settimeout(0.1)
        listeners.append(listener)
    workers = [threading.Thread(target=accept, args=(index,)) for index in range(2)]
    for worker in workers:
        worker.start()
    try:
        result = subprocess.run(
            [args.executable, *(str(listener.getsockname()[1]) for listener in listeners)],
            capture_output=True, text=True, timeout=90,
        )
    finally:
        stopped.set()
        for worker in workers:
            worker.join()
        for listener in listeners:
            listener.close()
        for worker in clients:
            worker.join()
    report = {"returncode": result.returncode, "stdout": result.stdout, "stderr": result.stderr,
              "connections": counts, "received": data, "peer_errors": errors}
    print(json.dumps(report, indent=2), flush=True)
    expected = 40 if args.runtime else 16
    if result.returncode or counts != [expected, 0] or data != [""] * expected or errors:
        raise SystemExit("credential failure/failover regression failed")


if __name__ == "__main__":
    main()
