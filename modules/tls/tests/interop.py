"""Exercise both Weave TLS roles against Python's independent TLS adapter."""

import argparse
import asyncio
import os
import ssl
import subprocess


async def line(process):
    value = await asyncio.wait_for(process.stdout.readline(), 10)
    if not value:
        raise RuntimeError("TLS fixture exited before completing setup")
    return value.decode().strip()


def context(role, version):
    value = ssl.SSLContext(role)
    value.minimum_version = version
    value.maximum_version = version
    value.set_alpn_protocols(["echo"])
    return value


async def check(executable, mode, version):
    process = await asyncio.create_subprocess_exec(
        executable, mode, "12" if version == ssl.TLSVersion.TLSv1_2 else "13",
        stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )
    listener = None
    writers = []
    handlers = []

    try:
        ca, certificate, key = await line(process), await line(process), await line(process)
        mutual = mode.startswith("mtls-")
        if mutual:
            client_certificate, client_key = await line(process), await line(process)
        if mode in ("server", "mtls-server"):
            tls = context(ssl.PROTOCOL_TLS_CLIENT, version)
            tls.load_verify_locations(cafile=ca)
            if mutual:
                tls.load_cert_chain(client_certificate, client_key)
            port = int(await line(process))
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection("127.0.0.1", port, ssl=tls, server_hostname="localhost"), 10)
            writers.append(writer)
            if writer.get_extra_info("ssl_object").selected_alpn_protocol() != "echo":
                raise RuntimeError("ALPN negotiation mismatch")

            payload = bytes(range(256)) * 257
            writer.write(payload)
            await asyncio.wait_for(writer.drain(), 10)
            if await asyncio.wait_for(reader.readexactly(len(payload)), 10) != payload:
                raise RuntimeError("TLS payload mismatch")

            writer.close()
            await asyncio.wait_for(writer.wait_closed(), 10)
        else:
            tls = context(ssl.PROTOCOL_TLS_SERVER, version)
            tls.load_cert_chain(certificate, key)
            if mutual:
                tls.verify_mode = ssl.CERT_REQUIRED
                tls.load_verify_locations(cafile=ca)

            async def echo(reader, writer):
                writers.append(writer)
                try:
                    while data := await reader.read(4096):
                        writer.write(data)
                        await writer.drain()
                finally:
                    writer.close()
                    await writer.wait_closed()

            def connected(reader, writer):
                handlers.append(asyncio.create_task(echo(reader, writer)))

            listener = await asyncio.start_server(connected, "127.0.0.1", 0, ssl=tls)
            port = listener.sockets[0].getsockname()[1]
            process.stdin.write(f"{port}\n".encode())
            await process.stdin.drain()

        if await asyncio.wait_for(process.wait(), 10) != 0:
            raise RuntimeError((await process.stderr.read()).decode(errors="replace"))
        await asyncio.wait_for(asyncio.gather(*handlers), 10)

        print(f"Weave {mode} / {version.name}: passed")
    finally:
        if listener:
            listener.close()
            await listener.wait_closed()
        for writer in writers:
            writer.close()
        for handler in handlers:
            if not handler.done():
                handler.cancel()
        await asyncio.gather(*handlers, return_exceptions=True)
        if process.returncode is None:
            process.kill()
        await process.wait()
        output = (await process.stderr.read()).decode(errors="replace")
        if output:
            print(output)
        if "AddressSanitizer" in output:
            raise RuntimeError(output)


async def check_example(executable, example):
    fixture = await asyncio.create_subprocess_exec(
        executable, "client", "13", stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )
    server = None
    output = b""

    try:
        ca, certificate, key = await line(fixture), await line(fixture), await line(fixture)
        server = await asyncio.create_subprocess_exec(
            example, certificate, key, "0", stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        ready = await line(server)
        prefix = "[INFO] Weave TLS echo: 127.0.0.1:"
        if not ready.startswith(prefix):
            raise RuntimeError(f"Unexpected TLS example setup: {ready}")
        port = int(ready[len(prefix):])

        tls = context(ssl.PROTOCOL_TLS_CLIENT, ssl.TLSVersion.TLSv1_3)
        tls.load_verify_locations(cafile=ca)

        async def client(index):
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection("127.0.0.1", port, ssl=tls, server_hostname="localhost"), 10)
            try:
                payload = bytes([index]) * 65537
                writer.write(payload)
                await asyncio.wait_for(writer.drain(), 10)
                reply = await asyncio.wait_for(reader.readexactly(len(payload)), 10)
                if reply != payload:
                    raise RuntimeError("Concurrent TLS example payload mismatch")
            finally:
                writer.close()
                await asyncio.wait_for(writer.wait_closed(), 10)

        await asyncio.gather(*(client(index) for index in range(8)))
        fixture.stdin.close()
        if await asyncio.wait_for(fixture.wait(), 5) != 2:
            raise RuntimeError("Credential fixture did not exit cleanly after input EOF")
        print("TLS echo example / eight concurrent verified clients: passed")
    finally:
        if fixture.returncode is None:
            fixture.stdin.close()
            try:
                await asyncio.wait_for(fixture.wait(), 5)
            except TimeoutError:
                fixture.kill()
                await fixture.wait()
        if server:
            if server.returncode is None:
                server.kill()
            output = (await server.communicate())[0]
        diagnostics = output + await fixture.stderr.read()
        if diagnostics:
            print(diagnostics.decode(errors="replace"))
        if b"AddressSanitizer" in diagnostics:
            raise RuntimeError(diagnostics.decode(errors="replace"))


async def main(executable, example):
    if example:
        await check_example(executable, example)
        return

    for version in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3):
        for mode in ("server", "client", "mtls-server", "mtls-client"):
            await check(executable, mode, version)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--echo-example")
    arguments = parser.parse_args()
    asyncio.run(main(arguments.executable, arguments.echo_example))
