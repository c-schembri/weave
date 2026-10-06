"""Functional checks shared by the standalone and comparison echo servers."""

import argparse
import asyncio
from contextlib import asynccontextmanager, suppress
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import traceback


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


@asynccontextmanager
async def server(executable, port):
    process = await asyncio.create_subprocess_exec(
        str(executable), str(port), stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )
    first_line = asyncio.get_running_loop().create_future()
    output = bytearray()

    async def capture_output():
        try:
            while line := await process.stdout.readline():
                output.extend(line)
                if not first_line.done():
                    first_line.set_result(line)
        finally:
            if not first_line.done():
                first_line.set_result(b"")

    capture = asyncio.create_task(capture_output())
    try:
        yield process, first_line
    except Exception:
        print(f"Echo server failure: executable={executable.name}, argument={port!r}, "
              f"pid={process.pid}, exit_code={process.returncode}", file=sys.stderr)
        raise
    finally:
        if process.returncode is None:
            process.kill()
        await asyncio.wait_for(process.wait(), 5)
        await asyncio.wait_for(capture, 5)
        text = output.decode("utf-8", errors="replace")
        require("AddressSanitizer" not in text, text)
        if text:
            print(text, end="")


async def connect(port):
    return await asyncio.wait_for(asyncio.open_connection("127.0.0.1", port), 5)


async def read_exactly(reader, count):
    return await asyncio.wait_for(reader.readexactly(count), 5)


async def check_eof(client):
    reader, writer = client
    writer.write_eof()
    await asyncio.wait_for(writer.drain(), 5)
    require(await asyncio.wait_for(reader.read(1), 5) == b"", "Unexpected trailing echo data.")


async def check(executable, usage_exit_code):
    for argument in ("-1", "65536", "abc", "80junk"):
        async with server(executable, argument) as (process, _):
            require(await asyncio.wait_for(process.wait(), 5) == usage_exit_code,
                    f"Invalid port {argument!r} did not return usage error {usage_exit_code}.")
    with socket.socket() as reserved:
        if os.name == "nt":
            reserved.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        reserved.bind(("127.0.0.1", 0))
        reserved.listen()
        async with server(executable, reserved.getsockname()[1]) as (process, _):
            require(await asyncio.wait_for(process.wait(), 5) == 1, "Occupied port did not return startup error 1.")

    async with server(executable, 0) as (process, first_line):
        line = (await asyncio.wait_for(first_line, 10)).decode("ascii").strip()
        match = re.fullmatch(r"(?:Listening on|\[INFO\] Weave echo:) 127\.0\.0\.1:(\d+)", line)
        require(match is not None, f"Unexpected startup output: {line!r}")
        port = int(match[1])
        clients = []
        sending = None
        try:
            # The first client stays idle while the other four make progress.
            for _ in range(5):
                clients.append(await connect(port))
            payload = bytes((index * 17 + 3) % 256 for index in range(65537))
            payloads = [bytes((value + index) % 256 for value in payload) for index in range(4)]
            for size in (1, 31, 4096, 8193, 65537):
                for (_, writer), data in zip(clients[1:], payloads):
                    expected = data[:size]
                    writer.write(expected[:1])
                    writer.write(expected[1:])
                    await asyncio.wait_for(writer.drain(), 5)
                for (reader, _), data in zip(clients[1:], payloads):
                    require(await read_exactly(reader, size) == data[:size], "Per-client echo payload mismatch.")
            for client in clients:
                await check_eof(client)

            # FIN may arrive while echo data is still outstanding: close only after echoing it all.
            for size in (0, 1, 4096, 65537):
                client = await connect(port)
                clients.append(client)
                reader, writer = client
                expected = payload[:size]
                writer.write(expected)
                writer.write_eof()
                await asyncio.wait_for(writer.drain(), 5)
                require(await read_exactly(reader, size) == expected, "Immediate half-close lost echo data.")
                require(await asyncio.wait_for(reader.read(1), 5) == b"", "Missing EOF after immediate half-close.")

            slow = await connect(port)
            clients.append(slow)
            reader, writer = slow
            native = writer.get_extra_info("socket")
            native.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
            native.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096)
            large = (payload * ((4 * 1024 * 1024 // len(payload)) + 1))[:4 * 1024 * 1024]

            async def send_large():
                writer.write(large)
                await writer.drain()
                writer.write_eof()

            sending = asyncio.create_task(send_large())
            await asyncio.sleep(0.15)

            # A backpressured session must not hold up another client's response.
            responsive = await connect(port)
            clients.append(responsive)
            responsive[1].write(payload)
            await asyncio.wait_for(responsive[1].drain(), 5)
            require(await read_exactly(responsive[0], len(payload)) == payload, "Slow reader blocked another client.")
            await check_eof(responsive)

            received = bytearray()
            while len(received) < len(large):
                chunk = await asyncio.wait_for(reader.read(min(16384, len(large) - len(received))), 5)
                require(bool(chunk),
                        f"Early EOF with backpressured echo data outstanding: {len(received)} of {len(large)} bytes.")
                received.extend(chunk)
            await asyncio.wait_for(sending, 5)
            require(received == large, "Bulk echo payload mismatch.")
            require(await asyncio.wait_for(reader.read(1), 5) == b"", "Trailing bulk echo data.")

            last = await connect(port)
            clients.append(last)
            last[1].write(payload[:1])
            await asyncio.wait_for(last[1].drain(), 5)
            require(await read_exactly(last[0], 1) == payload[:1], "Existing session did not start.")

            reset = await connect(port)
            clients.append(reset)
            reset[1].get_extra_info("socket").setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                                       struct.pack("HH" if os.name == "nt" else "ii", 1, 0))
            reset[1].write(payload[:4096])
            await asyncio.wait_for(reset[1].drain(), 5)
            reset[1].transport.abort()
            last[1].write(payload)
            await asyncio.wait_for(last[1].drain(), 5)
            require(await read_exactly(last[0], len(payload)) == payload, "Existing session failed after peer reset.")
            await check_eof(last)

            fresh = await connect(port)
            clients.append(fresh)
            fresh[1].write(payload)
            await asyncio.wait_for(fresh[1].drain(), 5)
            require(await read_exactly(fresh[0], len(payload)) == payload, "New session failed after peer reset.")
            await check_eof(fresh)
            require(process.returncode is None, "Server exited unexpectedly.")
            print("PASS: concurrent clients, binary/fragmented data, backpressure, half-close, reset recovery, startup errors.")
        finally:
            if sending is not None and not sending.done():
                sending.cancel()
            if sending is not None:
                with suppress(asyncio.CancelledError, OSError):
                    await sending
            for _, writer in clients:
                writer.close()
            for _, writer in clients:
                with suppress(OSError, TimeoutError):
                    await asyncio.wait_for(writer.wait_closed(), 5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--usage-exit-code", type=int, default=2)
    args = parser.parse_args()
    try:
        asyncio.run(check(args.executable.resolve(strict=True), args.usage_exit_code))
        return 0
    except (OSError, RuntimeError, TimeoutError, asyncio.IncompleteReadError) as error:
        print(f"Error: {type(error).__name__}: {error}", file=sys.stderr)
        traceback.print_exc()
        return 1


if __name__ == "__main__":
    sys.exit(main())
