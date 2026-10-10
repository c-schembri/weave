"""Test-only LDAP observer and empty-value injector, never production protocol code."""
import select
import socket
import threading
import time


def element(data, offset):
    if len(data) < offset + 2:
        return None
    tag = data[offset]
    length = data[offset + 1]
    start = offset + 2
    if length & 128:
        count = length & 127
        if count == 0 or count > 4:
            raise RuntimeError("Unexpected LDAP envelope length")
        if len(data) < start + count:
            return None
        length = int.from_bytes(data[start:start + count], "big")
        start += count
    if length > 2 * 1024 * 1024:
        raise RuntimeError("Fixture LDAP envelope exceeded bound")
    if len(data) < start + length:
        return None
    return tag, start, start + length


def encode(tag, data):
    length = len(data)
    if length < 128:
        prefix = bytes([length])
    else:
        width = (length.bit_length() + 7) // 8
        prefix = bytes([128 + width]) + length.to_bytes(width, "big")
    return bytes([tag]) + prefix + data


def empty_attribute(packet):
    """Inject one empty binary value, which slapadd deliberately refuses to import."""
    envelope = element(packet, 0)
    identifier = element(packet, envelope[1])
    operation = element(packet, identifier[2])
    if operation[0] != 0x64:
        return packet, False
    name = element(packet, operation[1])
    if name[0] != 4:
        raise RuntimeError("Expected search entry DN")
    attribute = encode(0x30, encode(4, b"description") + encode(0x31, encode(4, b"")))
    entry = encode(0x64, packet[operation[1]:name[2]] + encode(0x30, attribute))
    return encode(0x30, packet[envelope[1]:identifier[2]] + entry), True


class Observer:
    def __init__(self, upstream=None, stall_base=None):
        self.upstream = upstream
        self.stall_base = stall_base
        self.stop = threading.Event()
        self.listener = socket.socket()
        self.listener.bind(("0.0.0.0", 0))
        self.listener.listen(8)
        self.listener.settimeout(.1)
        self.port = self.listener.getsockname()[1]
        self.accepted = 0
        self.stalled = 0
        self.zero_entries = 0
        self.operations = []
        self.errors = []
        self.connections = []
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def run(self):
        try:
            while not self.stop.is_set():
                try:
                    client, _ = self.listener.accept()
                except TimeoutError:
                    continue
                self.accepted += 1
                if self.accepted > 64:
                    client.close()
                    raise RuntimeError("Fixture connection limit")
                with client:
                    if self.upstream is None:
                        self.hold(client)
                    else:
                        with socket.create_connection(self.upstream, timeout=2) as server:
                            self.forward(client, server)
        except OSError as error:
            if not self.stop.is_set():
                self.errors.append(repr(error))
        except Exception as error:
            self.errors.append(repr(error))

    def hold(self, client):
        deadline = time.monotonic() + 10
        while not self.stop.is_set() and time.monotonic() < deadline:
            readable, _, _ = select.select([client], [], [], .1)
            if readable and not client.recv(65536):
                return

    def forward(self, client, server):
        buffered = bytearray()
        responses = bytearray()
        rewrite_empty = False
        deadline = time.monotonic() + 10
        while not self.stop.is_set() and time.monotonic() < deadline:
            readable, _, _ = select.select([client, server], [], [], .1)
            for source in readable:
                received = source.recv(65536)
                if not received:
                    return
                if source is server:
                    responses.extend(received)
                    while envelope := element(responses, 0):
                        packet = bytes(responses[:envelope[2]])
                        del responses[:envelope[2]]
                        if rewrite_empty:
                            packet, changed = empty_attribute(packet)
                            self.zero_entries += int(changed)
                        client.sendall(packet)
                    continue
                buffered.extend(received)
                if len(buffered) > 2 * 1024 * 1024:
                    raise RuntimeError("Fixture receive limit")
                while envelope := element(buffered, 0):
                    tag, start, end = envelope
                    if tag != 0x30:
                        raise RuntimeError("Expected LDAPMessage")
                    identifier = element(buffered, start)
                    if identifier is None or identifier[0] != 2:
                        raise RuntimeError("Expected LDAP message identifier")
                    operation = element(buffered, identifier[2])
                    if operation is None:
                        raise RuntimeError("Missing LDAP operation")
                    base = None
                    if operation[0] == 0x63:
                        field = element(buffered, operation[1])
                        if field is None or field[0] != 4:
                            raise RuntimeError("Expected search base")
                        base = bytes(buffered[field[1]:field[2]])
                    self.operations.append({"tag": operation[0], "base": base.decode("utf8") if base else ""})
                    if base == b"cn=zero,dc=weave,dc=test":
                        rewrite_empty = True
                    packet = bytes(buffered[:end])
                    del buffered[:end]
                    if base == self.stall_base:
                        self.stalled += 1
                    else:
                        server.sendall(packet)

    def close(self):
        self.stop.set()
        self.thread.join(timeout=12)
        self.listener.close()
        if self.thread.is_alive():
            raise RuntimeError("Owned LDAP observer did not drain")
        if self.errors:
            raise RuntimeError("Owned LDAP observer failed: " + str(self.errors))
